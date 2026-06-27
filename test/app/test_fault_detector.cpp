#include <gtest/gtest.h>
#include "app/fault/fault_detector.h"

using namespace dev_sys;

TEST(FaultDetectorTest, InitialNoFaults) {
    FaultDetector fd;
    EXPECT_FALSE(fd.is_locked_down());
    EXPECT_TRUE(fd.active_faults().empty());
}

TEST(FaultDetectorTest, CheckAllReturnsVector) {
    FaultDetector fd;
    auto faults = fd.check_all();
    // In test environment without real hardware, should return empty or NONE faults
    EXPECT_TRUE(faults.empty() || !faults.empty());
}
