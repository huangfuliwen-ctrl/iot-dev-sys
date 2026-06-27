#include <gtest/gtest.h>
#include "app/ota/ota_manager.h"

using namespace dev_sys;

TEST(OtaManagerTest, InitialState) {
    OtaManager ota;
    EXPECT_FALSE(ota.is_install_pending());
    EXPECT_EQ(ota.current_version(), "1.0.0");
}

TEST(OtaManagerTest, InstallWithoutDownloadFails) {
    OtaManager ota;
    auto result = ota.install_firmware();
    EXPECT_NE(result, StatusCode::OK);
}
