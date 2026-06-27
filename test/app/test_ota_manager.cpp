#include <gtest/gtest.h>
#include "app/ota/ota_manager.h"

using namespace dev_sys;

TEST(OtaManagerTest, RegisterFirmware) {
    OtaManager ota;
    FirmwareVersion fw;
    fw.version = "v2.0.0";
    fw.product_id = "coffee_machine";
    fw.download_url = "https://cdn.example.com/fw.bin";
    fw.checksum_sha256 = "abc123";
    fw.changelog = "Test firmware";

    auto sc = ota.register_firmware(fw);
    EXPECT_EQ(sc, StatusCode::OK);

    auto fws = ota.list_firmwares();
    EXPECT_EQ(fws.size(), 1);
    EXPECT_EQ(fws[0].version, "v2.0.0");
}

TEST(OtaManagerTest, GetFirmware) {
    OtaManager ota;
    FirmwareVersion fw;
    fw.version = "v3.0.0";
    fw.product_id = "water_dispenser";
    fw.download_url = "https://cdn.example.com/water.bin";
    ota.register_firmware(fw);

    auto found = ota.get_firmware("v3.0.0");
    EXPECT_TRUE(found.has_value());
    EXPECT_EQ(found->product_id, "water_dispenser");

    auto missing = ota.get_firmware("nonexistent");
    EXPECT_FALSE(missing.has_value());
}

TEST(OtaManagerTest, DeleteFirmware) {
    OtaManager ota;
    FirmwareVersion fw;
    fw.version = "v4.0.0";
    fw.product_id = "coffee_machine";
    fw.download_url = "https://cdn.example.com/fw4.bin";
    ota.register_firmware(fw);

    auto sc = ota.delete_firmware("v4.0.0");
    EXPECT_EQ(sc, StatusCode::OK);

    auto fws = ota.list_firmwares();
    EXPECT_TRUE(fws.empty());
}

TEST(OtaManagerTest, PushToDeviceCreatesRecord) {
    OtaManager ota;

    // Register firmware first
    FirmwareVersion fw;
    fw.version = "v2.1.0";
    fw.product_id = "coffee_machine";
    fw.download_url = "https://cdn.example.com/v2.1.0.bin";
    fw.checksum_sha256 = "sha256check";
    ota.register_firmware(fw);

    auto sc = ota.push_to_device("tenant_test", "coffee_v1", "dev-001", "v2.1.0");
    EXPECT_EQ(sc, StatusCode::OK);

    auto record = ota.get_device_ota_status("dev-001");
    EXPECT_EQ(record.target_version, "v2.1.0");
    EXPECT_EQ(record.stage, "pending");
}

TEST(OtaManagerTest, ActiveUpgrades) {
    OtaManager ota;

    FirmwareVersion fw;
    fw.version = "v5.0.0";
    fw.product_id = "coffee_machine";
    fw.download_url = "https://cdn.example.com/v5.bin";
    ota.register_firmware(fw);

    ota.push_to_device("t1", "p1", "dev-a", "v5.0.0");
    ota.push_to_device("t1", "p1", "dev-b", "v5.0.0");

    auto active = ota.active_upgrades();
    EXPECT_EQ(active.size(), 2);
}
