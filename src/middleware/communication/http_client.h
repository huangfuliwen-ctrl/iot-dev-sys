#pragma once

#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>
#include <map>

namespace dev_sys {

// ============================================================
// HTTP Client (REQ-IF-001 HTTPS fallback)
// ============================================================
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Configure TLS
    StatusCode set_tls(const std::string& ca_cert_path,
                       const std::string& client_cert_path,
                       const std::string& client_key_path);

    // HTTP methods
    StatusCode get(const std::string& url,
                   std::string& response_body,
                   int timeout_sec = 30);

    StatusCode post(const std::string& url,
                    const std::string& body,
                    const std::map<std::string, std::string>& headers,
                    std::string& response_body,
                    int timeout_sec = 30);

    // File download (for OTA)
    StatusCode download_file(const std::string& url,
                              const std::string& output_path,
                              bool resume = true);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
