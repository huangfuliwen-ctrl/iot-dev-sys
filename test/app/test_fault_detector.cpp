#include <gtest/gtest.h>
#include "app/fault/fault_detector.h"

using namespace dev_sys;

TEST(FaultManagerTest, InitialNoFaults) {
    FaultManager fm;
    EXPECT_TRUE(fm.active_faults().empty());
    EXPECT_TRUE(fm.all_faults().empty());
}

TEST(FaultManagerTest, OnFaultEventCreatesFault) {
    FaultManager fm;

    std::string payload = R"({
        "event_type": "fault_alert",
        "code": 3,
        "level": 3,
        "description": "Pump failure detected",
        "timestamp": "2026-06-27T12:00:00Z",
        "sensor_snapshot": "{\"flow_rate\":0}"
    })";

    fm.on_fault_event("tenant_test", "dev-001", payload);

    auto active = fm.active_faults();
    EXPECT_EQ(active.size(), 1);
    EXPECT_EQ(active[0].tenant_id, "tenant_test");
    EXPECT_EQ(active[0].device_id, "dev-001");
    EXPECT_EQ(active[0].code, FaultCode::PUMP_FAILURE);
    EXPECT_EQ(active[0].level, FaultLevel::L3_SEVERE);
}

TEST(FaultManagerTest, ResolveFault) {
    FaultManager fm;

    // Create a fault
    std::string payload = R"({
        "event_type": "fault_alert",
        "code": 8,
        "level": 1,
        "description": "Material low",
        "timestamp": "2026-06-27T12:00:00Z"
    })";
    fm.on_fault_event("tenant_test", "dev-002", payload);

    EXPECT_EQ(fm.active_faults().size(), 1);

    // Resolve it
    auto sc = fm.resolve_fault("tenant_test", "dev-002", FaultCode::MATERIAL_LOW);
    EXPECT_EQ(sc, StatusCode::OK);

    // Should no longer be active
    EXPECT_TRUE(fm.active_faults().empty());
    // But should still be in all_faults
    EXPECT_EQ(fm.all_faults().size(), 1);
}

TEST(FaultManagerTest, FaultsByDevice) {
    FaultManager fm;

    std::string payload1 = R"({
        "event_type": "fault_alert",
        "code": 3,
        "level": 3,
        "description": "Pump failure",
        "timestamp": "2026-06-27T12:00:00Z"
    })";
    fm.on_fault_event("t1", "dev-a", payload1);

    std::string payload2 = R"({
        "event_type": "fault_alert",
        "code": 7,
        "level": 2,
        "description": "Comm failure",
        "timestamp": "2026-06-27T12:01:00Z"
    })";
    fm.on_fault_event("t1", "dev-b", payload2);

    auto dev_a_faults = fm.faults_by_device("t1", "dev-a");
    EXPECT_EQ(dev_a_faults.size(), 1);
    EXPECT_EQ(dev_a_faults[0].code, FaultCode::PUMP_FAILURE);

    auto dev_b_faults = fm.faults_by_device("t1", "dev-b");
    EXPECT_EQ(dev_b_faults.size(), 1);
    EXPECT_EQ(dev_b_faults[0].code, FaultCode::COMM_FAIL);

    auto dev_c_faults = fm.faults_by_device("t1", "dev-c");
    EXPECT_TRUE(dev_c_faults.empty());
}
