//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <gtest/gtest.h>

#include "lir/test/fixtures/mqtt_client.h"

namespace lir::edge {
namespace {

using namespace std::chrono_literals;
using mqtt::QoS;
using mqtt::Version;
using test::MqttClient;

class EdgeTest : public ::testing::Test {
 protected:
  explicit EdgeTest(EdgeConfig config = with_any_port()) : edge(io.context, std::move(config), dannan::contract::steady_time_source(), allow, logger()) {
    edge.start();
  }

  static EdgeConfig with_any_port() {
    EdgeConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    return config;
  }
  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  bool connections_reach(size_t wanted) {
    return test::eventually([&] { return edge.call<size_t>([](broker::Broker& b) { return b.connections(); }) == wanted; }, 2s);
  }

  test::Io io;
  contract::AllowAll allow;
  Edge edge;
};

TEST_F(EdgeTest, ListensOnTheBoundPort) { EXPECT_NE(edge.port(), 0); }

TEST_F(EdgeTest, ConnectAndPing) {
  MqttClient client(edge.port());
  client.connect("a");
  client.send(mqtt::Pingreq{});
  EXPECT_TRUE(client.receive_a<mqtt::Pingresp>());
}

TEST_F(EdgeTest, PublishReachesAnotherSocket) {
  MqttClient sub(edge.port(), Version::v3_1_1);
  MqttClient pub(edge.port());
  sub.connect("sub");
  pub.connect("pub");
  sub.subscribe("room/+/temp");
  pub.publish("room/1/temp", "21.5", QoS::at_least_once);
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "room/1/temp");
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "21.5");
}

TEST_F(EdgeTest, ProtocolErrorClosesTheSocket) {
  MqttClient client(edge.port());
  client.connect("a");
  client.send(mqtt::Connect{Version::v5, true, 0, "a", {}, {}, {}, {}});
  const auto disconnect = client.receive_a<mqtt::Disconnect>();
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, mqtt::reason::protocol_error);
  EXPECT_TRUE(client.closed_by_server());
  EXPECT_TRUE(connections_reach(0));
}

TEST_F(EdgeTest, DroppedSocketPublishesTheWill) {
  MqttClient watcher(edge.port());
  watcher.connect("watcher");
  watcher.subscribe("wills/#", QoS::at_most_once);
  {
    MqttClient dying(edge.port());
    mqtt::Will will;
    will.topic = "wills/dying";
    will.payload = {'x'};
    dying.connect("dying", true, will);
    dying.abandon();
  }
  const auto got = watcher.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "wills/dying");
}

TEST_F(EdgeTest, StopClosesEveryClient) {
  MqttClient client(edge.port());
  client.connect("a");
  edge.stop();
  EXPECT_TRUE(client.closed_by_server());
}

class SlowEdgeTest : public EdgeTest {
 protected:
  SlowEdgeTest() : EdgeTest(small_queue()) {}
  static EdgeConfig small_queue() {
    auto config = with_any_port();
    config.maximum_outbound_bytes = 256 * 1024;
    return config;
  }
};

TEST_F(SlowEdgeTest, AClientThatDoesNotReadIsClosedNotWaitedFor) {
  MqttClient slow(edge.port(), Version::v5, 4096);
  slow.connect("slow");
  slow.subscribe("flood", QoS::at_most_once);
  MqttClient pub(edge.port());
  pub.connect("pub");
  const std::string big(60000, 'x');
  for (int i = 0; i < 400; ++i) pub.publish("flood", big);
  EXPECT_TRUE(connections_reach(1));
  // The publisher was never held up: it still gets its answers.
  pub.send(mqtt::Pingreq{});
  EXPECT_TRUE(pub.receive_a<mqtt::Pingresp>());
}

}  // namespace
}  // namespace lir::edge
