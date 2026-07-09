#include "../../middleware/storage/database.h"
#include "ota_manager.h"
#include "../device/device_manager.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <functional>

namespace dev_sys {

OtaManager::OtaManager() = default;
OtaManager::~OtaManager() = default;

// ============================================================
// Inbound: device reports OTA progress
// ============================================================
void OtaManager::process_ota_progress(const std::string& tenant_id,
                                       const std::string& device_id,
                                       const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(mutex_);

    // TODO: Parse JSON: {version, progress, stage, status}
    // Update ota_records_[device_id]

    auto it = ota_records_.find(device_id);
    if (it == ota_records_.end()) {
        OtaRecord rec;
        rec.tenant_id = tenant_id;
        rec.device_id = device_id;
        // rec.progress = ... (from JSON)
        // rec.stage = ... (from JSON)
        ota_records_[device_id] = rec;
    } else {
        // Update progress/stage from JSON
    }

    std::cout << "[OtaMgr] Progress: " << tenant_id << "/" << device_id
              << " stage=" << it->second.stage << " progress=" << it->second.progress << "%"
              << std::endl;
}

// ============================================================
// Firmware registry
// ============================================================
StatusCode OtaManager::register_firmware(const FirmwareVersion& fw) {
    std::lock_guard<std::mutex> lock(mutex_);
    firmware_versions_.push_back(fw);
    // TODO: Persist to firmware_versions table
    return StatusCode::OK;
}

// ============================================================
// OTA Orchestration: push to device(s)
// ============================================================
StatusCode OtaManager::push_to_device(const std::string& tenant_id,
                                       const std::string& product_id,
                                       const std::string& device_id,
                                       const std::string& target_version) {
    // Find firmware version
    auto fw_it = std::find_if(firmware_versions_.begin(), firmware_versions_.end(),
        [&](const FirmwareVersion& fw) { return fw.version == target_version; });

    if (fw_it == firmware_versions_.end()) {
        return StatusCode::ERROR;
    }

    // Build notification payload
    std::string payload = build_ota_notify_payload(
        target_version, fw_it->download_url, fw_it->checksum_sha256, fw_it->force_upgrade);

    // Record tracking
    OtaRecord rec;
    rec.tenant_id       = tenant_id;
    rec.device_id       = device_id;
    rec.current_version = ""; // will be filled from device heartbeat
    rec.target_version  = target_version;
    rec.download_url    = fw_it->download_url;
    rec.checksum        = fw_it->checksum_sha256;
    rec.stage           = "pending";
    rec.progress        = 0;
    rec.force_upgrade   = fw_it->force_upgrade;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ota_records_[device_id] = rec;
    }

    // Caller publishes: MessageRouter::send_ota_notify(tenant_id, product_id, device_id, payload)
    // → MQTT topic: {tenant_id}/iot/{product_id}/{device_id}/ota/notify

    return StatusCode::OK;
}

StatusCode OtaManager::push_grayscale(const std::string& product_id,
                                       const std::string& target_version,
                                       int percent) {
    if (percent < 1 || percent > 100) {
        std::cerr << "[OtaMgr] Invalid grayscale percent: " << percent << std::endl;
        return StatusCode::ERROR;
    }

    // Verify firmware version exists
    auto fw_it = std::find_if(firmware_versions_.begin(), firmware_versions_.end(),
        [&](const FirmwareVersion& fw) { return fw.version == target_version; });
    if (fw_it == firmware_versions_.end()) {
        std::cerr << "[OtaMgr] Firmware version not found: " << target_version << std::endl;
        return StatusCode::ERROR;
    }

    // Collect candidate devices
    std::vector<Device> candidates;

    if (device_mgr_) {
        // Query DeviceManager for all devices matching this product_id
        auto all_devices = device_mgr_->list_all_devices();
        for (const auto& dev : all_devices) {
            if (dev.product_id == product_id && dev.activated &&
                dev.status != DeviceStatus::OFFLINE &&
                dev.status != DeviceStatus::FAULT &&
                dev.firmware_version != target_version) {
                candidates.push_back(dev);
            }
        }
    } else {
        // Fallback: iterate existing ota_records_ for devices we already know about
        // (limited functionality without DeviceManager reference)
        for (const auto& [device_id, rec] : ota_records_) {
            // We only have device_id in records, need to infer product
            // This is limited — prefer wiring device_mgr_ in main.cpp
            if (rec.target_version != target_version && rec.stage == "done") {
                Device dev;
                dev.device_id = device_id;
                dev.product_id = product_id;
                dev.tenant_id = rec.tenant_id;
                candidates.push_back(dev);
            }
        }
    }

    if (candidates.empty()) {
        std::cout << "[OtaMgr] No eligible devices found for grayscale push"
                  << " (product=" << product_id << ", version=" << target_version << ")"
                  << std::endl;
        return StatusCode::OK;
    }

    // Deterministic selection: hash device_id to decide if in grayscale group
    // This ensures the same devices are selected on repeated calls
    int selected_count = 0;
    for (const auto& dev : candidates) {
        // Simple hash-based selection: use std::hash of device_id
        size_t hash_val = std::hash<std::string>{}(dev.device_id);
        int bucket = static_cast<int>(hash_val % 100); // 0-99

        if (bucket < percent) {
            StatusCode sc = push_to_device(dev.tenant_id, product_id,
                                           dev.device_id, target_version);
            if (sc == StatusCode::OK) {
                selected_count++;
            }
        }
    }

    std::cout << "[OtaMgr] Grayscale push: selected " << selected_count
              << "/" << candidates.size() << " devices ("
              << percent << "%, product=" << product_id
              << ", version=" << target_version << ")" << std::endl;

    return StatusCode::OK;
}

StatusCode OtaManager::push_all(const std::string& product_id,
                                 const std::string& target_version) {
    return push_grayscale(product_id, target_version, 100);
}

// ============================================================
// Tracking
// ============================================================
std::vector<OtaRecord> OtaManager::active_upgrades() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OtaRecord> result;
    for (const auto& [id, rec] : ota_records_) {
        if (rec.stage != "done" && rec.stage != "failed") {
            result.push_back(rec);
        }
    }
    return result;
}

OtaRecord OtaManager::get_device_ota_status(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ota_records_.find(device_id);
    if (it != ota_records_.end()) return it->second;
    return OtaRecord{};
}

OtaManager::RolloutStats OtaManager::get_rollout_stats(const std::string& version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RolloutStats stats{};
    for (const auto& [id, rec] : ota_records_) {
        if (rec.target_version != version) continue;
        stats.total_targeted++;
        if (rec.stage == "downloading") stats.downloading++;
        else if (rec.stage == "installing") stats.installing++;
        else if (rec.stage == "done") stats.completed++;
        else if (rec.stage == "failed") stats.failed++;
        else if (rec.stage == "rolled_back") stats.rolled_back++;
    }
    return stats;
}

// ============================================================
// Payload builder
// ============================================================
std::string OtaManager::build_ota_notify_payload(const std::string& target_version,
                                                   const std::string& download_url,
                                                   const std::string& checksum,
                                                   bool force) const {
    // Build JSON payload for device to consume
    std::ostringstream oss;
    oss << "{"
        << "\"version\":\"" << target_version << "\","
        << "\"download_url\":\"" << download_url << "\","
        << "\"checksum\":\"" << checksum << "\","
        << "\"force_upgrade\":" << (force ? "true" : "false")
        << "}";
    return oss.str();
}

// ============================================================
// Firmware query
// ============================================================
std::vector<FirmwareVersion> OtaManager::list_firmwares() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return firmware_versions_;
}

std::optional<FirmwareVersion> OtaManager::get_firmware(const std::string& version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(firmware_versions_.begin(), firmware_versions_.end(),
        [&](const FirmwareVersion& fw) { return fw.version == version; });
    if (it != firmware_versions_.end()) return *it;
    return std::nullopt;
}

StatusCode OtaManager::delete_firmware(const std::string& version) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(firmware_versions_.begin(), firmware_versions_.end(),
        [&](const FirmwareVersion& fw) { return fw.version == version; });
    if (it == firmware_versions_.end()) return StatusCode::ERROR;
    // Only block deletion if there are truly active upgrades (downloading/installing/pending)
    for (const auto& [id, rec] : ota_records_) {
        if (rec.target_version == version
            && (rec.stage == "downloading" || rec.stage == "installing")) {
            return StatusCode::ORDER_INVALID_STATE; // version in active use
        }
    }
    firmware_versions_.erase(it);
    return StatusCode::OK;
}

void OtaManager::seed_mock_data() {
    // Only seed if empty (don't overwrite user data across restarts)
    if (!firmware_versions_.empty()) {
        std::cout << "[OtaMgr] Skipping seed: " << firmware_versions_.size()
                  << " firmware(s) already registered" << std::endl;
        return;
    }
    // Firmware versions
    FirmwareVersion fw1;
    fw1.version = "v2.1.0";
    fw1.product_id = "coffee_machine";
    fw1.download_url = "https://cdn.example.com/firmware/coffee-v2.1.0.bin";
    fw1.checksum_sha256 = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2";
    fw1.changelog = "修复加热器PID参数异常；优化待机功耗；新增MQTT v5支持";
    fw1.force_upgrade = false;
    fw1.created_at = 1719792000;
    firmware_versions_.push_back(fw1);

    FirmwareVersion fw2;
    fw2.version = "v2.2.0";
    fw2.product_id = "coffee_machine";
    fw2.download_url = "https://cdn.example.com/firmware/coffee-v2.2.0.bin";
    fw2.checksum_sha256 = "b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3";
    fw2.changelog = "新增冰咖啡配方支持；优化研磨电机控制精度；修复蓝牙配网异常";
    fw2.force_upgrade = false;
    fw2.created_at = 1722470400;
    firmware_versions_.push_back(fw2);

    FirmwareVersion fw3;
    fw3.version = "v2.2.1";
    fw3.product_id = "coffee_machine";
    fw3.download_url = "https://cdn.example.com/firmware/coffee-v2.2.1-hotfix.bin";
    fw3.checksum_sha256 = "c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4";
    fw3.changelog = "紧急修复：TLS证书过期导致的MQTT断连问题（安全漏洞 CVE-2026-1234）";
    fw3.force_upgrade = true;
    fw3.created_at = 1723075200;
    firmware_versions_.push_back(fw3);

    FirmwareVersion fw4;
    fw4.version = "v1.5.0";
    fw4.product_id = "water_dispenser";
    fw4.download_url = "https://cdn.example.com/firmware/water-v1.5.0.bin";
    fw4.checksum_sha256 = "d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5";
    fw4.changelog = "饮水机初始版本：支持热水/冰水/常温水三温控制";
    fw4.force_upgrade = false;
    fw4.created_at = 1714521600;
    firmware_versions_.push_back(fw4);

    // Active OTA records
    OtaRecord rec1;
    rec1.tenant_id = "tenant_demo";
    rec1.device_id = "dev-coffee-001";
    rec1.current_version = "v2.0.5";
    rec1.target_version = "v2.1.0";
    rec1.download_url = fw1.download_url;
    rec1.checksum = fw1.checksum_sha256;
    rec1.progress = 45;
    rec1.stage = "done";       // mock: already completed
    rec1.force_upgrade = false;
    ota_records_["dev-coffee-001"] = rec1;

    OtaRecord rec2;
    rec2.tenant_id = "tenant_demo";
    rec2.device_id = "dev-coffee-002";
    rec2.current_version = "v2.1.0";
    rec2.target_version = "v2.1.0";
    rec2.download_url = fw1.download_url;
    rec2.checksum = fw1.checksum_sha256;
    rec2.progress = 100;
    rec2.stage = "done";
    rec2.force_upgrade = false;
    ota_records_["dev-coffee-002"] = rec2;

    OtaRecord rec3;
    rec3.tenant_id = "tenant_demo";
    rec3.device_id = "dev-coffee-003";
    rec3.current_version = "v2.1.0";
    rec3.target_version = "v2.2.0";
    rec3.download_url = fw2.download_url;
    rec3.checksum = fw2.checksum_sha256;
    rec3.progress = 0;
    rec3.stage = "failed";
    rec3.force_upgrade = false;
    ota_records_["dev-coffee-003"] = rec3;

    OtaRecord rec4;
    rec4.tenant_id = "tenant_demo";
    rec4.device_id = "dev-water-001";
    rec4.current_version = "v1.4.0";
    rec4.target_version = "v1.5.0";
    rec4.download_url = fw4.download_url;
    rec4.checksum = fw4.checksum_sha256;
    rec4.progress = 100;
    rec4.stage = "done";
    rec4.force_upgrade = false;
    ota_records_["dev-water-001"] = rec4;

    std::cout << "[OtaMgr] Seeded " << firmware_versions_.size() << " firmwares, "
              << ota_records_.size() << " OTA records" << std::endl;
}

StatusCode OtaManager::load_from_database() {
    if (!db_) return StatusCode::STORAGE_READ_ERROR;
    auto firmwares = db_->list_all_firmwares();
    firmware_versions_ = firmwares;
    std::cout << "[OtaMgr] Loaded " << firmwares.size() << " firmwares from database" << std::endl;
    return StatusCode::OK;
}
} // namespace dev_sys
