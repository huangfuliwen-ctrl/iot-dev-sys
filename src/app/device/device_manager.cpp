#include "device_manager.h"
#include "../../middleware/storage/database.h"
#include <iostream>
#include <chrono>
#include <algorithm>

namespace dev_sys {

DeviceManager::DeviceManager() = default;
DeviceManager::~DeviceManager() = default;

// ============================================================
// Heartbeat processing (from all devices, all tenants)
// ============================================================
void DeviceManager::process_heartbeat(const std::string& tenant_id,
                                       const std::string& device_id,
                                       const std::string& product_id,
                                       const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(mutex_);

    // TODO: Parse JSON payload to extract status, firmware_version, signal_strength, alarm_count

    auto& td = tenants_[tenant_id];
    auto it = td.map.find(device_id);

    if (it == td.map.end()) {
        // New device discovered via heartbeat — auto-register
        Device dev;
        dev.tenant_id  = tenant_id;
        dev.device_id  = device_id;
        dev.product_id = product_id;
        dev.network_status = NetworkStatus::ONLINE;
        dev.work_status    = WorkStatus::IDLE;
        dev.last_heartbeat_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        dev.activated  = true;
        td.map[device_id] = dev;
        std::cout << "[DeviceMgr] Auto-registered device: "
                  << tenant_id << "/" << device_id << std::endl;
    } else {
        // Update heartbeat timestamp — device is online
        it->second.last_heartbeat_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        it->second.network_status = NetworkStatus::ONLINE;
        // TODO: parse work_status, firmware_version, signal_strength from payload
    }

    // TODO: Persist to database asynchronously
}

void DeviceManager::process_property(const std::string& tenant_id,
                                      const std::string& device_id,
                                      const std::string& product_id,
                                      const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    // TODO: Parse reported properties JSON
    // TODO: Update device state in memory + database
    // This handles device shadow (reported state)
}

// ============================================================
// Registration
// ============================================================
StatusCode DeviceManager::register_device(const Device& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    tenants_[device.tenant_id].map[device.device_id] = device;
    if (db_) {
        StatusCode sc = db_->insert_device(device);
        if (sc != StatusCode::OK) {
            std::cerr << "[DeviceMgr] Failed to persist device: " << device.device_id << std::endl;
            return sc;
        }
    }
    std::cout << "[DeviceMgr] Registered device: " << device.device_id
              << " (tenant: " << device.tenant_id << ")" << std::endl;
    return StatusCode::OK;
}

StatusCode DeviceManager::remove_device(const std::string& tenant_id,
                                         const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tenants_.find(tenant_id);
    if (it == tenants_.end()) return StatusCode::DEV_NOT_ACTIVATED;
    auto erased = it->second.map.erase(device_id);
    if (erased == 0) return StatusCode::DEV_NOT_ACTIVATED;
    if (it->second.map.empty()) tenants_.erase(it);
    std::cout << "[DeviceMgr] Removed device: " << device_id << std::endl;
    return StatusCode::OK;
}

StatusCode DeviceManager::update_device_status(const std::string& tenant_id,
                                                const std::string& device_id,
                                                NetworkStatus net_st,
                                                WorkStatus work_st) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto td_it = tenants_.find(tenant_id);
    if (td_it == tenants_.end()) return StatusCode::DEV_NOT_ACTIVATED;

    auto dev_it = td_it->second.map.find(device_id);
    if (dev_it == td_it->second.map.end()) return StatusCode::DEV_NOT_ACTIVATED;

    dev_it->second.network_status = net_st;
    dev_it->second.work_status    = work_st;
    return StatusCode::OK;
}

StatusCode DeviceManager::migrate_device(const std::string& device_id,
                                          const std::string& new_tenant) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Find device across all tenants
    for (auto& [tid, td] : tenants_) {
        auto it = td.map.find(device_id);
        if (it != td.map.end()) {
            Device dev = it->second;           // copy device
            td.map.erase(it);                  // remove from old tenant
            dev.tenant_id = new_tenant;         // update tenant_id
            tenants_[new_tenant].map[device_id] = dev;  // insert into new tenant
            return StatusCode::OK;
        }
    }
    return StatusCode::DEV_NOT_ACTIVATED;
}

std::optional<Device> DeviceManager::find_device_by_id(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [tid, td] : tenants_) {
        auto it = td.map.find(device_id);
        if (it != td.map.end()) return it->second;
    }
    return std::nullopt;
}

// ============================================================
// Query
// ============================================================
std::optional<Device> DeviceManager::get_device(const std::string& tenant_id,
                                                 const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto td_it = tenants_.find(tenant_id);
    if (td_it == tenants_.end()) return std::nullopt;

    auto dev_it = td_it->second.map.find(device_id);
    if (dev_it == td_it->second.map.end()) return std::nullopt;

    return dev_it->second;
}

std::vector<Device> DeviceManager::list_devices(const std::string& tenant_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Device> result;
    auto td_it = tenants_.find(tenant_id);
    if (td_it != tenants_.end()) {
        for (const auto& [id, dev] : td_it->second.map) {
            result.push_back(dev);
        }
    }
    return result;
}

std::vector<Device> DeviceManager::list_all_devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Device> result;
    for (const auto& [tid, td] : tenants_) {
        for (const auto& [did, dev] : td.map) {
            result.push_back(dev);
        }
    }
    return result;
}

std::vector<Device> DeviceManager::list_offline_devices(int timeout_sec) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Device> result;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& [tid, td] : tenants_) {
        for (const auto& [did, dev] : td.map) {
            if (now - dev.last_heartbeat_at > timeout_sec) {
                result.push_back(dev);
            }
        }
    }
    return result;
}

// ============================================================
// Offline detection (called periodically)
// ============================================================
void DeviceManager::check_offline_devices() {
    auto offline = list_offline_devices();
    for (const auto& dev : offline) {
        update_device_status(dev.tenant_id, dev.device_id,
                             NetworkStatus::OFFLINE, dev.work_status);
        if (offline_cb_) {
            offline_cb_(dev.tenant_id, dev.device_id);
        }
    }
}

void DeviceManager::set_offline_callback(DeviceOfflineCallback cb) {
    offline_cb_ = std::move(cb);
}

// ============================================================
// Tenant
// ============================================================
std::vector<Tenant> DeviceManager::list_tenants() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tenant> result;
    for (const auto& [tid, td] : tenants_) {
        Tenant t;
        t.tenant_id = tid;
        t.name = tid;
        t.active = true;
        result.push_back(t);
    }
    return result;
}

int DeviceManager::device_count(const std::string& tenant_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tenants_.find(tenant_id);
    return it != tenants_.end() ? static_cast<int>(it->second.map.size()) : 0;
}

int DeviceManager::total_device_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& [tid, td] : tenants_) count += td.map.size();
    return count;
}

// ============================================================
// Persistence
// ============================================================
StatusCode DeviceManager::load_from_database() {
    if (!db_) {
        std::cerr << "[DeviceMgr] No database reference, skipping load" << std::endl;
        return StatusCode::STORAGE_READ_ERROR;
    }
    auto devices = db_->list_all_devices();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& dev : devices) {
        if (dev.activated) {
            tenants_[dev.tenant_id].map[dev.device_id] = dev;
        }
    }
    std::cout << "[DeviceMgr] Loaded " << devices.size()
              << " devices from database (" << tenants_.size() << " tenants)" << std::endl;
    return StatusCode::OK;
}

StatusCode DeviceManager::sync_to_database(const Device& device) {
    if (!db_) {
        return StatusCode::STORAGE_READ_ERROR;
    }
    return db_->upsert_device(device);
}

} // namespace dev_sys
