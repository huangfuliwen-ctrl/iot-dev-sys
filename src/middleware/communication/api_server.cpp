#include "api_server.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
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
    std::unordered_map<std::string, RouteHandler> routes; // key: "METHOD /path"
};

ApiServer::ApiServer()
    : impl_(std::make_unique<Impl>()) {}

ApiServer::~ApiServer() {
    stop();
}

void ApiServer::add_route(const std::string& method,
                           const std::string& path,
                           RouteHandler handler) {
    std::string key = method + " " + path;
    impl_->routes[key] = std::move(handler);
}

void ApiServer::get(const std::string& path, RouteHandler handler) {
    add_route("GET", path, std::move(handler));
}

void ApiServer::post(const std::string& path, RouteHandler handler) {
    add_route("POST", path, std::move(handler));
}

void ApiServer::put(const std::string& path, RouteHandler handler) {
    add_route("PUT", path, std::move(handler));
}

void ApiServer::del(const std::string& path, RouteHandler handler) {
    add_route("DELETE", path, std::move(handler));
}

// ============================================================
// Start / Stop
// ============================================================
StatusCode ApiServer::start(int port) {
    impl_->port = port;

    // Create socket
    impl_->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->server_fd < 0) {
        std::cerr << "[ApiServer] Failed to create socket" << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    setsockopt(impl_->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(impl_->server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[ApiServer] Failed to bind port " << port << std::endl;
        close(impl_->server_fd);
        return StatusCode::COMM_DISCONNECTED;
    }

    // Listen
    if (listen(impl_->server_fd, SOMAXCONN) < 0) {
        std::cerr << "[ApiServer] Failed to listen" << std::endl;
        close(impl_->server_fd);
        return StatusCode::COMM_DISCONNECTED;
    }

    // Set non-blocking
    fcntl(impl_->server_fd, F_SETFL, O_NONBLOCK);

    impl_->running = true;
    impl_->accept_thread = std::thread(&ApiServer::accept_loop, this);

    std::cout << "[ApiServer] Started on port " << port << std::endl;
    return StatusCode::OK;
}

StatusCode ApiServer::stop() {
    impl_->running = false;
    if (impl_->server_fd >= 0) {
        shutdown(impl_->server_fd, SHUT_RDWR);
        close(impl_->server_fd);
        impl_->server_fd = -1;
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    return StatusCode::OK;
}

bool ApiServer::is_running() const {
    return impl_->running;
}

// ============================================================
// Accept loop (runs in background thread)
// ============================================================
void ApiServer::accept_loop() {
    while (impl_->running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(impl_->server_fd,
                                reinterpret_cast<struct sockaddr*>(&client_addr),
                                &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No pending connections, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break; // fatal error
        }

        // Handle connection in a new thread (simple approach)
        // TODO: Use thread pool for production
        std::thread(&ApiServer::handle_connection, this, client_fd).detach();
    }
}

void ApiServer::handle_connection(int client_fd) {
    // Read request
    char buffer[8192];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }
    buffer[bytes_read] = '\0';

    std::string raw(buffer, bytes_read);
    HttpRequest req = parse_request(raw);

    // Route matching: supports {param} patterns
    // Priority: exact match first, then pattern match
    HttpResponse resp;
    bool matched = false;

    // Phase 1: Try exact match first
    for (const auto& [route_key, handler] : impl_->routes) {
        size_t space = route_key.find(' ');
        if (space == std::string::npos) continue;
        std::string r_method = route_key.substr(0, space);
        std::string r_path   = route_key.substr(space + 1);

        if (r_method != req.method) continue;

        // Only consider routes without pattern parameters
        if (r_path.find('{') == std::string::npos && r_path == req.path) {
            resp = handler(req);
            matched = true;
            break;
        }
    }

    // Phase 2: Try pattern match if no exact match found
    if (!matched) {
        for (const auto& [route_key, handler] : impl_->routes) {
            size_t space = route_key.find(' ');
            if (space == std::string::npos) continue;
            std::string r_method = route_key.substr(0, space);
            std::string r_path   = route_key.substr(space + 1);

            if (r_method != req.method) continue;

            size_t brace = r_path.find('{');
            if (brace != std::string::npos) {
                std::string prefix = r_path.substr(0, brace);
                if (req.path.find(prefix) == 0) {
                    // Verify the rest after prefix is not empty and doesn't contain '/'
                    // This prevents /api/v1/orgs/tree from matching /api/v1/orgs/{id}
                    std::string suffix = req.path.substr(prefix.size());
                    if (!suffix.empty() && suffix.find('/') == std::string::npos) {
                        resp = handler(req);
                        matched = true;
                        break;
                    }
                }
            }
        }
    }

    if (!matched) {
        resp = error_response(404, -404, "Not Found: " + req.path);
    }

    // Send response
    std::string response_str = build_response(resp);
    send(client_fd, response_str.c_str(), response_str.size(), 0);
    close(client_fd);
}

// ============================================================
// HTTP parsing (minimal)
// ============================================================
HttpRequest ApiServer::parse_request(const std::string& raw) const {
    HttpRequest req;
    std::istringstream stream(raw);
    std::string line;

    // First line: METHOD PATH HTTP/1.1
    if (std::getline(stream, line)) {
        // Trim \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream first(line);
        first >> req.method >> req.path;
    }

    // Headers
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break; // end of headers
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // trim leading space
            if (!val.empty() && val.front() == ' ') val.erase(0, 1);
            req.headers[key] = val;
        }
    }

    // Body (remaining)
    std::string remaining;
    while (std::getline(stream, line)) {
        remaining += line + "\n";
    }
    req.body = remaining;

    return req;
}

std::string ApiServer::build_response(const HttpResponse& resp) const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " ";
    switch (resp.status_code) {
        case 200: oss << "OK"; break;
        case 201: oss << "Created"; break;
        case 400: oss << "Bad Request"; break;
        case 401: oss << "Unauthorized"; break;
        case 404: oss << "Not Found"; break;
        case 405: oss << "Method Not Allowed"; break;
        case 409: oss << "Conflict"; break;
        case 500: oss << "Internal Server Error"; break;
        default:  oss << "Unknown"; break;
    }
    oss << "\r\n";
    oss << "Content-Type: " << resp.content_type << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;
    return oss.str();
}

// ============================================================
// JSON response helpers
// ============================================================
HttpResponse ApiServer::json_response(int status_code, const std::string& json_body) {
    HttpResponse resp;
    resp.status_code  = status_code;
    resp.content_type = "application/json";
    resp.body         = json_body;
    return resp;
}

HttpResponse ApiServer::error_response(int status_code,
                                        int error_code,
                                        const std::string& message) {
    std::ostringstream json;
    json << "{"
         << "\"success\":false,"
         << "\"error_code\":" << error_code << ","
         << "\"error_message\":\"" << message << "\""
         << "}";
    return json_response(status_code, json.str());
}

} // namespace dev_sys
