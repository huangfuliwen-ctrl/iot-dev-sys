#pragma once

#include "sensor_base.h"

namespace dev_sys::hal {

// ============================================================
// Temperature Sensor (1-Wire DS18B20 / NTC ADC)
// ============================================================
class TempSensor : public SensorBase {
public:
    enum class Type { DS18B20, NTC_ADC };

    explicit TempSensor(Type type, int bus_id, int addr = 0);

    bool init() override;
    bool is_healthy() const override;
    double read_value() override;  // returns Celsius
    std::string name() const override { return "TempSensor"; }

private:
    Type type_;
    int bus_id_;
    int addr_;
    double last_value_ = 0.0;
};

} // namespace dev_sys::hal
