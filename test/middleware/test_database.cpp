#include <gtest/gtest.h>
#include "middleware/storage/database.h"

using namespace dev_sys;

TEST(DatabaseTest, OpenClose) {
    Database db;
    auto result = db.open(":memory:");
    EXPECT_EQ(result, StatusCode::OK);
    db.close();
}

TEST(DatabaseTest, ExecuteWithoutOpen) {
    Database db;
    auto result = db.execute("CREATE TABLE test (id INTEGER)");
    EXPECT_NE(result, StatusCode::OK);
}
