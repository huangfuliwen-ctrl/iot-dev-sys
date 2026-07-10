#include "device_activation.h"
#include "../../middleware/storage/database.h"
#include "../../middleware/security/tls_manager.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>

#ifdef HAS_OPENSSL
#include <openssl/sha.h>
#include <openssl/rand.h>
#endif

namespace dev_sys {

DeviceActivation::DeviceActivation(Database& db) : db_(db) {}
DeviceActivation::~DeviceActivation() = default;
void DeviceActivation::set_tls_manager(TlsManager* tls) { tls_mgr_ = tls; }

// ============================================================
// Core activation flow
// ============================================================
ActivationResponse DeviceActivation::process_activation(const ActivationRequest& request,
                                                          const std::string& remote_ip) {
    ActivationResponse resp;

    // Step 1: Validate input
    StatusCode valid = validate_request(request);
    if (valid != StatusCode::OK) {
        resp.success = false;
        resp.error_code = static_cast<int32_t>(valid);
        resp.error_message = status_message(valid);
        return resp;
    }

    // Step 2: Look up model by model_key
    auto model = db_.get_device_model_by_key(request.model_key);
    if (!model) {
        resp.success = false;
        resp.error_code = -404;
        resp.error_message = "model_key not found: " + request.model_key;
        return resp;
    }
    auto dtype = db_.get_device_type(model->type_code);
    if (!dtype) {
        resp.success = false;
        resp.error_code = -404;
        resp.error_message = "device_type not found: " + model->type_code;
        return resp;
    }

    // Step 3: Check if device already exists
    if (db_.device_exists(request.uid)) {
        resp.success = false;
        resp.error_code = static_cast<int32_t>(StatusCode::DEV_ALREADY_ACTIVE);
        resp.error_message = "Device already registered";
        return resp;
    }

    // Step 4: Ensure tenant (using default for now)
    std::string tenant = "default";
    {
        std::ostringstream sql;
        sql << "INSERT INTO tenants (tenant_id, name) VALUES ('default','Default') ON CONFLICT DO NOTHING";
        db_.execute(sql.str());
    }

    // Step 5: Generate device identity
    std::string device_id  = generate_device_id(request);
    std::string product_id = generate_product_id(static_cast<DeviceType>(dtype->internal_type));
    std::string token      = generate_activation_token(device_id);

    // Step 6: Build & persist device record
    Device dev;
    dev.device_id        = device_id;
    dev.tenant_id        = tenant;
    dev.product_id       = product_id;
    dev.type             = static_cast<DeviceType>(dtype->internal_type);
    dev.model            = model->model_code;
    dev.hardware_uid     = request.uid;
    dev.firmware_version = model->firmware_base;
    dev.status           = DeviceStatus::OFFLINE;
    dev.activated        = true;
    dev.last_heartbeat_at = 0;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    dev.activated_at = now;

    StatusCode db_result = db_.insert_device(dev);
    if (db_result != StatusCode::OK) {
        resp.success = false;
        resp.error_code = static_cast<int32_t>(db_result);
        resp.error_message = "Database insert failed";
        return resp;
    }

    // Step 7: Store activation token
    int64_t ttl = 365 * 24 * 3600;
    int64_t expires_at = now + ttl;
    db_.store_activation_token(device_id, token, now, expires_at);
    db_.log_activation(device_id, request.uid, remote_ip, true, "activated via " + request.model_key);

    // Build response
    resp.success = true;
    resp.device_id = device_id;
    resp.model_key = request.model_key;
    resp.model_code = model->model_code;
    resp.tenant_id = tenant;
    resp.product_id = product_id;
    resp.device_type = dtype->type_code;
    resp.firmware_version = model->firmware_base;
    resp.activation_token = token;
    resp.mqtt_broker_uri = "ssl://mqtt.example.com:8883";
    resp.ttl_seconds = static_cast<int32_t>(ttl);

    std::cout << "[Activation] Device activated: " << device_id
              << " uid=" << request.uid << " model=" << model->model_code
              << " key=" << request.model_key << std::endl;
    return resp;
}

// ============================================================
// Validation
// ============================================================
StatusCode DeviceActivation::validate_request(const ActivationRequest& request) const {
    if (request.uid.empty()) return StatusCode::DEV_AUTH_FAILED;
    if (request.model_key.empty()) return StatusCode::DEV_AUTH_FAILED;
    if (request.uid.size() < 4 || request.uid.size() > 128) return StatusCode::DEV_AUTH_FAILED;
    if (request.model_key.size() > 64) return StatusCode::DEV_AUTH_FAILED;
    return StatusCode::OK;
}

// ============================================================
// ID generation
// ============================================================
std::string DeviceActivation::generate_device_id(const ActivationRequest& request) {
#ifdef HAS_OPENSSL
    unsigned char hash[SHA256_DIGEST_LENGTH];
    std::string input = request.uid + ":" + request.model_key;
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    std::ostringstream oss;
    oss << "dev_";
    for (int i = 0; i < 8; i++)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    return oss.str();
#else
    std::hash<std::string> h;
    std::ostringstream oss;
    oss << "dev_" << std::hex << h(request.uid + request.model_key);
    return oss.str();
#endif
}

std::string DeviceActivation::generate_activation_token(const std::string& device_id) {
    (void)device_id;
#ifdef HAS_OPENSSL
    unsigned char buf[32];
    RAND_bytes(buf, sizeof(buf));
    std::ostringstream oss;
    for (int i = 0; i < 32; i++)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)buf[i];
    return oss.str();
#else
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    std::ostringstream oss;
    for (int i = 0; i < 8; i++)
        oss << std::hex << std::setfill('0') << std::setw(8) << gen();
    return oss.str().substr(0, 64);
#endif
}

std::string DeviceActivation::generate_product_id(DeviceType type) {
    switch (type) {
        case DeviceType::COFFEE_MACHINE:  return "coffee_v1";
        case DeviceType::INSTANT_MACHINE: return "instant_v1";
        case DeviceType::WATER_DISPENSER: return "water_v1";
        default: return "other_v1";
    }
}

// ============================================================
// Token management
// ============================================================
StatusCode DeviceActivation::revoke_token(const std::string& device_id) {
    db_.execute("UPDATE activation_tokens SET revoked=TRUE WHERE device_id='" + device_id + "'");
    return StatusCode::OK;
}

StatusCode DeviceActivation::renew_token(const std::string& device_id, ActivationResponse& resp) {
    auto dev = db_.get_device(device_id);
    if (!dev) return StatusCode::DEV_NOT_ACTIVATED;
    std::string token = generate_activation_token(device_id);
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t ttl = 365 * 24 * 3600;
    db_.store_activation_token(device_id, token, now, now + ttl);
    resp.success = true;
    resp.device_id = device_id;
    resp.activation_token = token;
    resp.ttl_seconds = static_cast<int32_t>(ttl);
    return StatusCode::OK;
}

bool DeviceActivation::is_device_activated(const std::string& hwuid) const {
    return db_.device_exists(hwuid);
}

std::optional<Device> DeviceActivation::get_device_by_hwuid(const std::string& hwuid) const {
    return db_.get_device_by_hwuid(hwuid);
}

ActivationResponse DeviceActivation::reactivate_device(const std::string& hwuid,
                                                         const std::string&) {
    ActivationResponse resp;
    resp.success = false;
    resp.error_code = -500;
    resp.error_message = "Reactivation not yet implemented";
    return resp;
}

std::string DeviceActivation::generate_certificate(const std::string&) { return ""; }
std::string DeviceActivation::generate_private_key(const std::string&) { return ""; }

} // namespace dev_sys
