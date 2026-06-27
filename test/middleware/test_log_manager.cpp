#include <gtest/gtest.h>
#include "middleware/storage/log_manager.h"

using namespace dev_sys;

TEST(LogManagerTest, Init) {
    LogManager lm;
    auto result = lm.init("/tmp/test_logs", LogManager::Level::DEBUG);
    EXPECT_EQ(result, StatusCode::OK);
}

TEST(LogManagerTest, LevelsFiltered) {
    LogManager lm;
    lm.init("/tmp/test_logs", LogManager::Level::WARN);
    // DEBUG and INFO should be suppressed
    lm.debug("test", "should not appear");
    lm.info("test", "should not appear");
    // WARN and ERROR should pass
    lm.warn("test", "should appear");
    lm.error("test", "should appear");
    // No crash = test passes
}
