#include "motor.h"

namespace dev_sys::hal {

Motor::Motor(int pwm_pin, int dir_pin)
    : pwm_pin_(pwm_pin), dir_pin_(dir_pin) {}

bool Motor::init() {
    // TODO: Configure PWM + direction GPIO
    return true;
}

bool Motor::start() {
    running_ = true;
    // TODO: Enable PWM output
    return true;
}

bool Motor::stop() {
    running_ = false;
    // TODO: Disable PWM, brake or coast
    return true;
}

bool Motor::is_running() const {
    return running_;
}

uint32_t Motor::run_time_ms() const {
    return 0;
}

void Motor::set_speed(int percent) {
    speed_ = percent;
}

void Motor::set_direction(bool forward) {
    // TODO: Set direction GPIO
}

bool Motor::is_stalled() const {
    // TODO: Check current sense resistor / hall sensor feedback
    return false;
}

} // namespace dev_sys::hal
