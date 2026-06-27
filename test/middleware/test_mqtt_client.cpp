#include <gtest/gtest.h>
#include "middleware/communication/mqtt_client.h"

using namespace dev_sys;

TEST(MqttClientTest, InitialState) {
    MqttClient client;
    EXPECT_FALSE(client.is_connected());
}

TEST(MqttClientTest, PublishWhenDisconnected) {
    MqttClient client;
    auto result = client.publish("test/topic", "hello");
    EXPECT_EQ(result, StatusCode::COMM_DISCONNECTED);
}
