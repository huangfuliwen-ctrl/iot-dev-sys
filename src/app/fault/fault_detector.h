#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <mutex>

namespace dev_sys {

using FaultAlertCallback = std::function<void(const FaultInfo& fault)>;

// ============================================================
// Fault Manager (云服务端：接收全设备故障告警)
// ============================================================
class FaultManager {
public:
    FaultManager();
    ~FaultManager();

    // Inbound: process fault event from device
    void on_fault_event(const std::string& tenant_id,
                        const std::string& device_id,
                        const std::string& payload_json);
    void add_fault(const FaultInfo& fault);  // pre-parsed fault from router

    // Query
    std::vector<FaultInfo> active_faults() const;
    std::vector<FaultInfo> all_faults() const;               // active + historical
    std::vector<FaultInfo> faults_by_device(const std::string& tenant_id,
                                             const std::string& device_id) const;
    StatusCode resolve_fault(const std::string& tenant_id,
                             const std::string& device_id,
                             FaultCode code);

    // Alert notification
    void set_alert_callback(FaultAlertCallback cb);

    // Mock data for frontend development
    void seed_mock_data();
    void set_database(class Database* db) { db_ = db; }
    StatusCode load_from_database();

private:
    mutable std::mutex mutex_;
    Database* db_ = nullptr;
    std::vector<FaultInfo> active_faults_;
    FaultAlertCallback alert_cb_;
};

} // namespace dev_sys
