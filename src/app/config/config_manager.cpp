#include "config_manager.h"
#include <iostream>

namespace dev_sys {

struct ConfigManager::Impl {
    DeviceConfig config;
};

ConfigManager::ConfigManager()
    : impl_(std::make_unique<Impl>()) {}

ConfigManager::~ConfigManager() = default;

StatusCode ConfigManager::load() {
    // TODO: Load config from SQLite / JSON file
    impl_->config = DeviceConfig{}; // defaults
    return StatusCode::OK;
}

StatusCode ConfigManager::apply_cloud_config(const DeviceConfig& config) {
    DeviceConfig old = impl_->config;
    impl_->config = config;

    // Log config change
    // TODO: Log old vs new values for audit

    return save_to_storage(config);
}

StatusCode ConfigManager::save_to_storage(const DeviceConfig& config) {
    // TODO: Persist to SQLite / JSON file
    return StatusCode::OK;
}

StatusCode ConfigManager::report_to_cloud() {
    // TODO: Publish current config to MQTT property/post
    return StatusCode::OK;
}

const DeviceConfig& ConfigManager::current() const {
    return impl_->config;
}

int32_t ConfigManager::get_heartbeat_interval() const {
    return impl_->config.heartbeat_interval;
}

int32_t ConfigManager::get_order_expire_minutes() const {
    return impl_->config.order_expire_minutes;
}

int32_t ConfigManager::get_max_queue_depth() const {
    return impl_->config.max_queue_depth;
}

} // namespace dev_sys
