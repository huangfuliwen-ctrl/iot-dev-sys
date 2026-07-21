#include "message_router.h"
#include "device/device_manager.h"
#include "order/order_manager.h"
#include "ota/ota_manager.h"
#include "fault/fault_detector.h"
#include "recipe/recipe_manager.h"
#include <iostream>
#include <sstream>

namespace dev_sys {

MessageRouter::MessageRouter() = default;
MessageRouter::~MessageRouter() = default;

void MessageRouter::set_device_manager(DeviceManager* mgr) { device_mgr_ = mgr; }
void MessageRouter::set_order_manager(OrderManager* mgr)   { order_mgr_  = mgr; }
void MessageRouter::set_ota_manager(OtaManager* mgr)       { ota_mgr_    = mgr; }
void MessageRouter::set_fault_manager(FaultManager* mgr)   { fault_mgr_  = mgr; }
void MessageRouter::set_recipe_manager(RecipeManager* mgr) { recipe_mgr_ = mgr; }

// ============================================================
// Wildcard subscriptions - cloud service subscribes to ALL device messages
// ============================================================
std::vector<std::string> MessageRouter::subscription_topics() {
    return {
        "+/v1/heartbeat",
        "+/v1/event/post",
        "+/v1/event/post_reply",
        "+/v1/property/post",
        "+/v1/property/set_reply",
        "+/v1/ota/progress",
        "+/v1/status",
    };
}

// ============================================================
// Core: parse topic → dispatch to handler
// ============================================================
void MessageRouter::on_message(const std::string& topic, const std::string& payload) {
    ParsedTopic pt = ParsedTopic::parse(topic);
    // MQTT log: print every received message
    std::string preview = payload.size() > 200 ? payload.substr(0, 200) + "..." : payload;
    std::cout << "[MQTT] ← " << topic << " | " << preview << std::endl;

    if (!pt.valid) {
        std::cerr << "[Router] Invalid topic: " << topic << std::endl;
        return;
    }

    // Route by message type
    if (pt.message_type == "heartbeat") {
        handle_heartbeat(pt, payload);
    } else if (pt.message_type == "event") {
        handle_event(pt, payload);
    } else if (pt.message_type == "property") {
        handle_property(pt, payload);
    } else if (pt.message_type == "ota") {
        handle_ota_progress(pt, payload);
    } else if (pt.message_type == "status") {
        handle_status(pt, payload);
    } else {
        std::cerr << "[Router] Unknown message type: " << pt.message_type << std::endl;
    }
}

// ============================================================
// Helpers
// ============================================================
static std::string resolve_tenant(DeviceManager* mgr, const std::string& did) {
    if (!mgr) return "default";
    auto d = mgr->find_device_by_id(did);
    return d ? d->tenant_id : "default";
}
static std::string resolve_product(DeviceManager* mgr, const std::string& did) {
    if (!mgr) return "";
    auto d = mgr->find_device_by_id(did);
    return d ? d->product_id : "";
}

// ============================================================
// Handlers
// ============================================================
void MessageRouter::handle_heartbeat(const ParsedTopic& pt, const std::string& payload) {
    if (device_mgr_) {
        device_mgr_->process_heartbeat(
            resolve_tenant(device_mgr_, pt.device_id),
            pt.device_id,
            resolve_product(device_mgr_, pt.device_id),
            payload);
    }
}

void MessageRouter::handle_event(const ParsedTopic& pt, const std::string& payload) {
    std::string event_type = JsonHelper::get_string(payload, "event_type");

    if (event_type == "order_completed") {
        // 咖啡机制作完成上报 (咖啡机协议 §3.2.1)
        // payload: {order_id, recipe_id, recipe_name, cup_size, timestamp}
        if (order_mgr_) {
            Order order;
            order.order_id   = JsonHelper::get_string(payload, "order_id");
            order.device_id  = pt.device_id;
            order.tenant_id  = resolve_tenant(device_mgr_, pt.device_id);
            order.recipe_id  = JsonHelper::get_string(payload, "recipe_id");
            order.cup_size   = JsonHelper::get_string(payload, "cup_size");
            order.status     = OrderStatus::COMPLETED;
            order_mgr_->create_order(order);
        }
    } else if (event_type == "fault_alert") {
        // 故障/警告上报 (咖啡机协议 §3.2.2)
        if (fault_mgr_) {
            std::string level = JsonHelper::get_string(payload, "level");
            FaultInfo fault;
            fault.tenant_id  = resolve_tenant(device_mgr_, pt.device_id);
            fault.device_id  = pt.device_id;
            fault.code       = static_cast<FaultCode>(JsonHelper::get_int(payload, "fault_code", 0));
            fault.level      = (level == "error") ? FaultLevel::ERROR : FaultLevel::WARNING;
            fault.description= JsonHelper::get_string(payload, "description");
            fault.timestamp  = JsonHelper::get_string(payload, "timestamp");
            // sensor_snapshot is optional raw JSON
            fault_mgr_->add_fault(fault);
        }
    } else if (event_type == "fault_resolved") {
        // 故障恢复 (咖啡机协议 §3.2.3)
        if (fault_mgr_) {
            std::string tid = resolve_tenant(device_mgr_, pt.device_id);
            FaultCode code = static_cast<FaultCode>(JsonHelper::get_int(payload, "fault_code", 0));
            fault_mgr_->resolve_fault(tid, pt.device_id, code);
        }
    } else if (event_type == "device_status" || event_type == "order_status") {
        // Legacy events — handle gracefully
        if (event_type == "device_status" && device_mgr_) {
            device_mgr_->process_property(resolve_tenant(device_mgr_, pt.device_id),
                pt.device_id, resolve_product(device_mgr_, pt.device_id), payload);
        }
    } else {
        std::cerr << "[Router] Unknown event_type: " << event_type
                  << " from " << pt.device_id << std::endl;
    }
}

void MessageRouter::handle_property(const ParsedTopic& pt, const std::string& payload) {
    if (device_mgr_) {
        device_mgr_->process_property(resolve_tenant(device_mgr_, pt.device_id),
            pt.device_id, resolve_product(device_mgr_, pt.device_id), payload);
    }
}

void MessageRouter::handle_status(const ParsedTopic& pt, const std::string& payload) {
    if (device_mgr_) {
        std::string tid = resolve_tenant(device_mgr_, pt.device_id);
        std::string st = JsonHelper::get_string(payload, "network_status");
        if (st.empty()) st = JsonHelper::get_string(payload, "status");
        if (st == "offline" || st == "1") {
            device_mgr_->update_device_status(tid, pt.device_id,
                NetworkStatus::OFFLINE, WorkStatus::IDLE);
        } else {
            device_mgr_->update_device_status(tid, pt.device_id,
                NetworkStatus::ONLINE, WorkStatus::IDLE);
        }
    }
}

void MessageRouter::handle_ota_progress(const ParsedTopic& pt, const std::string& payload) {
    if (ota_mgr_) {
        ota_mgr_->process_ota_progress(resolve_tenant(device_mgr_, pt.device_id),
            pt.device_id, payload);
    }
}

// ============================================================
// Outbound: build precise topics for sending commands down
// ============================================================
std::string MessageRouter::send_command(const std::string& tenant_id,
                                         const std::string& product_id,
                                         const std::string& device_id,
                                         const std::string& command,
                                         const std::string& payload_json) {
    std::string topic = ParsedTopic::build_downlink(tenant_id, product_id, device_id,
                                                      "command", command);
    // Caller: mqtt_client->publish(topic, payload_json)
    return topic;
}

std::string MessageRouter::send_ota_notify(const std::string& tenant_id,
                                            const std::string& product_id,
                                            const std::string& device_id,
                                            const std::string& payload_json) {
    return ParsedTopic::build_downlink(tenant_id, product_id, device_id,
                                        "ota", "notify");
}

std::string MessageRouter::send_property_set(const std::string& tenant_id,
                                              const std::string& product_id,
                                              const std::string& device_id,
                                              const std::string& payload_json) {
    return ParsedTopic::build_downlink(tenant_id, product_id, device_id,
                                        "property", "set");
}

} // namespace dev_sys
