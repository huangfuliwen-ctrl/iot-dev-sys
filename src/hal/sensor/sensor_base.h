#pragma once

#include <cstdint>
#include <string>

namespace dev_sys::hal {

// ============================================================
// Common HAL interface for all sensors
// ============================================================
class SensorBase {
public:
    virtual ~SensorBase() = default;

    virtual bool init() = 0;
    virtual bool is_healthy() const = 0;
    virtual double read_value() = 0;
    virtual std::string name() const = 0;
};

} // namespace dev_sys::hal
