#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <sstream>
#include <chrono>
#include <random>
#include <cstring>

#ifdef HAS_OPENSSL
#include <openssl/rand.h>
#endif

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
    std::string model_code;           // unique display code, e.g. "ACM-200"
    std::string model_key;            // device-side key, e.g. "acm_200_v1" (no spaces/special chars)
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
// ULID Generator — 26-char sortable ID for device model_key
// Crockford base32: 0123456789ABCDEFGHJKMNPQRSTVWXYZ
// ============================================================
struct UlidGenerator {
    static std::string generate() {
        // 48-bit timestamp (milliseconds since epoch) → 10 base32 chars
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // 80-bit random → 16 base32 chars
        uint8_t rand_bytes[10];
#ifdef HAS_OPENSSL
        // cryptographically secure
        RAND_bytes(rand_bytes, sizeof(rand_bytes));
#else
        // fallback: std::random_device
        static thread_local std::random_device rd;
        static thread_local std::mt19937_64 gen(rd());
        static thread_local std::uniform_int_distribution<uint64_t> dis;
        for (int i = 0; i < 5; i++) {
            uint64_t v = dis(gen);
            rand_bytes[i*2]   = v & 0xFF;
            rand_bytes[i*2+1] = (v >> 8) & 0xFF;
        }
#endif
        std::string ulid;
        ulid.reserve(26);
        // Encode timestamp (10 chars from 6 bytes)
        encode_time(ulid, ms);
        // Encode random (16 chars from 10 bytes)
        encode_random(ulid, rand_bytes);
        return ulid;
    }

private:
    static const char* crockford() {
        return "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    }

    static void encode_time(std::string& s, uint64_t ms) {
        // 48 bits = 6 bytes, encode as 10 base32 chars
        // Each 5 bits → 1 char, 48 bits = 10 chars (need 50 bits, pad 2 zero bits)
        uint64_t v = ms & 0xFFFFFFFFFFFFULL; // 48-bit mask
        for (int i = 0; i < 10; i++) {
            int shift = 45 - i * 5; // 45, 40, 35, ..., 0
            s += crockford()[(v >> shift) & 0x1F];
            if (shift == 0) break;
        }
    }

    static void encode_random(std::string& s, const uint8_t* r) {
        // 80 bits = 10 bytes = 16 groups of 5 bits
        for (int i = 0; i < 16; i++) {
            int bit_off = i * 5;
            int byte_off = bit_off / 8;
            int bit_pos  = bit_off % 8;
            uint16_t word = (r[byte_off] << 8) | (byte_off + 1 < 10 ? r[byte_off + 1] : 0);
            s += crockford()[(word >> (11 - bit_pos)) & 0x1F];
        }
    }
};

// ============================================================
// Device Network Status (MQTT connection state)
// ============================================================
enum class NetworkStatus : uint8_t {
    ONLINE  = 0,
    OFFLINE = 1,
};

// ============================================================
// Device Work Status (operational state)
// ============================================================
enum class WorkStatus : uint8_t {
    IDLE        = 0,
    BREWING     = 1,
    FAULT       = 2,
    UPGRADING   = 3,
    MAINTENANCE = 4,
};

// ============================================================
// Fault Codes & Levels
// ============================================================
// 故障码: W=警告(Warning) E=错误(Error)
enum class FaultCode : uint8_t {
    NONE               = 0,
    // 错误 (Error) — 影响运行，停止制作
    E001_HEATER_OVERTEMP    = 1,
    E002_HEATER_NOT_HEATING = 2,
    E003_PUMP_FAILURE       = 3,
    E004_MOTOR_STALL        = 4,
    E005_WATER_LEAK         = 5,
    // 警告 (Warning) — 不中断运行，提醒运维
    W001_SENSOR_OFFLINE     = 6,
    W002_COMM_FAIL          = 7,
    W003_MATERIAL_LOW       = 8,
};

enum class FaultLevel : uint8_t {
    WARNING = 0,  // 警告：不中断运行，提醒运维关注
    ERROR   = 1,  // 错误：影响运行，停止制作，需人工处理
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
    NetworkStatus network_status = NetworkStatus::OFFLINE;  // MQTT connection state
    WorkStatus   work_status   = WorkStatus::IDLE;          // operational state
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
    std::string  uid;         // hardware unique ID (chip serial / eFuse)
    std::string  model_key;   // device model key (ULID, e.g. "01KX5M2KM8EBW9G1CWVMJ94TSK")
    // Note: tenant_id is NOT accepted from device — server assigns based on DB config
};

// Response: cloud → device
struct ActivationResponse {
    bool         success = false;
    std::string  device_id;         // assigned by cloud
    std::string  model_key;         // echo back
    std::string  model_code;        // resolved from model_key
    std::string  tenant_id;
    std::string  product_id;
    std::string  device_type;       // resolved: "coffee_machine" etc
    std::string  firmware_version;  // from model's firmware_base
    std::string  activation_token;
    std::string  mqtt_broker_uri;
    int32_t      ttl_seconds = 0;
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
    // Format A: {device_id}/v1/{type}/...
    // Format B: $T/{tenant}/{device_id}/v1/{type}/...  (broker prefix)
    static ParsedTopic parse(const std::string& topic) {
        ParsedTopic pt;
        pt.raw_topic = topic;

        std::vector<std::string> parts;
        std::stringstream ss(topic);
        std::string segment;
        while (std::getline(ss, segment, '/')) {
            parts.push_back(segment);
        }

        if (parts.size() < 3) return pt;

        int offset = 0;
        // Detect broker prefix: $T/{tenant}/...
        if (parts[0] == "$T" && parts.size() >= 5) {
            pt.tenant_id  = parts[1];
            offset = 2;
        }

        // device_id at parts[offset], "v1" at parts[offset+1], type at parts[offset+2]
        pt.device_id    = parts[offset];
        pt.message_type = (offset + 2 < static_cast<int>(parts.size())) ? parts[offset + 2] : "";

        if (offset + 3 < static_cast<int>(parts.size())) {
            pt.command = parts[offset + 3];
        }

        std::string ver = (offset + 1 < static_cast<int>(parts.size())) ? parts[offset + 1] : "";
        pt.valid = (ver == "v1" && !pt.device_id.empty() && !pt.message_type.empty());
        return pt;
    }

    // Build downlink topic for sending to a specific device
    static std::string build_downlink(const std::string& tenant_id,
                                       const std::string& /*product_id*/,
                                       const std::string& device_id,
                                       const std::string& type,
                                       const std::string& sub = "") {
        // $T/{tenant}/{device}/v1/{type}/...
        std::string t = tenant_id.empty() ? "default" : tenant_id;
        std::string topic = "$T/" + t + "/" + device_id + "/v1/" + type;
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
    std::string recipe_name;
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
    FaultLevel   level = FaultLevel::WARNING;
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
    NetworkStatus network_status = NetworkStatus::OFFLINE;
    WorkStatus   work_status   = WorkStatus::IDLE;
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
    std::string file_name;
    int64_t     file_size = 0;
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
