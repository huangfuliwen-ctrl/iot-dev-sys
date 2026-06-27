#pragma once

#include "actuator_base.h"

namespace dev_sys::hal {

// ============================================================
// Heater Control (SSR + PID + PWM)
// ============================================================
class Heater : public ActuatorBase {
public:
    explicit Heater(int gpio_pin);

    bool init() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    uint32_t run_time_ms() const override;

    // PID control
    void set_target_temp(double celsius);
    double current_temp() const;
    void set_pid_params(double kp, double ki, double kd);

    // Safety
    bool is_overtemp() const;
    double max_temp_limit() const;

private:
    int gpio_pin_;
    double target_temp_ = 0.0;
    double kp_ = 2.0, ki_ = 0.1, kd_ = 0.5;
    bool running_ = false;
};

} // namespace dev_sys::hal
