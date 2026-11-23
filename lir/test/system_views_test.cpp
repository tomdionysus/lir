//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "dannan/json/json.h"
#include "lir/test/fixtures/servers.h"
#include "lir/test/fixtures/stream_client.h"

// The system topics (TODO/2026-11-22-system-topics.md), on a cluster where
// every node runs Lir's server: each view rendered by the subscriber's own
// node from what Dannan gossips, saying so; the connection events published
// by the node that saw them. No node publishes for another.
namespace lir::system {
namespace {

using test::max_steps;
namespace json = dannan::json;

json::object payload(const mqtt::Publish& publish) {
  const auto parsed = json::parse(std::span<const uint8_t>(publish.payload.data(), publish.payload.size()));
  return parsed && parsed->is_object() ? parsed->as_object() : json::object{};
}
std::string text(const json::object& object, std::string_view key) {
  const auto* value = object.if_contains(key);
  return value && value->is_string() ? std::string(value->as_string()) : std::string();
}

struct Cluster : test::Servers {
  using Servers::Servers;
  // Long enough for every node's figures to be advertised and rendered.
  void settle() {
    world.run_until([] { return false; }, 400'000);
  }
  test::StreamClient client(size_t node) { return test::StreamClient(world, servers.at(node)->listener()); }
};

std::vector<mqtt::Publish> on(const std::vector<mqtt::Publish>& all, std::string_view topic) {
  std::vector<mqtt::Publish> out;
  std::copy_if(all.begin(), all.end(), std::back_inserter(out), [&](const auto& publish) { return publish.topic == topic; });
  return out;
}

// Each node's figures reach a subscriber on any node at SUBSCRIBE, with
// RETAIN 1, each saying which node rendered it: the subscriber's own.
TEST(SystemViews, ASubscriberOnAnyNodeGetsEveryNodesFigures) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  cluster.settle();
  auto subscriber = cluster.client(1);
  subscriber.connect();
  subscriber.subscribe("$lir/nodes/+");
  const auto received = subscriber.publishes();
  std::set<std::string> nodes;
  for (const auto& publish : received) {
    if (!publish.retain) continue;
    const auto figures = payload(publish);
    EXPECT_EQ(text(figures, "from"), cluster.id(1));
    EXPECT_EQ(publish.topic, "$lir/nodes/" + text(figures, "node"));
    EXPECT_TRUE(figures.contains("connections"));
    EXPECT_TRUE(figures.contains("limits"));
    nodes.insert(text(figures, "node"));
  }
  EXPECT_EQ(nodes, (std::set<std::string>{cluster.id(0), cluster.id(1), cluster.id(2)}));
}

// A node that goes empties its topics on every other node; back, it fills
// them again.
TEST(SystemViews, ANodeLeavingEmptiesItsTopicsAndRejoiningFillsThem) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  cluster.settle();
  auto subscriber = cluster.client(0);
  subscriber.connect();
  subscriber.subscribe("$lir/nodes/+");
  (void)subscriber.publishes();
  cluster.nodes.restart(2);
  const auto received = on(subscriber.publishes(800'000), "$lir/nodes/" + cluster.id(2));
  ASSERT_FALSE(received.empty());
  const auto emptied = std::find_if(received.begin(), received.end(), [](const auto& publish) { return publish.payload.empty(); });
  ASSERT_NE(emptied, received.end()) << "no empty message when node 2 went";
  EXPECT_TRUE(std::any_of(emptied, received.end(), [](const auto& publish) { return !publish.payload.empty(); })) << "nothing when node 2 came back";
}

// `$lir/cluster` is computed alike on every node from the same adverts: once
// gossip has converged two nodes agree, each saying it is its own answer.
TEST(SystemViews, TheClusterViewAgreesAcrossNodesOnceConverged) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  auto first = cluster.client(0);
  auto second = cluster.client(2);
  first.connect();
  second.connect();
  cluster.settle();
  first.subscribe("$lir/cluster");
  second.subscribe("$lir/cluster");
  const auto a = on(first.publishes(), "$lir/cluster");
  const auto b = on(second.publishes(), "$lir/cluster");
  ASSERT_FALSE(a.empty());
  ASSERT_FALSE(b.empty());
  auto from_first = payload(a.back());
  auto from_second = payload(b.back());
  EXPECT_EQ(text(from_first, "from"), cluster.id(0));
  EXPECT_EQ(text(from_second, "from"), cluster.id(2));
  from_first.erase("from");
  from_second.erase("from");
  EXPECT_EQ(from_first, from_second);
  EXPECT_EQ(from_first.at("nodes").to_number<uint64_t>(), 3u);
  EXPECT_EQ(from_first.at("connections").to_number<uint64_t>(), 2u);
  EXPECT_TRUE(from_first.at("limits_agree").as_bool());
}

// A roster event reaches a subscriber once per change, as its own node saw
// it, never once for every node in the cluster.
TEST(SystemViews, ARosterEventArrivesOncePerChange) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  cluster.settle();
  auto subscriber = cluster.client(0);
  subscriber.connect();
  subscriber.subscribe("$dannan/roster/#");
  cluster.nodes.stop(2);
  const auto left = on(subscriber.publishes(800'000), "$dannan/roster/left");
  const auto of_two = std::count_if(left.begin(), left.end(), [&](const auto& publish) { return text(payload(publish), "node") == cluster.id(2); });
  EXPECT_EQ(of_two, 1);
  for (const auto& publish : left) EXPECT_EQ(text(payload(publish), "from"), cluster.id(0));
}

// A client connecting on one node is seen by a subscriber on another: the
// node that saw it publishes it, with no client address (the maintainer,
// 2026-11-22).
TEST(SystemViews, AClientConnectingOnOneNodeIsSeenOnAnother) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  auto subscriber = cluster.client(0);
  subscriber.connect();
  ASSERT_EQ(subscriber.subscribe("$lir/clients/#").reasons, std::vector<uint8_t>{mqtt::reason::granted_qos_0});
  auto watched = cluster.client(1);
  mqtt::Connect connect;
  connect.client_id = "watched";
  watched.connect(connect);
  watched.send(mqtt::Disconnect{});
  const auto received = subscriber.publishes();
  const auto connected = on(received, "$lir/clients/connected");
  const auto disconnected = on(received, "$lir/clients/disconnected");
  ASSERT_EQ(connected.size(), 1u);
  ASSERT_EQ(disconnected.size(), 1u);
  const auto event = payload(connected[0]);
  EXPECT_EQ(text(event, "from"), cluster.id(1));
  EXPECT_EQ(text(event, "client_id"), "watched");
  EXPECT_EQ(text(event, "version"), "5.0");
  EXPECT_EQ(text(event, "transport"), "memory");
  EXPECT_FALSE(event.contains("peer"));
  const auto gone = payload(disconnected[0]);
  EXPECT_EQ(text(gone, "client_id"), "watched");
  EXPECT_EQ(gone.at("reason").to_number<uint64_t>(), mqtt::reason::normal_disconnection);
}

// Retain Handling says when the views are sent: 0 at every SUBSCRIBE, 1 only
// for a subscription that did not exist, 2 never [MQTT-3.3.1-9] to
// [MQTT-3.3.1-11].
TEST(SystemViews, RetainHandlingSaysWhenTheViewsAreSent) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  cluster.settle();
  const auto retained = [](const std::vector<mqtt::Publish>& received) {
    return std::count_if(received.begin(), received.end(), [](const auto& publish) { return publish.retain; });
  };
  auto never = cluster.client(0);
  never.connect();
  mqtt::Subscription two{"$lir/cluster"};
  two.retain_handling = 2;
  never.subscribe(two);
  EXPECT_EQ(retained(never.publishes()), 0);

  auto client = cluster.client(0);
  client.connect();
  mqtt::Subscription one{"$lir/cluster"};
  one.retain_handling = 1;
  client.subscribe(one);
  EXPECT_EQ(retained(client.publishes()), 1);
  client.subscribe(one, 2);
  EXPECT_EQ(retained(client.publishes()), 0);
  client.subscribe(mqtt::Subscription{"$lir/cluster"}, 3);
  EXPECT_EQ(retained(client.publishes()), 1);
}

// '#' does not match a `$` topic [MQTT-4.7.2-1]: no view or event reaches it.
TEST(SystemViews, MQTT_4_7_2_1) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  auto subscriber = cluster.client(0);
  subscriber.connect();
  subscriber.subscribe("#");
  auto other = cluster.client(1);
  other.connect();
  cluster.nodes.restart(1);
  for (const auto& publish : subscriber.publishes(800'000)) EXPECT_NE(publish.topic.front(), '$') << publish.topic;
}

}  // namespace
}  // namespace lir::system
