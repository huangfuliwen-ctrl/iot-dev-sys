/**
 * device_simulator.cpp — IoT设备模拟器（MQTT + HTTP 双通道完整流程）
 *
 * 模拟一台新设备从激活到正常运行的全生命周期。
 * 通信方式：
 *   HTTP — 设备激活、配方同步、配置拉取、OTA固件下载
 *   MQTT — 心跳、属性上报、事件上报、OTA进度、接收下行指令
 *
 * 流程（对应SRS附录8）：
 *   阶段1: HTTP设备激活 → 获取 device_id + token
 *   阶段2: MQTT连接Broker → 订阅下行Topic
 *   阶段3: 正常运行（心跳+属性+事件+配置+配方）
 *   阶段4: OTA升级（接收MQTT通知→HTTP下载→MQTT上报进度）
 *
 * 编译：项目CMakeLists.txt自动编译此文件（无需外部MQTT库）
 *   cmake -B build && make -C build device_simulator
 *
 * 使用：
 *   ./build/bin/device_simulator --url http://127.0.0.1:9080 --mqtt tcp://127.0.0.1:1883
 */
#include <iostream>
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
#include <fstream>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

// ============================================================
// 工具函数
// ============================================================
namespace util {
inline std::string now_iso() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
inline std::string random_hex(int bytes) {
    static std::mt19937 gen(std::random_device{}());
    std::ostringstream oss; oss << std::hex << std::setfill('0');
    for (int i = 0; i < bytes; i++) oss << std::setw(2) << (gen() & 0xFF);
    return oss.str();
}
inline std::string escape(const std::string& s) {
    std::string o; for (char c : s) { if (c=='"') o+="\\\""; else if (c=='\\') o+="\\\\"; else o+=c; } return o;
}
inline std::string json_get_str(const std::string& j, const std::string& k) {
    auto p = j.find("\""+k+"\":\""); if (p==std::string::npos) return "";
    p += k.size()+4; auto e = j.find('"', p); return (e!=std::string::npos) ? j.substr(p,e-p) : "";
}
inline int json_get_int(const std::string& j, const std::string& k, int d=0) {
    auto p = j.find("\""+k+"\":"); if (p==std::string::npos) return d;
    p += k.size()+3; auto e = j.find_first_of(",}\n\r ]",p);
    try { return std::stoi(j.substr(p,e-p)); } catch(...) { return d; }
}
inline bool json_get_bool(const std::string& j, const std::string& k, bool d=false) {
    auto p = j.find("\""+k+"\":"); if (p==std::string::npos) return d;
    return j.find("true",p) < j.find_first_of(",}\n\r",p);
}
} // namespace util

// ============================================================
// 轻量HTTP客户端（POSIX socket）
// ============================================================
class SimHttpClient {
    std::string host_; int port_ = 80;
public:
    struct Response { int status = 0; std::string body; };
    SimHttpClient(const std::string& base_url) {
        std::string u = base_url;
        if (u.find("http://")==0) u=u.substr(7);
        size_t c=u.find(':'), s=u.find('/');
        host_ = (c!=std::string::npos) ? u.substr(0,c) : u.substr(0,s);
        port_ = (c!=std::string::npos) ? std::stoi(u.substr(c+1,s-c-1)) : 80;
    }
    Response post(const std::string& path, const std::string& body) { return req("POST",path,body); }
    Response get(const std::string& path) { return req("GET",path,""); }
private:
    Response req(const std::string& m, const std::string& path, const std::string& body) {
        Response r; int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return r;
        timeval tv{10,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        hostent* h=gethostbyname(host_.c_str()); if(!h){close(fd);return r;}
        sockaddr_in a{}; a.sin_family=AF_INET; memcpy(&a.sin_addr,h->h_addr,h->h_length); a.sin_port=htons(port_);
        if(connect(fd,(sockaddr*)&a,sizeof(a))<0){close(fd);return r;}
        std::ostringstream rs; rs<<m<<" "<<path<<" HTTP/1.1\r\nHost: "<<host_<<":"<<port_
           <<"\r\nContent-Type: application/json\r\nConnection: close\r\n";
        if(!body.empty()) rs<<"Content-Length: "<<body.size()<<"\r\n";
        rs<<"\r\n"<<body;
        std::string s=rs.str(); send(fd,s.c_str(),s.size(),0);
        char buf[65536]; ssize_t n=recv(fd,buf,sizeof(buf)-1,0); close(fd);
        if(n<=0)return r; buf[n]=0;
        size_t st=std::string(buf).find(' '); if(st!=std::string::npos) r.status=std::stoi(std::string(buf).substr(st+1,3));
        size_t bs=std::string(buf).find("\r\n\r\n"); if(bs!=std::string::npos) r.body=std::string(buf).substr(bs+4);
        return r;
    }
};

// ============================================================
// 极简MQTT 3.1.1客户端（POSIX socket，零外部依赖）
// 仅实现CONNECT/PUBLISH/SUBSCRIBE/PINGREQ/DISCONNECT
// ============================================================
class SimMqttClient {
    int fd_ = -1;
    std::string host_; int port_ = 1883;
    std::string client_id_, user_, pass_;
    std::atomic<bool> connected_{false};
    std::thread recv_thread_;
    std::function<void(const std::string& topic, const std::string& payload)> msg_cb_;
    uint16_t pkt_id_ = 1;

    // 编码 remaining length (MQTT 3.1.1)
    static std::string encode_len(uint32_t len) {
        std::string r; do { uint8_t b=len&0x7F; len>>=7; if(len)b|=0x80; r+=(char)b; } while(len); return r;
    }
    // 编码 UTF-8 string
    static std::string encode_str(const std::string& s) {
        uint16_t l=htons(s.size()); return std::string((char*)&l,2)+s;
    }
    // 读取 exactly N 字节
    bool read_exact(void* buf, size_t n) {
        size_t off=0; auto* p=(char*)buf;
        while(off<n){ auto r=recv(fd_,p+off,n-off,0); if(r<=0)return false; off+=r; }
        return true;
    }
    // 读取一个 MQTT 包
    bool read_packet(uint8_t& type, std::string& payload) {
        uint8_t hdr; if(!read_exact(&hdr,1)) return false;
        type=hdr>>4; uint32_t len=0,shift=0;
        while(true){ uint8_t b; if(!read_exact(&b,1))return false; len|=(b&0x7F)<<shift; shift+=7; if(!(b&0x80))break; }
        payload.resize(len); if(len>0&&!read_exact(&payload[0],len)) return false;
        return true;
    }

public:
    bool connect(const std::string& uri, const std::string& cid, const std::string& user, const std::string& pass) {
        std::string u=uri;
        if(u.find("tcp://")==0) u=u.substr(6);
        else if(u.find("ssl://")==0) u=u.substr(6);
        else if(u.find("tls://")==0) u=u.substr(6);
        else if(u.find("mqtt://")==0) u=u.substr(7);
        size_t c=u.find(':'); host_=(c!=std::string::npos)?u.substr(0,c):u; port_=(c!=std::string::npos)?std::stoi(u.substr(c+1)):1883;
        client_id_=cid; user_=user; pass_=pass;

        fd_=socket(AF_INET,SOCK_STREAM,0); if(fd_<0)return false;
        timeval tv{30,0}; setsockopt(fd_,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
        hostent* h=gethostbyname(host_.c_str()); if(!h){close(fd_);fd_=-1;return false;}
        sockaddr_in a{}; a.sin_family=AF_INET; memcpy(&a.sin_addr,h->h_addr,h->h_length); a.sin_port=htons(port_);
        if(::connect(fd_,(sockaddr*)&a,sizeof(a))<0){close(fd_);fd_=-1;return false;}

        // Build CONNECT
        std::string var; var+="\x00\x04MQTT\x04\x02\x00\x3C"; // protocol MQTT v3.1.1, keepalive 60s, clean session
        var+=encode_str(client_id_);
        if(!user_.empty()&&!pass_.empty()){ var+=encode_str(user_); var+=encode_str(pass_); }
        std::string pkt; pkt+=(char)0x10; pkt+=encode_len(var.size()); pkt+=var;
        send(fd_,pkt.c_str(),pkt.size(),0);

        // Read CONNACK
        std::string pl; uint8_t tp;
        if(!read_packet(tp,pl)||tp!=2||pl.size()<2){close(fd_);fd_=-1;return false;}
        if(pl[1]!=0){ std::cerr<<"[MQTT] CONNACK code="<<(int)pl[1]<<std::endl; close(fd_);fd_=-1;return false; }
        connected_=true;
        recv_thread_=std::thread([this]{ recv_loop(); });
        std::cout<<"[MQTT] Connected to "<<host_<<":"<<port_<<std::endl;
        return true;
    }
    bool subscribe(const std::string& topic, int qos=1) {
        if(!connected_)return false;
        uint16_t pid=htons(pkt_id_++);
        std::string var; var.append((char*)&pid,2);
        var+=encode_str(topic); var+=(char)qos;
        std::string pkt; pkt+=(char)0x82; pkt+=encode_len(var.size()); pkt+=var;
        send(fd_,pkt.c_str(),pkt.size(),0);
        return true;
    }
    bool publish(const std::string& topic, const std::string& payload, int qos=1) {
        if(!connected_)return false;
        std::string var; var+=encode_str(topic);
        if(qos>0){ uint16_t pid=htons(pkt_id_++); var.append((char*)&pid,2); }
        var+=payload;
        uint8_t hdr=(uint8_t)(0x30|(qos<<1));
        std::string pkt; pkt+=(char)hdr; pkt+=encode_len(var.size()); pkt+=var;
        send(fd_,pkt.c_str(),pkt.size(),0);
        return true;
    }
    void disconnect() {
        connected_=false;
        if(fd_>=0){ char d[]={(char)0xE0,0x00}; send(fd_,d,2,0); close(fd_); fd_=-1; }
        if(recv_thread_.joinable()) recv_thread_.join();
    }
    bool is_connected() const { return connected_; }
    void on_message(std::function<void(const std::string&,const std::string&)> cb) { msg_cb_=cb; }

private:
    void recv_loop() {
        while(connected_){
            std::string pl; uint8_t tp;
            if(!read_packet(tp,pl)){ connected_=false; break; }
            if(tp==3 && msg_cb_){ // PUBLISH
                size_t o=0; uint16_t tlen=(pl[0]<<8)|pl[1]; o=2+tlen;
                std::string topic=pl.substr(2,tlen);
                uint8_t qos=(tp>>1)&3;
                if(qos>0) o+=2; // skip packet ID
                std::string payload=pl.substr(o);
                msg_cb_(topic,payload);
            }
            // PINGRESP, SUBACK, PUBACK are silently handled
        }
    }
};

// ============================================================
// 设备模拟器
// ============================================================
class DeviceSimulator {
public:
    struct Config {
        std::string cloud_url  = "http://127.0.0.1:9080";
        std::string mqtt_uri   = "tcp://127.0.0.1:1883";
        std::string tenant_id  = "company_zhangsan";
        std::string hardware_uid = "";
        std::string model_key  = "01KX5M2KM8EBW9G1CWVMJ94TSK";
        std::string firmware_ver = "v1.0.0";
        std::string mac        = "";
        int device_type = 1;       // 1=COFFEE_MACHINE
        int heartbeat_sec = 30;
        int sim_duration_sec = 0;
        bool skip_mqtt = false;    // 纯HTTP模式（无Broker时）
    };

    DeviceSimulator(const Config& c) : cfg_(c), http_(c.cloud_url) {
        if(cfg_.hardware_uid.empty()) cfg_.hardware_uid = "SIM-" + util::random_hex(8);
        if(cfg_.mac.empty()) cfg_.mac = util::random_hex(2)+":"+util::random_hex(2)+":"+util::random_hex(2)+":"+util::random_hex(2)+":"+util::random_hex(2)+":"+util::random_hex(2);
    }
    ~DeviceSimulator() { if(mqtt_) mqtt_->disconnect(); }

    // ============================================================
    // 阶段1: HTTP设备激活
    // ============================================================
    bool activate() {
        log("══════ 阶段1: HTTP设备激活 (uid + model_key) ══════");
        std::ostringstream b;
        b<<"{\"uid\":\""<<cfg_.hardware_uid<<"\",\"model_key\":\""<<cfg_.model_key<<"\"}";
        log("→ POST /api/v1/device/activate");
        log("  {uid:"+cfg_.hardware_uid+", model_key:"+cfg_.model_key+"}");

        auto r=http_.post("/api/v1/device/activate",b.str());
        log("← HTTP "+std::to_string(r.status)+" "+(r.body.size()>300?r.body.substr(0,300)+"...":r.body));

        if(r.status==201||r.status==200){
            device_id_  = util::json_get_str(r.body,"device_id");
            tenant_id_  = util::json_get_str(r.body,"tenant_id");
            product_id_ = util::json_get_str(r.body,"product_id");
            token_      = util::json_get_str(r.body,"activation_token");
            broker_uri_ = util::json_get_str(r.body,"mqtt_broker_uri");
            model_code_ = util::json_get_str(r.body,"model_code");
            fw_ver_     = util::json_get_str(r.body,"firmware_version");
            if(broker_uri_.empty()) broker_uri_=cfg_.mqtt_uri;

            log("✓ 激活成功");
            log("  device_id:  "+device_id_);
            log("  model:      "+model_code_);
            log("  product_id: "+product_id_);
            log("  firmware:   "+fw_ver_);
            log("  token:      "+token_.substr(0,32)+"...");
            save_credentials();
            return true;
        }
        log("✗ 激活失败"); return false;
    }

    // 检查是否已激活（本地有凭证文件则跳过HTTP激活）
    bool try_load_credentials() {
        std::ifstream f("./device_creds.json");
        if (!f.is_open()) return false;
        std::string j((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        device_id_  = util::json_get_str(j, "device_id");
        tenant_id_  = util::json_get_str(j, "tenant_id");
        product_id_ = util::json_get_str(j, "product_id");
        token_      = util::json_get_str(j, "token");
        broker_uri_ = util::json_get_str(j, "broker_uri");
        model_code_ = util::json_get_str(j, "model_code");
        fw_ver_     = util::json_get_str(j, "firmware_version");
        if (device_id_.empty() || token_.empty()) return false;
        log("✓ 已激活设备（本地凭证）→ 跳过HTTP激活");
        log("  device_id: "+device_id_+" model: "+model_code_);
        return true;
    }
    void save_credentials() {
        std::ofstream f("./device_creds.json");
        f << "{\"device_id\":\"" << device_id_ << "\",\"tenant_id\":\"" << tenant_id_
          << "\",\"product_id\":\"" << product_id_ << "\",\"token\":\"" << token_
          << "\",\"broker_uri\":\"" << broker_uri_ << "\",\"model_code\":\"" << model_code_
          << "\",\"firmware_version\":\"" << fw_ver_ << "\"}";
    }

    // ============================================================
    // 阶段2: MQTT连接 + 订阅下行Topic
    // ============================================================
    bool connect_mqtt() {
        if(cfg_.skip_mqtt){ log("⚠ MQTT skipped (--skip-mqtt)"); return true; }
        log("\n══════ 阶段2: MQTT连接Broker ══════");
        mqtt_=std::make_unique<SimMqttClient>();
        if(!mqtt_->connect(broker_uri_,device_id_,device_id_,token_)){
            log("✗ MQTT连接失败: "+broker_uri_);
            return false;
        }
        // 接收下行消息
        mqtt_->on_message([this](const std::string& t,const std::string& p){
            log("[MQTT RECV] "+t+" → "+p.substr(0,120));
            if(t.find("/ota/notify")!=std::string::npos) handle_ota_notify(p);
            if(t.find("/property/set")!=std::string::npos) handle_config_set(p);
            if(t.find("/command/")!=std::string::npos) handle_command(t,p);
        });

        // 订阅下行Topic
        std::string base=tenant_id_+"/iot/"+product_id_+"/"+device_id_;
        mqtt_->subscribe(base+"/property/set");
        mqtt_->subscribe(base+"/ota/notify");
        mqtt_->subscribe(base+"/command/+");
        log("✓ MQTT已连接，已订阅3个下行Topic");

        // 发布上线Will（下次connect时生效，此处手动发一条上线消息）
        mqtt_->publish(base+"/heartbeat",R"({"status":"online","ts":")"+util::now_iso()+"\"}");
        return true;
    }

    // ============================================================
    // 阶段3: 正常运行循环
    // ============================================================
    void run() {
        log("\n══════ 阶段3: 正常运行（心跳"+std::to_string(cfg_.heartbeat_sec)+"s）══════");
        auto start=std::chrono::steady_clock::now();
        int cyc=0;

        // 启动时拉取配置
        pull_config();
        sync_recipes();

        while(running_){
            cyc++;
            auto e=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start).count();
            log("\n── 周期 "+std::to_string(cyc)+" (t+"+std::to_string(e)+"s) ──");

            send_heartbeat();
            if(cyc%3==0) send_properties();
            simulate_events(cyc);

            if(cfg_.sim_duration_sec>0&&e>=cfg_.sim_duration_sec){ running_=false; break; }
            sleep_interval(cfg_.heartbeat_sec);
        }
    }
    void stop() { running_=false; }
    bool is_activated() const { return !device_id_.empty(); }
    const std::string& device_id() const { return device_id_; }

private:
    void send_heartbeat() {
        std::ostringstream b;
        b<<"{\"device_id\":\""<<device_id_
         <<"\",\"timestamp\":\""<<util::now_iso()
         <<"\",\"status\":"<<(int)status_
         <<",\"firmware_version\":\""<<cfg_.firmware_ver
         <<"\",\"signal_strength\":"<<(80+rand()%20)
         <<",\"alarm_count\":"<<((status_==DeviceStatus::FAULT)?1:0)<<"}";
        if(mqtt_&&mqtt_->is_connected()){
            std::string t=tenant_id_+"/iot/"+product_id_+"/"+device_id_+"/heartbeat";
            mqtt_->publish(t,b.str());
            log("[MQTT] heartbeat → "+t);
        } else {
            auto r=http_.post("/api/v1/device/heartbeat",b.str());
            log("[HTTP] heartbeat → "+std::to_string(r.status));
        }
    }
    void send_properties() {
        std::ostringstream b;
        b<<"{\"device_id\":\""<<device_id_<<"\",\"properties\":{"
         <<"\"cpu_temp_c\":"<<(45+rand()%20)<<",\"water_temp_c\":"<<(85+rand()%10)
         <<",\"bean_remaining_g\":"<<(100+rand()%400)<<"}}";
        if(mqtt_&&mqtt_->is_connected()){
            mqtt_->publish(tenant_id_+"/iot/"+product_id_+"/"+device_id_+"/property/post",b.str());
            log("[MQTT] properties sent");
        }
    }
    void simulate_events(int cyc) {
        if(cyc==5&&status_==DeviceStatus::IDLE){
            status_=DeviceStatus::BREWING;
            publish_event("order_status","{\"order_id\":\"SIM-"+util::random_hex(4)+"\",\"status\":2,\"recipe_id\":\"REC-001\"}");
            log("[Event] 开始制作");
        }
        if(cyc==6){ status_=DeviceStatus::IDLE; publish_event("order_status","{\"order_id\":\"SIM-"+util::random_hex(4)+"\",\"status\":3}"); log("[Event] 制作完成"); }
        if(cyc==12){ status_=DeviceStatus::FAULT; publish_event("fault_alert","{\"fault_code\":3,\"fault_level\":3,\"description\":\"水泵异常(模拟)\"}"); log("[Event] 故障告警 L3"); }
        if(cyc==16){ status_=DeviceStatus::IDLE; publish_event("fault_resolved","{\"fault_code\":3}"); log("[Event] 故障恢复"); }
    }
    void publish_event(const std::string& type, const std::string& data) {
        std::string b="{\"event_type\":\""+type+"\","+data.substr(1);
        if(mqtt_&&mqtt_->is_connected())
            mqtt_->publish(tenant_id_+"/iot/"+product_id_+"/"+device_id_+"/event/post",b);
    }
    void pull_config() {
        auto r=http_.get("/api/v1/config");
        log("[Config] HTTP "+std::to_string(r.status));
    }
    void sync_recipes() {
        auto r=http_.get("/api/v1/recipes");
        int n=0; for(size_t p=0;(p=r.body.find("\"recipe_id\":\"",p))!=std::string::npos;p++)n++;
        log("[Recipes] HTTP "+std::to_string(r.status)+" count="+std::to_string(n));
    }

    // ============================================================
    // 阶段4: OTA升级（MQTT通知 → HTTP下载 → MQTT上报）
    // ============================================================
    void handle_ota_notify(const std::string& payload) {
        log("\n══════ 阶段4: OTA固件升级 ══════");
        std::string ver=util::json_get_str(payload,"version");
        std::string url=util::json_get_str(payload,"download_url");
        std::string csum=util::json_get_str(payload,"checksum");
        log("[OTA] 收到升级通知: "+ver+" url="+url);

        auto report=[this](const std::string& v,int p,const std::string& s){
            std::string b="{\"version\":\""+v+"\",\"progress\":"+std::to_string(p)+",\"stage\":\""+s+"\"}";
            mqtt_->publish(tenant_id_+"/iot/"+product_id_+"/"+device_id_+"/ota/progress",b);
        };
        report(ver,0,"downloading");

        // HTTP下载固件
        std::string fw_file="./firmware_dl/"+ver+".ipk";
        std::system(("mkdir -p ./firmware_dl && curl -s -o "+fw_file+" '"+url+"'").c_str());
        log("[OTA] 下载完成: "+fw_file);

        report(ver,100,"downloading");
        report(ver,0,"installing");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        report(ver,100,"done");

        cfg_.firmware_ver=ver;
        log("[OTA] ✓ 升级完成! 新版本: "+ver);
    }
    void handle_config_set(const std::string& p) {
        log("[Config Set] 云端下发配置: "+p.substr(0,100));
    }
    void handle_command(const std::string& t,const std::string& p) {
        log("[Command] 收到指令: "+t+" → "+p);
    }

    void sleep_interval(int s) { for(int i=0;i<s&&running_;i++)std::this_thread::sleep_for(std::chrono::seconds(1)); }
    void log(const std::string& msg) { std::cout<<"[Sim] "<<msg<<std::endl; }

    enum class DeviceStatus:uint8_t{IDLE=0,BREWING=1,FAULT=2,UPGRADING=3,MAINTENANCE=4,OFFLINE=5};

    Config cfg_; SimHttpClient http_; std::unique_ptr<SimMqttClient> mqtt_;
    std::atomic<bool> running_{true};
    std::string device_id_,tenant_id_,product_id_,token_,broker_uri_,model_code_,fw_ver_;
    DeviceStatus status_=DeviceStatus::IDLE;
};

// ============================================================
// Main
// ============================================================
static DeviceSimulator* g_sim=nullptr;
void sig_handler(int s){(void)s; if(g_sim)g_sim->stop();}

int main(int argc, char* argv[]) {
    signal(SIGINT,sig_handler); signal(SIGTERM,sig_handler);
    DeviceSimulator::Config cfg;

    for(int i=1;i<argc;i++){ std::string a=argv[i];
        if(a=="--url"&&i+1<argc) cfg.cloud_url=argv[++i];
        else if(a=="--mqtt"&&i+1<argc) cfg.mqtt_uri=argv[++i];
        else if(a=="--tenant"&&i+1<argc) cfg.tenant_id=argv[++i];
        else if(a=="--model-key"&&i+1<argc) cfg.model_key=argv[++i];
        else if(a=="--interval"&&i+1<argc) cfg.heartbeat_sec=std::stoi(argv[++i]);
        else if(a=="--duration"&&i+1<argc) cfg.sim_duration_sec=std::stoi(argv[++i]);
        else if(a=="--hw-uid"&&i+1<argc) cfg.hardware_uid=argv[++i];
        else if(a=="--mac"&&i+1<argc) cfg.mac=argv[++i];
        else if(a=="--skip-mqtt") cfg.skip_mqtt=true;
        else if(a=="--reactivate") { std::remove("./device_creds.json"); std::cout<<"[Sim] 本地凭证已清除，将重新激活"<<std::endl; }
        else if(a=="--help"){ std::cout<<R"(
Usage: device_simulator [OPTIONS]
  --url URL      云端地址 (default: http://127.0.0.1:9080)
  --mqtt URI     MQTT Broker (default: tcp://127.0.0.1:1883)
  --tenant ID    租户ID
  --model-key KEY  设备型号Key(ULID, 必填)
  --interval SEC   心跳间隔 (default: 30)
  --duration SEC 模拟时长,0=永久
  --skip-mqtt    纯HTTP模式(无Broker时使用)
  --reactivate   强制重新激活(清除本地凭证)
  --help
)"; return 0; }

    std::cout<<R"(
╔══════════════════════════════════════════════════════╗
║       IoT Device Simulator v2.0 (MQTT+HTTP)         ║
╚══════════════════════════════════════════════════════╝
)"<<"  云端: "<<cfg.cloud_url<<"  MQTT: "<<cfg.mqtt_uri
<<"  model_key: "<<cfg.model_key<<std::endl;

    DeviceSimulator sim(cfg); g_sim=&sim;

    // 阶段1: 激活（如果已激活则复用本地凭证）
    if (!sim.try_load_credentials()) {
        if (!sim.activate()) { std::cerr<<"[FATAL] 激活失败"<<std::endl; return 1; }
    }

    // 阶段2: MQTT连接
    if(!sim.connect_mqtt()){ std::cerr<<"[WARN] MQTT连接失败，使用HTTP降级模式"<<std::endl; }

    // 阶段3+4: 运行
    sim.run();
    std::cout<<"\n[Sim] 模拟结束 device="<<sim.device_id()<<std::endl;
    return 0;
}
}
