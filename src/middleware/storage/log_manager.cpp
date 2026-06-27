#include "log_manager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>

namespace dev_sys {

namespace {
    // Thread-safe write guard
    std::mutex g_log_mutex;

    // Current timestamp in ISO 8601 UTC
    std::string now_iso() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm tm_buf;
        gmtime_r(&t, &tm_buf);
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
            << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
        return oss.str();
    }

    const char* level_str(LogManager::Level lvl) {
        switch (lvl) {
            case LogManager::Level::DEBUG: return "DEBUG";
            case LogManager::Level::INFO:  return "INFO";
            case LogManager::Level::WARN:  return "WARN";
            case LogManager::Level::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }

    // Escape special chars for JSON string
    std::string json_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 16);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;
            }
        }
        return out;
    }

    // Get file size in bytes
    int64_t file_size_bytes(const std::string& path) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        return ec ? 0 : static_cast<int64_t>(sz);
    }
}

struct LogManager::Impl {
    std::string log_dir;
    Level       min_level = Level::INFO;
    int         max_file_size_mb = 50;
    int         max_files = 10;
    std::string trace_id;
    std::string current_file_path;
    std::ofstream file_stream;
};

LogManager::LogManager()
    : impl_(std::make_unique<Impl>()) {}

LogManager::~LogManager() {
    if (impl_->file_stream.is_open()) {
        impl_->file_stream.flush();
        impl_->file_stream.close();
    }
}

StatusCode LogManager::init(const std::string& log_dir,
                             Level min_level,
                             int max_file_size_mb,
                             int max_files) {
    impl_->log_dir          = log_dir;
    impl_->min_level        = min_level;
    impl_->max_file_size_mb = max_file_size_mb;
    impl_->max_files        = max_files;

    // Create log directory if it doesn't exist
    std::error_code ec;
    if (!std::filesystem::exists(log_dir, ec)) {
        if (!std::filesystem::create_directories(log_dir, ec)) {
            std::cerr << "[LogManager] Failed to create log dir: " << log_dir
                      << " (" << ec.message() << ")" << std::endl;
            return StatusCode::ERROR;
        }
    }

    // Open current log file
    impl_->current_file_path = log_dir + "/dev-sys-cloud.log";
    impl_->file_stream.open(impl_->current_file_path, std::ios::out | std::ios::app);
    if (!impl_->file_stream.is_open()) {
        std::cerr << "[LogManager] Failed to open log file: "
                  << impl_->current_file_path << std::endl;
        return StatusCode::ERROR;
    }

    // Write startup marker
    write(Level::INFO, "log_manager", "Log system initialized. dir=" + log_dir);

    return StatusCode::OK;
}

void LogManager::set_level(Level level) {
    impl_->min_level = level;
}

void LogManager::debug(const std::string& module, const std::string& msg) {
    write(Level::DEBUG, module, msg);
}

void LogManager::info(const std::string& module, const std::string& msg) {
    write(Level::INFO, module, msg);
}

void LogManager::warn(const std::string& module, const std::string& msg) {
    write(Level::WARN, module, msg);
}

void LogManager::error(const std::string& module, const std::string& msg) {
    write(Level::ERROR, module, msg);
}

void LogManager::write(Level level, const std::string& module,
                        const std::string& msg) {
    if (level < impl_->min_level) return;

    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::string ts = now_iso();

    // Build structured JSON log line
    // {"ts":"...", "level":"INFO", "module":"main", "msg":"..."}
    std::ostringstream json;
    json << "{\"ts\":\"" << ts << "\""
         << ",\"level\":\"" << level_str(level) << "\""
         << ",\"module\":\"" << json_escape(module) << "\""
         << ",\"msg\":\"" << json_escape(msg) << "\"";
    if (!impl_->trace_id.empty()) {
        json << ",\"trace_id\":\"" << json_escape(impl_->trace_id) << "\"";
    }
    json << "}\n";

    std::string line = json.str();

    // Also echo to stderr in debug builds
#ifndef NDEBUG
    std::cerr << line;
#endif

    // Write to file
    if (impl_->file_stream.is_open()) {
        impl_->file_stream << line;
        impl_->file_stream.flush();

        // Check if rotation needed
        // Use tellp to estimate current file size
        auto pos = impl_->file_stream.tellp();
        int64_t max_bytes = static_cast<int64_t>(impl_->max_file_size_mb) * 1024 * 1024;
        if (pos > 0 && static_cast<int64_t>(pos) >= max_bytes) {
            // We must release the stream temporarily for rotate (rotate acquires lock)
            // Use a deferred approach: close stream, rotate, reopen
            impl_->file_stream.flush();
            impl_->file_stream.close();
            rotate();
            impl_->file_stream.open(impl_->current_file_path,
                                     std::ios::out | std::ios::app);
            if (impl_->file_stream.is_open()) {
                impl_->file_stream << line; // re-write to new file
                impl_->file_stream.flush();
            }
        }
    }
}

StatusCode LogManager::rotate() {
    // Close current file
    if (impl_->file_stream.is_open()) {
        impl_->file_stream.flush();
        impl_->file_stream.close();
    }

    std::string base = impl_->log_dir + "/dev-sys-cloud.log";

    // Shift existing rotated files: log.9 -> log.10, log.8 -> log.9, etc.
    for (int i = impl_->max_files - 1; i >= 0; --i) {
        std::string old_name = base + "." + std::to_string(i);
        std::string new_name = base + "." + std::to_string(i + 1);
        std::error_code ec;
        if (std::filesystem::exists(old_name, ec)) {
            if (i + 1 >= impl_->max_files) {
                // Delete the oldest file beyond max_files
                std::filesystem::remove(old_name, ec);
            } else {
                std::filesystem::rename(old_name, new_name, ec);
            }
        }
    }

    // Rename current log file to .0
    std::error_code ec;
    if (std::filesystem::exists(base, ec)) {
        std::filesystem::rename(base, base + ".0", ec);
    }

    return StatusCode::OK;
}

std::string LogManager::export_recent(int hours) const {
    std::lock_guard<std::mutex> lock(g_log_mutex);

    auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(hours);
    auto cutoff_ts = std::chrono::duration_cast<std::chrono::seconds>(
        cutoff.time_since_epoch()).count();

    std::ostringstream result;
    result << "[";

    // Collect log files to scan
    std::vector<std::string> files;
    files.push_back(impl_->log_dir + "/dev-sys-cloud.log");
    for (int i = 0; i < impl_->max_files; ++i) {
        std::string f = impl_->log_dir + "/dev-sys-cloud.log." + std::to_string(i);
        std::error_code ec;
        if (std::filesystem::exists(f, ec)) {
            files.push_back(f);
        }
    }

    bool first_line = true;
    for (const auto& fp : files) {
        std::ifstream in(fp);
        if (!in.is_open()) continue;

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;

            // Extract timestamp from JSON: {"ts":"2026-06-27T12:00:00.000Z",...
            size_t ts_start = line.find("\"ts\":\"");
            if (ts_start == std::string::npos) continue;
            ts_start += 6; // skip "ts":"
            size_t ts_end = line.find('"', ts_start);
            if (ts_end == std::string::npos) continue;
            std::string ts_str = line.substr(ts_start, ts_end - ts_start);

            // Parse ISO 8601 to epoch seconds (simple approach: parse YYYY-MM-DDTHH:MM:SS)
            std::tm tm_buf = {};
            std::istringstream iss(ts_str.substr(0, 19)); // ignore milliseconds
            iss >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
            if (iss.fail()) continue;
            int64_t epoch = static_cast<int64_t>(timegm(&tm_buf));

            if (epoch >= cutoff_ts) {
                if (!first_line) result << ",";
                first_line = false;
                result << line;
            }
        }
    }

    result << "]";
    return result.str();
}

} // namespace dev_sys
