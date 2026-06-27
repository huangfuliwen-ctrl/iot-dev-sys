#include "flow_sensor.h"

namespace dev_sys::hal {

FlowSensor::FlowSensor(int gpio_pin)
    : gpio_pin_(gpio_pin) {}

bool FlowSensor::init() {
    // TODO: Configure GPIO as interrupt input (rising edge)
    // TODO: Set up timer for flow rate calculation (pulses per second -> ml/s)
    return true;
}

bool FlowSensor::is_healthy() const {
    // TODO: Check if GPIO is responding, flow rate within expected range
    return true;
}

double FlowSensor::read_value() {
    // TODO: Compute flow rate from pulse count / time interval
    // TODO: Accumulate total volume
    return flow_rate_;
}

double FlowSensor::total_volume_ml() const {
    return total_ml_;
}

void FlowSensor::reset_total() {
    total_ml_ = 0.0;
}

} // namespace dev_sys::hal
