/**
 * device_simulator.cpp — 模拟IoT设备程序
 *
 * 用途：模拟一台真实设备，用于测试dev-sys-cloud云平台的所有API接口。
 * 通信方式：HTTP (REST API)，无需MQTT Broker或TLS证书。
 *
 * 模拟流程：
 *   1. 设备激活 (POST /api/v1/device/activate)
 *   2. 心跳保活 (POST /api/v1/device/heartbeat，周期发送)
 *   3. 属性上报 (POST /api/v1/device/properties)
 *   4. 事件上报 (POST /api/v1/device/events) — 订单/故障
 *   5. OTA检查与进度上报 (GET /api/v1/device/ota/check)
 *   6. 配方同步 (GET /api/v1/recipes/sync)
 *   7. 配置拉取 (GET /api/v1/device/config)
 *
 * 编译：
 *   g++ -std=c++17 -o device_simulator test/device_simulator.cpp
 *
 * 使用：
 *   ./device_simulator [--url http://127.0.0.1:8080] [--tenant demo] [--interval 10]
 */

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <random>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

// ============================================================
// 轻量级HTTP客户端（POSIX socket，零外部依赖）
// ============================================================
class SimHttpClient {
public:
    SimHttpClient(const std::string& base_url)
        : base_url_(base_url) {
        // Parse host:port from base_url
        std::string url = base_url;
        if (url.find("http://") == 0) url = url.substr(7);
        else if (url.find("https://") == 0) url = url.substr(8);
        size_t colon = url.find(':');
        size_t slash = url.find('/');
        if (colon != std::string::npos) {
            host_ = url.substr(0, colon);
            port_ = std::stoi(url.substr(colon + 1, slash - colon - 1));
        } else {
            host_ = slash != std::string::npos ? url.substr(0, slash) : url;
            port_ = 80;
        }
    }

    struct Response {
        int status = 0;
        std::string body;
    };

    Response post(const std::string& path, const std::string& json_body) {
        return request("POST", path, json_body);
    }

    Response get(const std::string& path) {
        return request("GET", path, "");
    }

    Response put(const std::string& path, const std::string& json_body) {
        return request("PUT", path, json_body);
    }

private:
    Response request(const std::string& method, const std::string& path,
                     const std::string& body) {
        Response resp;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "[HTTP] socket() failed" << std::endl;
            return resp;
        }

        struct timeval tv{};
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct hostent* server = gethostbyname(host_.c_str());
        if (!server) {
            std::cerr << "[HTTP] gethostbyname() failed for " << host_ << std::endl;
            close(sock);
            return resp;
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr.s_addr, server->h_addr, static_cast<size_t>(server->h_length));
        addr.sin_port = htons(port_);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[HTTP] connect() to " << host_ << ":" << port_ << " failed" << std::endl;
            close(sock);
            return resp;
        }

        // Build HTTP request
        std::ostringstream req;
        req << method << " " << path << " HTTP/1.1\r\n";
        req << "Host: " << host_ << ":" << port_ << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Connection: close\r\n";
        if (!body.empty()) {
            req << "Content-Length: " << body.size() << "\r\n";
        }
        req << "\r\n";
        if (!body.empty()) {
            req << body;
        }

        std::string req_str = req.str();
        ssize_t sent = send(sock, req_str.c_str(), req_str.size(), 0);
        if (sent < 0) {
            std::cerr << "[HTTP] send() failed" << std::endl;
            close(sock);
            return resp;
        }

        // Read response
        char buf[65536];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        close(sock);

        if (n <= 0) return resp;

        buf[n] = '\0';
        std::string raw(buf, n);

        // Parse HTTP response
        size_t status_start = raw.find(' ');
        if (status_start != std::string::npos) {
            resp.status = std::stoi(raw.substr(status_start + 1, 3));
        }

        size_t body_start = raw.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            resp.body = raw.substr(body_start + 4);
        }

        return resp;
    }

    std::string base_url_;
    std::string host_;
    int port_ = 80;
};

// ============================================================
// JSON构建辅助（最小实现，无需第三方库）
// ============================================================
namespace json {

inline std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

inline std::string get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos != std::string::npos) {
        size_t start = pos + search.size();
        size_t end = json.find('"', start);
        if (end != std::string::npos) return json.substr(start, end - start);
    }
    return "";
}

inline int get_int(const std::string& json, const std::string& key, int def = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos != std::string::npos) {
        size_t start = pos + search.size();
        size_t end = json.find_first_of(",}\n\r ]", start);
        std::string num = json.substr(start, end - start);
        if (!num.empty()) return std::stoi(num);
    }
    return def;
}

inline bool get_bool(const std::string& json, const std::string& key, bool def = false) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos != std::string::npos) {
        if (json.find("true", pos) < json.find_first_of(",}\n\r", pos)) return true;
        if (json.find("false", pos) < json.find_first_of(",}\n\r", pos)) return false;
    }
    return def;
}

} // namespace json

// ============================================================
// 设备模拟器
// ============================================================
class DeviceSimulator {
public:
    struct Config {
        std::string cloud_url    = "http://127.0.0.1:9080";
        std::string tenant_id    = "tenant_demo";
        std::string hardware_uid = "";
        std::string model        = "CM-2000";
        std::string firmware_ver = "v2.0.5";
        std::string mac_address  = "";
        int         device_type  = 1;    // COFFEE_MACHINE
        int         heartbeat_sec = 10;  // heartbeat interval
        int         sim_duration_sec = 0; // 0 = run forever
        bool        auto_mode    = true;
    };

    DeviceSimulator(const Config& cfg)
        : cfg_(cfg), http_(cfg.cloud_url) {
        if (cfg_.hardware_uid.empty()) {
            cfg_.hardware_uid = "SIM-" + random_hex(16);
        }
        if (cfg_.mac_address.empty()) {
            cfg_.mac_address = random_mac();
        }
    }

    // ============================================================
    // Phase 1: 设备激活
    // ============================================================
    bool activate() {
        std::cout << "\n========== Phase 1: 设备激活 ==========" << std::endl;

        std::ostringstream body;
        body << "{"
             << R"("hardware_uid":")" << cfg_.hardware_uid << R"(",)";
        body << R"("model":")" << cfg_.model << R"(",)";
        body << R"("firmware_version":")" << cfg_.firmware_ver << R"(",)";
        body << R"("mac_address":")" << cfg_.mac_address << R"(",)";
        body << R"("tenant_id":")" << cfg_.tenant_id << R"(",)";
        body << R"("device_type":)" << cfg_.device_type;
        body << "}";

        std::cout << "[Activate] Request: " << body.str() << std::endl;

        auto resp = http_.post("/api/v1/device/activate", body.str());
        std::cout << "[Activate] Response HTTP " << resp.status << ": "
                  << (resp.body.size() > 500 ? resp.body.substr(0, 500) + "..." : resp.body)
                  << std::endl;

        if (resp.status == 200 || resp.status == 201) {
            success_ = json::get_bool(resp.body, "success", false);
            device_id_ = json::get_string(resp.body, "device_id");
            tenant_id_ = json::get_string(resp.body, "tenant_id");
            product_id_ = json::get_string(resp.body, "product_id");
            token_ = json::get_string(resp.body, "activation_token");
            broker_uri_ = json::get_string(resp.body, "mqtt_broker_uri");

            std::cout << "[Activate] SUCCESS!" << std::endl;
            std::cout << "  device_id:    " << device_id_ << std::endl;
            std::cout << "  tenant_id:    " << tenant_id_ << std::endl;
            std::cout << "  product_id:   " << product_id_ << std::endl;
            std::cout << "  token:        " << token_.substr(0, 32) << "..." << std::endl;
            std::cout << "  broker_uri:   " << broker_uri_ << std::endl;
            return true;
        } else {
            std::cerr << "[Activate] FAILED!" << std::endl;
            return false;
        }
    }

    // ============================================================
    // Phase 2: 心跳循环 + 事件模拟
    // ============================================================
    void run() {
        if (!success_) {
            std::cerr << "[Sim] Device not activated, cannot run" << std::endl;
            return;
        }

        std::cout << "\n========== Phase 2: 运行中 (心跳周期: "
                  << cfg_.heartbeat_sec << "秒) ==========" << std::endl;

        auto start_time = std::chrono::steady_clock::now();
        int cycle = 0;

        while (running_) {
            cycle++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time).count();

            std::cout << "\n--- Cycle " << cycle << " (t+" << elapsed << "s) ---" << std::endl;

            // 1. 心跳
            send_heartbeat(cycle);

            // 2. 属性上报（每3个周期）
            if (cycle % 3 == 0) {
                send_properties(cycle);
            }

            // 3. 自动模式：模拟业务事件
            if (cfg_.auto_mode) {
                simulate_events(cycle, elapsed);
            }

            // 4. 检查OTA更新（每5个周期）
            if (cycle % 5 == 0) {
                check_ota_update();
            }

            // 5. 配方同步（每10个周期）
            if (cycle % 10 == 0) {
                sync_recipes();
            }

            // 6. 拉取配置（每10个周期）
            if (cycle % 10 == 0) {
                pull_config();
            }

            // Duration check
            if (cfg_.sim_duration_sec > 0 && elapsed >= cfg_.sim_duration_sec) {
                std::cout << "\n[Sim] Duration reached, stopping..." << std::endl;
                running_ = false;
                break;
            }

            // Sleep until next heartbeat
            sleep_interval(cfg_.heartbeat_sec);
        }
    }

    void stop() { running_ = false; }

    const std::string& device_id() const { return device_id_; }
    const std::string& tenant_id() const { return tenant_id_; }
    bool is_activated() const { return success_; }

private:
    // ============================================================
    // 心跳上报
    // ============================================================
    void send_heartbeat(int cycle) {
        DeviceStatus status = current_status_;
        int signal = 80 + (rand() % 20); // 80-99
        int alarms = (status == DeviceStatus::FAULT) ? 1 + rand() % 3 : 0;

        std::ostringstream body;
        body << "{"
             << R"("device_id":")" << device_id_ << R"(",)";
        body << R"("timestamp":")" << now_iso() << R"(",)";
        body << R"("status":)" << static_cast<int>(status) << ",";
        body << R"("firmware_version":")" << cfg_.firmware_ver << R"(",)";
        body << R"("signal_strength":)" << signal << ",";
        body << R"("alarm_count":)" << alarms;
        body << "}";

        auto resp = http_.post("/api/v1/device/heartbeat", body.str());
        std::cout << "[Heartbeat] HTTP " << resp.status
                  << " status=" << static_cast<int>(status)
                  << " signal=" << signal << std::endl;
    }

    // ============================================================
    // 属性上报
    // ============================================================
    void send_properties(int cycle) {
        (void)cycle;
        std::ostringstream body;
        body << "{"
             << R"("device_id":")" << device_id_ << R"(",)";
        body << R"("properties":{)";
        body << R"("cpu_temp_c":)" << (45 + rand() % 20) << ",";
        body << R"("water_temp_c":)" << (85 + rand() % 10) << ",";
        body << R"("bean_remaining_g":)" << (100 + rand() % 400) << ",";
        body << R"("waste_bin_pct":)" << (10 + rand() % 30) << ",";
        body << R"("uptime_seconds":)" << (cycle * cfg_.heartbeat_sec);
        body << "}}";

        auto resp = http_.post("/api/v1/device/properties", body.str());
        std::cout << "[Properties] HTTP " << resp.status << std::endl;
    }

    // ============================================================
    // 自动事件模拟
    // ============================================================
    void simulate_events(int cycle, int64_t elapsed) {
        // 周期 7: 模拟下单
        if (cycle == 7 && current_status_ == DeviceStatus::IDLE) {
            simulate_order();
        }

        // 周期 15: 模拟故障
        if (cycle == 15) {
            simulate_fault();
        }

        // 周期 20: 故障恢复
        if (cycle == 20 && current_status_ == DeviceStatus::FAULT) {
            simulate_fault_recovery();
        }

        // 周期 25: 维护模式
        if (cycle == 25) {
            current_status_ = DeviceStatus::MAINTENANCE;
            report_event("maintenance", R"({"action":"start","reason":"定期维护"})");
            std::cout << "[Event] Entered MAINTENANCE mode" << std::endl;
        }

        if (cycle == 27) {
            current_status_ = DeviceStatus::IDLE;
            report_event("maintenance", R"({"action":"complete"})");
            std::cout << "[Event] Exited MAINTENANCE mode" << std::endl;
        }
    }

    void simulate_order() {
        std::ostringstream body;
        body << "{"
             << R"("device_id":")" << device_id_ << R"(",)";
        body << R"("tenant_id":")" << tenant_id_ << R"(",)";
        body << R"("event_type":"order_created",)";
        body << R"("payload":{)";
        body << R"("order_id":"SIM-ORD-)" << random_hex(6) << R"(",)";
        body << R"("recipe_id":"REC-AMERICANO-001",)";
        body << R"("cup_size":"中",)";
        body << R"("quantity":1,)";
        body << R"("total_amount":1500,)";
        body << R"("payment_method":"wechat")";
        body << "}}";

        auto resp = http_.post("/api/v1/device/events", body.str());
        std::cout << "[Event] Order created, HTTP " << resp.status << std::endl;

        // Then start brewing
        current_status_ = DeviceStatus::BREWING;
        std::cout << "[Event] Status → BREWING" << std::endl;

        // Simulate brewing time (don't actually wait, just report)
        report_event("brewing_started", R"({"order_id":"SIM-ORD"})");
    }

    void simulate_fault() {
        current_status_ = DeviceStatus::FAULT;
        std::ostringstream body;
        body << "{"
             << R"("device_id":")" << device_id_ << R"(",)";
        body << R"("tenant_id":")" << tenant_id_ << R"(",)";
        body << R"("event_type":"fault_detected",)";
        body << R"("payload":{)";
        body << R"("fault_code":3,)";       // PUMP_FAILURE
        body << R"("fault_level":3,)";       // L3_SEVERE
        body << R"("description":"水泵流量异常（模拟故障）",)";
        body << R"("sensor_snapshot":{"flow_rate_ml_s":2.5,"target_flow":8.0)";
        body << "}}";

        auto resp = http_.post("/api/v1/device/events", body.str());
        std::cout << "[Event] FAULT reported (PUMP_FAILURE L3), HTTP " << resp.status << std::endl;
    }

    void simulate_fault_recovery() {
        current_status_ = DeviceStatus::IDLE;
        std::ostringstream body;
        body << "{"
             << R"("device_id":")" << device_id_ << R"(",)";
        body << R"("tenant_id":")" << tenant_id_ << R"(",)";
        body << R"("event_type":"fault_resolved",)";
        body << R"("payload":{)";
        body << R"("fault_code":3)";
        body << "}}";

        auto resp = http_.post("/api/v1/device/events", body.str());
        std::cout << "[Event] FAULT RESOLVED, HTTP " << resp.status << std::endl;
    }

    void report_event(const std::string& event_type, const std::string& payload_json) {
        std::ostringstream body;
        body << "{"
             << R"("device_id":")" << device_id_ << R"(",)";
        body << R"("tenant_id":")" << tenant_id_ << R"(",)";
        body << R"("event_type":")" << event_type << R"(",)";
        body << R"("payload":)" << payload_json;
        body << "}";
        http_.post("/api/v1/device/events", body.str());
    }

    // ============================================================
    // OTA更新检查
    // ============================================================
    void check_ota_update() {
        std::string path = "/api/v1/device/ota/check?device_id=" + device_id_;
        auto resp = http_.get(path);
        std::cout << "[OTA Check] HTTP " << resp.status;

        if (resp.status == 200 && !resp.body.empty()) {
            std::string version = json::get_string(resp.body, "version");
            std::string url = json::get_string(resp.body, "download_url");
            if (!version.empty()) {
                std::cout << " update available: " << version;
                // Simulate OTA progress
                simulate_ota_progress(version);
            } else {
                std::cout << " no update available";
            }
        }
        std::cout << std::endl;
    }

    void simulate_ota_progress(const std::string& target_ver) {
        // Report OTA progress in stages
        struct OtaStage { int progress; const char* stage; };
        const OtaStage stages[] = {
            {10, "downloading"},
            {40, "downloading"},
            {70, "downloading"},
            {100, "downloading"},
            {0, "installing"},
            {50, "installing"},
            {100, "done"},
        };

        for (const auto& s : stages) {
            std::ostringstream body;
            body << "{"
                 << R"("device_id":")" << device_id_ << R"(",)";
            body << R"("version":")" << target_ver << R"(",)";
            body << R"("progress":)" << s.progress << ",";
            body << R"("stage":")" << s.stage << R"(")";
            body << "}";

            http_.post("/api/v1/device/ota/callback", body.str());
            std::cout << "  [OTA Progress] " << s.stage << " " << s.progress << "%" << std::endl;

            // Small delay between stages
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        // Update firmware version
        cfg_.firmware_ver = target_ver;
        std::cout << "[OTA] Upgrade complete, new firmware: " << target_ver << std::endl;
    }

    // ============================================================
    // 配方同步
    // ============================================================
    void sync_recipes() {
        auto resp = http_.get("/api/v1/recipes");
        int count = 0;
        if (resp.status == 200) {
            // Count recipes in JSON array
            size_t pos = 0;
            while ((pos = resp.body.find("\"recipe_id\":\"", pos)) != std::string::npos) {
                count++;
                pos++;
            }
        }
        std::cout << "[Recipes Sync] HTTP " << resp.status
                  << " count=" << count << std::endl;
    }

    // ============================================================
    // 拉取配置
    // ============================================================
    void pull_config() {
        auto resp = http_.get("/api/v1/config");
        std::cout << "[Config Pull] HTTP " << resp.status << std::endl;
    }

    // ============================================================
    // 工具函数
    // ============================================================
    void sleep_interval(int seconds) {
        // Sleep in 1-second chunks to allow clean shutdown
        for (int i = 0; i < seconds && running_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::string now_iso() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    std::string random_hex(int bytes) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, 255);
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < bytes; i++) {
            oss << std::setw(2) << dis(gen);
        }
        return oss.str();
    }

    std::string random_mac() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, 255);
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 6; i++) {
            if (i > 0) oss << ":";
            oss << std::setw(2) << dis(gen);
        }
        return oss.str();
    }

    enum class DeviceStatus : uint8_t {
        IDLE = 0, BREWING = 1, FAULT = 2, UPGRADING = 3, MAINTENANCE = 4, OFFLINE = 5
    };

    Config cfg_;
    SimHttpClient http_;
    std::atomic<bool> running_{true};
    bool success_ = false;
    std::string device_id_;
    std::string tenant_id_;
    std::string product_id_;
    std::string token_;
    std::string broker_uri_;
    DeviceStatus current_status_ = DeviceStatus::IDLE;
};

// ============================================================
// 全局变量（信号处理用）
// ============================================================
static DeviceSimulator* g_sim = nullptr;

void signal_handler(int sig) {
    (void)sig;
    std::cout << "\n[Sim] Signal " << sig << " received, shutting down..." << std::endl;
    if (g_sim) g_sim->stop();
}

// ============================================================
// 使用说明
// ============================================================
void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  --url URL         Cloud platform URL (default: http://127.0.0.1:8080)\n"
              << "  --tenant ID       Tenant ID (default: tenant_demo)\n"
              << "  --model MODEL     Device model code (default: CM-2000)\n"
              << "  --type TYPE       Device type 1=COFFEE 2=INSTANT 3=WATER 4=OTHER (default: 1)\n"
              << "  --firmware VER    Firmware version (default: v2.0.5)\n"
              << "  --interval SEC    Heartbeat interval seconds (default: 10)\n"
              << "  --duration SEC    Total simulation duration, 0=forever (default: 0)\n"
              << "  --hw-uid UID      Hardware UID (default: auto-generated)\n"
              << "  --mac ADDR        MAC address (default: auto-generated)\n"
              << "  --manual          Manual mode: wait for Enter between cycles\n"
              << "  --help            Show this help\n"
              << std::endl;
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    DeviceSimulator::Config cfg;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            cfg.cloud_url = argv[++i];
        } else if (arg == "--tenant" && i + 1 < argc) {
            cfg.tenant_id = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            cfg.model = argv[++i];
        } else if (arg == "--type" && i + 1 < argc) {
            cfg.device_type = std::stoi(argv[++i]);
        } else if (arg == "--firmware" && i + 1 < argc) {
            cfg.firmware_ver = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            cfg.heartbeat_sec = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            cfg.sim_duration_sec = std::stoi(argv[++i]);
        } else if (arg == "--hw-uid" && i + 1 < argc) {
            cfg.hardware_uid = argv[++i];
        } else if (arg == "--mac" && i + 1 < argc) {
            cfg.mac_address = argv[++i];
        } else if (arg == "--manual") {
            cfg.auto_mode = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Print banner
    std::cout << R"(
╔══════════════════════════════════════════════════════╗
║           IoT Device Simulator v1.0.0                ║
║                                                      ║
║  模拟一台IoT设备，通过HTTP REST API与云平台通信       ║
╚══════════════════════════════════════════════════════╝
)" << std::endl;

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Cloud URL:     " << cfg.cloud_url << std::endl;
    std::cout << "  Tenant:        " << cfg.tenant_id << std::endl;
    std::cout << "  Model:         " << cfg.model << std::endl;
    std::cout << "  Device Type:   " << cfg.device_type << std::endl;
    std::cout << "  Firmware:      " << cfg.firmware_ver << std::endl;
    std::cout << "  Heartbeat:     " << cfg.heartbeat_sec << "s" << std::endl;
    std::cout << "  Duration:      "
              << (cfg.sim_duration_sec == 0 ? "forever" : std::to_string(cfg.sim_duration_sec) + "s")
              << std::endl;
    std::cout << "  Mode:          " << (cfg.auto_mode ? "auto" : "manual") << std::endl;
    std::cout << "  HW UID:        " << cfg.hardware_uid << std::endl;
    std::cout << "  MAC:           " << cfg.mac_address << std::endl;

    // Create simulator
    DeviceSimulator sim(cfg);
    g_sim = &sim;

    // Phase 1: Activate
    if (!sim.activate()) {
        std::cerr << "\n[FATAL] Device activation failed. Is the cloud platform running at "
                  << cfg.cloud_url << " ?" << std::endl;
        return 1;
    }

    std::cout << "\n[Sim] Device ready. device_id=" << sim.device_id()
              << " tenant=" << sim.tenant_id() << std::endl;

    if (!cfg.auto_mode) {
        std::cout << "\n[Manual Mode] Press Enter to start simulation..." << std::endl;
        std::cin.get();
    }

    // Phase 2: Run
    sim.run();

    std::cout << "\n[Sim] Simulation ended. Device " << sim.device_id()
              << " shutting down." << std::endl;
    return 0;
}
