#include "../../middleware/storage/database.h"
#include "order_manager.h"
#include "dev_sys/common/constants.h"
#include <algorithm>
#include <iostream>
#include <chrono>

namespace dev_sys {

struct OrderManager::Impl {
    std::deque<Order> order_queue;
    std::optional<Order> current_order;  // order currently being brewed
    std::vector<Order> history_orders;   // completed / cancelled / failed
};

OrderManager::OrderManager()
    : impl_(std::make_unique<Impl>()) {}

OrderManager::~OrderManager() = default;

// ============================================================
// Order creation (REQ-OR-002)
// ============================================================
StatusCode OrderManager::create_order(const Order& order) {
    if (impl_->order_queue.size() >= ORDER_MAX_QUEUE_DEPTH) {
        return StatusCode::ORDER_QUEUE_FULL;
    }
    impl_->order_queue.push_back(order);
    if (db_) db_->insert_order(order);

    // Report order created to cloud
    sync_status_to_cloud(order.order_id);
    return StatusCode::OK;
}

StatusCode OrderManager::cancel_order(const std::string& order_id) {
    // Can only cancel if not brewing
    if (impl_->current_order && impl_->current_order->order_id == order_id) {
        return StatusCode::ORDER_INVALID_STATE;
    }

    auto it = std::find_if(impl_->order_queue.begin(), impl_->order_queue.end(),
        [&order_id](const Order& o) { return o.order_id == order_id; });

    if (it == impl_->order_queue.end()) {
        return StatusCode::ORDER_NOT_FOUND;
    }

    it->status = OrderStatus::CANCELLED;
    sync_status_to_cloud(order_id);
    impl_->history_orders.push_back(*it);
    impl_->order_queue.erase(it);
    return StatusCode::OK;
}

StatusCode OrderManager::confirm_payment(const std::string& order_id) {
    for (auto& order : impl_->order_queue) {
        if (order.order_id == order_id) {
            order.status = OrderStatus::PAID;
            sync_status_to_cloud(order_id);
            return StatusCode::OK;
        }
    }
    return StatusCode::ORDER_NOT_FOUND;
}

// ============================================================
// Pre-brew check (REQ-OR-002)
// ============================================================
StatusCode OrderManager::run_pre_brew_check(const Order& order) {
    // TODO: Check material sufficiency (water, beans, powder, cups...)
    // TODO: Check device status (no fault, not in maintenance)
    // TODO: Check waste bin not full
    // On failure: cancel order + refund
    return StatusCode::OK;
}

// ============================================================
// Brewing flow (REQ-OR-002)
// ============================================================
StatusCode OrderManager::start_brewing(const std::string& order_id) {
    if (impl_->current_order.has_value()) {
        return StatusCode::ORDER_INVALID_STATE; // already brewing
    }

    // Find the order in queue
    auto it = std::find_if(impl_->order_queue.begin(), impl_->order_queue.end(),
        [&order_id](const Order& o) { return o.order_id == order_id; });

    if (it == impl_->order_queue.end()) {
        return StatusCode::ORDER_NOT_FOUND;
    }

    // Pre-brew check
    StatusCode check = run_pre_brew_check(*it);
    if (check != StatusCode::OK) {
        it->status = OrderStatus::CANCELLED;
        it->failure_reason = "Pre-brew check failed";
        sync_status_to_cloud(order_id);
        impl_->order_queue.erase(it);
        return check;
    }

    it->status = OrderStatus::BREWING;
    impl_->current_order = *it;
    impl_->order_queue.erase(it);
    sync_status_to_cloud(order_id);

    // TODO: Execute recipe steps via HAL (sensors + actuators)
    // TODO: Monitor sensors during brewing (temperature, flow rate)
    // TODO: Timeout = recipe theoretical duration * 1.5

    return StatusCode::OK;
}

StatusCode OrderManager::complete_brewing(const std::string& order_id) {
    if (!impl_->current_order || impl_->current_order->order_id != order_id) {
        return StatusCode::ORDER_NOT_FOUND;
    }

    impl_->current_order->status = OrderStatus::COMPLETED;
    sync_status_to_cloud(order_id);
    impl_->history_orders.push_back(*impl_->current_order);
    impl_->current_order.reset();
    return StatusCode::OK;
}

StatusCode OrderManager::fail_brewing(const std::string& order_id, const std::string& reason) {
    if (!impl_->current_order || impl_->current_order->order_id != order_id) {
        return StatusCode::ORDER_NOT_FOUND;
    }

    impl_->current_order->status = OrderStatus::FAILED;
    impl_->current_order->failure_reason = reason;
    sync_status_to_cloud(order_id);
    impl_->history_orders.push_back(*impl_->current_order);
    impl_->current_order.reset();
    return StatusCode::OK;
}

// Emergency stop
StatusCode OrderManager::emergency_stop() {
    if (impl_->current_order) {
        impl_->current_order->status = OrderStatus::FAILED;
        impl_->current_order->failure_reason = "Emergency stop";
        sync_status_to_cloud(impl_->current_order->order_id);
        impl_->current_order.reset();
    }
    // TODO: Cut power to all actuators immediately
    return StatusCode::OK;
}

// ============================================================
// Cloud sync (REQ-OR-003)
// ============================================================
StatusCode OrderManager::sync_status_to_cloud(const std::string& order_id) {
    // TODO: Publish order status to MQTT event/post topic
    // If offline: cache locally, batch sync when reconnected
    return StatusCode::OK;
}

StatusCode OrderManager::sync_orders_batch() {
    // TODO: POST /api/v1/orders/sync for batch upload (HTTPS fallback)
    return StatusCode::OK;
}

// ============================================================
// Query
// ============================================================
std::optional<Order> OrderManager::get_order(const std::string& order_id) const {
    if (impl_->current_order && impl_->current_order->order_id == order_id) {
        return impl_->current_order;
    }
    auto it = std::find_if(impl_->order_queue.begin(), impl_->order_queue.end(),
        [&order_id](const Order& o) { return o.order_id == order_id; });
    if (it != impl_->order_queue.end()) {
        return *it;
    }
    return std::nullopt;
}

std::vector<Order> OrderManager::pending_orders() const {
    return std::vector<Order>(impl_->order_queue.begin(), impl_->order_queue.end());
}

int32_t OrderManager::queue_depth() const {
    return static_cast<int32_t>(impl_->order_queue.size());
}

std::vector<Order> OrderManager::list_all_orders() const {
    std::vector<Order> all;
    // Current brewing order
    if (impl_->current_order) {
        all.push_back(*impl_->current_order);
    }
    // Pending queue
    for (const auto& o : impl_->order_queue) {
        all.push_back(o);
    }
    // History
    for (const auto& o : impl_->history_orders) {
        all.push_back(o);
    }
    return all;
}

int32_t OrderManager::total_count() const {
    int32_t count = static_cast<int32_t>(impl_->order_queue.size())
                  + static_cast<int32_t>(impl_->history_orders.size());
    if (impl_->current_order) count++;
    return count;
}

void OrderManager::seed_mock_data() {
    // Pending orders
    Order o1;
    o1.order_id = "ORD-20260626-001";
    o1.tenant_id = "tenant_demo";
    o1.device_id = "dev-coffee-001";
    o1.recipe_id = "REC-AMERICANO-001";
    o1.cup_size = "中";
    o1.quantity = 1;
    o1.total_amount = 1500;
    o1.payment_method = "wechat";
    o1.status = OrderStatus::PENDING;
    o1.created_at = "2026-06-26T09:15:00Z";
    impl_->order_queue.push_back(o1);

    Order o2;
    o2.order_id = "ORD-20260626-002";
    o2.tenant_id = "tenant_demo";
    o2.device_id = "dev-coffee-001";
    o2.recipe_id = "REC-LATTE-001";
    o2.cup_size = "大";
    o2.quantity = 2;
    o2.total_amount = 4400;
    o2.payment_method = "alipay";
    o2.status = OrderStatus::PAID;
    o2.created_at = "2026-06-26T09:20:00Z";
    impl_->order_queue.push_back(o2);

    Order o3;
    o3.order_id = "ORD-20260626-003";
    o3.tenant_id = "tenant_demo";
    o3.device_id = "dev-water-001";
    o3.recipe_id = "REC-HOTWATER-001";
    o3.cup_size = "小";
    o3.quantity = 1;
    o3.total_amount = 500;
    o3.payment_method = "member";
    o3.status = OrderStatus::PENDING;
    o3.created_at = "2026-06-26T09:25:00Z";
    impl_->order_queue.push_back(o3);

    // History (completed)
    Order h1;
    h1.order_id = "ORD-20260625-101";
    h1.tenant_id = "tenant_demo";
    h1.device_id = "dev-coffee-001";
    h1.recipe_id = "REC-AMERICANO-001";
    h1.cup_size = "中";
    h1.quantity = 1;
    h1.total_amount = 1500;
    h1.payment_method = "wechat";
    h1.status = OrderStatus::COMPLETED;
    h1.created_at = "2026-06-25T14:30:00Z";
    impl_->history_orders.push_back(h1);

    Order h2;
    h2.order_id = "ORD-20260625-102";
    h2.tenant_id = "tenant_demo";
    h2.device_id = "dev-coffee-002";
    h2.recipe_id = "REC-MOCHA-001";
    h2.cup_size = "大";
    h2.quantity = 1;
    h2.total_amount = 2800;
    h2.payment_method = "alipay";
    h2.status = OrderStatus::FAILED;
    h2.failure_reason = "水泵流量异常";
    h2.created_at = "2026-06-25T15:10:00Z";
    impl_->history_orders.push_back(h2);

    Order h3;
    h3.order_id = "ORD-20260625-103";
    h3.tenant_id = "tenant_demo";
    h3.device_id = "dev-coffee-001";
    h3.recipe_id = "REC-CAPPUCCINO-001";
    h3.cup_size = "中";
    h3.quantity = 1;
    h3.total_amount = 2200;
    h3.payment_method = "wechat";
    h3.status = OrderStatus::CANCELLED;
    h3.created_at = "2026-06-25T16:45:00Z";
    impl_->history_orders.push_back(h3);

    std::cout << "[OrderMgr] Seeded " << total_count() << " mock orders" << std::endl;
}

StatusCode OrderManager::load_from_database() {
    if (!db_) return StatusCode::STORAGE_READ_ERROR;
    auto orders = db_->list_all_orders();
    for (const auto& o : orders) { impl_->history_orders.push_back(o); }
    std::cout << "[OrderMgr] Loaded " << orders.size() << " orders from database" << std::endl;
    return StatusCode::OK;
}
} // namespace dev_sys
