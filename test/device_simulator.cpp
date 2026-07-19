/**
 * device_simulator.cpp — IoT设备模拟器 (SRS 附录8 完整流程)
 *
 * 编译: cmake -B build && make -C build device_simulator
 * 使用: ./build/bin/device_simulator --skip-mqtt --duration 20
 */
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>
#include <random>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

// ============================================================
// 工具
// ============================================================
namespace U {
inline std::string now() { auto t=time(nullptr); std::ostringstream o; o<<std::put_time(gmtime(&t),"%Y-%m-%dT%H:%M:%SZ"); return o.str(); }
inline std::string rhex(int n){ static std::mt19937 g(std::random_device{}()); std::ostringstream o; o<<std::hex<<std::setfill('0'); for(int i=0;i<n;i++)o<<std::setw(2)<<(g()&0xFF); return o.str(); }
inline std::string jg(const std::string& j,const std::string& k){ auto p=j.find("\""+k+"\":\""); if(p==std::string::npos)return ""; p+=k.size()+4; auto e=j.find('"',p); return e!=std::string::npos?j.substr(p,e-p):""; }
inline int ji(const std::string& j,const std::string& k,int d=0){ auto p=j.find("\""+k+"\":"); if(p==std::string::npos)return d; try{return std::stoi(j.substr(p+k.size()+3));}catch(...){return d;} }
}

// ============================================================
// HTTP客户端 (POSIX socket)
// ============================================================
struct Http {
    std::string h_; int p_=80;
    struct R { int s=0; std::string b; };
    Http(const std::string& url){ auto u=url; if(u.find("http://")==0)u=u.substr(7); auto c=u.find(':'); h_=c!=std::string::npos?u.substr(0,c):u; p_=c!=std::string::npos?std::stoi(u.substr(c+1)):80; }
    R post(const std::string& path,const std::string& body){ return req("POST",path,body); }
    R get(const std::string& path){ return req("GET",path,""); }
    R req(const std::string& m,const std::string& path,const std::string& body){
        R r; int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return r;
        timeval tv{5,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)); setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        addrinfo h={},*res; h.ai_family=AF_INET; h.ai_socktype=SOCK_STREAM;
        if(getaddrinfo(h_.c_str(),std::to_string(p_).c_str(),&h,&res)!=0){close(fd);return r;}
        sockaddr_in a{}; memcpy(&a,res->ai_addr,sizeof(a)); freeaddrinfo(res);
        if(connect(fd,(sockaddr*)&a,sizeof(a))<0){close(fd);return r;}
        std::ostringstream rs; rs<<m<<" "<<path<<" HTTP/1.1\r\nHost: "<<h_<<":"<<p_<<"\r\nContent-Type: application/json\r\nConnection: close\r\n";
        if(!body.empty())rs<<"Content-Length: "<<body.size()<<"\r\n";
        rs<<"\r\n"<<body;
        auto s=rs.str(); send(fd,s.c_str(),s.size(),0);
        char buf[65536]; auto n=recv(fd,buf,sizeof(buf)-1,0); close(fd);
        if(n<=0)return r; buf[n]=0; auto rs2=std::string(buf);
        auto st=rs2.find(' '); if(st!=std::string::npos)r.s=std::stoi(rs2.substr(st+1,3));
        auto bs=rs2.find("\r\n\r\n"); if(bs!=std::string::npos)r.b=rs2.substr(bs+4);
        return r;
    }
};

// ============================================================
// 简版MQTT 3.1.1 (POSIX socket, 5秒超时)
// ============================================================
struct Mqtt {
    int fd_=-1; std::string h_; int p_=1883; std::atomic<bool> ok_{false};
    uint16_t pid_=1; std::string err_;
    std::string will_topic_, will_payload_;
    static std::string el(uint32_t l){ std::string r; do{uint8_t b=l&0x7F;l>>=7;if(l)b|=0x80;r+=(char)b;}while(l); return r; }
    static std::string es(const std::string& s){ uint16_t l=htons(s.size()); return std::string((char*)&l,2)+s; }
    bool re(void* b,size_t n){ size_t o=0; auto*p=(char*)b; while(o<n){auto r=recv(fd_,p+o,n-o,0);if(r<=0)return false;o+=r;} return true; }
    bool rp(uint8_t& t,std::string& pl){ uint8_t h; if(!re(&h,1))return false; t=h>>4; uint32_t l=0,sh=0; while(1){uint8_t b;if(!re(&b,1))return false;l|=(b&0x7F)<<sh;sh+=7;if(!(b&0x80))break;} pl.resize(l); if(l>0&&!re(&pl[0],l))return false; return true; }
public:
    void set_will(const std::string& t, const std::string& m){ will_topic_=t; will_payload_=m; }
    bool conn(const std::string& uri,const std::string& cid,const std::string& u,const std::string& p){
        auto s=uri; if(s.find("tcp://")==0)s=s.substr(6); else if(s.find("ssl://")==0||s.find("tls://")==0)s=s.substr(6); else if(s.find("mqtt://")==0)s=s.substr(7);
        auto c=s.find(':'); h_=(c!=std::string::npos)?s.substr(0,c):s; p_=(c!=std::string::npos)?std::stoi(s.substr(c+1)):1883;
        fd_=socket(AF_INET,SOCK_STREAM,0); if(fd_<0){err_="socket() failed";return false;}
        timeval tv{5,0}; setsockopt(fd_,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)); setsockopt(fd_,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        addrinfo h{},*res; h.ai_family=AF_INET; h.ai_socktype=SOCK_STREAM;
        if(getaddrinfo(h_.c_str(),std::to_string(p_).c_str(),&h,&res)!=0){err_="DNS: "+h_+" not found"; close(fd_);fd_=-1;return false;}
        sockaddr_in a{}; memcpy(&a,res->ai_addr,sizeof(a)); freeaddrinfo(res);
        if(::connect(fd_,(sockaddr*)&a,sizeof(a))<0){err_="TCP connect refused "+h_+":"+std::to_string(p_); close(fd_);fd_=-1;return false;}
        bool has_will = !will_topic_.empty() && !will_payload_.empty();
        uint8_t flg=0x02; // clean session
        if(!u.empty()){flg|=0x80; if(!p.empty())flg|=0x40;}
        if(has_will){ flg|=0x04; flg|=(1<<3); } // Will flag + Will QoS 1 + Will Retain
        std::string v;
        v.push_back(0x00); v.push_back(0x04); v+="MQTT";
        v.push_back(0x04); v.push_back((char)flg);
        v.push_back(0x00); v.push_back(0x3C); // keepalive 60s
        v+=es(cid);
        if(has_will){ v+=es(will_topic_); v+=es(will_payload_); }
        if(!u.empty()){v+=es(u); if(!p.empty())v+=es(p);}
        std::string pk; pk+=(char)0x10; pk+=el(v.size()); pk+=v;
        // 打印发送的包（hex）
        std::cerr<<"[MQTT DEBUG] CONNECT packet ("<<pk.size()<<" bytes): ";
        for(size_t i=0;i<pk.size()&&i<40;i++) std::cerr<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)(unsigned char)pk[i]<<" ";
        std::cerr<<std::dec<<std::endl;
        send(fd_,pk.c_str(),pk.size(),0);
        std::string pl; uint8_t tp; if(!rp(tp,pl)){err_="no CONNACK response"; close(fd_);fd_=-1;return false;}
        std::cerr<<"[MQTT DEBUG] CONNACK type="<<(int)tp<<" pl.size="<<pl.size();
        if(pl.size()>=2) std::cerr<<" code="<<(int)(unsigned char)pl[1];
        std::cerr<<std::endl;
        if(tp!=2||pl.size()<2){err_="bad CONNACK type="+std::to_string(tp)+" len="+std::to_string(pl.size()); close(fd_);fd_=-1;return false;}
        if(pl[1]==5){err_="CONNACK: not authorized (username/password)"; close(fd_);fd_=-1;return false;}
        if(pl[1]!=0){err_="CONNACK code="+std::to_string((int)(unsigned char)pl[1]); close(fd_);fd_=-1;return false;}
        ok_=true; return true;
    }
    std::string error(){return err_;}
    bool sub(const std::string& t,int q=1){ if(!ok_)return false; uint16_t i=htons(pid_++); std::string v; v.append((char*)&i,2); v+=es(t); v+=(char)q; std::string pk; pk+=(char)0x82; pk+=el(v.size()); pk+=v; send(fd_,pk.c_str(),pk.size(),0); return true; }
    bool pub(const std::string& t,const std::string& pl,int q=1){ if(!ok_)return false; std::string v; v+=es(t); if(q>0){uint16_t i=htons(pid_++);v.append((char*)&i,2);} v+=pl; uint8_t h=(uint8_t)(0x30|(q<<1)); std::string pk; pk+=(char)h; pk+=el(v.size()); pk+=v; send(fd_,pk.c_str(),pk.size(),0); return true; }
    void disc(){ ok_=false; if(fd_>=0){char d[]={(char)0xE0,0x00};send(fd_,d,2,0);close(fd_);fd_=-1;} }
    bool ok(){return ok_;}
};

// ============================================================
// 设备模拟器
// ============================================================
struct Sim {
    struct Cfg { std::string url="http://127.0.0.1:9080", mqtt_uri="tcp://127.0.0.1:1883", hwuid, mk="01KX5M2KM8EBW9G1CWVMJ94TSK"; int hb=30,dur=0; bool no_mqtt=false; };
    Cfg c_; Http http_; Mqtt mqtt_;
    std::atomic<bool> run_{true};
    std::string did_,tid_,pid_,tok_,bri_,mc_,fw_,tname_;
    int stat_=0;

    Sim(const Cfg& c):c_(c),http_(c.url){ if(c_.hwuid.empty())c_.hwuid="SIM-"+U::rhex(8); }
    ~Sim(){ mqtt_.disc(); }

    void L(const std::string& m){ std::cerr<<"[Sim] "<<m<<std::endl; }

    // 阶段1: 激活
    bool activate(){
        L("══════ 阶段1: HTTP设备激活 ══════");
        std::ostringstream b; b<<"{\"uid\":\""<<c_.hwuid<<"\",\"model_key\":\""<<c_.mk<<"\"}";
        L("→ POST /api/v1/device/activate  uid="+c_.hwuid+" mk="+c_.mk);
        auto r=http_.post("/api/v1/device/activate",b.str());
        L("← HTTP "+std::to_string(r.s));
        if(r.s!=201&&r.s!=200){ L("✗ 激活失败: "+r.b.substr(0,200)); return false; }
        did_=U::jg(r.b,"device_id"); tid_=U::jg(r.b,"tenant_id"); pid_=U::jg(r.b,"product_id");
        tok_=U::jg(r.b,"activation_token"); bri_=U::jg(r.b,"mqtt_broker_uri");
        mc_=U::jg(r.b,"model_code"); fw_=U::jg(r.b,"firmware_version");
        L("✓ 激活成功 device="+did_+" model="+mc_+" fw="+fw_);
        resolve_mqtt_tenant();
        save_creds(); return true;
    }
    void resolve_mqtt_tenant(){
        auto kr=http_.get("/api/v1/mqtt-tenant-key");
        std::string key=U::jg(kr.b,"tenant_key");
        if(!key.empty()&&key!="null"){
            auto vr=http_.get("/api/v1/mqtt-tenant-key/verify?key="+key);
            std::string nm=U::jg(vr.b,"tenant_name");
            if(!nm.empty()){ tname_=nm; L("  MQTT租户: "+tname_+" (接口)"); return; }
        }
        if(!tname_.empty()) L("  MQTT租户: "+tname_+" (缓存)");
    }
    bool try_load(){
        std::ifstream f("./device_creds.json"); if(!f.is_open())return false;
        std::string j((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        did_=U::jg(j,"device_id"); tid_=U::jg(j,"tenant_id"); pid_=U::jg(j,"product_id");
        tok_=U::jg(j,"token"); bri_=U::jg(j,"broker_uri"); mc_=U::jg(j,"model_code");
        fw_=U::jg(j,"firmware_version"); tname_=U::jg(j,"mqtt_tenant");
        if(did_.empty())return false;
        L("✓ 已激活(本地凭证) device="+did_+" model="+mc_+" MQTT租户="+tid_);
        return true;
    }
    void save_creds(){ std::ofstream f("./device_creds.json");
        f<<"{\"device_id\":\""<<did_<<"\",\"tenant_id\":\""<<tid_
         <<"\",\"product_id\":\""<<pid_<<"\",\"token\":\""<<tok_
         <<"\",\"broker_uri\":\""<<bri_<<"\",\"model_code\":\""<<mc_
         <<"\",\"firmware_version\":\""<<fw_
         <<"\",\"mqtt_tenant\":\""<<tname_<<"\"}"; }

    // 阶段2: MQTT
    bool connect_mqtt(){
        if(c_.no_mqtt){ L("⚠ MQTT skipped"); return true; }
        std::string mqtt_user = (tname_.empty() ? tid_ : tname_) + "/" + did_;
        std::string base=tid_+"/iot/"+pid_+"/"+did_;
        L("══════ 阶段2: MQTT连接 ══════");
        L("  broker:   "+bri_);
        L("  clientId: "+did_);
        L("  username: "+mqtt_user);
        // Set Will Message: broker publishes "offline" if device disconnects abnormally
        mqtt_.set_will(base+"/status", "{\"status\":\"offline\"}");
        if(!mqtt_.conn(bri_,did_,mqtt_user,"")){ L("✗ MQTT失败: "+mqtt_.error()); return false; }
        mqtt_.sub(base+"/property/set"); mqtt_.sub(base+"/ota/notify"); mqtt_.sub(base+"/command/+");
        // Publish retained "online" — cloud sees device is alive via MQTT state
        mqtt_.pub(base+"/status","{\"status\":\"online\"}",1);
        L("✓ MQTT已连接, 已订阅下行Topic");
        mqtt_.pub(base+"/heartbeat","{\"status\":0,\"ts\":\""+U::now()+"\"}");
        return true;
    }

    // 阶段3: 运行
    void run(){
        L("══════ 阶段3: 运行 (心跳"+std::to_string(c_.hb)+"s) ══════");
        config(); recipes();
        auto st=std::chrono::steady_clock::now(); int cy=0;
        while(run_){
            cy++; auto e=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-st).count();
            L("── 周期 "+std::to_string(cy)+" (t+"+std::to_string(e)+"s) ──");
            heartbeat(); if(cy%3==0) properties(); events(cy);
            if(c_.dur>0&&e>=c_.dur){ run_=false; break; }
            for(int i=0;i<c_.hb&&run_;i++)std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    void stop(){ run_=false; }

    void heartbeat(){
        std::ostringstream b; b<<"{\"device_id\":\""<<did_<<"\",\"timestamp\":\""<<U::now()<<"\",\"status\":"<<stat_<<",\"firmware_version\":\""<<fw_<<"\",\"signal_strength\":"<<(80+rand()%20)<<",\"alarm_count\":0}";
        if(mqtt_.ok()){ mqtt_.pub(tid_+"/iot/"+pid_+"/"+did_+"/heartbeat",b.str()); L("[MQTT] heartbeat"); }
        else{ auto r=http_.post("/api/v1/device/heartbeat",b.str()); L("[HTTP] heartbeat → "+std::to_string(r.s)); }
    }
    void properties(){
        std::ostringstream b; b<<"{\"device_id\":\""<<did_<<"\",\"properties\":{\"cpu_temp_c\":"<<(45+rand()%20)<<",\"water_temp_c\":"<<(85+rand()%10)<<"}}";
        if(mqtt_.ok()){ mqtt_.pub(tid_+"/iot/"+pid_+"/"+did_+"/property/post",b.str()); L("[MQTT] properties"); }
    }
    void events(int cy){
        auto ev=[this](const std::string& t,const std::string& d){ std::string b="{\"event_type\":\""+t+"\","+d.substr(1); if(mqtt_.ok())mqtt_.pub(tid_+"/iot/"+pid_+"/"+did_+"/event/post",b); };
        if(cy==3){ stat_=1; ev("order_status","{\"order_id\":\"S"+U::rhex(3)+"\",\"status\":2}"); L("[Event] 制作中"); }
        if(cy==4){ stat_=0; ev("order_status","{\"status\":3}"); L("[Event] 完成"); }
        if(cy==8){ stat_=2; ev("fault_alert","{\"fault_code\":3,\"fault_level\":3,\"description\":\"水泵异常(模拟)\"}"); L("[Event] 故障L3"); }
        if(cy==12){ stat_=0; ev("fault_resolved","{\"fault_code\":3}"); L("[Event] 恢复"); }
    }
    void config(){ auto r=http_.get("/api/v1/config"); L("[Config] HTTP "+std::to_string(r.s)); }
    void recipes(){ auto r=http_.get("/api/v1/recipes"); int n=0; for(size_t p=0;(p=r.b.find("\"recipe_id\":\"",p))!=std::string::npos;p++)n++; L("[Recipes] HTTP "+std::to_string(r.s)+" count="+std::to_string(n)); }
};

static Sim* gs=nullptr;
void sh(int){ if(gs)gs->stop(); }

int main(int argc,char* argv[]){
    std::cerr<<std::unitbuf; signal(SIGINT,sh); signal(SIGTERM,sh);
    Sim::Cfg c;
    for(int i=1;i<argc;i++){ std::string a=argv[i];
        if(a=="--url"&&i+1<argc) c.url=argv[++i];
        else if(a=="--mqtt"&&i+1<argc) c.mqtt_uri=argv[++i];
        else if(a=="--model-key"&&i+1<argc) c.mk=argv[++i];
        else if(a=="--interval"&&i+1<argc) c.hb=std::stoi(argv[++i]);
        else if(a=="--duration"&&i+1<argc) c.dur=std::stoi(argv[++i]);
        else if(a=="--hw-uid"&&i+1<argc) c.hwuid=argv[++i];
        else if(a=="--skip-mqtt") c.no_mqtt=true;
        else if(a=="--reactivate"){ std::remove("./device_creds.json"); std::cerr<<"[Sim] 凭证已清除"<<std::endl; }
        else if(a=="--help"){ std::cerr<<R"(
Usage: device_simulator [OPTIONS]
  --url URL        云端地址 (default: http://127.0.0.1:9080)
  --mqtt URI       MQTT Broker (default: tcp://127.0.0.1:1883)
  --model-key KEY  型号Key (26-char ULID)
  --interval SEC   心跳间隔 (default: 30)
  --duration SEC   运行时长,0=永久 (default: 0)
  --skip-mqtt      纯HTTP模式
  --reactivate     清除凭证重新激活
)"; return 0; }
    }

    std::cerr<<"\n═══ IoT Device Simulator v3.0 ═══\n  URL: "<<c.url<<"  MQTT: "<<c.mqtt_uri<<"  model_key: "<<c.mk<<"\n"<<std::endl;
    Sim sim(c); gs=&sim;

    // 阶段1: 激活或复用凭证
    if(!sim.try_load()){
        if(!sim.activate()){ std::cerr<<"[FATAL] 激活失败, 云端是否运行? "<<c.url<<std::endl; return 1; }
    }

    // 阶段2: MQTT
    sim.connect_mqtt();

    // 阶段3: 运行
    sim.run();
    std::cerr<<"\n[Sim] 结束"<<std::endl;
    return 0;
}
