#include "device_activation.h"
#include "middleware/storage/database.h"
#include "middleware/security/tls_manager.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>
#ifdef HAS_OPENSSL
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

namespace dev_sys {

DeviceActivation::DeviceActivation(Database& db)
    : db_(db) {}

DeviceActivation::~DeviceActivation() = default;

void DeviceActivation::set_tls_manager(TlsManager* tls) {
    tls_mgr_ = tls;
}

// ============================================================
// Core: Process activation request (REQ-DM-002)
// ============================================================
ActivationResponse DeviceActivation::process_activation(const ActivationRequest& request,
                                                          const std::string& remote_ip) {
    ActivationResponse resp;

    // Step 1: Validate request
    StatusCode valid = validate_request(request);
    if (valid != StatusCode::OK) {
        resp.success      = false;
        resp.error_code   = static_cast<int32_t>(valid);
        resp.error_message = status_message(valid);
        db_.log_activation("", request.hardware_uid, remote_ip, false, resp.error_message);
        return resp;
    }

    // Step 2: Check if device already exists
    if (db_.device_exists(request.hardware_uid)) {
        auto existing = db_.get_device_by_hwuid(request.hardware_uid);
        // Actually, we need the get_device_by_hwuid method. For now check via query.
    }

    // Check if this hardware_uid already has a device record
    // (We'll check via the device_exists flag)
    if (db_.device_exists(request.hardware_uid)) {
        // Already registered — check if activated
        // TODO: proper lookup by hardware_uid
        // For now, treat as re-activation request
        resp.success      = false;
        resp.error_code   = static_cast<int32_t>(StatusCode::DEV_ALREADY_ACTIVE);
        resp.error_message = "Device hardware UID already registered. Use re-activation endpoint.";
        db_.log_activation("", request.hardware_uid, remote_ip, false, resp.error_message);
        return resp;
    }

    // Step 3: Generate device identity
    std::string device_id   = generate_device_id(request);
    std::string product_id  = generate_product_id(request.device_type);
    std::string token       = generate_activation_token(device_id);

    // Step 4: Build device record
    Device dev;
    dev.device_id        = device_id;
    dev.tenant_id        = request.tenant_id;
    dev.product_id       = product_id;
    dev.type             = request.device_type;
    dev.status           = DeviceStatus::OFFLINE;   // will go IDLE after first heartbeat
    dev.firmware_version = request.firmware_version;
    dev.mac_address      = request.mac_address;
    dev.model            = request.model;
    dev.hardware_uid     = request.hardware_uid;
    dev.last_heartbeat_at = 0;
    dev.activated        = true;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    dev.activated_at = now;

    // Step 5: Persist to database
    StatusCode db_result = db_.insert_device(dev);
    if (db_result != StatusCode::OK) {
        resp.success      = false;
        resp.error_code   = static_cast<int32_t>(db_result);
        resp.error_message = "Database insert failed";
        db_.log_activation(device_id, request.hardware_uid, remote_ip, false, resp.error_message);
        return resp;
    }

    // Step 6: Store activation token
    int64_t ttl_seconds = 365 * 24 * 3600; // 1 year default
    int64_t expires_at = now + ttl_seconds;
    db_.store_activation_token(device_id, token, now, expires_at);

    // Step 7: Generate mTLS certificate (optional)
    std::string cert_pem;
    std::string key_pem;
    if (tls_mgr_) {
        cert_pem = generate_certificate(device_id);
        key_pem  = generate_private_key(device_id);
    }

    // Step 8: Build response
    resp.success           = true;
    resp.device_id         = device_id;
    resp.tenant_id         = request.tenant_id;
    resp.product_id        = product_id;
    resp.activation_token  = token;
    resp.ttl_seconds       = static_cast<int32_t>(ttl_seconds);
    resp.mqtt_broker_uri   = "ssl://mqtt.example.com:8883"; // TODO: from config
    resp.certificate_pem   = cert_pem;
    resp.private_key_pem   = key_pem;

    // Step 9: Audit log
    db_.log_activation(device_id, request.hardware_uid, remote_ip, true,
                        "Activation successful, token issued");

    std::cout << "[Activation] Device activated: " << device_id
              << " (tenant=" << request.tenant_id
              << ", type=" << static_cast<int>(request.device_type)
              << ", model=" << request.model << ")" << std::endl;

    return resp;
}

// ============================================================
// Reactivation (device reset or re-provisioning)
// ============================================================
ActivationResponse DeviceActivation::reactivate_device(const std::string& hardware_uid,
                                                         const std::string& remote_ip) {
    ActivationResponse resp;

    // TODO: Look up device by hardware_uid in DB
    // TODO: Verify device isn't blacklisted
    // TODO: Revoke old token
    // TODO: Generate new device_id (or reuse old) and new token
    // TODO: Update device record
    // TODO: Log in audit log

    resp.success      = false;
    resp.error_code   = -1;
    resp.error_message = "Re-activation not yet implemented";
    return resp;
}

// ============================================================
// Generate device_id from hardware_uid
// ============================================================
std::string DeviceActivation::generate_device_id(const ActivationRequest& request) {
    std::ostringstream oss;
    oss << "dev_";
#ifdef HAS_OPENSSL
    // device_id = "dev_" + SHA256(hardware_uid)[:16] (hex)
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(request.hardware_uid.c_str()),
           request.hardware_uid.size(), hash);
    for (int i = 0; i < 8; i++) {  // first 8 bytes = 16 hex chars
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
#else
    // Mock: simple hash of hardware_uid
    std::hash<std::string> str_hash;
    size_t h = str_hash(request.hardware_uid);
    oss << std::hex << std::setfill('0') << std::setw(16) << h;
#endif
    return oss.str();
}

// ============================================================
// Generate product_id from device_type
// ============================================================
std::string DeviceActivation::generate_product_id(DeviceType type) {
    switch (type) {
        case DeviceType::COFFEE_MACHINE:  return "coffee_v1";
        case DeviceType::INSTANT_MACHINE: return "instant_v1";
        case DeviceType::WATER_DISPENSER: return "water_v1";
        default:                          return "generic_v1";
    }
}

// ============================================================
// Generate activation token (256-bit random hex string)
// ============================================================
std::string DeviceActivation::generate_activation_token(const std::string& device_id) {
    constexpr int TOKEN_BYTES = 32; // 256 bits
    unsigned char buf[TOKEN_BYTES];

#ifdef HAS_OPENSSL
    if (RAND_bytes(buf, TOKEN_BYTES) != 1) {
        std::cerr << "[Activation] ERROR: RAND_bytes failed, token generation weak" << std::endl;
        std::random_device rd;
        for (int i = 0; i < TOKEN_BYTES; i++) {
            buf[i] = static_cast<unsigned char>(rd() & 0xFF);
        }
    }
#else
    // Mock: use random_device directly
    {
        std::random_device rd;
        for (int i = 0; i < TOKEN_BYTES; i++) {
            buf[i] = static_cast<unsigned char>(rd() & 0xFF);
        }
    }
#endif

    (void)device_id;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < TOKEN_BYTES; i++) {
        oss << std::setw(2) << static_cast<int>(buf[i]);
    }
    return oss.str();
}

// ============================================================
// Certificate generation (for mTLS, optional)
// ============================================================
std::string DeviceActivation::generate_certificate(const std::string& device_id) {
    // TODO: Use OpenSSL X509 API to generate a device client certificate
    // Signed by the platform's intermediate CA
    // CN = device_id, OU = IoT Devices
    // For now, return placeholder
    return "";
}

std::string DeviceActivation::generate_private_key(const std::string& device_id) {
    // TODO: Use OpenSSL EVP_PKEY API to generate EC P-256 key pair
    // For now, return placeholder
    return "";
}

// ============================================================
// Request validation
// ============================================================
StatusCode DeviceActivation::validate_request(const ActivationRequest& request) const {
    if (request.hardware_uid.empty()) {
        return StatusCode::DEV_AUTH_FAILED; // missing identity
    }
    if (request.hardware_uid.length() < 8 || request.hardware_uid.length() > 128) {
        return StatusCode::DEV_AUTH_FAILED; // invalid length
    }
    if (request.tenant_id.empty()) {
        return StatusCode::DEV_AUTH_FAILED; // must specify tenant
    }
    if (request.model.empty()) {
        return StatusCode::DEV_AUTH_FAILED; // model required
    }
    if (request.device_type == DeviceType::OTHER && request.model.find("UNKNOWN") == 0) {
        return StatusCode::DEV_AUTH_FAILED; // must specify valid device type
    }
    return StatusCode::OK;
}

// ============================================================
// Token management
// ============================================================
StatusCode DeviceActivation::revoke_token(const std::string& device_id) {
    // TODO: UPDATE activation_tokens SET revoked=1 WHERE device_id=...
    return StatusCode::OK;
}

StatusCode DeviceActivation::renew_token(const std::string& device_id,
                                           ActivationResponse& response) {
    // TODO: Revoke old token, generate new one, return in response
    return StatusCode::OK;
}

bool DeviceActivation::is_device_activated(const std::string& hardware_uid) const {
    return db_.device_exists(hardware_uid);
}

} // namespace dev_sys
