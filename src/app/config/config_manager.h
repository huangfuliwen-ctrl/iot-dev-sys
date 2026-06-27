#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>

namespace dev_sys {

// ============================================================
// Config Manager (REQ-CF-001)
// ============================================================
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // Load config from local storage
    StatusCode load();

    // Apply cloud-desired config (from MQTT property/set)
    StatusCode apply_cloud_config(const DeviceConfig& config);

    // Current config
    const DeviceConfig& current() const;

    // Individual getters/setters
    int32_t get_heartbeat_interval() const;
    int32_t get_order_expire_minutes() const;
    int32_t get_max_queue_depth() const;

    // Report actual config to cloud
    StatusCode report_to_cloud();

private:
    StatusCode save_to_storage(const DeviceConfig& config);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
