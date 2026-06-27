#include "http_client.h"
#include <iostream>

namespace dev_sys {

struct HttpClient::Impl {
    std::string ca_cert;
    std::string client_cert;
    std::string client_key;
};

HttpClient::HttpClient()
    : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() = default;

StatusCode HttpClient::set_tls(const std::string& ca_cert_path,
                                const std::string& client_cert_path,
                                const std::string& client_key_path) {
    impl_->ca_cert     = ca_cert_path;
    impl_->client_cert = client_cert_path;
    impl_->client_key  = client_key_path;
    // TODO: Load certificates
    return StatusCode::OK;
}

StatusCode HttpClient::get(const std::string& url,
                            std::string& response_body,
                            int timeout_sec) {
    // TODO: libcurl HTTP GET with TLS
    return StatusCode::OK;
}

StatusCode HttpClient::post(const std::string& url,
                             const std::string& body,
                             const std::map<std::string, std::string>& headers,
                             std::string& response_body,
                             int timeout_sec) {
    // TODO: libcurl HTTP POST with TLS
    return StatusCode::OK;
}

StatusCode HttpClient::download_file(const std::string& url,
                                      const std::string& output_path,
                                      bool resume) {
    // TODO: libcurl file download with Range header for resume
    // TODO: Write to output_path, report progress
    // TODO: Return OK or OTA_DOWNLOAD_FAILED
    return StatusCode::OK;
}

} // namespace dev_sys
