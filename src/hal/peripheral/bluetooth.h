#pragma once

#include <string>

namespace dev_sys::hal {

// ============================================================
// Bluetooth BLE (Near-field configuration + diagnostics)
// ============================================================
class Bluetooth {
public:
    Bluetooth();
    ~Bluetooth();

    bool init(const std::string& device_name);
    bool start_advertising();
    bool stop_advertising();

    // PIN verification for connection
    std::string generate_pin();
    bool verify_pin(const std::string& pin);

    // WiFi provisioning
    bool configure_wifi(const std::string& ssid, const std::string& password);

private:
    std::string device_name_;
};

} // namespace dev_sys::hal
