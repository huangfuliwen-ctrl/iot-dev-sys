#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>
#include <queue>

namespace dev_sys {

// ============================================================
// Order Manager (REQ-OR-001/002/003)
// ============================================================
class OrderManager {
public:
    OrderManager();
    ~OrderManager();

    // Order lifecycle
    StatusCode create_order(const Order& order);
    StatusCode cancel_order(const std::string& order_id);
    StatusCode confirm_payment(const std::string& order_id);

    // Brewing integration
    StatusCode start_brewing(const std::string& order_id);
    StatusCode complete_brewing(const std::string& order_id);
    StatusCode fail_brewing(const std::string& order_id, const std::string& reason);

    // Status sync (REQ-OR-003)
    StatusCode sync_status_to_cloud(const std::string& order_id);

    // Query
    std::optional<Order> get_order(const std::string& order_id) const;
    std::vector<Order> pending_orders() const;
    std::vector<Order> list_all_orders() const;     // all orders (active + history)
    int32_t queue_depth() const;
    int32_t total_count() const;

    // Emergency stop
    StatusCode emergency_stop();

    // Persistence
    void set_database(class Database* db) { db_ = db; }
    StatusCode load_from_database();

    // Mock data for frontend development
    void seed_mock_data();

private:
    StatusCode transition_status(Order& order, OrderStatus new_status);
    StatusCode run_pre_brew_check(const Order& order);
    StatusCode sync_orders_batch();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    Database* db_ = nullptr;
};

} // namespace dev_sys
