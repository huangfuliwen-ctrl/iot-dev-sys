#pragma once

#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>
#include <functional>

namespace dev_sys {

using MqttMessageCallback = std::function<void(const std::string& topic,
                                                const std::string& payload)>;

// ============================================================
// MQTT Client — Eclipse Paho C++ backend (REQ-IF-001)
//
// Uses mqtt::async_client for non-blocking MQTT v3.1.1 / v5.0
// communication with automatic reconnection and TLS support.
//
// Required packages:
//   sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev
//
// When built without HAS_PAHO, falls back to no-op stub.
// ============================================================
class MqttClient {
public:
    MqttClient();
    ~MqttClient();

    // Connection — broker_uri format: tcp://host:port or ssl://host:port
    StatusCode connect(const std::string& broker_uri,
                       const std::string& client_id,
                       const std::string& ca_cert_path,
                       const std::string& device_cert_path,
                       const std::string& device_key_path);
    StatusCode disconnect();
    bool is_connected() const;

    // Pub/Sub
    StatusCode publish(const std::string& topic, const std::string& payload,
                       int qos = 1, bool retain = false);
    StatusCode subscribe(const std::string& topic, int qos = 1);
    StatusCode unsubscribe(const std::string& topic);

    // Message handler — invoked on incoming MQTT messages
    void set_message_callback(MqttMessageCallback cb);

    // Will message (Last Will & Testament) — sent by broker on unexpected disconnect
    StatusCode set_will_message(const std::string& topic,
                                 const std::string& payload, int qos = 1);

    // Auto-reconnect with exponential backoff
    void set_reconnect_enabled(bool enabled);

private:
    void on_connection_lost();
    void on_message(const std::string& topic, const std::string& payload);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class PahoCallback;
};

} // namespace dev_sys
