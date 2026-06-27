#pragma once

#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>

namespace dev_sys {

// ============================================================
// TLS Manager (mTLS, REQ-SC-001)
// ============================================================
class TlsManager {
public:
    TlsManager();
    ~TlsManager();

    // Initialize TLS context with device certificates
    StatusCode init(const std::string& ca_cert_path,
                    const std::string& device_cert_path,
                    const std::string& device_key_path);

    // Check certificate validity (expiry, revocation)
    bool is_cert_valid() const;
    int cert_days_until_expiry() const;

    // Certificate renewal
    StatusCode request_cert_renewal();

    // Secure storage for device credentials
    StatusCode store_credential(const std::string& key, const std::string& value);
    StatusCode load_credential(const std::string& key, std::string& value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
