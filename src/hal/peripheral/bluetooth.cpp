#include "bluetooth.h"

namespace dev_sys::hal {

Bluetooth::Bluetooth() = default;
Bluetooth::~Bluetooth() = default;

bool Bluetooth::init(const std::string& device_name) {
    device_name_ = device_name;
    // TODO: Initialize BLE adapter, set device name, configure services
    return true;
}

bool Bluetooth::start_advertising() {
    // TODO: Start BLE advertising
    return true;
}

bool Bluetooth::stop_advertising() {
    // TODO: Stop advertising
    return true;
}

std::string Bluetooth::generate_pin() {
    // TODO: Generate 6-digit random PIN, show on display
    return "000000";
}

bool Bluetooth::verify_pin(const std::string& pin) {
    // TODO: Verify PIN against generated one
    return true;
}

bool Bluetooth::configure_wifi(const std::string& ssid, const std::string& password) {
    // TODO: Write WiFi config to wpa_supplicant / connman
    return true;
}

} // namespace dev_sys::hal
