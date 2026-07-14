#include "api_server.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <algorithm>

namespace dev_sys {

struct ApiServer::Impl {
    int port = 8080;
    int server_fd = -1;
    std::atomic<bool> running{false};
    std::thread accept_thread;
    std::unordered_map<std::string, RouteHandler> routes;
};

ApiServer::ApiServer() : impl_(std::make_unique<Impl>()) {}
ApiServer::~ApiServer() { stop(); }

void ApiServer::add_route(const std::string& method, const std::string& path, RouteHandler handler) {
    impl_->routes[method + " " + path] = std::move(handler);
}
void ApiServer::get(const std::string& p, RouteHandler h)  { add_route("GET", p, std::move(h)); }
void ApiServer::post(const std::string& p, RouteHandler h) { add_route("POST", p, std::move(h)); }
void ApiServer::put(const std::string& p, RouteHandler h)  { add_route("PUT", p, std::move(h)); }
void ApiServer::del(const std::string& p, RouteHandler h)  { add_route("DELETE", p, std::move(h)); }

// ============================================================
// Start / Stop
// ============================================================
StatusCode ApiServer::start(int port) {
    impl_->port = port;
    impl_->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->server_fd < 0) {
        std::cerr << "[ApiServer] socket() failed: " << strerror(errno) << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }

    int opt = 1;
    setsockopt(impl_->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(impl_->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ApiServer] bind(" << port << ") failed: " << strerror(errno) << std::endl;
        close(impl_->server_fd);
        return StatusCode::COMM_DISCONNECTED;
    }
    if (listen(impl_->server_fd, SOMAXCONN) < 0) {
        std::cerr << "[ApiServer] listen() failed: " << strerror(errno) << std::endl;
        close(impl_->server_fd);
        return StatusCode::COMM_DISCONNECTED;
    }
    fcntl(impl_->server_fd, F_SETFL, O_NONBLOCK);

    impl_->running = true;
    impl_->accept_thread = std::thread(&ApiServer::accept_loop, this);
    std::cout << "[ApiServer] Started on port " << port << std::endl;
    return StatusCode::OK;
}

StatusCode ApiServer::stop() {
    impl_->running = false;
    if (impl_->server_fd >= 0) { shutdown(impl_->server_fd, SHUT_RDWR); close(impl_->server_fd); impl_->server_fd = -1; }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    return StatusCode::OK;
}
bool ApiServer::is_running() const { return impl_->running; }

// ============================================================
// Accept loop
// ============================================================
void ApiServer::accept_loop() {
    while (impl_->running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(impl_->server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;
        }
        std::thread(&ApiServer::handle_connection, this, client_fd).detach();
    }
}

// ============================================================
// I/O helpers — handle partial reads/writes and large bodies
// ============================================================
static bool read_exact(int fd, std::string& buf, size_t needed) {
    buf.reserve(needed);
    while (buf.size() < needed) {
        char tmp[65536];
        size_t remain = needed - buf.size();
        size_t n = remain < sizeof(tmp) ? remain : sizeof(tmp);
        ssize_t r = recv(fd, tmp, n, 0);
        if (r <= 0) return false;
        buf.append(tmp, r);
    }
    return true;
}

static bool write_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// ============================================================
// Connection handler — reads headers then Content-Length body
// ============================================================
void ApiServer::handle_connection(int client_fd) {
    // Set socket timeout to prevent hanging
    struct timeval tv;
    tv.tv_sec = 30; tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Phase 1: read headers efficiently (64KB chunks)
    std::string raw;
    raw.reserve(65536);
    while (raw.find("\r\n\r\n") == std::string::npos) {
        char tmp[65536];
        ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
        if (n <= 0) { close(client_fd); return; }
        raw.append(tmp, n);
        if (raw.size() > 131072) { close(client_fd); return; }
    }

    size_t header_end = raw.find("\r\n\r\n");
    std::string header_str = raw.substr(0, header_end);

    // Handle Expect: 100-continue — client waits for go-ahead before sending body
    std::string hlower = header_str;
    std::transform(hlower.begin(), hlower.end(), hlower.begin(), ::tolower);
    if (hlower.find("expect: 100-continue") != std::string::npos) {
        const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
        send(client_fd, cont, strlen(cont), 0);
    }

    // Extract Content-Length (case-insensitive)
    size_t content_len = 0;
    size_t cl_pos = hlower.find("content-length:");
    if (cl_pos != std::string::npos) {
        cl_pos += 15;
        while (cl_pos < header_str.size() && header_str[cl_pos] == ' ') cl_pos++;
        size_t cl_end = header_str.find('\r', cl_pos);
        if (cl_end == std::string::npos) cl_end = header_str.size();
        try { content_len = std::stoull(header_str.substr(cl_pos, cl_end - cl_pos)); }
        catch (...) {}
    }

    // Phase 2: read remaining body based on Content-Length
    size_t body_start = header_end + 4;
    if (raw.size() < body_start + content_len) {
        if (!read_exact(client_fd, raw, body_start + content_len)) {
            close(client_fd);
            return;
        }
    }

    // Parse request — pass content_len so body is extracted properly
    HttpRequest req = parse_request(raw, content_len);

    // Handle CORS preflight (OPTIONS) — required for direct uploads bypassing proxy
    if (req.method == "OPTIONS") {
        HttpResponse cors;
        cors.status_code = 204;
        cors.content_type = "text/plain";
        cors.extra_headers =
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type,Authorization,X-Firmware-Version,X-Firmware-Product,X-Firmware-Changelog,Range\r\n"
            "Access-Control-Max-Age: 86400\r\n";
        std::string out = build_response(cors);
        write_all(client_fd, out);
        close(client_fd);
        return;
    }

    // Route matching: exact first, then pattern
    HttpResponse resp;
    bool matched = false;

    // Phase 1: exact match
    for (const auto& [rk, h] : impl_->routes) {
        size_t sp = rk.find(' ');
        if (sp == std::string::npos) continue;
        std::string rm = rk.substr(0, sp);
        std::string rp = rk.substr(sp + 1);
        if (rm != req.method) continue;
        if (rp.find('{') == std::string::npos && rp == req.path) {
            resp = h(req); matched = true; break;
        }
    }

    // Phase 2: pattern match
    // Supports single-segment {param} and multi-segment {a}/{b} patterns.
    // Rejects slashes in suffix ONLY if the pattern itself has no slashes after {param}.
    // e.g. /orgs/{id} rejects "tree/extra", but /dl/{pid}/{fn} accepts "p1/file.bin"
    if (!matched) {
        for (const auto& [rk, h] : impl_->routes) {
            size_t sp = rk.find(' ');
            if (sp == std::string::npos) continue;
            std::string rm = rk.substr(0, sp);
            std::string rp = rk.substr(sp + 1);
            if (rm != req.method) continue;
            size_t brace = rp.find('{');
            if (brace != std::string::npos) {
                std::string prefix = rp.substr(0, brace);
                if (req.path.find(prefix) == 0) {
                    std::string suffix = req.path.substr(prefix.size());
                    if (!suffix.empty()) {
                        // Allow slash only if pattern has more segments after {param}
                        bool pattern_has_slash = (rp.find('/', brace) != std::string::npos);
                        if (pattern_has_slash || suffix.find('/') == std::string::npos) {
                            resp = h(req); matched = true; break;
                        }
                    }
                }
            }
        }
    }

    if (!matched) resp = error_response(404, -404, "Not Found: " + req.path);

    // Send response (loop to ensure all bytes sent)
    std::string out = build_response(resp);
    write_all(client_fd, out);
    close(client_fd);
}

// ============================================================
// HTTP parsing — uses Content-Length for body, NOT line-by-line
// ============================================================
HttpRequest ApiServer::parse_request(const std::string& raw, size_t content_len) const {
    HttpRequest req;
    std::istringstream stream(raw);
    std::string line;

    // Request line
    if (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream first(line);
        first >> req.method >> req.path;
    }

    // Strip query string
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        req.query = req.path.substr(qpos + 1);
        req.path = req.path.substr(0, qpos);
    }

    // Headers
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            if (!val.empty() && val.front() == ' ') val.erase(0, 1);
            req.headers[key] = val;
        }
    }

    // Body: extract exactly content_len bytes from raw data (NOT line-by-line)
    // Find where body starts in raw string: after headers + \r\n\r\n
    size_t body_start = raw.find("\r\n\r\n");
    if (body_start != std::string::npos && content_len > 0) {
        body_start += 4; // skip \r\n\r\n
        if (body_start + content_len <= raw.size()) {
            req.body = raw.substr(body_start, content_len);
        }
    }

    return req;
}

// ============================================================
// Response building
// ============================================================
std::string ApiServer::build_response(const HttpResponse& resp) const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " ";
    switch (resp.status_code) {
        case 200: oss << "OK"; break;
        case 201: oss << "Created"; break;
        case 206: oss << "Partial Content"; break;
        case 400: oss << "Bad Request"; break;
        case 401: oss << "Unauthorized"; break;
        case 404: oss << "Not Found"; break;
        case 500: oss << "Internal Server Error"; break;
        default:  oss << "Unknown"; break;
    }
    oss << "\r\n";
    oss << "Content-Type: " << resp.content_type << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    if (!resp.extra_headers.empty()) {
        oss << resp.extra_headers;
        if (resp.extra_headers.back() != '\n') oss << "\r\n";
    }
    oss << "\r\n";
    oss << resp.body;
    return oss.str();
}

HttpResponse ApiServer::json_response(int code, const std::string& json) {
    HttpResponse r;
    r.status_code = code;
    r.content_type = "application/json";
    r.body = json;
    return r;
}

HttpResponse ApiServer::error_response(int code, int err, const std::string& msg) {
    std::ostringstream j;
    j << "{\"success\":false,\"error_code\":" << err
      << ",\"error_message\":\"" << msg << "\"}";
    return json_response(code, j.str());
}

} // namespace dev_sys
