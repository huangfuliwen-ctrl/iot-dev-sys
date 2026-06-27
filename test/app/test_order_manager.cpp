#include <gtest/gtest.h>
#include "app/order/order_manager.h"

using namespace dev_sys;

TEST(OrderManagerTest, CreateOrderIncreasesQueue) {
    OrderManager om;
    Order order;
    order.order_id = "test-001";
    order.recipe_id = "recipe-001";

    auto result = om.create_order(order);
    EXPECT_EQ(result, StatusCode::OK);
    EXPECT_EQ(om.queue_depth(), 1);
}

TEST(OrderManagerTest, CancelOrderNotFound) {
    OrderManager om;
    auto result = om.cancel_order("nonexistent");
    EXPECT_EQ(result, StatusCode::ORDER_NOT_FOUND);
}

TEST(OrderManagerTest, QueueDepthLimit) {
    OrderManager om;
    for (int i = 0; i < 15; i++) {
        Order order;
        order.order_id = "test-" + std::to_string(i);
        auto result = om.create_order(order);
        if (i < 10) {
            EXPECT_EQ(result, StatusCode::OK);
        } else {
            EXPECT_EQ(result, StatusCode::ORDER_QUEUE_FULL);
        }
    }
}
