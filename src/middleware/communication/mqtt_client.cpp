#include "mqtt_client.h"
#include "dev_sys/common/constants.h"
#include <iostream>
#include <cstring>
#include <atomic>

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
                                const std::string& device_key_path) {
    impl_->broker_uri = broker_uri;
    impl_->client_id = client_id;
    impl_->ca_cert_path = ca_cert_path;
    impl_->device_cert_path = device_cert_path;
    impl_->device_key_path = device_key_path;

    try {
        // Use MQTT v5 (Mosquitto 2.0+ default)
        mqtt::create_options create_opts;
        create_opts.set_mqtt_verison(MQTTVERSION_5);

        impl_->async_client = std::make_unique<mqtt::async_client>(broker_uri, client_id,
                                                                    create_opts);
        impl_->paho_cb = std::make_unique<PahoCallback>(impl_.get());
        impl_->async_client->set_callback(*impl_->paho_cb);

        mqtt::connect_options conn_opts;
        conn_opts.set_mqtt_version(MQTTVERSION_5);
        conn_opts.set_keep_alive_interval(MQTT_DEFAULT_KEEPALIVE_SEC);
        conn_opts.set_clean_start(true);
        conn_opts.set_automatic_reconnect(impl_->reconnect_enabled);

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

        impl_->async_client->connect(conn_opts)->wait();
        impl_->connected = true;
        std::cout << "[MqttClient] Connected: " << broker_uri << std::endl;
        return StatusCode::OK;
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

namespace dev_sys {

struct MqttClient::Impl {
    std::string broker_uri, client_id;
    bool connected = false, reconnect_enabled = true;
    MqttMessageCallback msg_cb;
    std::string will_topic, will_payload;
    int will_qos = 1;
};

MqttClient::MqttClient() : impl_(std::make_unique<Impl>()) {}
MqttClient::~MqttClient() {}
StatusCode MqttClient::connect(const std::string& u, const std::string& c, const std::string&,
                                const std::string&, const std::string&) {
    impl_->broker_uri = u; impl_->client_id = c; impl_->connected = true;
    std::cerr << "[MqttClient] WARNING: Built without Paho C++. MQTT is stubbed." << std::endl;
    return StatusCode::OK;
}
StatusCode MqttClient::disconnect() { impl_->connected = false; return StatusCode::OK; }
bool MqttClient::is_connected() const { return impl_->connected; }
StatusCode MqttClient::publish(const std::string&, const std::string&, int, bool) {
    return impl_->connected ? StatusCode::OK : StatusCode::COMM_DISCONNECTED;
}
StatusCode MqttClient::subscribe(const std::string&, int) { return StatusCode::OK; }
StatusCode MqttClient::unsubscribe(const std::string&) { return StatusCode::OK; }
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
#endif
