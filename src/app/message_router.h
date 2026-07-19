#pragma once

#include "dev_sys/common/types.h"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

namespace dev_sys {

// Forward declarations
class DeviceManager;
class OrderManager;
class OtaManager;
class FaultManager;
class RecipeManager;

// ============================================================
// Message Router (云平台核心：通配符订阅 + Topic解析 + 分发)
//
// Topic格式: {tenant_id}/iot/{product_id}/{device_id}/{type}/...
//
// 上行消费（通配符）:
//   +/iot/+/+/heartbeat       → DeviceManager.on_heartbeat()
//   +/iot/+/+/event/post      → 按事件类型分发到 Order/Ota/Fault
//   +/iot/+/+/property/post   → DeviceManager.on_property()
//   +/iot/+/+/ota/progress    → OtaManager.on_progress()
//
// 下行发送（精确topic）:
//   {tenant}/iot/{product}/{device}/command/{cmd}
//   {tenant}/iot/{product}/{device}/property/set
//   {tenant}/iot/{product}/{device}/ota/notify
// ============================================================
class MessageRouter {
public:
    MessageRouter();
    ~MessageRouter();

    // Register business module handlers
    void set_device_manager(DeviceManager* mgr);
    void set_order_manager(OrderManager* mgr);
    void set_ota_manager(OtaManager* mgr);
    void set_fault_manager(FaultManager* mgr);
    void set_recipe_manager(RecipeManager* mgr);

    // ======== Inbound: called by MQTT client on every message ========
    // Parses topic, extracts tenant/device, dispatches to handler
    void on_message(const std::string& topic, const std::string& payload);

    // ======== Outbound: send command to specific device ========
    // Returns the topic string, caller publishes via MQTT
    std::string send_command(const std::string& tenant_id,
                             const std::string& product_id,
                             const std::string& device_id,
                             const std::string& command,
                             const std::string& payload_json);

    std::string send_ota_notify(const std::string& tenant_id,
                                 const std::string& product_id,
                                 const std::string& device_id,
                                 const std::string& payload_json);

    std::string send_property_set(const std::string& tenant_id,
                                   const std::string& product_id,
                                   const std::string& device_id,
                                   const std::string& payload_json);

    // ======== Wildcard subscription topics ========
    static std::vector<std::string> subscription_topics();

private:
    void handle_heartbeat(const ParsedTopic& pt, const std::string& payload);
    void handle_event(const ParsedTopic& pt, const std::string& payload);
    void handle_property(const ParsedTopic& pt, const std::string& payload);
    void handle_ota_progress(const ParsedTopic& pt, const std::string& payload);
    void handle_status(const ParsedTopic& pt, const std::string& payload);

    DeviceManager* device_mgr_ = nullptr;
    OrderManager*  order_mgr_  = nullptr;
    OtaManager*    ota_mgr_    = nullptr;
    FaultManager*  fault_mgr_  = nullptr;
    RecipeManager* recipe_mgr_ = nullptr;
};

} // namespace dev_sys
