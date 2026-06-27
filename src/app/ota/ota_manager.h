#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

namespace dev_sys {

// ============================================================
// OTA Manager (云服务端：固件版本管理 + 推送编排 + 进度追踪)
//
// 设备端负责实际的下载/安装/回滚
// 云服务端负责：版本注册 → 灰度推送 → 进度监控 → 结果统计
// ============================================================
class OtaManager {
public:
    OtaManager();
    ~OtaManager();

    // ======== Inbound: OTA progress from devices ========
    void process_ota_progress(const std::string& tenant_id,
                               const std::string& device_id,
                               const std::string& payload_json);

    // ======== Firmware version registry ========
    StatusCode register_firmware(const FirmwareVersion& fw);
    std::vector<FirmwareVersion> list_firmwares() const;
    std::optional<FirmwareVersion> get_firmware(const std::string& version) const;
    StatusCode delete_firmware(const std::string& version);

    // ======== OTA Orchestration ========
    // Push upgrade to a single device
    StatusCode push_to_device(const std::string& tenant_id,
                               const std::string& product_id,
                               const std::string& device_id,
                               const std::string& target_version);

    // Grayscale rollout: push to N% of devices
    StatusCode push_grayscale(const std::string& product_id,
                               const std::string& target_version,
                               int percent);

    // Push to all devices of a product
    StatusCode push_all(const std::string& product_id,
                         const std::string& target_version);

    // ======== Progress tracking ========
    std::vector<OtaRecord> active_upgrades() const;
    OtaRecord get_device_ota_status(const std::string& device_id) const;

    // Rollout statistics
    struct RolloutStats {
        int total_targeted;
        int downloading;
        int installing;
        int completed;
        int failed;
        int rolled_back;
    };
    RolloutStats get_rollout_stats(const std::string& version) const;

    // ======== Outbound notification builder ========
    std::string build_ota_notify_payload(const std::string& target_version,
                                          const std::string& download_url,
                                          const std::string& checksum,
                                          bool force) const;

    // Mock data for frontend development
    void seed_mock_data();
    void set_database(class Database* db) { db_ = db; }
    StatusCode load_from_database();

    // Device manager reference for grayscale push (queries device list)
    void set_device_manager(class DeviceManager* mgr) { device_mgr_ = mgr; }

    // Caller publishes via MessageRouter::send_ota_notify() → MQTT

private:
    mutable std::mutex mutex_;
    Database* db_ = nullptr;
    class DeviceManager* device_mgr_ = nullptr;
    std::vector<FirmwareVersion> firmware_versions_;
    std::unordered_map<std::string, OtaRecord> ota_records_; // key: device_id
};

} // namespace dev_sys
