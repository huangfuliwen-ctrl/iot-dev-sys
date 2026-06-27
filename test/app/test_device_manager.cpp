#include <gtest/gtest.h>
#include "app/device/device_manager.h"
#include "app/device/device_activation.h"
#include "middleware/storage/database.h"

using namespace dev_sys;

TEST(DeviceManagerTest, InitialDeviceCountIsZero) {
    DeviceManager dm;
    EXPECT_EQ(dm.total_device_count(), 0);
    EXPECT_TRUE(dm.list_tenants().empty());
}

TEST(DeviceManagerTest, RegisterDeviceIncreasesCount) {
    DeviceManager dm;
    Device dev;
    dev.device_id = "test-dev-001";
    dev.tenant_id = "tenant_test";
    dev.product_id = "coffee_v1";
    dev.type = DeviceType::COFFEE_MACHINE;
    dev.status = DeviceStatus::OFFLINE;
    dev.firmware_version = "1.0.0";

    auto sc = dm.register_device(dev);
    EXPECT_EQ(sc, StatusCode::OK);
    EXPECT_EQ(dm.total_device_count(), 1);
    EXPECT_EQ(dm.device_count("tenant_test"), 1);

    auto found = dm.get_device("tenant_test", "test-dev-001");
    EXPECT_TRUE(found.has_value());
    EXPECT_EQ(found->device_id, "test-dev-001");
    EXPECT_EQ(found->type, DeviceType::COFFEE_MACHINE);
}

TEST(DeviceManagerTest, ProcessHeartbeatUpdatesStatus) {
    DeviceManager dm;

    // Register a device first
    Device dev;
    dev.device_id = "test-dev-002";
    dev.tenant_id = "tenant_test";
    dev.product_id = "coffee_v1";
    dev.type = DeviceType::COFFEE_MACHINE;
    dev.status = DeviceStatus::OFFLINE;
    dev.firmware_version = "1.0.0";
    dm.register_device(dev);

    // Process heartbeat — should auto-register if not found, or update status
    // Heartbeat payload: {"device_id":"test-dev-002","timestamp":"...","status":0,...}
    std::string hb = R"({"device_id":"test-dev-002","timestamp":"2026-06-27T12:00:00Z","status":0,"firmware_version":"1.0.1","signal_strength":-45,"alarm_count":0})";
    dm.process_heartbeat("tenant_test", "test-dev-002", "coffee_v1", hb);

    auto found = dm.get_device("tenant_test", "test-dev-002");
    EXPECT_TRUE(found.has_value());
    EXPECT_EQ(found->status, DeviceStatus::IDLE);    // status 0 = IDLE
    EXPECT_EQ(found->firmware_version, "1.0.1");      // updated from heartbeat
}

TEST(DeviceManagerTest, ListOfflineDevices) {
    DeviceManager dm;

    // Register two devices, one with old heartbeat
    Device dev1;
    dev1.device_id = "dev-online";
    dev1.tenant_id = "t1";
    dev1.product_id = "p1";
    dev1.status = DeviceStatus::IDLE;
    dev1.last_heartbeat_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    dm.register_device(dev1);

    Device dev2;
    dev2.device_id = "dev-offline";
    dev2.tenant_id = "t1";
    dev2.product_id = "p1";
    dev2.status = DeviceStatus::IDLE;
    dev2.last_heartbeat_at = 0; // never sent heartbeat
    dm.register_device(dev2);

    auto offline = dm.list_offline_devices(180);
    // dev2 should be offline (last_heartbeat_at = 0)
    bool found_offline = false;
    for (const auto& d : offline) {
        if (d.device_id == "dev-offline") found_offline = true;
    }
    EXPECT_TRUE(found_offline);
}

TEST(DeviceManagerTest, ActivationIntegration) {
    Database db;
    ASSERT_EQ(db.open(":memory:"), StatusCode::OK);

    DeviceActivation activation(db);
    DeviceManager dm;
    dm.set_database(&db);

    ActivationRequest req;
    req.hardware_uid = "HW-UNIT-TEST-001";
    req.model = "ACM-200";
    req.firmware_version = "1.0.0";
    req.mac_address = "AA:BB:CC:DD:EE:FF";
    req.tenant_id = "tenant_test";
    req.device_type = DeviceType::COFFEE_MACHINE;

    ActivationResponse resp = activation.process_activation(req, "127.0.0.1");
    EXPECT_TRUE(resp.success);
    EXPECT_FALSE(resp.device_id.empty());
    EXPECT_FALSE(resp.activation_token.empty());

    // Register with DeviceManager
    Device dev;
    dev.device_id = resp.device_id;
    dev.tenant_id = resp.tenant_id;
    dev.product_id = resp.product_id;
    dev.type = req.device_type;
    dev.model = req.model;
    dev.hardware_uid = req.hardware_uid;
    dev.mac_address = req.mac_address;
    dev.firmware_version = req.firmware_version;
    dev.status = DeviceStatus::OFFLINE;
    dev.activated = true;
    dm.register_device(dev);

    EXPECT_EQ(dm.total_device_count(), 1);
    auto found = dm.get_device("tenant_test", resp.device_id);
    EXPECT_TRUE(found.has_value());
    EXPECT_TRUE(found->activated);

    db.close();
}
