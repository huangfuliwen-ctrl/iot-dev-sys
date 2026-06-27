#include "mqtt_client.h"
#include "dev_sys/common/constants.h"
#include <iostream>

namespace dev_sys {

struct MqttClient::Impl {
    std::string broker_uri;
    std::string client_id;
    bool connected = false;
    bool reconnect_enabled = true;
    MqttMessageCallback msg_cb;

    // Will message
    std::string will_topic;
    std::string will_payload;
    int will_qos = 1;
};

MqttClient::MqttClient()
    : impl_(std::make_unique<Impl>()) {}

MqttClient::~MqttClient() {
    disconnect();
}

StatusCode MqttClient::connect(const std::string& broker_uri,
                                const std::string& client_id,
                                const std::string& ca_cert_path,
                                const std::string& device_cert_path,
                                const std::string& device_key_path) {
    impl_->broker_uri = broker_uri;
    impl_->client_id  = client_id;

    // TODO: Initialize Eclipse Paho MQTT client
    // TODO: Configure TLS (ca cert, device cert, device key)
    // TODO: Set will message if configured
    // TODO: Connect to broker with keep-alive = MQTT_DEFAULT_KEEPALIVE_SEC
    // TODO: On connect success, subscribe to downlink topics:
    //   - iot/{product_id}/{device_id}/property/set
    //   - iot/{product_id}/{device_id}/ota/notify
    //   - iot/{product_id}/{device_id}/command/+

    impl_->connected = true;
    return StatusCode::OK;
}

StatusCode MqttClient::disconnect() {
    impl_->connected = false;
    // TODO: Graceful MQTT disconnect
    return StatusCode::OK;
}

bool MqttClient::is_connected() const {
    return impl_->connected;
}

StatusCode MqttClient::publish(const std::string& topic,
                                const std::string& payload,
                                int qos, bool retain) {
    if (!impl_->connected) {
        return StatusCode::COMM_DISCONNECTED;
    }
    // TODO: Publish to MQTT broker
    return StatusCode::OK;
}

StatusCode MqttClient::subscribe(const std::string& topic, int qos) {
    if (!impl_->connected) {
        return StatusCode::COMM_DISCONNECTED;
    }
    // TODO: Subscribe to MQTT topic
    return StatusCode::OK;
}

StatusCode MqttClient::unsubscribe(const std::string& topic) {
    if (!impl_->connected) {
        return StatusCode::COMM_DISCONNECTED;
    }
    // TODO: Unsubscribe
    return StatusCode::OK;
}

void MqttClient::set_message_callback(MqttMessageCallback cb) {
    impl_->msg_cb = std::move(cb);
}

StatusCode MqttClient::set_will_message(const std::string& topic,
                                         const std::string& payload, int qos) {
    impl_->will_topic   = topic;
    impl_->will_payload = payload;
    impl_->will_qos     = qos;
    return StatusCode::OK;
}

void MqttClient::set_reconnect_enabled(bool enabled) {
    impl_->reconnect_enabled = enabled;
}

void MqttClient::on_connection_lost() {
    impl_->connected = false;
    // TODO: Exponential backoff reconnect (1s -> 2s -> 4s -> ... -> 60s)
}

void MqttClient::on_message(const std::string& topic,
                             const std::string& payload) {
    if (impl_->msg_cb) {
        impl_->msg_cb(topic, payload);
    }
}

} // namespace dev_sys
