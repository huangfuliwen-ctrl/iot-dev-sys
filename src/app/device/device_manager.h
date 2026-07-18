#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace dev_sys {

class Database;

// Callback when device goes offline
using DeviceOfflineCallback = std::function<void(const std::string& tenant_id,
                                                  const std::string& device_id)>;

// ============================================================
// Device Manager (云服务端：管理所有租户的所有设备)
// ============================================================
class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    void set_database(Database* db) { db_ = db; }

    // ======== Inbound: process device messages ========
    void process_heartbeat(const std::string& tenant_id,
                           const std::string& device_id,
                           const std::string& product_id,
                           const std::string& payload_json);

    void process_property(const std::string& tenant_id,
                          const std::string& device_id,
                          const std::string& product_id,
                          const std::string& payload_json);

    // ======== Device registration ========
    StatusCode register_device(const Device& device);
    StatusCode remove_device(const std::string& tenant_id, const std::string& device_id);
    StatusCode update_device_status(const std::string& tenant_id,
                                     const std::string& device_id,
                                     DeviceStatus status);
    StatusCode migrate_device(const std::string& device_id,
                               const std::string& new_tenant);

    // ======== Query ========
    std::optional<Device> get_device(const std::string& tenant_id,
                                      const std::string& device_id) const;
    std::vector<Device> list_devices(const std::string& tenant_id) const;
    std::vector<Device> list_all_devices() const;
    std::vector<Device> list_offline_devices(int timeout_sec = 180) const;

    // ======== Tenant ========
    std::vector<Tenant> list_tenants() const;
    int device_count(const std::string& tenant_id) const;
    int total_device_count() const;

    // ======== Offline detection ========
    void check_offline_devices(); // called periodically from main loop
    void set_offline_callback(DeviceOfflineCallback cb);

    // ======== Load/Sync ========
    StatusCode load_from_database();
    StatusCode sync_to_database(const Device& device);

private:
    struct TenantDevices {
        std::unordered_map<std::string, Device> map; // device_id -> Device
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, TenantDevices> tenants_; // tenant_id -> devices
    DeviceOfflineCallback offline_cb_;
    Database* db_ = nullptr;
};

} // namespace dev_sys
