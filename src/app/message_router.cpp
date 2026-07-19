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
        "$T/+/+/v1/heartbeat",
        "$T/+/+/v1/event/post",
        "$T/+/+/v1/event/post_reply",
        "$T/+/+/v1/property/post",
        "$T/+/+/v1/property/set_reply",
        "$T/+/+/v1/ota/progress",
        "$T/+/+/v1/status",
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
    // payload JSON: {event_type: "order_status"|"fault_alert"|"device_status", ...data}
    std::string event_type = JsonHelper::get_string(payload, "event_type");

    if (event_type == "order_status") {
        // Order status change reported by device via MQTT (SRS 4.1.2 item 2)
        // payload: {event_type:"order_status", order_id:"...", status:<int>, reason:"..."}
        if (order_mgr_) {
            std::string order_id = JsonHelper::get_string(payload, "order_id");
            int status_code = JsonHelper::get_int(payload, "status", -1);
            std::string reason = JsonHelper::get_string(payload, "reason");

            if (order_id.empty()) {
                std::cerr << "[Router] order_status event missing order_id" << std::endl;
                return;
            }

            switch (status_code) {
                case 1: // PAID
                    order_mgr_->confirm_payment(order_id);
                    break;
                case 2: // BREWING
                    order_mgr_->start_brewing(order_id);
                    break;
                case 3: // COMPLETED
                    order_mgr_->complete_brewing(order_id);
                    break;
                case 4: // CANCELLED
                    order_mgr_->cancel_order(order_id);
                    break;
                case 5: // FAILED
                    order_mgr_->fail_brewing(order_id,
                        reason.empty() ? "Device reported failure" : reason);
                    break;
                default:
                    std::cerr << "[Router] Unknown order status code: " << status_code << std::endl;
                    break;
            }
        }
    } else if (event_type == "fault_alert") {
        // Fault alert reported by device via MQTT (SRS 4.1.2 item 2)
        if (fault_mgr_) {
            fault_mgr_->on_fault_event(resolve_tenant(device_mgr_, pt.device_id), pt.device_id, payload);
        }
    } else if (event_type == "device_status") {
        if (device_mgr_) {
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
