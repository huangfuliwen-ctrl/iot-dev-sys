#include "heater.h"
#include "dev_sys/common/constants.h"

namespace dev_sys::hal {

Heater::Heater(int gpio_pin)
    : gpio_pin_(gpio_pin) {}

bool Heater::init() {
    // TODO: Configure GPIO output + SSR control
    // TODO: Initialize PID controller
    return true;
}

bool Heater::start() {
    // TODO: Enable PID loop
    running_ = true;
    return true;
}

bool Heater::stop() {
    // TODO: Disable PID loop, turn off SSR
    running_ = false;
    return true;
}

bool Heater::is_running() const {
    return running_;
}

uint32_t Heater::run_time_ms() const {
    // TODO: Track accumulated run time
    return 0;
}

void Heater::set_target_temp(double celsius) {
    target_temp_ = celsius;
    // TODO: Update PID setpoint
}

double Heater::current_temp() const {
    // TODO: Read from TempSensor
    return 0.0;
}

void Heater::set_pid_params(double kp, double ki, double kd) {
    kp_ = kp; ki_ = ki; kd_ = kd;
    // TODO: Update PID gains
}

bool Heater::is_overtemp() const {
    return current_temp() > max_temp_limit();
}

double Heater::max_temp_limit() const {
    return 105.0; // Celsius, safety cutoff
}

} // namespace dev_sys::hal
