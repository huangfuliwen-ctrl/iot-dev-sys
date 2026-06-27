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
        "+/iot/+/+/heartbeat",
        "+/iot/+/+/event/post",
        "+/iot/+/+/event/post_reply",
        "+/iot/+/+/property/post",
        "+/iot/+/+/property/set_reply",
        "+/iot/+/+/ota/progress",
    };
}

// ============================================================
// Core: parse topic → dispatch to handler
// ============================================================
void MessageRouter::on_message(const std::string& topic, const std::string& payload) {
    ParsedTopic pt = ParsedTopic::parse(topic);
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
    } else {
        std::cerr << "[Router] Unknown message type: " << pt.message_type << std::endl;
    }
}

// ============================================================
// Handlers
// ============================================================
void MessageRouter::handle_heartbeat(const ParsedTopic& pt, const std::string& payload) {
    // payload JSON: {device_id, timestamp, status, firmware_version, signal_strength, alarm_count}
    if (device_mgr_) {
        device_mgr_->process_heartbeat(pt.tenant_id, pt.device_id, pt.product_id, payload);
    }
}

void MessageRouter::handle_event(const ParsedTopic& pt, const std::string& payload) {
    // payload JSON: {event_type: "order_status"|"fault"|"device_status", ...data}
    // TODO: Parse event_type from JSON and dispatch to order_mgr_ / fault_mgr_
    // Example:
    //   if (event_type == "order_status") order_mgr_->on_order_event(pt, payload);
    //   if (event_type == "fault_alert")   fault_mgr_->on_fault_event(pt, payload);
}

void MessageRouter::handle_property(const ParsedTopic& pt, const std::string& payload) {
    // payload JSON: reported device properties
    if (device_mgr_) {
        device_mgr_->process_property(pt.tenant_id, pt.device_id, pt.product_id, payload);
    }
}

void MessageRouter::handle_ota_progress(const ParsedTopic& pt, const std::string& payload) {
    // payload JSON: {version, progress, stage, status}
    if (ota_mgr_) {
        ota_mgr_->process_ota_progress(pt.tenant_id, pt.device_id, payload);
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
