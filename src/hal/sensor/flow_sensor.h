#pragma once

#include "sensor_base.h"

namespace dev_sys::hal {

// ============================================================
// Flow Sensor (Hall effect pulse counting)
// ============================================================
class FlowSensor : public SensorBase {
public:
    explicit FlowSensor(int gpio_pin);

    bool init() override;
    bool is_healthy() const override;
    double read_value() override;   // returns flow rate in ml/s
    std::string name() const override { return "FlowSensor"; }

    // Cumulative volume since last reset
    double total_volume_ml() const;
    void reset_total();

private:
    int gpio_pin_;
    double flow_rate_ = 0.0;
    double total_ml_ = 0.0;
    // TODO: pulse counter, timer for rate calculation
};

} // namespace dev_sys::hal
