#pragma once

#include "actuator_base.h"

namespace dev_sys::hal {

// ============================================================
// DC Motor (PWM speed + direction control)
// ============================================================
class Motor : public ActuatorBase {
public:
    explicit Motor(int pwm_pin, int dir_pin);

    bool init() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    uint32_t run_time_ms() const override;

    void set_speed(int percent);
    void set_direction(bool forward);

    // Stall detection via current sensing
    bool is_stalled() const;

private:
    int pwm_pin_;
    int dir_pin_;
    int speed_ = 100;
    bool running_ = false;
};

} // namespace dev_sys::hal
