#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <functional>
#include <set>
#include <cstring>
#include <sys/stat.h>
#include <filesystem>
#include <vector>
#include <regex>
#include <sys/socket.h>
#include <netdb.h>
#include <cstring>

#ifdef HAS_OPENSSL
#include <openssl/sha.h>
#endif

// App layer
#include "app/message_router.h"
#include "app/device/device_manager.h"
#include "app/device/device_activation.h"
#include "app/device/device_type_manager.h"
#include "app/recipe/recipe_manager.h"
#include "app/order/order_manager.h"
#include "app/ota/ota_manager.h"
#include "app/fault/fault_detector.h"
#include "app/organization/org_manager.h"
#include "app/organization/account_manager.h"

// Middleware
#include "middleware/communication/mqtt_client.h"
#include "middleware/communication/api_server.h"
#include "middleware/security/tls_manager.h"
#include "middleware/storage/database.h"
#include "middleware/storage/log_manager.h"

using namespace dev_sys;

static std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    (void)sig;
    std::cout << "\n[Main] Shutting down (signal " << sig << ")..." << std::endl;
    g_running = false;
}

// ============================================================
// Auth Helper — extract & verify Bearer token from request
// Returns std::nullopt if token is missing/invalid
// ============================================================
static std::optional<TokenPayload> extract_auth(const HttpRequest& req, AccountManager& acct_mgr) {
    std::string token;
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end() && it->second.find("Bearer ") == 0)
        token = it->second.substr(7);
    // Also support token in body for POST/PUT
    if (token.empty())
        token = JsonHelper::get_string(req.body, "token");
    if (token.empty())
        return std::nullopt;
    return acct_mgr.verify_token(token);
}

// ============================================================
// Main — IoT云平台服务程序
//
// 启动流程:
//   1. 初始化数据库/日志/TLS
//   2. 加载设备注册信息
//   3. 创建消息路由器和各业务模块
//   4. 连接MQTT Broker，订阅通配符topic: +/v1/...
//   5. 进入消息驱动主循环
//
// MQTT角色: 特权客户端(可订阅所有租户所有设备topic)
// 消息流:
//   IN:  +/v1/heartbeat       → DeviceManager
//   IN:  +/v1/event/post       → OrderManager / FaultManager
//   IN:  +/v1/property/post    → DeviceManager
//   IN:  +/v1/ota/progress     → OtaManager
//   OUT: {device}/v1/command/{cmd}
//   OUT: {device}/v1/ota/notify
// ============================================================
int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ======== Phase 1: Infrastructure ========
    LogManager log_mgr;
    log_mgr.init("./logs", LogManager::Level::INFO);
    std::filesystem::create_directories("./firmware");

    // Load MQTT config (broker host/port/api from mqtt_config.json)
    std::string broker_host = "127.0.0.1";
    std::string mqtt_user = "devsys", mqtt_pass = "";
    int broker_port = 1883, broker_api_port = 8080;
    {
        std::ifstream cf("./config/mqtt_config.json");
        if (cf.is_open()) {
            std::string j((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
            auto jg = [&j](const std::string& k) -> std::string {
                auto p = j.find("\"" + k + "\":");
                if (p == std::string::npos) return "";
                p += k.size() + 3;
                while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
                if (p < j.size() && j[p] == '"') { p++; auto e = j.find('"', p); return j.substr(p, e - p); }
                auto e = j.find_first_of(",}\n\r \t", p);
                return j.substr(p, e - p);
            };
            auto ji = [&j, &jg](const std::string& k, int d) -> int {
                auto s = jg(k); return s.empty() ? d : std::stoi(s);
            };
            broker_host = jg("broker_host"); if (broker_host.empty()) broker_host = "127.0.0.1";
            broker_port = ji("broker_port", 1883);
            broker_api_port = ji("broker_api_port", 8080);
            mqtt_user = jg("username");
            mqtt_pass = jg("password");
            std::cout << "[Main] MQTT broker: " << broker_host << ":" << broker_port
                      << "  API: " << broker_host << ":" << broker_api_port
                      << "  user: " << mqtt_user << std::endl;
        }
    }
    auto mqtt_broker_uri = std::make_shared<std::string>(
        "tcp://" + broker_host + ":" + std::to_string(broker_port));
    auto mqtt_broker_api = std::make_shared<std::string>(
        "http://" + broker_host + ":" + std::to_string(broker_api_port));

    Database db;
    // PostgreSQL connection string (libpq format)
    // When HAS_LIBPQ not defined, falls back to in-memory storage
    const char* db_conn = std::getenv("DEV_SYS_DB");
    std::string conn_str = db_conn ? db_conn
        : "postgresql://devsys:devsys@127.0.0.1:5432/devsys_cloud";
    if (db.open(conn_str) != StatusCode::OK) {
        std::cerr << "[Main] Database open failed. Connection: " << conn_str << std::endl;
        return 1;
    }

    TlsManager tls_mgr;
    tls_mgr.init("/etc/dev-sys-cloud/certs/ca.pem",
                  "/etc/dev-sys-cloud/certs/client.pem",
                  "/etc/dev-sys-cloud/certs/client_key.pem");

    // ======== Phase 2: Business modules ========
    DeviceManager device_mgr;
    device_mgr.set_database(&db);
    device_mgr.load_from_database();
    device_mgr.set_offline_callback(
        [&log_mgr](const std::string& tenant, const std::string& device) {
            log_mgr.warn("device", "Device offline: " + tenant + "/" + device);
        });

    OrderManager order_mgr;
    OtaManager ota_mgr;
    ota_mgr.set_device_manager(&device_mgr);
    FaultManager fault_mgr;
    RecipeManager recipe_mgr;

    // Wire database to managers for persistence
    recipe_mgr.set_database(&db);
    recipe_mgr.load_from_database();
    if (recipe_mgr.all_recipes().empty()) recipe_mgr.seed_mock_data();

    order_mgr.set_database(&db);
    order_mgr.load_from_database();
    if (order_mgr.list_all_orders().empty()) order_mgr.seed_mock_data();
    ota_mgr.set_database(&db);
    ota_mgr.load_from_database();
    // Scan disk for firmware files (survives restart)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::exists("./firmware", ec)) {
            for (const auto& prod_entry : fs::directory_iterator("./firmware", ec)) {
                if (!prod_entry.is_directory(ec)) continue;
                std::string product = prod_entry.path().filename().string();
                for (const auto& file_entry : fs::directory_iterator(prod_entry.path(), ec)) {
                    if (!file_entry.is_regular_file(ec)) continue;
                    std::string fname = file_entry.path().filename().string();
                    auto sz = file_entry.file_size(ec);
                    // Derive version from filename (strip extension)
                    std::string ver = fname;
                    size_t dot = ver.rfind('.');
                    if (dot != std::string::npos) ver = ver.substr(0, dot);
                    // Only register if not already present
                    if (!ota_mgr.get_firmware(ver)) {
                        FirmwareVersion fw;
                        fw.version = ver; fw.product_id = product;
                        fw.file_name = fname; fw.file_size = static_cast<int64_t>(sz);
                        fw.download_url = "/api/v1/ota/firmwares/" + ver + "/download";
                        fw.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        ota_mgr.register_firmware(fw);
                    }
                }
            }
            std::cout << "[OtaMgr] Disk scan complete" << std::endl;
        }
    }
    fault_mgr.set_database(&db);
    fault_mgr.load_from_database();

    // ======== Phase 2.3: Organization & Account Management ========
    OrgManager org_mgr;
    org_mgr.set_database(&db);
    org_mgr.load_from_database();

    AccountManager acct_mgr(&org_mgr);
    acct_mgr.set_database(&db);
    acct_mgr.load_from_database();

    // ======== Phase 2.5: HTTP API Server + Device Activation (REQ-DM-002) ========
    DeviceActivation activation(db);
    activation.set_tls_manager(&tls_mgr);
    activation.set_broker_uri(*mqtt_broker_uri);
    // 从数据库读取默认租户名
    { std::string tn = db.load_tenant_config("tenant_name");
      if (!tn.empty()) activation.set_default_tenant(tn); }

    DeviceTypeManager type_mgr(db);

    ApiServer api;

    // Shared connection status for health endpoint reporting
    auto mqtt_connected = std::make_shared<std::atomic<bool>>(false);
    auto database_ok = std::make_shared<bool>(true);  // DB is initialized above

    // Health check
    api.get("/api/v1/health", [mqtt_connected, database_ok](const HttpRequest&) -> HttpResponse {
        std::ostringstream json;
        json << R"({"status":"ok","service":"dev-sys-cloud","version":"1.0.0")"
             << R"(,"uptime_seconds":)" << std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()
             << R"(,"mqtt_connected":)" << (mqtt_connected->load() ? "true" : "false")
             << R"(,"database_ok":)" << (*database_ok ? "true" : "false")
             << "}";
        return ApiServer::json_response(200, json.str());
    });

    // Device activation endpoint (REQ-DM-002)
    api.post("/api/v1/device/activate",
        [&activation, &device_mgr](const HttpRequest& req) -> HttpResponse {
            ActivationRequest act_req;
            act_req.uid      = JsonHelper::get_string(req.body, "uid");
            act_req.model_key = JsonHelper::get_string(req.body, "model_key");
            // tenant_id is NOT accepted from device — server assigns it
            std::string remote_ip = req.remote_ip.empty() ? "127.0.0.1" : req.remote_ip;

            ActivationResponse resp = activation.process_activation(act_req, remote_ip);

            if (resp.success) {
                Device dev;
                dev.device_id    = resp.device_id;
                dev.tenant_id    = resp.tenant_id;
                dev.product_id   = resp.product_id;
                dev.type         = DeviceType::OTHER;

                // Resolve internal_type from device_type string
                if (resp.device_type == "coffee_machine")  dev.type = DeviceType::COFFEE_MACHINE;
                else if (resp.device_type == "instant_machine") dev.type = DeviceType::INSTANT_MACHINE;
                else if (resp.device_type == "water_dispenser") dev.type = DeviceType::WATER_DISPENSER;

                dev.model        = resp.model_code;
                dev.hardware_uid = act_req.uid;
                dev.firmware_version = resp.firmware_version;
                dev.network_status = NetworkStatus::ONLINE;
                dev.work_status    = WorkStatus::IDLE;
                dev.activated    = true;
                dev.last_heartbeat_at = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                dev.activated_at = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                device_mgr.register_device(dev);
            }

            std::ostringstream json;
            json << "{"
                 << "\"success\":" << (resp.success ? "true" : "false") << ","
                 << "\"device_id\":\"" << resp.device_id << "\","
                 << "\"model_key\":\"" << resp.model_key << "\","
                 << "\"model_code\":\"" << resp.model_code << "\","
                 << "\"tenant_id\":\"" << resp.tenant_id << "\","
                 << "\"product_id\":\"" << resp.product_id << "\","
                 << "\"device_type\":\"" << resp.device_type << "\","
                 << "\"firmware_version\":\"" << resp.firmware_version << "\","
                 << "\"activation_token\":\"" << resp.activation_token << "\","
                 << "\"mqtt_broker_uri\":\"" << resp.mqtt_broker_uri << "\","
                 << "\"ttl_seconds\":" << resp.ttl_seconds;
            if (!resp.error_message.empty())
                json << ",\"error_code\":" << resp.error_code
                     << ",\"error_message\":\"" << resp.error_message << "\"";
            json << "}";
            return ApiServer::json_response(resp.success ? 201 : 400, json.str());
        });

    // Device heartbeat: MQTT only (see message_router.cpp handle_heartbeat)
    // No HTTP fallback — devices must use MQTT for heartbeat

    // Device status query (supports org_scope filtering via Bearer token)
    api.get("/api/v1/device/status",
        [&device_mgr, &acct_mgr, &org_mgr](const HttpRequest& req) -> HttpResponse {
            auto devices = device_mgr.list_all_devices();

            // Apply org_scope filter if valid token provided
            auto tp = extract_auth(req, acct_mgr);
            if (tp && tp->role_code != "super_admin") {
                // Filter to only devices within the account's org scope
                std::vector<Device> filtered;
                std::set<std::string> visible_tenants;
                for (int32_t oid : tp->org_scope) {
                    auto org = org_mgr.get_org(oid);
                    if (org) visible_tenants.insert(org->tenant_id);
                }
                for (const auto& dev : devices) {
                    if (visible_tenants.count(dev.tenant_id)) {
                        filtered.push_back(dev);
                    }
                }
                devices = std::move(filtered);
            }

            std::ostringstream json;
            json << "{\"total\":" << devices.size() << ",\"devices\":[";
            bool first = true;
            for (const auto& dev : devices) {
                if (!first) json << ",";
                first = false;
                json << "{\"device_id\":\"" << dev.device_id << "\","
                     << "\"tenant_id\":\"" << dev.tenant_id << "\","
                     << "\"network_status\":" << static_cast<int>(dev.network_status) << ","
                     << "\"work_status\":" << static_cast<int>(dev.work_status) << ","
                     << "\"type\":" << static_cast<int>(dev.type)
                     << "}";
            }
            json << "]}";
            return ApiServer::json_response(200, json.str());
        });

    // List all devices with full detail (admin panel)
    api.get("/api/v1/devices",
        [&device_mgr, &acct_mgr, &org_mgr, &db](const HttpRequest& req) -> HttpResponse {
            auto devices = device_mgr.list_all_devices();
            auto tp = extract_auth(req, acct_mgr);
            if (tp && tp->role_code != "super_admin") {
                std::set<std::string> vt;
                for (int32_t oid : tp->org_scope) {
                    auto o = org_mgr.get_org(oid);
                    if (o) vt.insert(o->tenant_id);
                }
                devices.erase(std::remove_if(devices.begin(), devices.end(),
                    [&vt](const Device& d) { return !vt.count(d.tenant_id); }),
                    devices.end());
            }
            std::ostringstream json;
            json << "{\"code\":0,\"message\":\"success\",\"data\":{\"total\":" << devices.size()
                 << ",\"devices\":[";
            for (size_t i = 0; i < devices.size(); i++) {
                if (i > 0) json << ",";
                const auto& d = devices[i];
                json << "{"
                     << R"("device_id":")" << d.device_id << R"(",)";
                json << R"("tenant_id":")" << d.tenant_id << R"(",)";
                json << R"("product_id":")" << d.product_id << R"(",)";
                json << R"("model":")" << d.model << R"(",)";
                json << R"("hardware_uid":")" << d.hardware_uid << R"(",)";
                json << R"("firmware_version":")" << d.firmware_version << R"(",)";
                json << R"("type":)" << static_cast<int>(d.type) << ",";
                json << R"("network_status":)" << static_cast<int>(d.network_status) << ",";
                json << R"("work_status":)" << static_cast<int>(d.work_status) << ",";
                json << R"("activated":)" << (d.activated ? "true" : "false") << ",";
                json << R"("ts":)" << d.last_heartbeat_at << ",";
                json << R"("activated_at":)" << d.activated_at;
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Migrate device to a different tenant (admin operation)
    api.put("/api/v1/devices/{device_id}/tenant",
        [&device_mgr, &db](const HttpRequest& req) -> HttpResponse {
            std::string device_id = req.path.substr(
                std::string("/api/v1/devices/").size());
            size_t slash = device_id.rfind("/tenant");
            if (slash == std::string::npos)
                return ApiServer::error_response(404, 1002, "Not found");
            device_id = device_id.substr(0, slash);
            std::string new_tenant = JsonHelper::get_string(req.body, "tenant_id");
            if (new_tenant.empty())
                return ApiServer::error_response(400, 1001, "tenant_id is required");

            // Ensure tenant exists in DB
            std::ostringstream ensure_sql;
            ensure_sql << "INSERT INTO tenants (tenant_id, name) VALUES ('"
                       << new_tenant << "','" << new_tenant
                       << "') ON CONFLICT (tenant_id) DO NOTHING";
            db.execute(ensure_sql.str());

            // Migrate device to new tenant in DeviceManager (in-memory)
            StatusCode mg_sc = device_mgr.migrate_device(device_id, new_tenant);
            if (mg_sc != StatusCode::OK)
                return ApiServer::error_response(404, 1002, "Device not found: " + device_id);

            // Update in database
            std::ostringstream sql;
            sql << "UPDATE devices SET tenant_id='" << new_tenant
                << "', updated_at=EXTRACT(EPOCH FROM NOW())::BIGINT "
                << "WHERE device_id='" << device_id << "'";
            StatusCode sc = db.execute(sql.str());
            if (sc != StatusCode::OK)
                return ApiServer::error_response(500, 2000, "Database update failed");

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("device_id":")" << device_id << R"(",)";
            json << R"("tenant_id":")" << new_tenant << R"("}})";
            return ApiServer::json_response(200, json.str());
        });

    // Delete device
    api.del("/api/v1/devices/{device_id}",
        [&device_mgr, &db](const HttpRequest& req) -> HttpResponse {
            std::string did = req.path.substr(std::string("/api/v1/devices/").size());
            // Find tenant for this device
            std::string tid;
            for (const auto& d : device_mgr.list_all_devices()) {
                if (d.device_id == did) { tid = d.tenant_id; break; }
            }
            if (tid.empty()) return ApiServer::error_response(404, 1002, "Device not found: " + did);
            // Remove from in-memory
            device_mgr.remove_device(tid, did);
            // Remove from DB
            db.execute("DELETE FROM activation_tokens WHERE device_id='" + did + "'");
            db.execute("DELETE FROM devices WHERE device_id='" + did + "'");
            std::ostringstream j;
            j << R"({"code":0,"message":"success","data":{"device_id":")" << did << R"(","deleted":true}})";
            return ApiServer::json_response(200, j.str());
        });

    // ======== Device Types API (CRUD for frontend) ========
    // List all types (active only by default)
    api.get("/api/v1/device-types",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            bool active_only = req.path.find("include_inactive=true") == std::string::npos;
            auto types = type_mgr.list_types(active_only);
            std::ostringstream json;
            json << "{\"total\":" << types.size() << ",\"types\":[";
            for (size_t i = 0; i < types.size(); i++) {
                if (i > 0) json << ",";
                const auto& t = types[i];
                json << "{\"id\":" << t.id
                     << ",\"type_code\":\"" << t.type_code << "\""
                     << ",\"display_name\":\"" << t.display_name << "\""
                     << ",\"description\":\"" << t.description << "\""
                     << ",\"internal_type\":" << t.internal_type
                     << ",\"icon_url\":\"" << t.icon_url << "\""
                     << ",\"sort_order\":" << t.sort_order
                     << ",\"is_active\":" << (t.is_active ? "true" : "false")
                     << ",\"created_at\":\"" << t.created_at << "\""
                     << ",\"updated_at\":\"" << t.updated_at << "\""
                     << "}";
            }
            json << "]}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single type
    api.get("/api/v1/device-types/{code}",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            std::string code = req.path.substr(std::string("/api/v1/device-types/").size());
            auto t = type_mgr.get_type(code);
            if (!t) return ApiServer::error_response(404, -404, "Type not found: " + code);
            std::ostringstream json;
            json << "{\"id\":" << t->id
                 << ",\"type_code\":\"" << t->type_code << "\""
                 << ",\"display_name\":\"" << t->display_name << "\""
                 << ",\"description\":\"" << t->description << "\""
                 << ",\"internal_type\":" << t->internal_type
                 << ",\"icon_url\":\"" << t->icon_url << "\""
                 << ",\"sort_order\":" << t->sort_order
                 << ",\"is_active\":" << (t->is_active ? "true" : "false")
                 << "}";
            return ApiServer::json_response(200, json.str());
        });

    // Create type
    api.post("/api/v1/device-types",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            DeviceTypeInfo info;
            info.type_code     = JsonHelper::get_string(req.body, "type_code");
            info.display_name  = JsonHelper::get_string(req.body, "display_name");
            info.description   = JsonHelper::get_string(req.body, "description");
            info.internal_type = JsonHelper::get_int(req.body, "internal_type", 4);
            info.icon_url      = JsonHelper::get_string(req.body, "icon_url");
            info.sort_order    = JsonHelper::get_int(req.body, "sort_order", 0);

            StatusCode sc = type_mgr.create_type(info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));

            return ApiServer::json_response(201,
                "{\"success\":true,\"type_code\":\"" + info.type_code + "\"}");
        });

    // Update type
    api.put("/api/v1/device-types/{code}",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            std::string code = req.path.substr(std::string("/api/v1/device-types/").size());
            if (!type_mgr.type_exists(code))
                return ApiServer::error_response(404, -404, "Type not found: " + code);

            DeviceTypeInfo info;
            info.type_code     = code;
            info.display_name  = JsonHelper::get_string(req.body, "display_name");
            info.description   = JsonHelper::get_string(req.body, "description");
            info.internal_type = JsonHelper::get_int(req.body, "internal_type", 4);
            info.icon_url      = JsonHelper::get_string(req.body, "icon_url");
            info.sort_order    = JsonHelper::get_int(req.body, "sort_order", 0);
            info.is_active     = JsonHelper::get_bool(req.body, "is_active", true);

            StatusCode sc = type_mgr.update_type(code, info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));

            return ApiServer::json_response(200,
                "{\"success\":true,\"type_code\":\"" + code + "\"}");
        });

    // Delete type (soft)
    api.del("/api/v1/device-types/{code}",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            std::string code = req.path.substr(std::string("/api/v1/device-types/").size());
            StatusCode sc = type_mgr.delete_type(code);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                "{\"success\":true,\"type_code\":\"" + code + "\",\"deleted\":true}");
        });

    // ======== Device Models API (CRUD for frontend) ========
    // List models (optional ?type_code= filter)
    api.get("/api/v1/device-models",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            // Extract ?type_code= from path or query string
            std::string filter_type;
            auto it = req.headers.find("X-Filter-Type");
            if (it != req.headers.end()) filter_type = it->second;
            // Also try from query in path
            size_t qpos = req.path.find("?type_code=");
            if (qpos != std::string::npos)
                filter_type = req.path.substr(qpos + 11);

            auto models = type_mgr.list_models(filter_type, true);
            std::ostringstream json;
            json << "{\"total\":" << models.size() << ",\"models\":[";
            for (size_t i = 0; i < models.size(); i++) {
                if (i > 0) json << ",";
                const auto& m = models[i];
                json << "{\"id\":" << m.id
                     << ",\"model_code\":\"" << m.model_code << "\""
                     << ",\"model_key\":\"" << m.model_key << "\""
                     << ",\"type_code\":\"" << m.type_code << "\""
                     << ",\"display_name\":\"" << m.display_name << "\""
                     << ",\"description\":\"" << m.description << "\""
                     << ",\"specs_json\":\"" << m.specs_json << "\""
                     << ",\"firmware_base\":\"" << m.firmware_base << "\""
                     << ",\"is_active\":" << (m.is_active ? "true" : "false")
                     << ",\"sort_order\":" << m.sort_order
                     << ",\"created_at\":\"" << m.created_at << "\""
                     << "}";
            }
            json << "]}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single model
    api.get("/api/v1/device-models/{code}",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            std::string code = req.path.substr(std::string("/api/v1/device-models/").size());
            auto m = type_mgr.get_model(code);
            if (!m) return ApiServer::error_response(404, -404, "Model not found: " + code);
            std::ostringstream json;
            json << "{\"id\":" << m->id
                 << ",\"model_code\":\"" << m->model_code << "\""
                 << ",\"model_key\":\"" << m->model_key << "\""
                 << ",\"type_code\":\"" << m->type_code << "\""
                 << ",\"display_name\":\"" << m->display_name << "\""
                 << ",\"description\":\"" << m->description << "\""
                 << ",\"specs_json\":\"" << m->specs_json << "\""
                 << ",\"firmware_base\":\"" << m->firmware_base << "\""
                 << ",\"is_active\":" << (m->is_active ? "true" : "false")
                 << ",\"sort_order\":" << m->sort_order
                 << "}";
            return ApiServer::json_response(200, json.str());
        });

    // Create model
    api.post("/api/v1/device-models",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            DeviceModelInfo info;
            info.model_code    = JsonHelper::get_string(req.body, "model_code");
            info.model_key     = JsonHelper::get_string(req.body, "model_key");
            // Auto-generate ULID if model_key not provided
            if (info.model_key.empty()) info.model_key = UlidGenerator::generate();
            info.type_code     = JsonHelper::get_string(req.body, "type_code");
            info.display_name  = JsonHelper::get_string(req.body, "display_name");
            info.description   = JsonHelper::get_string(req.body, "description");
            info.specs_json    = JsonHelper::get_string(req.body, "specs_json");
            info.firmware_base = JsonHelper::get_string(req.body, "firmware_base");
            info.sort_order    = JsonHelper::get_int(req.body, "sort_order", 0);

            if (info.specs_json.empty()) info.specs_json = "{}";
            if (info.firmware_base.empty()) info.firmware_base = "1.0.0";

            StatusCode sc = type_mgr.create_model(info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));

            return ApiServer::json_response(201,
                "{\"success\":true,\"model_code\":\"" + info.model_code
                + "\",\"model_key\":\"" + info.model_key + "\"}");
        });

    // Update model
    api.put("/api/v1/device-models/{code}",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            std::string code = req.path.substr(std::string("/api/v1/device-models/").size());
            if (!type_mgr.model_exists(code))
                return ApiServer::error_response(404, -404, "Model not found: " + code);

            DeviceModelInfo info;
            info.model_code    = code;
            info.model_key     = JsonHelper::get_string(req.body, "model_key");
            info.type_code     = JsonHelper::get_string(req.body, "type_code");
            info.display_name  = JsonHelper::get_string(req.body, "display_name");
            info.description   = JsonHelper::get_string(req.body, "description");
            info.specs_json    = JsonHelper::get_string(req.body, "specs_json");
            info.firmware_base = JsonHelper::get_string(req.body, "firmware_base");
            info.is_active     = JsonHelper::get_bool(req.body, "is_active", true);
            info.sort_order    = JsonHelper::get_int(req.body, "sort_order", 0);

            StatusCode sc = type_mgr.update_model(code, info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));

            return ApiServer::json_response(200,
                "{\"success\":true,\"model_code\":\"" + code
                + "\",\"model_key\":\"" + info.model_key + "\"}");
        });

    // Delete model (soft)
    api.del("/api/v1/device-models/{code}",
        [&type_mgr](const HttpRequest& req) -> HttpResponse {
            std::string code = req.path.substr(std::string("/api/v1/device-models/").size());
            StatusCode sc = type_mgr.delete_model(code);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                "{\"success\":true,\"model_code\":\"" + code + "\",\"deleted\":true}");
        });

    // ================================================================
    // Orders API (Mock — frontend preparation)
    // ================================================================
    // List all orders
    api.get("/api/v1/orders",
        [&order_mgr, &recipe_mgr](const HttpRequest&) -> HttpResponse {
            auto orders = order_mgr.list_all_orders();
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("total":)" << orders.size() << R"(,"orders":[)";
            for (size_t i = 0; i < orders.size(); i++) {
                if (i > 0) json << ",";
                const auto& o = orders[i];
                // Resolve recipe_name
                std::string rname;
                auto recipe = recipe_mgr.find_by_id(o.recipe_id);
                if (recipe) rname = recipe->recipe_name;

                // created_at is Unix timestamp (BIGINT), use directly
                std::string ts = (o.created_at.empty() || o.created_at == "0")
                    ? std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count())
                    : o.created_at;

                json << "{"
                     << R"("order_id":")" << o.order_id << R"(",)";
                json << R"("tenant_id":")" << o.tenant_id << R"(",)";
                json << R"("device_id":")" << o.device_id << R"(",)";
                json << R"("recipe_id":")" << o.recipe_id << R"(",)";
                json << R"("recipe_name":")" << rname << R"(",)";
                json << R"("cup_size":")" << o.cup_size << R"(",)";
                json << R"("quantity":)" << o.quantity << ",";
                json << R"("total_amount":)" << o.total_amount << ",";
                json << R"("payment_method":")" << o.payment_method << R"(",)";
                json << R"("status":)" << static_cast<int>(o.status) << ",";
                json << R"("created_at":)" << ts;
                if (!o.failure_reason.empty()) {
                    json << R"(,"failure_reason":")" << o.failure_reason << R"(")";
                }
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single order
    api.get("/api/v1/orders/{id}",
        [&order_mgr](const HttpRequest& req) -> HttpResponse {
            std::string order_id = req.path.substr(std::string("/api/v1/orders/").size());
            auto o = order_mgr.get_order(order_id);
            if (!o) return ApiServer::error_response(404, 1002, "Order not found: " + order_id);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("order_id":")" << o->order_id << R"(",)";
            json << R"("tenant_id":")" << o->tenant_id << R"(",)";
            json << R"("device_id":")" << o->device_id << R"(",)";
            json << R"("recipe_id":")" << o->recipe_id << R"(",)";
            json << R"("cup_size":")" << o->cup_size << R"(",)";
            json << R"("quantity":)" << o->quantity << ",";
            json << R"("total_amount":)" << o->total_amount << ",";
            json << R"("payment_method":")" << o->payment_method << R"(",)";
            json << R"("status":)" << static_cast<int>(o->status) << ",";
            json << R"("created_at":)" << (o->created_at.empty() ? "0" : o->created_at);
            if (!o->failure_reason.empty()) {
                json << R"(,"failure_reason":")" << o->failure_reason << R"(")";
            }
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // Create order
    api.post("/api/v1/orders",
        [&order_mgr](const HttpRequest& req) -> HttpResponse {
            Order order;
            order.order_id      = JsonHelper::get_string(req.body, "order_id");
            order.tenant_id     = JsonHelper::get_string(req.body, "tenant_id");
            order.device_id     = JsonHelper::get_string(req.body, "device_id");
            order.recipe_id     = JsonHelper::get_string(req.body, "recipe_id");
            order.cup_size      = JsonHelper::get_string(req.body, "cup_size");
            order.quantity      = JsonHelper::get_int(req.body, "quantity", 1);
            order.total_amount  = JsonHelper::get_int(req.body, "total_amount", 0);
            order.payment_method= JsonHelper::get_string(req.body, "payment_method");
            if (order.order_id.empty() || order.device_id.empty()) {
                return ApiServer::error_response(400, 1001, "order_id and device_id are required");
            }
            if (order.tenant_id.empty()) order.tenant_id = "default";
            StatusCode sc = order_mgr.create_order(order);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(201,
                R"({"code":0,"message":"success","data":{"order_id":")" + order.order_id + R"("}})");
        });

    // Update order status
    api.put("/api/v1/orders/{id}",
        [&order_mgr](const HttpRequest& req) -> HttpResponse {
            std::string order_id = req.path.substr(std::string("/api/v1/orders/").size());
            std::string action = JsonHelper::get_string(req.body, "action");
            StatusCode sc;
            if (action == "confirm_payment") {
                sc = order_mgr.confirm_payment(order_id);
            } else if (action == "start_brewing") {
                sc = order_mgr.start_brewing(order_id);
            } else if (action == "complete") {
                sc = order_mgr.complete_brewing(order_id);
            } else if (action == "fail") {
                std::string reason = JsonHelper::get_string(req.body, "reason");
                sc = order_mgr.fail_brewing(order_id, reason);
            } else if (action == "cancel") {
                sc = order_mgr.cancel_order(order_id);
            } else {
                return ApiServer::error_response(400, 1001,
                    "Invalid action. Valid: confirm_payment, start_brewing, complete, fail, cancel");
            }
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"order_id":")" + order_id
                + R"(","action":")" + action + R"("}})");
        });

    // Cancel order (DELETE)
    api.del("/api/v1/orders/{id}",
        [&order_mgr](const HttpRequest& req) -> HttpResponse {
            std::string order_id = req.path.substr(std::string("/api/v1/orders/").size());
            StatusCode sc = order_mgr.cancel_order(order_id);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"order_id":")" + order_id
                + R"(","cancelled":true}})");
        });

    // ================================================================
    // Recipes API (Mock — frontend preparation)
    // ================================================================
    // List all recipes
    api.get("/api/v1/recipes",
        [&recipe_mgr](const HttpRequest&) -> HttpResponse {
            auto recipes = recipe_mgr.all_recipes();
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << recipes.size()
                 << R"(,"recipes":[)";
            for (size_t i = 0; i < recipes.size(); i++) {
                if (i > 0) json << ",";
                const auto& r = recipes[i];
                json << "{"
                     << R"("recipe_id":")" << r.recipe_id << R"(",)";
                json << R"("recipe_name":")" << r.recipe_name << R"(",)";
                json << R"("device_type":)" << static_cast<int>(r.device_type) << ",";
                json << R"("category":")" << r.category << R"(",)";
                json << R"("is_active":)" << (r.is_active ? "true" : "false") << ",";
                json << R"("version":)" << r.version << ",";
                // Steps
                json << R"("steps":[)";
                for (size_t s = 0; s < r.steps.size(); s++) {
                    if (s > 0) json << ",";
                    json << R"({"action":")" << r.steps[s].action << R"(")";
                    json << R"(,"duration_ms":)" << r.steps[s].duration_ms;
                    if (!r.steps[s].params.empty()) {
                        json << R"(,"params":{)";
                        bool first_p = true;
                        for (const auto& [k, v] : r.steps[s].params) {
                            if (!first_p) json << ",";
                            first_p = false;
                            json << R"(")" << k << R"(":)" << v;
                        }
                        json << "}";
                    }
                    json << "}";
                }
                json << "],";
                // Cup sizes
                json << R"("cup_sizes":[)";
                for (size_t c = 0; c < r.cup_sizes.size(); c++) {
                    if (c > 0) json << ",";
                    json << R"({"size":")" << r.cup_sizes[c].size << R"(")";
                    json << R"(,"price":)" << r.cup_sizes[c].price;
                    json << R"(,"volume_ml":)" << r.cup_sizes[c].volume_ml;
                    json << "}";
                }
                json << "]}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single recipe
    api.get("/api/v1/recipes/{id}",
        [&recipe_mgr](const HttpRequest& req) -> HttpResponse {
            std::string recipe_id = req.path.substr(std::string("/api/v1/recipes/").size());
            auto r = recipe_mgr.find_by_id(recipe_id);
            if (!r) return ApiServer::error_response(404, 1002, "Recipe not found: " + recipe_id);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("recipe_id":")" << r->recipe_id << R"(",)";
            json << R"("recipe_name":")" << r->recipe_name << R"(",)";
            json << R"("device_type":)" << static_cast<int>(r->device_type) << ",";
            json << R"("category":")" << r->category << R"(",)";
            json << R"("is_active":)" << (r->is_active ? "true" : "false") << ",";
            json << R"("version":)" << r->version << ",";
            json << R"("steps":[)";
            for (size_t s = 0; s < r->steps.size(); s++) {
                if (s > 0) json << ",";
                json << R"({"action":")" << r->steps[s].action << R"(")";
                json << R"(,"duration_ms":)" << r->steps[s].duration_ms;
                if (!r->steps[s].params.empty()) {
                    json << R"(,"params":{)";
                    bool first_p = true;
                    for (const auto& [k, v] : r->steps[s].params) {
                        if (!first_p) json << ",";
                        first_p = false;
                        json << R"(")" << k << R"(":)" << v;
                    }
                    json << "}";
                }
                json << "}";
            }
            json << R"(],"cup_sizes":[)";
            for (size_t c = 0; c < r->cup_sizes.size(); c++) {
                if (c > 0) json << ",";
                json << R"({"size":")" << r->cup_sizes[c].size << R"(")";
                json << R"(,"price":)" << r->cup_sizes[c].price;
                json << R"(,"volume_ml":)" << r->cup_sizes[c].volume_ml;
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Create recipe
    api.post("/api/v1/recipes",
        [&recipe_mgr](const HttpRequest& req) -> HttpResponse {
            Recipe recipe;
            recipe.recipe_id   = JsonHelper::get_string(req.body, "recipe_id");
            recipe.recipe_name = JsonHelper::get_string(req.body, "recipe_name");
            recipe.device_type = static_cast<DeviceType>(
                JsonHelper::get_int(req.body, "device_type", static_cast<int>(DeviceType::OTHER)));
            recipe.category    = JsonHelper::get_string(req.body, "category");
            recipe.is_active   = JsonHelper::get_bool(req.body, "is_active", true);
            recipe.version     = JsonHelper::get_int(req.body, "version", 1);
            if (recipe.recipe_id.empty() || recipe.recipe_name.empty()) {
                return ApiServer::error_response(400, 1001, "recipe_id and recipe_name are required");
            }
            StatusCode sc = recipe_mgr.create_recipe(recipe);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(201,
                R"({"code":0,"message":"success","data":{"recipe_id":")" + recipe.recipe_id + R"("}})");
        });

    // Update recipe
    api.put("/api/v1/recipes/{id}",
        [&recipe_mgr](const HttpRequest& req) -> HttpResponse {
            std::string recipe_id = req.path.substr(std::string("/api/v1/recipes/").size());
            auto existing = recipe_mgr.find_by_id(recipe_id);
            if (!existing) return ApiServer::error_response(404, 1002, "Recipe not found: " + recipe_id);

            Recipe updated = *existing;
            std::string name = JsonHelper::get_string(req.body, "recipe_name");
            if (!name.empty()) updated.recipe_name = name;
            std::string category = JsonHelper::get_string(req.body, "category");
            if (!category.empty()) updated.category = category;
            int dt = JsonHelper::get_int(req.body, "device_type", -1);
            if (dt >= 0) updated.device_type = static_cast<DeviceType>(dt);
            updated.is_active = JsonHelper::get_bool(req.body, "is_active", updated.is_active);

            StatusCode sc = recipe_mgr.update_recipe(updated);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"recipe_id":")" + recipe_id + R"("}})");
        });

    // Delete recipe
    api.del("/api/v1/recipes/{id}",
        [&recipe_mgr](const HttpRequest& req) -> HttpResponse {
            std::string recipe_id = req.path.substr(std::string("/api/v1/recipes/").size());
            StatusCode sc = recipe_mgr.remove_recipe(recipe_id);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"recipe_id":")" + recipe_id + R"(","deleted":true}})");
        });

    // ================================================================
    // OTA API (Mock — frontend preparation)
    // ================================================================
    // List firmwares — scans local ./firmware/ directory for real-time info
    api.get("/api/v1/ota/firmwares",
        [](const HttpRequest&) -> HttpResponse {
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)";

            std::string fw_root = "./firmware";
            std::error_code ec;

            // First pass: count total and build items
            std::vector<std::string> items;
            if (std::filesystem::exists(fw_root, ec)) {
                for (const auto& prod_entry : std::filesystem::directory_iterator(fw_root, ec)) {
                    if (!prod_entry.is_directory(ec)) continue;
                    std::string product_id = prod_entry.path().filename().string();

                    for (const auto& fw_entry : std::filesystem::directory_iterator(prod_entry.path(), ec)) {
                        if (!fw_entry.is_regular_file(ec)) continue;
                        std::string filename = fw_entry.path().filename().string();
                        int64_t fsize = fw_entry.file_size(ec);
                        std::string fw_path = fw_entry.path().string();

                        std::string checksum;
#ifdef HAS_OPENSSL
                        std::ifstream fin(fw_path, std::ios::binary);
                        if (fin.is_open()) {
                            SHA256_CTX ctx; SHA256_Init(&ctx);
                            char buf[65536];
                            while (fin.read(buf, sizeof(buf)) || fin.gcount() > 0)
                                SHA256_Update(&ctx, buf, fin.gcount());
                            unsigned char hash[SHA256_DIGEST_LENGTH];
                            SHA256_Final(hash, &ctx);
                            std::ostringstream hex;
                            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
                                hex << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
                            checksum = hex.str();
                        }
#else
                        checksum = "size:" + std::to_string(fsize);
#endif
                        // File modification time as ISO 8601 (via stat for correct epoch)
                        struct stat st;
                        time_t tt = (::stat(fw_path.c_str(), &st) == 0)
                                    ? st.st_mtime : 0;
                        std::ostringstream ts;
                        ts << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");;

                        std::ostringstream item;
                        item << "{"
                             << R"("version":")" << filename << R"(",)";
                        item << R"("product_id":")" << product_id << R"(",)";
                        item << R"("file_name":")" << filename << R"(",)";
                        item << R"("file_size":)" << fsize << ",";
                        item << R"("created_at":")" << ts.str() << R"(",)";
                        item << R"("download_url":"http://127.0.0.1:9081/api/v1/ota/firmwares/download/)"
                             << product_id << "/" << filename << R"(",)";
                        item << R"("checksum_sha256":")" << checksum << R"(")";
                        item << "}";
                        items.push_back(item.str());
                    }
                }
            }

            json << items.size() << R"(,"firmwares":[)";
            for (size_t i = 0; i < items.size(); i++) {
                if (i > 0) json << ",";
                json << items[i];
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get latest firmware version for a product
    // GET /api/v1/ota/firmwares/latest?product_id=coffee_v1
    // Returns highest semantic version (e.g., v2.1.0 > v1.5.0).
    // Falls back to newest file by mtime if version can't be parsed.
    api.get("/api/v1/ota/firmwares/latest",
        [](const HttpRequest& req) -> HttpResponse {
            auto qp = [&req](const std::string& k) -> std::string {
                std::string s = k + "=";
                size_t p = req.query.find(s);
                if (p == std::string::npos) return "";
                p += s.size();
                size_t e = req.query.find('&', p);
                if (e == std::string::npos) e = req.query.size();
                return req.query.substr(p, e - p);
            };
            std::string product_id = qp("product_id");
            if (product_id.empty())
                return ApiServer::error_response(400, 1001, "product_id required");

            std::string fw_dir = "./firmware/" + product_id;
            std::error_code ec;
            if (!std::filesystem::exists(fw_dir, ec))
                return ApiServer::error_response(404, 1002, "No firmware for product: " + product_id);

            // Parse semantic version from filename: extracts X.Y.Z
            // Matches patterns like "v2.1.0", "firmware_1.5.3", "2.0.0-beta"
            auto parse_version = [](const std::string& fname) -> std::tuple<int,int,int> {
                std::regex re(R"((\d+)\.(\d+)\.(\d+))");
                std::smatch m;
                if (std::regex_search(fname, m, re) && m.size() >= 4)
                    return {std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3])};
                return {0, 0, 0}; // unparseable — fallback to mtime
            };

            std::string best_file, best_path;
            time_t best_mtime = 0;
            int best_major = -1, best_minor = 0, best_patch = 0;
            int64_t best_size = 0;

            for (const auto& entry : std::filesystem::directory_iterator(fw_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                std::string fname = entry.path().filename().string();
                std::string fpath = entry.path().string();
                struct stat st;
                if (::stat(fpath.c_str(), &st) != 0) continue;

                auto [maj, min, pat] = parse_version(fname);
                bool is_newer = false;
                if (maj > 0 || min > 0 || pat > 0) {
                    // Versioned file: compare by semver, tie-break by mtime
                    if (maj > best_major) is_newer = true;
                    else if (maj == best_major && min > best_minor) is_newer = true;
                    else if (maj == best_major && min == best_minor && pat > best_patch) is_newer = true;
                    else if (maj == best_major && min == best_minor && pat == best_patch
                             && st.st_mtime > best_mtime) is_newer = true;
                } else if (best_major < 1) {
                    // Both unversioned: compare by mtime only
                    if (st.st_mtime > best_mtime) is_newer = true;
                }
                // If current is unversioned but best is versioned, skip

                if (is_newer) {
                    best_major = maj; best_minor = min; best_patch = pat;
                    best_mtime = st.st_mtime;
                    best_file = fname; best_path = fpath; best_size = st.st_size;
                }
            }
            if (best_file.empty())
                return ApiServer::error_response(404, 1002, "No firmware for product: " + product_id);

            // SHA256
            std::string checksum;
#ifdef HAS_OPENSSL
            std::ifstream fin(best_path, std::ios::binary);
            SHA256_CTX ctx; SHA256_Init(&ctx);
            char buf[65536];
            while (fin.read(buf, sizeof(buf)) || fin.gcount() > 0)
                SHA256_Update(&ctx, buf, fin.gcount());
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256_Final(hash, &ctx);
            std::ostringstream hex;
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
                hex << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
            checksum = hex.str();
#else
            checksum = "size:" + std::to_string(best_size);
#endif
            std::ostringstream ts;
            ts << std::put_time(std::gmtime(&best_mtime), "%Y-%m-%dT%H:%M:%SZ");

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("version":")" << best_file << R"(",)";
            json << R"("product_id":")" << product_id << R"(",)";
            json << R"("file_name":")" << best_file << R"(",)";
            json << R"("file_size":)" << best_size << ",";
            json << R"("version_major":)" << best_major << ",";
            json << R"("version_minor":)" << best_minor << ",";
            json << R"("version_patch":)" << best_patch << ",";
            json << R"("created_at":")" << ts.str() << R"(",)";
            json << R"("download_url":"http://127.0.0.1:9081/api/v1/ota/firmwares/download/)"
                 << product_id << "/" << best_file << R"(",)";
            json << R"("checksum_sha256":")" << checksum << R"(")";
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single firmware — looks up file on disk by filename in all product dirs
    api.get("/api/v1/ota/firmwares/{filename}",
        [](const HttpRequest& req) -> HttpResponse {
            std::string target = req.path.substr(std::string("/api/v1/ota/firmwares/").size());
            // Also support ?product_id=X to narrow search
            std::string filter_product;
            size_t qp = req.query.find("product_id=");
            if (qp != std::string::npos) {
                filter_product = req.query.substr(qp + 11);
                size_t ea = filter_product.find('&');
                if (ea != std::string::npos) filter_product = filter_product.substr(0, ea);
            }

            std::error_code ec;
            for (const auto& prod_entry : std::filesystem::directory_iterator("./firmware", ec)) {
                if (!prod_entry.is_directory(ec)) continue;
                std::string product_id = prod_entry.path().filename().string();
                if (!filter_product.empty() && product_id != filter_product) continue;

                std::string fw_path = prod_entry.path().string() + "/" + target;
                if (std::filesystem::exists(fw_path, ec)) {
                    int64_t fsize = std::filesystem::file_size(fw_path, ec);
                    struct stat st;
                    time_t tt = (::stat(fw_path.c_str(), &st) == 0) ? st.st_mtime : 0;
                    std::ostringstream ts;
                    ts << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");

                    // SHA256
                    std::string checksum;
#ifdef HAS_OPENSSL
                    std::ifstream fin(fw_path, std::ios::binary);
                    SHA256_CTX ctx; SHA256_Init(&ctx);
                    char buf[65536];
                    while (fin.read(buf, sizeof(buf)) || fin.gcount() > 0)
                        SHA256_Update(&ctx, buf, fin.gcount());
                    unsigned char hash[SHA256_DIGEST_LENGTH];
                    SHA256_Final(hash, &ctx);
                    std::ostringstream hex;
                    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
                        hex << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
                    checksum = hex.str();
#else
                    checksum = "size:" + std::to_string(fsize);
#endif
                    std::ostringstream json;
                    json << R"({"code":0,"message":"success","data":{)";
                    json << R"("version":")" << target << R"(",)";
                    json << R"("product_id":")" << product_id << R"(",)";
                    json << R"("file_name":")" << target << R"(",)";
                    json << R"("file_size":)" << fsize << ",";
                    json << R"("created_at":")" << ts.str() << R"(",)";
                    json << R"("download_url":"http://127.0.0.1:9081/api/v1/ota/firmwares/download/)"
                         << product_id << "/" << target << R"(",)";
                    json << R"("checksum_sha256":")" << checksum << R"(")";
                    json << "}}";
                    return ApiServer::json_response(200, json.str());
                }
            }
            return ApiServer::error_response(404, 1002, "Firmware not found: " + target);
        });


    // Register firmware
    api.post("/api/v1/ota/firmwares",
        [&ota_mgr](const HttpRequest& req) -> HttpResponse {
            FirmwareVersion fw;
            fw.version         = JsonHelper::get_string(req.body, "version");
            fw.product_id      = JsonHelper::get_string(req.body, "product_id");
            fw.download_url    = JsonHelper::get_string(req.body, "download_url");
            fw.file_name       = JsonHelper::get_string(req.body, "file_name");
            fw.file_size       = JsonHelper::get_int(req.body, "file_size", 0);
            fw.checksum_sha256 = JsonHelper::get_string(req.body, "checksum_sha256");
            fw.changelog       = JsonHelper::get_string(req.body, "changelog");
            fw.force_upgrade   = JsonHelper::get_bool(req.body, "force_upgrade", false);
            fw.created_at      = JsonHelper::get_int(req.body, "created_at", 0);
            if (fw.created_at == 0) {
                fw.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }
            if (fw.version.empty()) {
                return ApiServer::error_response(400, 1001, "version is required");
            }
            if (fw.download_url.empty()) {
                fw.download_url = "/api/v1/ota/firmwares/" + fw.version + "/download";
            }
            // Save firmware binary to disk if provided
            std::string file_data = JsonHelper::get_string(req.body, "file_data");
            if (!file_data.empty()) {
                // Use original filename, organized under product directory
                std::string filename = fw.file_name.empty() ? (fw.version + ".bin") : fw.file_name;
                std::string product = fw.product_id.empty() ? "default" : fw.product_id;
                std::string dir = "./firmware/" + product;
                mkdir("./firmware", 0755);
                mkdir(dir.c_str(), 0755);
                std::string fpath = dir + "/" + filename;
                std::string decoded;
                static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                int val = 0, valb = -8;
                for (char c : file_data) { if (c == '=') break; const char* p = strchr(b64, c); if (!p) continue; val = (val << 6) + (p - b64); valb += 6; if (valb >= 0) { decoded += char((val >> valb) & 0xFF); valb -= 8; } }
                std::ofstream out(fpath, std::ios::binary); out.write(decoded.data(), decoded.size()); out.close();
                if (fw.file_size == 0) fw.file_size = decoded.size();
            }
            StatusCode sc = ota_mgr.register_firmware(fw);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(201,
                R"({"code":0,"message":"success","data":{"version":")" + fw.version + R"("}})");
        });

    // Firmware binary upload — saves with original filename, organized by product
    // POST /api/v1/ota/firmwares/upload?version=X&product_id=Y&filename=Z
    api.post("/api/v1/ota/firmwares/upload",
        [&ota_mgr](const HttpRequest& req) -> HttpResponse {
            auto qp = [&req](const std::string& k) -> std::string {
                std::string s = k + "=";
                size_t p = req.query.find(s);
                if (p == std::string::npos) return "";
                p += s.size();
                size_t e = req.query.find('&', p);
                if (e == std::string::npos) e = req.query.size();
                return req.query.substr(p, e - p);
            };
            std::string version  = qp("version"),  product_id = qp("product_id"),
                        filename = qp("filename"), changelog  = qp("changelog");
            bool force = (qp("force_upgrade") == "1" || qp("force_upgrade") == "true");

            if (version.empty() || product_id.empty())
                return ApiServer::error_response(400, 1001, "version and product_id required");
            if (filename.empty()) filename = version + ".bin";

            // Ensure product directory exists
            std::string fw_dir = "./firmware/" + product_id;
            std::error_code ec;
            std::filesystem::create_directories(fw_dir, ec);

            // Save with original filename under product directory
            std::string fw_path = fw_dir + "/" + filename;
            std::ofstream out(fw_path, std::ios::binary);
            if (!out.is_open())
                return ApiServer::error_response(500, 2000, "Cannot write firmware file");
            out.write(req.body.data(), req.body.size());
            out.close();

            // SHA256 checksum
            std::string checksum;
#ifdef HAS_OPENSSL
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<const unsigned char*>(req.body.data()), req.body.size(), hash);
            std::ostringstream hex;
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
                hex << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
            checksum = hex.str();
#else
            checksum = "size:" + std::to_string(req.body.size());
#endif
            // Build download URL
            std::string host = "127.0.0.1:9080";
            auto hit = req.headers.find("Host");
            if (hit != req.headers.end()) host = hit->second;
            std::string dl_url = "http://" + host + "/api/v1/ota/firmwares/download/"
                               + product_id + "/" + filename;

            FirmwareVersion fw;
            fw.version = version; fw.product_id = product_id;
            fw.download_url = dl_url; fw.checksum_sha256 = checksum;
            fw.changelog = changelog; fw.force_upgrade = force;
            fw.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            StatusCode sc = ota_mgr.register_firmware(fw);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("version":")" << version << R"(",)";
            json << R"("filename":")" << filename << R"(",)";
            json << R"("product_id":")" << product_id << R"(",)";
            json << R"("size":)" << req.body.size() << ",";
            json << R"("checksum_sha256":")" << checksum << R"(",)";
            json << R"("download_url":")" << dl_url << R"(")";
            json << "}}";
            return ApiServer::json_response(201, json.str());
        });

    // Firmware binary download with Range support
    // GET /api/v1/ota/firmwares/download/{product_id}/{filename}
    api.get("/api/v1/ota/firmwares/download/{product_id}/{filename}",
        [](const HttpRequest& req) -> HttpResponse {
            std::string rest = req.path.substr(
                std::string("/api/v1/ota/firmwares/download/").size());
            size_t slash = rest.find('/');
            if (slash == std::string::npos)
                return ApiServer::error_response(404, 1002, "Invalid path");
            std::string product_id = rest.substr(0, slash);
            std::string filename   = rest.substr(slash + 1);

            std::string fw_path = "./firmware/" + product_id + "/" + filename;

            std::error_code ec;
            if (!std::filesystem::exists(fw_path, ec))
                return ApiServer::error_response(404, 1002,
                    "Firmware not found: " + product_id + "/" + filename);
            int64_t fsize = std::filesystem::file_size(fw_path, ec);

            std::ifstream in(fw_path, std::ios::binary);
            if (!in.is_open())
                return ApiServer::error_response(500, 2000, "Cannot read firmware");

            int64_t off = 0, end = fsize - 1;
            bool partial = false;
            auto it = req.headers.find("Range");
            if (it != req.headers.end() && it->second.find("bytes=") == 0) {
                std::string v = it->second.substr(6);
                size_t d = v.find('-');
                if (d != std::string::npos) {
                    off = std::stoll(v.substr(0, d));
                    std::string es = v.substr(d + 1);
                    end = es.empty() ? fsize - 1 : std::stoll(es);
                    partial = true;
                }
            }

            int64_t len = end - off + 1;
            in.seekg(off);
            std::string data(len, '\0');
            in.read(&data[0], len);

            HttpResponse resp;
            resp.status_code = partial ? 206 : 200;
            resp.content_type = "application/octet-stream";
            resp.body = std::move(data);
            return resp;
        });

    // Delete firmware (also remove file from disk)
    api.del("/api/v1/ota/firmwares/{version}",
        [&ota_mgr](const HttpRequest& req) -> HttpResponse {
            std::string ver = req.path.substr(std::string("/api/v1/ota/firmwares/").size());
            auto fw = ota_mgr.get_firmware(ver);
            StatusCode sc = ota_mgr.delete_firmware(ver);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            // Remove file from disk
            if (fw && !fw->file_name.empty()) {
                std::error_code ec;
                std::string product = fw->product_id.empty() ? "default" : fw->product_id;
                std::string fpath = "./firmware/" + product + "/" + fw->file_name;
                std::filesystem::remove(fpath, ec);
            }
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"version":")" + ver + R"(","deleted":true}})");
        });

    // List active upgrades
    api.get("/api/v1/ota/upgrades",
        [&ota_mgr](const HttpRequest&) -> HttpResponse {
            auto upgrades = ota_mgr.active_upgrades();
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << upgrades.size()
                 << R"(,"upgrades":[)";
            for (size_t i = 0; i < upgrades.size(); i++) {
                if (i > 0) json << ",";
                const auto& u = upgrades[i];
                json << "{"
                     << R"("tenant_id":")" << u.tenant_id << R"(",)";
                json << R"("device_id":")" << u.device_id << R"(",)";
                json << R"("current_version":")" << u.current_version << R"(",)";
                json << R"("target_version":")" << u.target_version << R"(",)";
                json << R"("progress":)" << u.progress << ",";
                json << R"("stage":")" << u.stage << R"(",)";
                json << R"("force_upgrade":)" << (u.force_upgrade ? "true" : "false");
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get device OTA status
    api.get("/api/v1/ota/upgrades/{device_id}",
        [&ota_mgr](const HttpRequest& req) -> HttpResponse {
            std::string device_id = req.path.substr(std::string("/api/v1/ota/upgrades/").size());
            auto u = ota_mgr.get_device_ota_status(device_id);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("tenant_id":")" << u.tenant_id << R"(",)";
            json << R"("device_id":")" << u.device_id << R"(",)";
            json << R"("current_version":")" << u.current_version << R"(",)";
            json << R"("target_version":")" << u.target_version << R"(",)";
            json << R"("progress":)" << u.progress << ",";
            json << R"("stage":")" << u.stage << R"(",)";
            json << R"("force_upgrade":)" << (u.force_upgrade ? "true" : "false");
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // Push OTA upgrade
    api.post("/api/v1/ota/push",
        [&ota_mgr](const HttpRequest& req) -> HttpResponse {
            std::string tenant_id  = JsonHelper::get_string(req.body, "tenant_id");
            std::string product_id = JsonHelper::get_string(req.body, "product_id");
            std::string device_id  = JsonHelper::get_string(req.body, "device_id");
            std::string version    = JsonHelper::get_string(req.body, "target_version");
            std::string mode       = JsonHelper::get_string(req.body, "mode");

            if (tenant_id.empty()) tenant_id = "default";
            StatusCode sc;
            std::string detail;

            if (mode == "all") {
                sc = ota_mgr.push_all(product_id, version);
                detail = "push_all to " + product_id;
            } else if (mode == "grayscale") {
                int percent = JsonHelper::get_int(req.body, "percent", 10);
                sc = ota_mgr.push_grayscale(product_id, version, percent);
                detail = "grayscale " + std::to_string(percent) + "% to " + product_id;
            } else {
                // single device
                if (device_id.empty())
                    return ApiServer::error_response(400, 1001,
                        "device_id is required for single-device push");
                sc = ota_mgr.push_to_device(tenant_id, product_id, device_id, version);
                detail = "push to " + device_id;
            }

            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"version":")" + version
                + R"(","detail":")" + detail + R"("}})");
        });

    // Rollout statistics
    api.get("/api/v1/ota/stats/{version}",
        [&ota_mgr](const HttpRequest& req) -> HttpResponse {
            std::string ver = req.path.substr(std::string("/api/v1/ota/stats/").size());
            auto stats = ota_mgr.get_rollout_stats(ver);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("version":")" << ver << R"(",)";
            json << R"("total_targeted":)" << stats.total_targeted << ",";
            json << R"("downloading":)" << stats.downloading << ",";
            json << R"("installing":)" << stats.installing << ",";
            json << R"("completed":)" << stats.completed << ",";
            json << R"("failed":)" << stats.failed << ",";
            json << R"("rolled_back":)" << stats.rolled_back;
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // ================================================================
    // Faults API (Mock — frontend preparation)
    // ================================================================
    // List all faults
    api.get("/api/v1/faults",
        [&fault_mgr](const HttpRequest&) -> HttpResponse {
            auto faults = fault_mgr.all_faults();
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << faults.size()
                 << R"(,"faults":[)";
            for (size_t i = 0; i < faults.size(); i++) {
                if (i > 0) json << ",";
                const auto& f = faults[i];
                json << "{"
                     << R"("tenant_id":")" << f.tenant_id << R"(",)";
                json << R"("device_id":")" << f.device_id << R"(",)";
                json << R"("code":)" << static_cast<int>(f.code) << ",";
                json << R"("level":)" << static_cast<int>(f.level) << ",";
                json << R"("description":")" << f.description << R"(",)";
                json << R"("ts":)" << ((f.timestamp.empty() || f.timestamp.find_first_not_of("0123456789") != std::string::npos) ? "0" : f.timestamp) << ",";
                json << R"("sensor_snapshot":)" << (f.sensor_snapshot.empty() ? "null" : f.sensor_snapshot);
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // List active faults only
    api.get("/api/v1/faults/active",
        [&fault_mgr](const HttpRequest&) -> HttpResponse {
            auto faults = fault_mgr.active_faults();
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << faults.size()
                 << R"(,"faults":[)";
            for (size_t i = 0; i < faults.size(); i++) {
                if (i > 0) json << ",";
                const auto& f = faults[i];
                json << "{"
                     << R"("tenant_id":")" << f.tenant_id << R"(",)";
                json << R"("device_id":")" << f.device_id << R"(",)";
                json << R"("code":)" << static_cast<int>(f.code) << ",";
                json << R"("level":)" << static_cast<int>(f.level) << ",";
                json << R"("description":")" << f.description << R"(",)";
                json << R"("ts":)" << ((f.timestamp.empty() || f.timestamp == "f" || f.timestamp.find_first_not_of("0123456789") != std::string::npos) ? "0" : f.timestamp);
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get faults by device
    api.get("/api/v1/faults/{device_id}",
        [&fault_mgr](const HttpRequest& req) -> HttpResponse {
            std::string device_id = req.path.substr(std::string("/api/v1/faults/").size());
            // Parse ?tenant_id= from path or use default
            std::string tenant_id = "default";
            size_t qpos = device_id.find("?tenant_id=");
            if (qpos != std::string::npos) {
                tenant_id = device_id.substr(qpos + 11);
                device_id = device_id.substr(0, qpos);
            }
            auto faults = fault_mgr.faults_by_device(tenant_id, device_id);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << faults.size()
                 << R"(,"faults":[)";
            for (size_t i = 0; i < faults.size(); i++) {
                if (i > 0) json << ",";
                const auto& f = faults[i];
                json << "{"
                     << R"("code":)" << static_cast<int>(f.code) << ","
                     << R"("level":)" << static_cast<int>(f.level) << ","
                     << R"("description":")" << f.description << R"(",)";
                json << R"("ts":)" << ((f.timestamp.empty() || f.timestamp == "f" || f.timestamp.find_first_not_of("0123456789") != std::string::npos) ? "0" : f.timestamp);
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Resolve fault
    api.put("/api/v1/faults/resolve",
        [&fault_mgr](const HttpRequest& req) -> HttpResponse {
            std::string tenant_id = JsonHelper::get_string(req.body, "tenant_id");
            std::string device_id = JsonHelper::get_string(req.body, "device_id");
            int code_int = JsonHelper::get_int(req.body, "code", 0);
            if (tenant_id.empty()) tenant_id = "default";
            if (device_id.empty() || code_int == 0)
                return ApiServer::error_response(400, 1001, "tenant_id, device_id, and code are required");
            StatusCode sc = fault_mgr.resolve_fault(tenant_id, device_id,
                                                     static_cast<FaultCode>(code_int));
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"resolved":true}})");
        });

    // ================================================================
    // Config & Tenant API
    // ================================================================
    // List tenants — uses OrgManager for real org data + DeviceManager for counts
    api.get("/api/v1/tenants",
        [&org_mgr, &device_mgr, &acct_mgr](const HttpRequest& req) -> HttpResponse {
            auto orgs = org_mgr.list_orgs(-1, "", true);

            // Apply org_scope filter if valid token provided
            auto tp = extract_auth(req, acct_mgr);
            if (tp && tp->role_code != "super_admin") {
                std::set<int32_t> scope_set(tp->org_scope.begin(), tp->org_scope.end());
                orgs.erase(std::remove_if(orgs.begin(), orgs.end(),
                    [&scope_set](const OrgInfo& o) { return !scope_set.count(o.org_id); }),
                    orgs.end());
            }

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << orgs.size()
                 << R"(,"tenants":[)";
            for (size_t i = 0; i < orgs.size(); i++) {
                if (i > 0) json << ",";
                json << "{"
                     << R"("org_id":)" << orgs[i].org_id << ","
                     << R"("tenant_id":")" << orgs[i].tenant_id << R"(",)";
                json << R"("org_name":")" << orgs[i].org_name << R"(",)";
                json << R"("org_type":")" << orgs[i].org_type << R"(",)";
                json << R"("parent_id":)" << orgs[i].parent_id << ","
                     << R"("level":)" << orgs[i].level << ","
                     << R"("path":")" << orgs[i].path << R"(",)";
                json << R"("active":)" << (orgs[i].is_active ? "true" : "false") << ",";
                json << R"("device_count":)" << device_mgr.device_count(orgs[i].tenant_id);
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get tenant detail — uses OrgManager + DeviceManager
    api.get("/api/v1/tenants/{tenant_id}",
        [&org_mgr, &device_mgr, &db](const HttpRequest& req) -> HttpResponse {
            std::string tenant_id = req.path.substr(std::string("/api/v1/tenants/").size());
            if (tenant_id.find('/') != std::string::npos) {
                return ApiServer::error_response(404, 1002, "Not found: " + req.path);
            }
            auto org = org_mgr.get_org_by_tenant(tenant_id);
            int count = device_mgr.device_count(tenant_id);
            int online = device_mgr.list_devices(tenant_id).size() -
                device_mgr.list_offline_devices().size();
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("tenant_id":")" << tenant_id << R"(",)";
            if (org) {
                json << R"("org_id":)" << org->org_id << ","
                     << R"("org_name":")" << org->org_name << R"(",)";
                json << R"("org_type":")" << org->org_type << R"(",)";
                json << R"("parent_id":)" << org->parent_id << ","
                     << R"("level":)" << org->level << ","
                     << R"("path":")" << org->path << R"(",)";
                json << R"("contact_name":")" << org->contact_name << R"(",)";
                json << R"("contact_phone":")" << org->contact_phone << R"(",)";
                json << R"("contact_email":")" << org->contact_email << R"(",)";
                json << R"("address":")" << org->address << R"(",)";
                json << R"("is_active":)" << (org->is_active ? "true" : "false") << ",";
            } else {
                json << R"("org_id":0,"org_name":"","org_type":"")";
                json << R"(,"is_active":true,)";
            }
            json << R"("device_count":)" << count << ",";
            json << R"("online_count":)" << (online > 0 ? online : 0);
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // List devices in tenant
    api.get("/api/v1/tenants/{tenant_id}/devices",
        [&device_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/tenants/";
            std::string suffix = "/devices";
            if (path.size() < suffix.size()
                || path.substr(path.size() - suffix.size()) != suffix) {
                return ApiServer::error_response(404, 1002, "Not found: " + path);
            }
            std::string tenant_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
            auto devices = device_mgr.list_devices(tenant_id);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << devices.size()
                 << R"(,"devices":[)";
            for (size_t i = 0; i < devices.size(); i++) {
                if (i > 0) json << ",";
                const auto& d = devices[i];
                json << "{"
                     << R"("device_id":")" << d.device_id << R"(",)";
                json << R"("product_id":")" << d.product_id << R"(",)";
                json << R"("type":)" << static_cast<int>(d.type) << ",";
                json << R"("network_status":)" << static_cast<int>(d.network_status) << ",";
                json << R"("work_status":)" << static_cast<int>(d.work_status) << ",";
                json << R"("firmware_version":")" << d.firmware_version << R"(",)";
                json << R"("model":")" << d.model << R"(",)";
                json << R"("ts":)" << d.last_heartbeat_at;
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get service config
    api.get("/api/v1/config",
        [mqtt_broker_uri, mqtt_broker_api](const HttpRequest&) -> HttpResponse {
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("service_name":"dev-sys-cloud",)";
            json << R"("version":"1.0.0",)";
            json << R"("mqtt_broker_uri":")" << *mqtt_broker_uri << R"(",)";
    json << R"("mqtt_broker_api":")" << *mqtt_broker_api << R"(",)";
            json << R"("mqtt_client_id":"cloud-platform-service-001",)";
            json << R"("heartbeat_timeout_sec":180,)";
            json << R"("offline_check_interval_sec":30,)";
            json << R"("order_expire_minutes":15,)";
            json << R"("max_queue_depth":10,)";
            json << R"("api_port":8080,)";
            json << R"("log_level":"INFO",)";
            json << R"("db_conn":"postgresql://devsys@127.0.0.1:5432/devsys_cloud")";
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // Update service config (mock)
    api.put("/api/v1/config",
        [](const HttpRequest& req) -> HttpResponse {
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("updated":true,)";
            json << R"("note":"Configuration update mocked. Restart required for some changes.")";
            json << "}}";
            (void)req; // mock: accept but don't actually change
            return ApiServer::json_response(200, json.str());
        });

    // ---- MQTT Tenant Key management (stored in database) ----
    auto load_tenant_key = [&db]() -> std::string { return db.load_tenant_config("tenant_key"); };
    auto save_tenant_key = [&db](const std::string& k) { db.save_tenant_config(k, db.load_tenant_config("tenant_name")); };
    auto load_tenant_name = [&db]() -> std::string { return db.load_tenant_config("tenant_name"); };
    auto save_tenant_name = [&db](const std::string& n) { db.save_tenant_config(db.load_tenant_config("tenant_key"), n); };

    // Helper: verify tenant key via MQTT broker API, falls back to local DB
    auto verify_tenant_via_broker = [mqtt_broker_api, &db](const std::string& key) -> std::pair<bool, std::string> {
        std::string api_url = *mqtt_broker_api;
        if (!api_url.empty() && !key.empty()) {
            std::string host = "127.0.0.1"; int port = 8080;
            std::string url(api_url); if (url.find("http://") == 0) url = url.substr(7);
            size_t c = url.find(':'); size_t s = url.find('/');
            host = (c != std::string::npos) ? url.substr(0, c) : url.substr(0, s);
            if (c != std::string::npos) port = std::stoi(url.substr(c + 1, s - c - 1));

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                timeval tv{2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                addrinfo h{}, *res; h.ai_family = AF_INET; h.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &h, &res) == 0) {
                    sockaddr_in a{}; memcpy(&a, res->ai_addr, sizeof(a)); freeaddrinfo(res);
                    if (connect(fd, (sockaddr*)&a, sizeof(a)) >= 0) {
                        std::ostringstream rs;
                        rs << "GET /api/v1/tenant/info?key=" << key << " HTTP/1.1\r\n"
                           << "Host: " << host << ":" << port << "\r\nConnection: close\r\n\r\n";
                        std::string rss = rs.str(); send(fd, rss.c_str(), rss.size(), 0);
                        char buf[4096]; auto n = recv(fd, buf, sizeof(buf) - 1, 0);
                        if (n > 0) {
                            buf[n] = 0; std::string raw(buf, n);
                            size_t bs = raw.find("\r\n\r\n");
                            if (bs != std::string::npos) {
                                std::string body = raw.substr(bs + 4);
                                std::string tname = JsonHelper::get_string(body, "name");
                                if (tname.empty()) tname = JsonHelper::get_string(body, "tenant_id");
                                if (!tname.empty()) { close(fd); return {true, tname}; }
                            }
                        }
                    }
                }
                close(fd);
            }
        }

        // Fallback: query local database tenants table
        if (!key.empty()) {
            std::ostringstream sql;
            sql << "SELECT name FROM tenants WHERE tenant_id='"
                << key << "' AND active=TRUE";
            std::vector<std::vector<std::string>> rows;
            if (db.query(sql.str(), rows) == StatusCode::OK && !rows.empty() && !rows[0].empty()) {
                return {true, rows[0][0]};
            }
        }

        return {false, ""};
    };

    api.get("/api/v1/mqtt-tenant-key",
        [&load_tenant_key, &load_tenant_name](const HttpRequest&) -> HttpResponse {
            std::string k = load_tenant_key();
            std::string n = load_tenant_name();
            std::ostringstream j;
            j << R"({"code":0,"message":"success","data":{)";
            j << R"("tenant_key":)" << (k.empty() ? "null" : "\"" + k + "\"") << ",";
            j << R"("tenant_name":)" << (n.empty() ? "null" : "\"" + n + "\"");
            j << "}}";
            return ApiServer::json_response(200, j.str());
        });

    api.put("/api/v1/mqtt-tenant-key",
        [&save_tenant_key, &save_tenant_name, &verify_tenant_via_broker](const HttpRequest& req) -> HttpResponse {
            std::string k = JsonHelper::get_string(req.body, "tenant_key");
            if (k.empty()) return ApiServer::error_response(400, 1001, "tenant_key required");

            // Verify key via MQTT broker before saving
            auto [valid, tname] = verify_tenant_via_broker(k);
            if (!valid) {
                return ApiServer::json_response(200,
                    R"({"code":1001,"message":"无效的租户Key，未找到对应租户","data":null})");
            }

            save_tenant_key(k);
            save_tenant_name(tname);
            std::ostringstream j;
            j << R"({"code":0,"message":"success","data":{)";
            j << R"("tenant_key":")" << k << R"(",)";
            j << R"("tenant_name":")" << tname << R"(")";
            j << "}}";
            return ApiServer::json_response(200, j.str());
        });

    api.del("/api/v1/mqtt-tenant-key",
        [&save_tenant_key, &save_tenant_name](const HttpRequest&) -> HttpResponse {
            save_tenant_key("");
            save_tenant_name("");
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"deleted":true}})");
        });

    // Verify mqtt tenant key (frontend uses this path)
    api.get("/api/v1/mqtt-tenant-key/verify",
        [&verify_tenant_via_broker](const HttpRequest& req) -> HttpResponse {
            auto qp = [&req](const std::string& k) -> std::string {
                std::string s = k + "=";
                size_t p = req.query.find(s); if (p == std::string::npos) return "";
                p += s.size(); size_t e = req.query.find('&', p);
                if (e == std::string::npos) e = req.query.size();
                return req.query.substr(p, e - p);
            };
            std::string key = qp("key");
            if (key.empty())
                return ApiServer::json_response(200,
                    R"({"code":0,"message":"success","data":{"valid":false,"tenant_name":null}})");

            auto [valid, tname] = verify_tenant_via_broker(key);
            std::ostringstream j;
            j << R"({"code":0,"message":"success","data":{"valid":)" << (valid ? "true" : "false")
              << R"(,"tenant_name":)" << (tname.empty() ? "null" : "\"" + tname + "\"") << "}}";
            return ApiServer::json_response(200, j.str());
        });

    // Lookup tenant name by tenant_key (proxies to mqtt_broker)
    api.get("/api/v1/tenants/by-key",
        [mqtt_broker_api](const HttpRequest& req) -> HttpResponse {
            auto qp = [&req](const std::string& k) -> std::string {
                std::string s = k + "=";
                size_t p = req.query.find(s); if (p == std::string::npos) return "";
                p += s.size(); size_t e = req.query.find('&', p);
                if (e == std::string::npos) e = req.query.size();
                return req.query.substr(p, e - p);
            };
            std::string key = qp("key");
            if (key.empty())
                return ApiServer::error_response(400, 1001, "key is required");

            std::string api_url = *mqtt_broker_api;
            if (api_url.empty())
                return ApiServer::error_response(503, 2000, "MQTT broker API not configured");

            // Proxy to mqtt_broker: GET /api/v1/tenant/info?key=xxx
            std::string host = "127.0.0.1"; int port = 8080;
            std::string url(api_url); if (url.find("http://") == 0) url = url.substr(7);
            size_t c = url.find(':'); size_t s = url.find('/');
            host = (c != std::string::npos) ? url.substr(0, c) : url.substr(0, s);
            if (c != std::string::npos) port = std::stoi(url.substr(c + 1, s - c - 1));

            int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0)
                return ApiServer::error_response(500, 2000, "socket failed");
            timeval tv{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            addrinfo h{}, *res; h.ai_family = AF_INET; h.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &h, &res) != 0) {
                close(fd); return ApiServer::error_response(502, 2000, "mqtt_broker unreachable");
            }
            sockaddr_in a{}; memcpy(&a, res->ai_addr, sizeof(a)); freeaddrinfo(res);
            if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) {
                close(fd); return ApiServer::error_response(502, 2000, "mqtt_broker connect failed");
            }
            std::ostringstream rs; rs << "GET /api/v1/tenant/info?key=" << key << " HTTP/1.1\r\n"
               << "Host: " << host << ":" << port << "\r\nConnection: close\r\n\r\n";
            std::string rss = rs.str(); send(fd, rss.c_str(), rss.size(), 0);
            char buf[8192]; auto n = recv(fd, buf, sizeof(buf) - 1, 0); close(fd);
            if (n <= 0) return ApiServer::error_response(502, 2000, "mqtt_broker no response");
            buf[n] = 0; std::string raw(buf, n);
            size_t bs = raw.find("\r\n\r\n");
            if (bs == std::string::npos) return ApiServer::error_response(502, 2000, "bad response");
            std::string body = raw.substr(bs + 4);
            return ApiServer::json_response(200, body);
        });

    // ================================================================
    // Organization & Account API (REQ-ORG-003/004)
    // ================================================================
    // ---- Organization CRUD ----
    // List orgs
    api.get("/api/v1/orgs",
        [&org_mgr](const HttpRequest& req) -> HttpResponse {
            // Parse query params from path (simple extraction)
            int32_t parent_id = -1;
            std::string org_type;
            size_t qpos = req.path.find("?parent_id=");
            if (qpos != std::string::npos)
                parent_id = std::stoi(req.path.substr(qpos + 11));
            qpos = req.path.find("?org_type=");
            if (qpos != std::string::npos)
                org_type = req.path.substr(qpos + 10, req.path.find('&', qpos) - qpos - 10);

            auto orgs = org_mgr.list_orgs(parent_id, org_type, false);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << orgs.size()
                 << R"(,"list":[)";
            for (size_t i = 0; i < orgs.size(); i++) {
                if (i > 0) json << ",";
                const auto& o = orgs[i];
                json << "{"
                     << R"("org_id":)" << o.org_id << ","
                     << R"("parent_id":)" << o.parent_id << ","
                     << R"("tenant_id":")" << o.tenant_id << R"(",)";
                json << R"("org_name":")" << o.org_name << R"(",)";
                json << R"("org_type":")" << o.org_type << R"(",)";
                json << R"("level":)" << o.level << ","
                     << R"("path":")" << o.path << R"(",)";
                json << R"("children_count":)" << o.children_count << ","
                     << R"("is_active":)" << (o.is_active ? "true" : "false");
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get org tree
    api.get("/api/v1/orgs/tree",
        [&org_mgr](const HttpRequest&) -> HttpResponse {
            auto tree = org_mgr.get_org_tree();
            // Serialize tree recursively
            std::function<std::string(const OrgTreeNode&)> serialize;
            serialize = [&serialize](const OrgTreeNode& node) -> std::string {
                if (node.info.org_id == 0 && node.children.empty()) return "";
                std::ostringstream js;
                bool has_self = node.info.org_id != 0;
                if (has_self) {
                    js << "{"
                       << R"("org_id":)" << node.info.org_id << ","
                       << R"("org_name":")" << node.info.org_name << R"(",)";
                    js << R"("org_type":")" << node.info.org_type << R"(",)";
                    js << R"("tenant_id":")" << node.info.tenant_id << R"(",)";
                    js << R"("parent_id":)" << node.info.parent_id << ","
                       << R"("level":)" << node.info.level << ","
                       << R"("children":[)";
                }
                bool first = true;
                for (const auto& child : node.children) {
                    if (!first && has_self) js << ",";
                    first = false;
                    js << serialize(child);
                }
                if (has_self) js << "]}";
                return js.str();
            };

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"tree":[)";
            bool first = true;
            for (const auto& root : tree.children) {
                if (!first) json << ",";
                first = false;
                json << serialize(root);
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single org
    api.get("/api/v1/orgs/{id}",
        [&org_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/orgs/";
            int32_t org_id = std::stoi(path.substr(prefix.size()));
            auto o = org_mgr.get_org(org_id);
            if (!o) return ApiServer::error_response(404, 1002, "Org not found: " + std::to_string(org_id));
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("org_id":)" << o->org_id << ","
                 << R"("parent_id":)" << o->parent_id << ","
                 << R"("tenant_id":")" << o->tenant_id << R"(",)";
            json << R"("org_name":")" << o->org_name << R"(",)";
            json << R"("org_type":")" << o->org_type << R"(",)";
            json << R"("contact_name":")" << o->contact_name << R"(",)";
            json << R"("contact_phone":")" << o->contact_phone << R"(",)";
            json << R"("contact_email":")" << o->contact_email << R"(",)";
            json << R"("address":")" << o->address << R"(",)";
            json << R"("level":)" << o->level << ","
                 << R"("path":")" << o->path << R"(",)";
            json << R"("children_count":)" << o->children_count << ","
                 << R"("is_active":)" << (o->is_active ? "true" : "false") << ","
                 << R"("created_at":")" << o->created_at << R"(",)";
            json << R"("updated_at":")" << o->updated_at << R"(")";
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // Create org
    api.post("/api/v1/orgs",
        [&org_mgr](const HttpRequest& req) -> HttpResponse {
            OrgInfo info;
            info.parent_id     = JsonHelper::get_int(req.body, "parent_id", 0);
            info.tenant_id     = JsonHelper::get_string(req.body, "tenant_id");
            info.org_name      = JsonHelper::get_string(req.body, "org_name");
            info.org_type      = JsonHelper::get_string(req.body, "org_type");
            info.contact_name  = JsonHelper::get_string(req.body, "contact_name");
            info.contact_phone = JsonHelper::get_string(req.body, "contact_phone");
            info.contact_email = JsonHelper::get_string(req.body, "contact_email");
            info.address       = JsonHelper::get_string(req.body, "address");
            if (info.tenant_id.empty()) {
                // Auto-generate tenant_id from org_name
                info.tenant_id = info.org_name;
            }
            if (info.org_name.empty())
                return ApiServer::error_response(400, 1001, "org_name is required");
            StatusCode sc = org_mgr.create_org(info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(201,
                R"({"code":0,"message":"success","data":{"org_id":)" + std::to_string(info.org_id)
                + R"(,"tenant_id":")" + info.tenant_id + R"("}})");
        });

    // Update org
    api.put("/api/v1/orgs/{id}",
        [&org_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/orgs/";
            int32_t org_id = std::stoi(path.substr(prefix.size()));
            if (!org_mgr.org_exists(org_id))
                return ApiServer::error_response(404, 1002, "Org not found: " + std::to_string(org_id));
            OrgInfo info;
            info.org_name      = JsonHelper::get_string(req.body, "org_name");
            info.contact_name  = JsonHelper::get_string(req.body, "contact_name");
            info.contact_phone = JsonHelper::get_string(req.body, "contact_phone");
            info.contact_email = JsonHelper::get_string(req.body, "contact_email");
            info.address       = JsonHelper::get_string(req.body, "address");
            info.is_active     = JsonHelper::get_bool(req.body, "is_active", true);
            StatusCode sc = org_mgr.update_org(org_id, info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"org_id":)" + std::to_string(org_id) + R"(}})");
        });

    // Delete org
    api.del("/api/v1/orgs/{id}",
        [&org_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/orgs/";
            int32_t org_id = std::stoi(path.substr(prefix.size()));
            StatusCode sc = org_mgr.delete_org(org_id);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"org_id":)" + std::to_string(org_id)
                + R"(,"deleted":true}})");
        });

    // ---- Auth ----
    // Login
    api.post("/api/v1/auth/login",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            LoginRequest login_req;
            login_req.username = JsonHelper::get_string(req.body, "username");
            login_req.password = JsonHelper::get_string(req.body, "password");
            if (login_req.username.empty() || login_req.password.empty())
                return ApiServer::error_response(400, 1001, "username and password are required");

            LoginResponse resp = acct_mgr.login(login_req);
            if (!resp.success) {
                return ApiServer::error_response(401, resp.error_code, resp.error_message);
            }

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("token":")" << resp.token << R"(",)";
            json << R"("expires_at":")" << resp.expires_at << R"(",)";
            json << R"("account":{)";
            json << R"("account_id":)" << resp.account.account_id << ",";
            json << R"("username":")" << resp.account.username << R"(",)";
            json << R"("display_name":")" << resp.account.display_name << R"(",)";
            json << R"("org_id":)" << resp.account.org_id << ",";
            json << R"("org_name":")" << resp.account.org_name << R"(",)";
            json << R"("role_code":")" << resp.account.role_code << R"(",)";
            json << R"("permissions":[)";
            for (size_t i = 0; i < resp.permissions.size(); i++) {
                if (i > 0) json << ",";
                json << R"(")" << resp.permissions[i] << R"(")";
            }
            json << "]}}";
            json << "}";
            return ApiServer::json_response(200, json.str());
        });

    // Logout
    api.post("/api/v1/auth/logout",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            std::string token = JsonHelper::get_string(req.body, "token");
            // Also support Bearer from header
            if (token.empty()) {
                auto it = req.headers.find("Authorization");
                if (it != req.headers.end() && it->second.find("Bearer ") == 0)
                    token = it->second.substr(7);
            }
            acct_mgr.logout(token);
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"logout":true}})");
        });

    // Profile
    api.get("/api/v1/auth/profile",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            std::string token;
            auto it = req.headers.find("Authorization");
            if (it != req.headers.end() && it->second.find("Bearer ") == 0)
                token = it->second.substr(7);
            if (token.empty())
                return ApiServer::error_response(401, 1004, "Missing Authorization Bearer Token");

            auto tp = acct_mgr.verify_token(token);
            if (!tp) return ApiServer::error_response(401, 1004, "Invalid or expired token");

            auto acct = acct_mgr.get_account(tp->account_id);
            if (!acct) return ApiServer::error_response(401, 1004, "Account not found");

            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("account_id":)" << acct->account_id << ",";
            json << R"("username":")" << acct->username << R"(",)";
            json << R"("display_name":")" << acct->display_name << R"(",)";
            json << R"("org_id":)" << acct->org_id << ",";
            json << R"("org_name":")" << acct->org_name << R"(",)";
            json << R"("role_code":")" << acct->role_code << R"(",)";
            json << R"("email":")" << acct->email << R"(",)";
            json << R"("phone":")" << acct->phone << R"(",)";
            json << R"("permissions":[)";
            auto perms = AccountManager::permissions_for_role(acct->role_code);
            for (size_t i = 0; i < perms.size(); i++) {
                if (i > 0) json << ",";
                json << R"(")" << perms[i] << R"(")";
            }
            json << "],";
            json << R"("org_scope":[)";
            for (size_t i = 0; i < tp->org_scope.size(); i++) {
                if (i > 0) json << ",";
                json << tp->org_scope[i];
            }
            json << "]}";
            json << "}";
            return ApiServer::json_response(200, json.str());
        });

    // ---- Account CRUD ----
    // List accounts
    api.get("/api/v1/accounts",
        [&acct_mgr](const HttpRequest&) -> HttpResponse {
            auto accounts = acct_mgr.list_accounts(-1, "", false);
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{"total":)" << accounts.size()
                 << R"(,"list":[)";
            for (size_t i = 0; i < accounts.size(); i++) {
                if (i > 0) json << ",";
                const auto& a = accounts[i];
                json << "{"
                     << R"("account_id":)" << a.account_id << ","
                     << R"("username":")" << a.username << R"(",)";
                json << R"("display_name":")" << a.display_name << R"(",)";
                json << R"("org_id":)" << a.org_id << ","
                     << R"("org_name":")" << a.org_name << R"(",)";
                json << R"("role_code":")" << a.role_code << R"(",)";
                json << R"("email":")" << a.email << R"(",)";
                json << R"("phone":")" << a.phone << R"(",)";
                json << R"("is_active":)" << (a.is_active ? "true" : "false");
                json << "}";
            }
            json << "]}}";
            return ApiServer::json_response(200, json.str());
        });

    // Get single account
    api.get("/api/v1/accounts/{id}",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/accounts/";
            int32_t account_id = std::stoi(path.substr(prefix.size()));
            auto a = acct_mgr.get_account(account_id);
            if (!a) return ApiServer::error_response(404, 1002, "Account not found: " + std::to_string(account_id));
            std::ostringstream json;
            json << R"({"code":0,"message":"success","data":{)";
            json << R"("account_id":)" << a->account_id << ","
                 << R"("username":")" << a->username << R"(",)";
            json << R"("display_name":")" << a->display_name << R"(",)";
            json << R"("org_id":)" << a->org_id << ","
                 << R"("org_name":")" << a->org_name << R"(",)";
            json << R"("role_code":")" << a->role_code << R"(",)";
            json << R"("email":")" << a->email << R"(",)";
            json << R"("phone":")" << a->phone << R"(",)";
            json << R"("is_active":)" << (a->is_active ? "true" : "false") << ","
                 << R"("created_at":")" << a->created_at << R"(",)";
            json << R"("last_login_at":")" << a->last_login_at << R"(")";
            json << "}}";
            return ApiServer::json_response(200, json.str());
        });

    // Create account
    api.post("/api/v1/accounts",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            AccountInfo info;
            info.username     = JsonHelper::get_string(req.body, "username");
            info.display_name = JsonHelper::get_string(req.body, "display_name");
            info.org_id       = JsonHelper::get_int(req.body, "org_id", 0);
            info.role_code    = JsonHelper::get_string(req.body, "role_code");
            info.email        = JsonHelper::get_string(req.body, "email");
            info.phone        = JsonHelper::get_string(req.body, "phone");
            std::string password = JsonHelper::get_string(req.body, "password");
            if (info.username.empty() || password.empty() || info.role_code.empty())
                return ApiServer::error_response(400, 1001, "username, password, and role_code are required");
            StatusCode sc = acct_mgr.create_account(info, password);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(201,
                R"({"code":0,"message":"success","data":{"account_id":)"
                + std::to_string(info.account_id)
                + R"(,"username":")" + info.username + R"("}})");
        });

    // Change password (registered BEFORE /accounts/{id} to avoid prefix conflict)
    api.put("/api/v1/accounts/{id}/password",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/accounts/";
            std::string suffix = "/password";
            // Guard: only match if path actually ends with /password
            if (path.size() < suffix.size()
                || path.substr(path.size() - suffix.size()) != suffix) {
                return ApiServer::error_response(404, 1002, "Not found: " + path);
            }
            std::string id_str = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
            int32_t account_id = std::stoi(id_str);
            std::string old_pwd = JsonHelper::get_string(req.body, "old_password");
            std::string new_pwd = JsonHelper::get_string(req.body, "new_password");
            if (old_pwd.empty() || new_pwd.empty())
                return ApiServer::error_response(400, 1001, "old_password and new_password are required");
            StatusCode sc = acct_mgr.change_password(account_id, old_pwd, new_pwd);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"updated":true}})");
        });

    // Update account (registered AFTER /password route)
    api.put("/api/v1/accounts/{id}",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/accounts/";
            std::string id_str = path.substr(prefix.size());
            // Guard: reject if path contains extra segments (like /password)
            if (id_str.find('/') != std::string::npos) {
                return ApiServer::error_response(404, 1002, "Not found: " + path);
            }
            int32_t account_id = std::stoi(id_str);
            AccountInfo info;
            info.display_name = JsonHelper::get_string(req.body, "display_name");
            info.email        = JsonHelper::get_string(req.body, "email");
            info.phone        = JsonHelper::get_string(req.body, "phone");
            info.is_active    = JsonHelper::get_bool(req.body, "is_active", true);
            StatusCode sc = acct_mgr.update_account(account_id, info);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"account_id":)" + std::to_string(account_id) + R"(}})");
        });

    // Delete account
    api.del("/api/v1/accounts/{id}",
        [&acct_mgr](const HttpRequest& req) -> HttpResponse {
            std::string path = req.path;
            std::string prefix = "/api/v1/accounts/";
            int32_t account_id = std::stoi(path.substr(prefix.size()));
            StatusCode sc = acct_mgr.delete_account(account_id);
            if (sc != StatusCode::OK)
                return ApiServer::error_response(400, static_cast<int>(sc), status_message(sc));
            return ApiServer::json_response(200,
                R"({"code":0,"message":"success","data":{"account_id":)"
                + std::to_string(account_id) + R"(,"deleted":true}})");
        });

    int api_port = 9080;
    const char* env_port = std::getenv("API_PORT");
    if (env_port) api_port = std::stoi(env_port);
    char api_base[64];
    snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d", api_port);
    if (api.start(api_port) != StatusCode::OK) {
        std::cerr << "[Main] API server start failed on port " << api_port << std::endl;
        return 1;
    }

    // ======== Phase 3: Message Router (核心：topic → handler) ========
    MessageRouter router;
    router.set_device_manager(&device_mgr);
    router.set_order_manager(&order_mgr);
    router.set_ota_manager(&ota_mgr);
    router.set_fault_manager(&fault_mgr);
    router.set_recipe_manager(&recipe_mgr);

    // ======== Phase 4: Connect to MQTT Broker ========
    MqttClient mqtt;

    // 云服务使用特权凭证，对broker有全局读写权限
    mqtt.set_message_callback(
        [&router](const std::string& topic, const std::string& payload) {
            router.on_message(topic, payload);
        });

    // Will message disabled (broker may require auth for retained/Will)
    // mqtt.set_will_message("cloud-platform/status", R"({"status":"offline"})", 1);

    ServiceConfig svc_config;
    svc_config.mqtt_broker_uri = *mqtt_broker_uri;
    svc_config.mqtt_client_id  = "cloud-platform-service-001";

    if (mqtt.connect(svc_config.mqtt_broker_uri,
                      svc_config.mqtt_client_id,
                      "", "", "",   // cert paths — use TCP without TLS
                      mqtt_user, mqtt_pass) != StatusCode::OK) {
        std::cerr << "[Main] MQTT connection failed — HTTP API still available" << std::endl;
        *mqtt_connected = false;
    } else {
        *mqtt_connected = true;
    }

    if (mqtt.is_connected()) {
        log_mgr.info("main", "MQTT connected to " + svc_config.mqtt_broker_uri);
    } else {
        log_mgr.warn("main", "MQTT stub mode — broker not connected, running with simulated status");
    }

    // 订阅所有设备上行topic（通配符）
    auto topics = MessageRouter::subscription_topics();
    for (const auto& t : topics) {
        mqtt.subscribe(t, 1);
        log_mgr.info("main", "Subscribed: " + t);
    }
    // 额外订阅设备状态Topic（Will Message用）
    mqtt.subscribe("+/iot/+/+/status", 1);
    log_mgr.info("main", "Subscribed: +/iot/+/+/status");

    // 发布服务上线通知
    mqtt.publish("cloud-platform/status",
                  R"({"service":"dev-sys-cloud","status":"online"})", 1, true);

    log_mgr.info("main", "Cloud platform service started. "
                 "Watching " + std::to_string(device_mgr.total_device_count()) + " devices.");

    // ======== Phase 5: Main loop ========
    auto last_offline_check   = std::chrono::steady_clock::now();
    auto last_stats_report    = std::chrono::steady_clock::now();
    auto last_mqtt_status_sync = std::chrono::steady_clock::now();

    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // 离线检测 (30s间隔)
        auto oc_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_offline_check).count();
        if (oc_elapsed >= 30) {
            device_mgr.check_offline_devices();
            last_offline_check = now;
        }

        // MQTT连接状态同步 (5s间隔)
        auto ms_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_mqtt_status_sync).count();
        if (ms_elapsed >= 5) {
            bool connected = mqtt.is_connected();
            if (mqtt_connected->load() != connected) {
                mqtt_connected->store(connected);
                if (!connected) {
                    log_mgr.warn("main", "MQTT connection lost");
                } else {
                    log_mgr.info("main", "MQTT connection restored");
                }
            }
            last_mqtt_status_sync = now;
        }

        // 统计日志 (5分钟)
        auto sr_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats_report).count();
        if (sr_elapsed >= 300) {
            log_mgr.info("main", "Stats: " +
                         std::to_string(device_mgr.total_device_count()) + " devices, " +
                         std::to_string(device_mgr.list_tenants().size()) + " tenants");
            last_stats_report = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ======== Phase 6: Shutdown ========
    api.stop();
    mqtt.publish("cloud-platform/status",
                  R"({"service":"dev-sys-cloud","status":"offline"})", 1, true);
    mqtt.disconnect();
    db.close();
    log_mgr.info("main", "Cloud platform service stopped.");

    return 0;
}
