#pragma once

#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

namespace dev_sys {

// ============================================================
// Simple embedded HTTP API Server
//
// Handles REST API endpoints for:
//   - POST /api/v1/device/activate     (REQ-DM-002)
//   - GET  /api/v1/device/status       (device query)
//   - POST /api/v1/device/ota/callback (OTA result callback)
// ============================================================

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string remote_ip;
};

struct HttpResponse {
    int         status_code = 200;
    std::string content_type = "application/json";
    std::string body;
    std::string extra_headers;  // appended after Content-* headers (CORS, etc.)
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

class ApiServer {
public:
    ApiServer();
    ~ApiServer();

    // Start/stop
    StatusCode start(int port);
    StatusCode stop();
    bool is_running() const;

    // Route registration
    void add_route(const std::string& method,
                   const std::string& path,
                   RouteHandler handler);

    // Convenience
    void get(const std::string& path, RouteHandler handler);
    void post(const std::string& path, RouteHandler handler);
    void put(const std::string& path, RouteHandler handler);
    void del(const std::string& path, RouteHandler handler);

    // JSON helpers
    static HttpResponse json_response(int status_code, const std::string& json_body);
    static HttpResponse error_response(int status_code,
                                        int error_code,
                                        const std::string& message);

private:
    void accept_loop();
    void handle_connection(int client_fd);
    HttpRequest parse_request(const std::string& raw, size_t content_len = 0) const;
    std::string build_response(const HttpResponse& resp) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
