#pragma once

#include <string>

namespace dev_sys::hal {

// ============================================================
// Display (Touch screen)
// ============================================================
class Display {
public:
    Display();
    ~Display();

    bool init(int width, int height);
    void show_page(const std::string& page_id);
    void update_progress(int percent, const std::string& message);
    void show_qr_code(const std::string& url);
    void set_brightness(int percent);
    void sleep();
    void wake();
};

} // namespace dev_sys::hal
