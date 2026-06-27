#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <sstream>

namespace dev_sys {

// ============================================================
// Device Types (internal enum, stable mapping)
// ============================================================
enum class DeviceType : uint8_t {
    COFFEE_MACHINE  = 1,
    INSTANT_MACHINE = 2,
    WATER_DISPENSER = 3,
    OTHER           = 4,
};

// ============================================================
// Dynamic Device Type (DB-backed, frontend-managed)
// ============================================================
struct DeviceTypeInfo {
    int32_t     id = 0;               // auto-increment PK
    std::string type_code;            // unique key, e.g. "smart_coffee_v2"
    std::string display_name;         // e.g. "智能现磨咖啡机"
    std::string description;          // e.g. "支持豆仓研磨+奶泡..."
    int32_t     internal_type = 4;    // maps to DeviceType enum (1-4)
    std::string icon_url;             // frontend display icon
    int32_t     sort_order = 0;       // display ordering
    bool        is_active = true;
    std::string created_at;
    std::string updated_at;
};

// ============================================================
// Dynamic Device Model (DB-backed, belongs to a DeviceTypeInfo)
// ============================================================
struct DeviceModelInfo {
    int32_t     id = 0;               // auto-increment PK
    std::string model_code;           // unique key, e.g. "ACM-200"
    std::string type_code;            // FK → device_types.type_code
    std::string display_name;         // e.g. "ACM-200 旗舰版"
    std::string description;
    std::string specs_json;           // arbitrary specs: {"power":"2000W","capacity":"5L",...}
    std::string firmware_base;        // base firmware for this model
    bool        is_active = true;
    int32_t     sort_order = 0;
    std::string created_at;
    std::string updated_at;
};

// ============================================================
// Device Status (per-device)
// ============================================================
enum class DeviceStatus : uint8_t {
    IDLE        = 0,
    BREWING     = 1,
    FAULT       = 2,
    UPGRADING   = 3,
    MAINTENANCE = 4,
    OFFLINE     = 5,
};

// ============================================================
// Fault Codes & Levels
// ============================================================
enum class FaultCode : uint8_t {
    NONE               = 0,
    HEATER_OVERTEMP    = 1,
    HEATER_NOT_HEATING = 2,
    PUMP_FAILURE       = 3,
    MOTOR_STALL        = 4,
    WATER_LEAK         = 5,
    SENSOR_OFFLINE     = 6,
    COMM_FAIL          = 7,
    MATERIAL_LOW       = 8,
};

enum class FaultLevel : uint8_t {
    L0_HINT    = 0,
    L1_MINOR   = 1,
    L2_GENERAL = 2,
    L3_SEVERE  = 3,
    L4_DANGER  = 4,
};

// ============================================================
// Order Status
// ============================================================
enum class OrderStatus : uint8_t {
    PENDING   = 0,
    PAID      = 1,
    BREWING   = 2,
    COMPLETED = 3,
    CANCELLED = 4,
    FAILED    = 5,
};

// ============================================================
// Tenant
// ============================================================
struct Tenant {
    std::string tenant_id;
    std::string name;
    bool        active = true;
};

// ============================================================
// Organization (REQ-ORG-001)
// ============================================================
struct OrgInfo {
    int32_t     org_id = 0;
    int32_t     parent_id = 0;
    std::string tenant_id;
    std::string org_name;
    std::string org_type;       // "company" / "department"
    std::string contact_name;
    std::string contact_phone;
    std::string contact_email;
    std::string address;
    bool        is_active = true;
    int32_t     level = 0;
    std::string path;           // e.g. "/1/5/12"
    int32_t     children_count = 0;
    std::string created_at;
    std::string updated_at;
};

// ============================================================
// Org Tree Node (for tree API response)
// ============================================================
struct OrgTreeNode {
    OrgInfo info;
    std::vector<OrgTreeNode> children;
};

// ============================================================
// Account (REQ-ORG-002)
// ============================================================
struct AccountInfo {
    int32_t     account_id = 0;
    std::string username;
    std::string password_hash;  // bcrypt/argon2, never exposed in API
    std::string display_name;
    int32_t     org_id = 0;
    std::string org_name;       // denormalized for display
    std::string role_code;      // super_admin / org_admin / dept_admin / operator / viewer
    std::string email;
    std::string phone;
    bool        is_active = true;
    std::string last_login_at;
    std::string created_at;
    std::string updated_at;
};

// ============================================================
// Login Request/Response
// ============================================================
struct LoginRequest {
    std::string username;
    std::string password;
};

struct LoginResponse {
    bool        success = false;
    std::string token;
    std::string expires_at;
    AccountInfo account;
    std::vector<std::string> permissions;
    int32_t     error_code = 0;
    std::string error_message;
};

// ============================================================
// JWT Token Payload (internal)
// ============================================================
struct TokenPayload {
    int32_t     account_id = 0;
    std::string username;
    int32_t     org_id = 0;
    std::vector<int32_t> org_scope;     // visible org IDs including descendants
    std::string role_code;
    std::vector<std::string> permissions;
    int64_t     iat = 0;                // issued at (epoch seconds)
    int64_t     exp = 0;                // expiration (epoch seconds)
};

// ============================================================
// Device (cloud-side view)
// ============================================================
struct Device {
    std::string  tenant_id;
    std::string  device_id;
    std::string  product_id;
    DeviceType   type = DeviceType::OTHER;
    DeviceStatus status = DeviceStatus::OFFLINE;
    std::string  firmware_version;
    std::string  mac_address;
    std::string  model;
    std::string  hardware_uid;              // chip serial / secure element ID
    int64_t      last_heartbeat_at = 0;     // epoch seconds
    int64_t      activated_at = 0;           // epoch seconds
    bool         activated = false;
};

// ============================================================
// Device Activation (REQ-DM-002)
// ============================================================

// Request: device → cloud
struct ActivationRequest {
    std::string  hardware_uid;      // unique hardware identifier
    std::string  model;             // device model string
    DeviceType   device_type = DeviceType::OTHER;
    std::string  firmware_version;
    std::string  mac_address;
    std::string  tenant_id;         // which tenant this device belongs to
};

// Response: cloud → device
struct ActivationResponse {
    bool         success = false;
    std::string  device_id;         // assigned by cloud
    std::string  tenant_id;
    std::string  product_id;
    std::string  activation_token;  // JWT or opaque token for MQTT auth
    std::string  mqtt_broker_uri;
    std::string  certificate_pem;   // optional: device client cert (mTLS)
    std::string  private_key_pem;   // optional: device private key
    int32_t      ttl_seconds = 0;   // token validity period
    std::string  error_message;
    int32_t      error_code = 0;
};

// ============================================================
// Parsed MQTT Topic (核心：通配符订阅后的消息路由)
// ============================================================
struct ParsedTopic {
    std::string raw_topic;
    std::string tenant_id;
    std::string product_id;
    std::string device_id;
    std::string message_type;   // heartbeat / event / property / ota
    std::string command;         // for command/{cmd} topics

    bool valid = false;

    // Parse from received topic string
    // Format: {tenant}/iot/{product_id}/{device_id}/{type}/...
    static ParsedTopic parse(const std::string& topic) {
        ParsedTopic pt;
        pt.raw_topic = topic;

        std::vector<std::string> parts;
        std::stringstream ss(topic);
        std::string segment;
        while (std::getline(ss, segment, '/')) {
            parts.push_back(segment);
        }

        // Expected minimum: tenant/iot/product/device/type
        if (parts.size() < 5) return pt;

        pt.tenant_id  = parts[0];
        // parts[1] should be "iot"
        pt.product_id = parts[2];
        pt.device_id  = parts[3];
        pt.message_type = parts[4];  // "heartbeat" / "event" / "property" / "ota" / "command"

        if (parts.size() >= 6) {
            if (parts[4] == "command") {
                pt.command = parts[5];
            } else {
                // e.g. "post" or "progress" or "set_reply"
                pt.command = parts[5];
            }
        }

        pt.valid = (parts[1] == "iot" && !pt.tenant_id.empty()
                    && !pt.device_id.empty() && !pt.message_type.empty());
        return pt;
    }

    // Build downlink topic for sending to a specific device
    static std::string build_downlink(const std::string& tenant_id,
                                       const std::string& product_id,
                                       const std::string& device_id,
                                       const std::string& type,
                                       const std::string& sub = "") {
        std::string topic = tenant_id + "/iot/" + product_id + "/" + device_id + "/" + type;
        if (!sub.empty()) topic += "/" + sub;
        return topic;
    }
};

// ============================================================
// Generic MQTT Message (received from broker)
// ============================================================
struct MqttMessage {
    std::string  topic;
    std::string  payload;       // JSON string
    ParsedTopic  parsed;
};

// ============================================================
// Order
// ============================================================
struct Order {
    std::string order_id;
    std::string tenant_id;
    std::string device_id;
    std::string recipe_id;
    std::string cup_size;
    int32_t     quantity = 1;
    int32_t     total_amount = 0;
    std::string payment_method;
    OrderStatus status = OrderStatus::PENDING;
    std::string created_at;
    std::string expired_at;
    std::string failure_reason;
};

// ============================================================
// Recipe
// ============================================================
struct RecipeStep {
    std::string action;
    int32_t     duration_ms = 0;
    std::map<std::string, double> params;
};

struct CupSizeOption {
    std::string size;
    int32_t     price = 0;
    int32_t     volume_ml = 0;
};

struct Recipe {
    std::string recipe_id;
    std::string recipe_name;
    DeviceType  device_type = DeviceType::OTHER;
    std::string category;
    std::string description;
    std::vector<RecipeStep> steps;
    std::vector<CupSizeOption> cup_sizes;
    bool        is_active = true;
    int32_t     version = 1;
};

// ============================================================
// Fault Info
// ============================================================
struct FaultInfo {
    std::string  tenant_id;
    std::string  device_id;
    FaultCode    code = FaultCode::NONE;
    FaultLevel   level = FaultLevel::L0_HINT;
    std::string  description;
    std::string  timestamp;
    std::string  sensor_snapshot;
};

// ============================================================
// Heartbeat Data (received from device)
// ============================================================
struct HeartbeatData {
    std::string  tenant_id;
    std::string  device_id;
    std::string  timestamp;
    DeviceStatus status = DeviceStatus::OFFLINE;
    std::string  firmware_version;
    int32_t      signal_strength = 0;
    int32_t      alarm_count = 0;
};

// ============================================================
// OTA Info (cloud-side: per-device OTA state tracking)
// ============================================================
struct OtaRecord {
    std::string tenant_id;
    std::string device_id;
    std::string current_version;
    std::string target_version;
    std::string download_url;
    std::string checksum;
    int         progress = 0;     // 0-100
    std::string stage;            // "downloading" / "installing" / "done" / "failed"
    bool        force_upgrade = false;
};

// ============================================================
// Firmware Version (cloud-side firmware registry)
// ============================================================
struct FirmwareVersion {
    std::string version;
    std::string product_id;
    std::string download_url;
    std::string checksum_sha256;
    std::string changelog;
    bool        force_upgrade = false;
    int64_t     created_at = 0;
};

// ============================================================
// Device Config (device-side, REQ-CF-001)
// ============================================================
struct DeviceConfig {
    int32_t heartbeat_interval      = 60;
    int32_t auto_clean_interval     = 20;
    int32_t idle_clean_delay        = 30;
    int32_t screen_brightness       = 80;
    int32_t volume_level            = 70;
    bool    energy_save_mode        = false;
    int32_t order_expire_minutes    = 15;
    int32_t max_queue_depth         = 10;
};

// ============================================================
// Service Config (cloud-side)
// ============================================================
struct ServiceConfig {
    std::string mqtt_broker_uri;
    std::string mqtt_client_id;       // "cloud-platform-service-{instance_id}"
    std::string ca_cert_path;
    std::string client_cert_path;
    std::string client_key_path;
    int32_t     heartbeat_timeout_sec = 180;   // 3x heartbeat interval
    int32_t     offline_check_interval_sec = 30;
};

// ============================================================
// Minimal JSON Helper (avoids heavy dependency for simple parsing)
// ============================================================
struct JsonHelper {
    // Extract string field from flat JSON body
    static std::string get_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos != std::string::npos) {
            size_t start = pos + search.size();
            size_t end = json.find('"', start);
            if (end != std::string::npos) return json.substr(start, end - start);
        }
        return "";
    }

    // Extract int field
    static int32_t get_int(const std::string& json, const std::string& key, int32_t default_val = 0) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos != std::string::npos) {
            size_t start = pos + search.size();
            size_t end = json.find_first_of(",}\n\r ]", start);
            std::string num = json.substr(start, end - start);
            if (!num.empty()) return std::stoi(num);
        }
        return default_val;
    }

    // Extract bool field
    static bool get_bool(const std::string& json, const std::string& key, bool default_val = false) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos != std::string::npos) {
            if (json.find("true", pos) < json.find_first_of(",}\n\r", pos)) return true;
            if (json.find("false", pos) < json.find_first_of(",}\n\r", pos)) return false;
        }
        return default_val;
    }
};

} // namespace dev_sys
