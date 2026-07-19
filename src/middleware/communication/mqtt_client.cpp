#include "mqtt_client.h"
#include "dev_sys/common/constants.h"
#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>

#ifdef HAS_PAHO
#include <mqtt/async_client.h>

namespace dev_sys {

class PahoCallback;

struct MqttClient::Impl {
    std::string broker_uri;
    std::string client_id;
    std::atomic<bool> connected{false};
    bool reconnect_enabled = true;
    MqttMessageCallback msg_cb;
    std::string ca_cert_path;
    std::string device_cert_path;
    std::string device_key_path;
    std::string will_topic;
    std::string will_payload;
    int will_qos = 1;
    std::unique_ptr<mqtt::async_client> async_client;
    std::unique_ptr<PahoCallback> paho_cb;
};

class PahoCallback : public virtual mqtt::callback {
public:
    PahoCallback(MqttClient::Impl* impl) : impl_(impl) {}
    void connected(const std::string&) override {
        std::cout << "[MqttClient] Paho connected" << std::endl;
    }
    void connection_lost(const std::string& cause) override {
        std::cerr << "[MqttClient] Connection lost: " << cause << std::endl;
        if (impl_) impl_->connected = false;
    }
    void message_arrived(mqtt::const_message_ptr msg) override {
        if (impl_ && impl_->msg_cb && msg)
            impl_->msg_cb(msg->get_topic(), msg->get_payload_str());
    }
    void delivery_complete(mqtt::delivery_token_ptr) override {}
private:
    MqttClient::Impl* impl_;
};

MqttClient::MqttClient() : impl_(std::make_unique<Impl>()) {}
MqttClient::~MqttClient() { disconnect(); }

StatusCode MqttClient::connect(const std::string& broker_uri,
                                const std::string& client_id,
                                const std::string& ca_cert_path,
                                const std::string& device_cert_path,
                                const std::string& device_key_path,
                                const std::string& username,
                                const std::string& password) {
    impl_->broker_uri = broker_uri;
    impl_->client_id = client_id;
    impl_->ca_cert_path = ca_cert_path;
    impl_->device_cert_path = device_cert_path;
    impl_->device_key_path = device_key_path;

    try {
        // Use MQTT v3.1.1 (Paho 1.2 has v5 issues)
        impl_->async_client = std::make_unique<mqtt::async_client>(broker_uri, client_id);
        impl_->paho_cb = std::make_unique<PahoCallback>(impl_.get());
        impl_->async_client->set_callback(*impl_->paho_cb);

        mqtt::connect_options conn_opts;
        conn_opts.set_mqtt_version(MQTTVERSION_3_1_1);
        conn_opts.set_keep_alive_interval(MQTT_DEFAULT_KEEPALIVE_SEC);
        conn_opts.set_clean_session(true);
        conn_opts.set_automatic_reconnect(impl_->reconnect_enabled);
        if (!username.empty()) conn_opts.set_user_name(username);
        if (!password.empty()) conn_opts.set_password(password);

        bool use_tls = broker_uri.find("ssl://") == 0 ||
                       broker_uri.find("tls://") == 0 ||
                       !ca_cert_path.empty();
        if (use_tls) {
            mqtt::ssl_options ssl_opts;
            if (!ca_cert_path.empty()) ssl_opts.set_trust_store(ca_cert_path);
            if (!device_cert_path.empty()) ssl_opts.set_key_store(device_cert_path);
            if (!device_key_path.empty()) ssl_opts.set_private_key(device_key_path);
            ssl_opts.set_ssl_version(MQTT_SSL_VERSION_TLS_1_2);
            ssl_opts.set_verify(true);
            conn_opts.set_ssl(ssl_opts);
        }

        if (!impl_->will_topic.empty()) {
            mqtt::message will_msg(impl_->will_topic, impl_->will_payload,
                                    impl_->will_qos, true);
            conn_opts.set_will(will_msg);
        }

        auto conn_tok = impl_->async_client->connect(conn_opts);
        if (conn_tok->wait_for(std::chrono::seconds(10))) {
            impl_->connected = true;
            std::cout << "[MqttClient] Connected: " << broker_uri << std::endl;
            return StatusCode::OK;
        } else {
            std::cerr << "[MqttClient] Connect timeout after 10s" << std::endl;
            impl_->async_client.reset();
            impl_->paho_cb.reset();
            return StatusCode::COMM_DISCONNECTED;
        }
    } catch (const mqtt::exception& e) {
        std::cerr << "[MqttClient] Connect failed: " << e.what() << std::endl;
        impl_->async_client.reset();
        impl_->paho_cb.reset();
        return StatusCode::COMM_DISCONNECTED;
    }
}

StatusCode MqttClient::disconnect() {
    try {
        if (impl_->async_client && impl_->connected)
            impl_->async_client->disconnect()->wait();
    } catch (const mqtt::exception& e) {
        std::cerr << "[MqttClient] Disconnect: " << e.what() << std::endl;
    }
    impl_->async_client.reset();
    impl_->paho_cb.reset();
    impl_->connected = false;
    return StatusCode::OK;
}

bool MqttClient::is_connected() const { return impl_->connected; }

StatusCode MqttClient::publish(const std::string& topic, const std::string& payload,
                                int qos, bool retain) {
    if (!impl_->connected || !impl_->async_client) return StatusCode::COMM_DISCONNECTED;
    try {
        auto msg = mqtt::make_message(topic, payload);
        msg->set_qos(qos);
        msg->set_retained(retain);
        impl_->async_client->publish(msg)->wait();
        return StatusCode::OK;
    } catch (const mqtt::exception& e) {
        std::cerr << "[MqttClient] Publish: " << e.what() << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }
}

StatusCode MqttClient::subscribe(const std::string& topic, int qos) {
    if (!impl_->connected || !impl_->async_client) return StatusCode::COMM_DISCONNECTED;
    try {
        impl_->async_client->subscribe(topic, qos)->wait();
        return StatusCode::OK;
    } catch (const mqtt::exception& e) {
        std::cerr << "[MqttClient] Subscribe: " << e.what() << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }
}

StatusCode MqttClient::unsubscribe(const std::string& topic) {
    if (!impl_->connected || !impl_->async_client) return StatusCode::COMM_DISCONNECTED;
    try {
        impl_->async_client->unsubscribe(topic)->wait();
        return StatusCode::OK;
    } catch (const mqtt::exception& e) {
        std::cerr << "[MqttClient] Unsubscribe: " << e.what() << std::endl;
        return StatusCode::COMM_DISCONNECTED;
    }
}

void MqttClient::set_message_callback(MqttMessageCallback cb) { impl_->msg_cb = std::move(cb); }
StatusCode MqttClient::set_will_message(const std::string& t, const std::string& p, int q) {
    impl_->will_topic = t; impl_->will_payload = p; impl_->will_qos = q; return StatusCode::OK;
}
void MqttClient::set_reconnect_enabled(bool e) { impl_->reconnect_enabled = e; }
void MqttClient::on_connection_lost() { impl_->connected = false; }
void MqttClient::on_message(const std::string& t, const std::string& p) {
    if (impl_->msg_cb) impl_->msg_cb(t, p);
}

} // namespace dev_sys

#else

// Embedded MQTT 3.1.1 client (POSIX, zero deps)
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>

namespace dev_sys {

static std::string el(uint32_t l){std::string r;do{uint8_t b=l&0x7F;l>>=7;if(l)b|=0x80;r+=(char)b;}while(l);return r;}
static std::string es(const std::string& s){uint16_t l=htons(s.size());return std::string((char*)&l,2)+s;}
static bool re(int fd,void*b,size_t n){size_t o=0;char*p=(char*)b;while(o<n){auto r=recv(fd,p+o,n-o,0);if(r<=0)return false;o+=r;}return true;}
static bool rp(int fd,uint8_t&t,std::string&pl){uint8_t h;if(!re(fd,&h,1))return false;t=h>>4;uint32_t l=0,sh=0;while(1){uint8_t b;if(!re(fd,&b,1))return false;l|=(b&0x7F)<<sh;sh+=7;if(!(b&0x80))break;}pl.resize(l);if(l>0&&!re(fd,&pl[0],l))return false;return true;}

struct MqttClient::Impl {
    std::string broker_uri, client_id, broker_host, username, password;
    int broker_port=1883, fd=-1;
    std::atomic<bool> connected{false}, running{false};
    MqttMessageCallback msg_cb;
    std::string will_topic, will_payload;
    int will_qos=1; uint16_t pid_counter=1;
    std::thread recv_thread;
};

static void parse_uri(const std::string& uri, std::string& host, int& port){
    host="127.0.0.1"; port=1883; std::string s=uri;
    if(s.find("tcp://")==0)s=s.substr(6); else if(s.find("mqtt://")==0)s=s.substr(7);
    auto p=s.find(':'); if(p!=std::string::npos){host=s.substr(0,p);port=std::stoi(s.substr(p+1));}
    else if(!s.empty())host=s;
}

MqttClient::MqttClient():impl_(std::make_unique<Impl>()){}
MqttClient::~MqttClient(){disconnect();}

StatusCode MqttClient::connect(const std::string& u,const std::string& c,const std::string&,
    const std::string&,const std::string&,const std::string& usr,const std::string& pwd){
    impl_->broker_uri=u; impl_->client_id=c; impl_->username=usr; impl_->password=pwd;
    parse_uri(u,impl_->broker_host,impl_->broker_port);
    std::cerr<<"[MqttClient] Embedded connecting to "<<impl_->broker_host<<":"<<impl_->broker_port<<std::endl;
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0){std::cerr<<"[MqttClient] socket failed"<<std::endl;return StatusCode::COMM_DISCONNECTED;}
    timeval tv{5,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    addrinfo h{},*res; h.ai_family=AF_INET; h.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(impl_->broker_host.c_str(),std::to_string(impl_->broker_port).c_str(),&h,&res)!=0){std::cerr<<"[MqttClient] DNS failed"<<std::endl;close(fd);return StatusCode::COMM_DISCONNECTED;}
    sockaddr_in a{}; memcpy(&a,res->ai_addr,sizeof(a)); freeaddrinfo(res);
    if(::connect(fd,(sockaddr*)&a,sizeof(a))<0){std::cerr<<"[MqttClient] TCP connect failed"<<std::endl;close(fd);return StatusCode::COMM_DISCONNECTED;}
    bool has_will=!impl_->will_topic.empty();
    uint8_t flg=0x02; if(!usr.empty()){flg|=0x80;if(!pwd.empty())flg|=0x40;}
    if(has_will){flg|=0x04;flg|=(1<<3);}
    std::string v; v.push_back(0x00);v.push_back(0x04);v+="MQTT"; v.push_back(0x04);v.push_back((char)flg);
    v.push_back(0x00);v.push_back(0x3C); v+=es(c);
    if(has_will){v+=es(impl_->will_topic);v+=es(impl_->will_payload);}
    if(!usr.empty()){v+=es(usr);if(!pwd.empty())v+=es(pwd);}
    std::string pk; pk+=(char)0x10;pk+=el(v.size());pk+=v; send(fd,pk.c_str(),pk.size(),0);
    std::string pl; uint8_t tp;
    if(!rp(fd,tp,pl)||tp!=2||pl.size()<2||pl[1]!=0){std::cerr<<"[MqttClient] CONNACK failed code="<<(pl.size()>=2?(int)(unsigned char)pl[1]:-1)<<std::endl;close(fd);return StatusCode::COMM_DISCONNECTED;}
    impl_->fd=fd; impl_->connected=true; impl_->running=true;
    std::cout<<"[MqttClient] MQTT connected!"<<std::endl;
    impl_->recv_thread=std::thread([this](){std::cerr<<"[MqttClient] recv thread started"<<std::endl;while(impl_->running){uint8_t tp;std::string pl;
        if(!rp(impl_->fd,tp,pl)){impl_->connected=false;break;}
        if(tp==3&&impl_->msg_cb){size_t tl=(uint8_t)pl[0]<<8|(uint8_t)pl[1];
            std::string topic=pl.substr(2,tl); size_t po=2+tl; if(((tp>>1)&1))po+=2;
            std::cerr<<"[MqttClient] ← PUBLISH "<<topic<<std::endl;
            impl_->msg_cb(topic,pl.substr(po));}}}); return StatusCode::OK;
}
StatusCode MqttClient::disconnect(){impl_->running=false; if(impl_->fd>=0){char d[]={(char)0xE0,0x00};send(impl_->fd,d,2,0);close(impl_->fd);impl_->fd=-1;} impl_->connected=false; if(impl_->recv_thread.joinable())impl_->recv_thread.join(); return StatusCode::OK;}
bool MqttClient::is_connected()const{return impl_->connected;}
StatusCode MqttClient::publish(const std::string& t,const std::string& p,int q,bool){if(!impl_->connected||impl_->fd<0)return StatusCode::COMM_DISCONNECTED; std::string v;v+=es(t);if(q>0){uint16_t i=htons(impl_->pid_counter++);v.append((char*)&i,2);}v+=p;uint8_t h=(uint8_t)(0x30|(q<<1));std::string pk;pk+=(char)h;pk+=el(v.size());pk+=v;send(impl_->fd,pk.c_str(),pk.size(),0);return StatusCode::OK;}
StatusCode MqttClient::subscribe(const std::string& t,int q){if(!impl_->connected||impl_->fd<0)return StatusCode::COMM_DISCONNECTED;uint16_t i=htons(impl_->pid_counter++);std::string v;v.append((char*)&i,2);v+=es(t);v+=(char)q;std::string pk;pk+=(char)0x82;pk+=el(v.size());pk+=v;send(impl_->fd,pk.c_str(),pk.size(),0);return StatusCode::OK;}
StatusCode MqttClient::unsubscribe(const std::string&){return StatusCode::OK;}
void MqttClient::set_message_callback(MqttMessageCallback cb){impl_->msg_cb=std::move(cb);}
StatusCode MqttClient::set_will_message(const std::string& t,const std::string& p,int q){impl_->will_topic=t;impl_->will_payload=p;impl_->will_qos=q;return StatusCode::OK;}
void MqttClient::set_reconnect_enabled(bool e){(void)e;}
void MqttClient::on_connection_lost(){impl_->connected=false;}
void MqttClient::on_message(const std::string& t,const std::string& p){if(impl_->msg_cb)impl_->msg_cb(t,p);}
} // namespace dev_sys
#endif
