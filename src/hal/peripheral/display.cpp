#include "display.h"

namespace dev_sys::hal {

Display::Display() = default;
Display::~Display() = default;

bool Display::init(int width, int height) {
    // TODO: Initialize framebuffer / display driver
    return true;
}

void Display::show_page(const std::string& page_id) {
    // TODO: Render page by ID (standby/menu/order/payment/brewing/maintenance)
}

void Display::update_progress(int percent, const std::string& message) {
    // TODO: Update brewing/OTA progress bar and message
}

void Display::show_qr_code(const std::string& url) {
    // TODO: Generate and display QR code for the given URL
}

void Display::set_brightness(int percent) {
    // TODO: Adjust backlight PWM
}

void Display::sleep() {
    // TODO: Turn off backlight
}

void Display::wake() {
    // TODO: Turn on backlight
}

} // namespace dev_sys::hal
