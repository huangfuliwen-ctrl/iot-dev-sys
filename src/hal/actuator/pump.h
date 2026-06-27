#pragma once

#include "actuator_base.h"

namespace dev_sys::hal {

// ============================================================
// Pump Control (PWM speed control)
// ============================================================
class Pump : public ActuatorBase {
public:
    explicit Pump(int gpio_pin, int pwm_channel = 0);

    bool init() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    uint32_t run_time_ms() const override;

    void set_speed(int percent); // 0-100
    int current_speed() const;

private:
    int gpio_pin_;
    int pwm_channel_;
    int speed_ = 100;
    bool running_ = false;
};

} // namespace dev_sys::hal
