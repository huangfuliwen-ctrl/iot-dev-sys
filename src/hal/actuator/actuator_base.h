#pragma once

#include <cstdint>

namespace dev_sys::hal {

// ============================================================
// Common HAL interface for all actuators
// ============================================================
class ActuatorBase {
public:
    virtual ~ActuatorBase() = default;

    virtual bool init() = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual bool is_running() const = 0;
    virtual uint32_t run_time_ms() const = 0; // accumulated run time
};

} // namespace dev_sys::hal
