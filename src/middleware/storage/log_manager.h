#pragma once

#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>

namespace dev_sys {

// ============================================================
// Log Manager
// Structured JSON log, rolling files, adjustable levels
// ============================================================
class LogManager {
public:
    enum class Level {
        DEBUG = 0,
        INFO  = 1,
        WARN  = 2,
        ERROR = 3,
    };

    LogManager();
    ~LogManager();

    StatusCode init(const std::string& log_dir,
                     Level min_level = Level::INFO,
                     int max_file_size_mb = 50,
                     int max_files = 10);

    void set_level(Level level);

    void debug(const std::string& module, const std::string& msg);
    void info(const std::string& module, const std::string& msg);
    void warn(const std::string& module, const std::string& msg);
    void error(const std::string& module, const std::string& msg);

    // Rotate logs (manual trigger)
    StatusCode rotate();

    // Export logs for remote diagnostics
    std::string export_recent(int hours) const;

private:
    void write(Level level, const std::string& module, const std::string& msg);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
