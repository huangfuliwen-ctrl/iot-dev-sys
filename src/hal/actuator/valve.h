#pragma once

#include "actuator_base.h"

namespace dev_sys::hal {

// ============================================================
// Solenoid Valve (on/off control)
// ============================================================
class Valve : public ActuatorBase {
public:
    explicit Valve(int gpio_pin, bool normally_closed = true);

    bool init() override;
    bool start() override;  // open valve
    bool stop() override;   // close valve
    bool is_running() const override; // true = open
    uint32_t run_time_ms() const override;

private:
    int gpio_pin_;
    bool normally_closed_;
    bool open_ = false;
};

} // namespace dev_sys::hal
