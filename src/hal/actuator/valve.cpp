#include "valve.h"

namespace dev_sys::hal {

Valve::Valve(int gpio_pin, bool normally_closed)
    : gpio_pin_(gpio_pin), normally_closed_(normally_closed) {}

bool Valve::init() {
    // TODO: Configure GPIO output (init to closed state)
    return true;
}

bool Valve::start() {
    open_ = true;
    // TODO: Set GPIO high (or low, depending on normally_closed_)
    return true;
}

bool Valve::stop() {
    open_ = false;
    // TODO: Set GPIO to closed state
    return true;
}

bool Valve::is_running() const {
    return open_;
}

uint32_t Valve::run_time_ms() const {
    return 0;
}

} // namespace dev_sys::hal
