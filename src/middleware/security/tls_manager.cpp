#include "tls_manager.h"
#include <iostream>

namespace dev_sys {

struct TlsManager::Impl {
    std::string ca_cert;
    std::string device_cert;
    std::string device_key;
    bool initialized = false;
};

TlsManager::TlsManager()
    : impl_(std::make_unique<Impl>()) {}

TlsManager::~TlsManager() = default;

StatusCode TlsManager::init(const std::string& ca_cert_path,
                             const std::string& device_cert_path,
                             const std::string& device_key_path) {
    // TODO: Load CA cert, device cert, device key
    // TODO: Initialize mbedTLS / OpenSSL context
    // TODO: Configure for TLS 1.2+ only
    impl_->initialized = true;
    return StatusCode::OK;
}

bool TlsManager::is_cert_valid() const {
    // TODO: Check expiry date, check CRL/OCSP
    return impl_->initialized;
}

int TlsManager::cert_days_until_expiry() const {
    // TODO: Read cert notAfter, compute days remaining
    return 365;
}

StatusCode TlsManager::request_cert_renewal() {
    // TODO: Generate CSR, send to cloud, install new cert
    // TODO: Should be triggered when cert_days_until_expiry <= 30
    return StatusCode::OK;
}

StatusCode TlsManager::store_credential(const std::string& key,
                                         const std::string& value) {
    // TODO: Encrypt and store in secure storage (TEE / secure flash partition)
    // Credential keys: "device_id", "activation_token", "device_cert", "device_key"
    return StatusCode::OK;
}

StatusCode TlsManager::load_credential(const std::string& key,
                                        std::string& value) {
    // TODO: Load and decrypt from secure storage
    return StatusCode::OK;
}

} // namespace dev_sys
