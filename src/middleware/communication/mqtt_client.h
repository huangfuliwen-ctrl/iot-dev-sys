#pragma once

#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>
#include <functional>

namespace dev_sys {

using MqttMessageCallback = std::function<void(const std::string& topic,
                                                const std::string& payload)>;

// ============================================================
// MQTT Client (REQ-IF-001)
// ============================================================
class MqttClient {
public:
    MqttClient();
    ~MqttClient();

    // Connection
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

    // Message handler
    void set_message_callback(MqttMessageCallback cb);

    // Will message (for abnormal disconnect)
    StatusCode set_will_message(const std::string& topic,
                                 const std::string& payload, int qos = 1);

    // Reconnect
    void set_reconnect_enabled(bool enabled);

private:
    void on_connection_lost();
    void on_message(const std::string& topic, const std::string& payload);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
