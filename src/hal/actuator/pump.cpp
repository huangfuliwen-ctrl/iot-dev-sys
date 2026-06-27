#include "pump.h"

namespace dev_sys::hal {

Pump::Pump(int gpio_pin, int pwm_channel)
    : gpio_pin_(gpio_pin), pwm_channel_(pwm_channel) {}

bool Pump::init() {
    // TODO: Configure GPIO + PWM output
    return true;
}

bool Pump::start() {
    running_ = true;
    // TODO: Enable PWM output at configured speed
    return true;
}

bool Pump::stop() {
    running_ = false;
    // TODO: Disable PWM output
    return true;
}

bool Pump::is_running() const {
    return running_;
}

uint32_t Pump::run_time_ms() const {
    // TODO: Track accumulated run time
    return 0;
}

void Pump::set_speed(int percent) {
    speed_ = percent;
    // TODO: Set PWM duty cycle = percent
}

int Pump::current_speed() const {
    return speed_;
}

} // namespace dev_sys::hal
