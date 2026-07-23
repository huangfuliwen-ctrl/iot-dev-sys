#include "../../middleware/storage/database.h"
#include "fault_detector.h"
#include <algorithm>
#include <iostream>

namespace dev_sys {

FaultManager::FaultManager() = default;
FaultManager::~FaultManager() = default;

void FaultManager::on_fault_event(const std::string& tenant_id,
                                   const std::string& device_id,
                                   const std::string& payload_json) {
    // Legacy: parse JSON payload (used by older clients)
    FaultInfo fault;
    fault.tenant_id = tenant_id;
    fault.device_id = device_id;
    fault.code       = static_cast<FaultCode>(JsonHelper::get_int(payload_json, "fault_code", 0));
    std::string level = JsonHelper::get_string(payload_json, "level");
    fault.level      = (level == "error") ? FaultLevel::ERROR : FaultLevel::WARNING;
    fault.description= JsonHelper::get_string(payload_json, "description");
    fault.timestamp  = std::to_string(JsonHelper::get_int(payload_json, "ts", 0));
    add_fault(fault);
}

void FaultManager::add_fault(const FaultInfo& fault) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_faults_.push_back(fault);

    // ERROR level: immediate alert
    if (fault.level == FaultLevel::ERROR && alert_cb_) {
        alert_cb_(fault);
    }

    if (db_) db_->insert_fault(fault);

    std::cout << "[FaultMgr] " << (fault.level == FaultLevel::ERROR ? "ERROR" : "WARNING")
              << " from " << fault.tenant_id << "/" << fault.device_id
              << " code=" << static_cast<int>(fault.code)
              << ": " << fault.description << std::endl;
}

std::vector<FaultInfo> FaultManager::active_faults() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_faults_;
}

std::vector<FaultInfo> FaultManager::faults_by_device(const std::string& tenant_id,
                                                       const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FaultInfo> result;
    for (const auto& f : active_faults_) {
        if (f.tenant_id == tenant_id && f.device_id == device_id) {
            result.push_back(f);
        }
    }
    return result;
}

void FaultManager::set_alert_callback(FaultAlertCallback cb) {
    alert_cb_ = std::move(cb);
}

std::vector<FaultInfo> FaultManager::all_faults() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_faults_;
}

StatusCode FaultManager::resolve_fault(const std::string& tenant_id,
                                        const std::string& device_id,
                                        FaultCode code) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(active_faults_.begin(), active_faults_.end(),
        [&](const FaultInfo& f) {
            return f.tenant_id == tenant_id && f.device_id == device_id && f.code == code;
        });
    if (it == active_faults_.end()) return StatusCode::ORDER_NOT_FOUND;
    active_faults_.erase(it);
    std::cout << "[FaultMgr] Resolved: " << tenant_id << "/" << device_id
              << " code=" << static_cast<int>(code) << std::endl;
    return StatusCode::OK;
}

void FaultManager::seed_mock_data() {
    FaultInfo f1;
    f1.tenant_id = "tenant_demo";
    f1.device_id = "dev-coffee-002";
    f1.code = FaultCode::E003_PUMP_FAILURE;
    f1.level = FaultLevel::ERROR;
    f1.description = "水泵流量异常：期望流量 8ml/s，实际 3ml/s";
    f1.timestamp = "1782660900";
    f1.sensor_snapshot = R"({"flow_rate_ml_s":3.0,"target_flow":8.0,"pump_current_a":0.3})";
    active_faults_.push_back(f1);

    FaultInfo f2;
    f2.tenant_id = "tenant_demo";
    f2.device_id = "dev-coffee-001";
    f2.code = FaultCode::W003_MATERIAL_LOW;
    f2.level = FaultLevel::WARNING;
    f2.description = "咖啡豆余量不足：当前 120g，阈值 150g";
    f2.timestamp = "1782663600";
    f2.sensor_snapshot = R"({"bean_remaining_g":120,"threshold_g":150})";
    active_faults_.push_back(f2);

    FaultInfo f3;
    f3.tenant_id = "tenant_demo";
    f3.device_id = "dev-water-001";
    f3.code = FaultCode::W002_COMM_FAIL;
    f3.level = FaultLevel::WARNING;
    f3.description = "通信模块连续3次心跳超时，设备可能离线";
    f3.timestamp = "1782665400";
    f3.sensor_snapshot = R"({"missed_heartbeats":3,"last_seen":"2026-06-26T09:27:00Z"})";
    active_faults_.push_back(f3);

    FaultInfo f4;
    f4.tenant_id = "tenant_demo";
    f4.device_id = "dev-coffee-003";
    f4.code = FaultCode::E005_WATER_LEAK;
    f4.level = FaultLevel::ERROR;
    f4.description = "机箱底部漏水传感器触发，已自动锁定";
    f4.timestamp = "1782662700";
    f4.sensor_snapshot = R"({"leak_sensor":true,"zone":"bottom_tray","auto_lockdown":true})";
    active_faults_.push_back(f4);

    std::cout << "[FaultMgr] Seeded " << active_faults_.size() << " mock faults" << std::endl;
}

StatusCode FaultManager::load_from_database() {
    if (!db_) return StatusCode::STORAGE_READ_ERROR;
    auto faults = db_->list_all_faults();
    active_faults_ = db_->list_active_faults();
    std::cout << "[FaultMgr] Loaded " << faults.size() << " faults (" << active_faults_.size() << " active) from database" << std::endl;
    return StatusCode::OK;
}
} // namespace dev_sys
