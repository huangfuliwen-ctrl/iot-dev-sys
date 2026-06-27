#include <gtest/gtest.h>
#include "app/device/device_manager.h"

using namespace dev_sys;

TEST(DeviceManagerTest, InitialState) {
    DeviceManager dm;
    EXPECT_FALSE(dm.is_activated());
    EXPECT_EQ(dm.status(), DeviceStatus::OFFLINE);
}

TEST(DeviceManagerTest, SetDeviceType) {
    DeviceManager dm;
    dm.set_device_type(DeviceType::COFFEE_MACHINE);
    EXPECT_EQ(dm.device_type(), DeviceType::COFFEE_MACHINE);
}

TEST(DeviceManagerTest, HeartbeatBuild) {
    DeviceManager dm;
    dm.set_device_type(DeviceType::WATER_DISPENSER);
    auto hb = dm.build_heartbeat();
    EXPECT_EQ(hb.status, DeviceStatus::OFFLINE);
    EXPECT_FALSE(hb.firmware_version.empty());
}
