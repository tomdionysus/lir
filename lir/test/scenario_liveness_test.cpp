//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lir/test/fixtures/servers.h"
#include "lir/test/fixtures/stream_client.h"

// AthenaSIP's liveness pattern (../athenasip/TODO/2026-11-23-lir-liveness-
// scenario.md), step 4's acceptance test: one topic per node,
// athenasip/nodes/<id>/status, a retained "ok" republished, a retained will
// "down", a retained "stopped" at a clean stop; every node subscribed to all
// of them with retained messages sent at subscribe. Each step is written now
// and waits, disabled, on the noun it needs: retained messages on Dagda's
// register map (D6), wills on Dagda's leases (D5), and after a heal the
// order of retained values, (hlc stamp, node id) (L1, settled), which D6
// carries.
namespace lir::scenario {
namespace {

using namespace std::chrono_literals;
using test::max_steps;

const std::string statuses = "athenasip/nodes/+/status";

std::string topic_of(const std::string& node) { return "athenasip/nodes/" + node + "/status"; }

mqtt::Publish status(const std::string& node, const std::string& state) {
  auto out = test::publish_of(topic_of(node), state);
  out.retain = true;
  return out;
}

// One AthenaSIP node, connected to Lir node `lir` as the pattern does.
struct Node {
  Node(test::Servers& cluster, size_t lir, std::string name) : client(cluster.world, cluster.servers.at(lir)->listener()), id(std::move(name)) {
    mqtt::Connect connect;
    connect.client_id = id;
    connect.keep_alive = 30;
    connect.will = mqtt::Will{topic_of(id), mqtt::Bytes{'d', 'o', 'w', 'n'}, mqtt::QoS::at_most_once, true, {}};
    EXPECT_EQ(client.connect(connect).reason, mqtt::reason::success);
    client.send(status(id, "ok"));
    mqtt::Subscription all{statuses};
    all.retain_handling = 0;
    client.subscribe(all);
  }
  // The last state each topic was seen in, from what arrived.
  std::map<std::string, std::string> seen() {
    for (const auto& publish : client.publishes()) last[publish.topic] = std::string(publish.payload.begin(), publish.payload.end());
    return last;
  }
  test::StreamClient client;
  std::string id;
  std::map<std::string, std::string> last;
};

// 1. Each node hears every other's retained "ok", whichever Lir node it is on.
TEST(ScenarioLiveness, DISABLED_EveryNodeHearsEveryOthersStatus_WaitsOnDagdaD6AndD5) {
  test::Servers cluster(3);
  ASSERT_TRUE(cluster.joined);
  Node a(cluster, 0, "a"), b(cluster, 1, "b"), c(cluster, 2, "c");
  for (auto* node : {&a, &b, &c})
    for (const auto* name : {"a", "b", "c"}) EXPECT_EQ(node->seen()[topic_of(name)], "ok") << node->id << " of " << name;
}

// 2. A late subscriber has every retained "ok" at subscribe, with nobody
// publishing again.
TEST(ScenarioLiveness, DISABLED_ALateSubscriberHasEveryStatusAtSubscribe_WaitsOnDagdaD6) {
  test::Servers cluster(3);
  ASSERT_TRUE(cluster.joined);
  Node a(cluster, 0, "a"), b(cluster, 1, "b"), c(cluster, 2, "c");
  (void)a.seen();
  test::StreamClient late(cluster.world, cluster.servers[2]->listener());
  late.connect();
  late.subscribe(statuses);
  std::map<std::string, std::string> retained;
  for (const auto& publish : late.publishes())
    if (publish.retain) retained[publish.topic] = std::string(publish.payload.begin(), publish.payload.end());
  EXPECT_EQ(retained, (std::map<std::string, std::string>{{topic_of("a"), "ok"}, {topic_of("b"), "ok"}, {topic_of("c"), "ok"}}));
}

// 3. A clean stop: "stopped" everywhere, and the will discarded by a normal
// DISCONNECT [MQTT-3.14.4-3].
TEST(ScenarioLiveness, DISABLED_ACleanStopLeavesStoppedAndNoWill_WaitsOnDagdaD6AndD5) {
  test::Servers cluster(3);
  ASSERT_TRUE(cluster.joined);
  Node a(cluster, 0, "a"), b(cluster, 1, "b");
  b.client.send(status("b", "stopped"));
  b.client.send(mqtt::Disconnect{});
  cluster.world.run_until([] { return false; }, 400'000);
  EXPECT_EQ(a.seen()[topic_of("b")], "stopped");
}

// 4. A node that dies without DISCONNECT: its will, "down", replaces its
// "ok" everywhere once one and a half keep-alives pass. How long that takes
// is what AthenaSIP measures.
TEST(ScenarioLiveness, DISABLED_ADeadNodesWillArrivesAfterItsKeepAlive_WaitsOnDagdaD6AndD5) {
  test::Servers cluster(3);
  ASSERT_TRUE(cluster.joined);
  Node a(cluster, 0, "a"), c(cluster, 2, "c");
  (void)a.seen();
  const auto cut = cluster.world.now_ms();
  c.client.link->stall();
  ASSERT_TRUE(cluster.world.run_until([&] { return a.seen()[topic_of("c")] == "down"; }, max_steps));
  const auto took = cluster.world.now_ms() - cut;
  EXPECT_GE(took, 45'000u);
  EXPECT_LE(took, 50'000u);
}

// 5. The Lir node a client is on dies: the client's will is published unless
// it is back first; once it reconnects elsewhere and says "ok", "ok" stays.
// A will from the older connection never overwrites the newer "ok": the
// client id's lease carries an epoch, and the will its own.
TEST(ScenarioLiveness, DISABLED_AStaleWillNeverOverwritesANewerOk_WaitsOnDagdaD5AndD6) {
  test::Servers cluster(3);
  ASSERT_TRUE(cluster.joined);
  Node b(cluster, 1, "b");
  std::optional<Node> a;
  a.emplace(cluster, 0, "a");
  (void)b.seen();
  cluster.nodes.stop(0);
  a.reset();
  Node again(cluster, 2, "a");
  cluster.world.run_until([] { return false; }, 800'000);
  EXPECT_EQ(b.seen()[topic_of("a")], "ok");
}

// 6 and 7. A split keeps delivering within each side and publishes no will
// for a client still connected; after the heal each topic's retained message
// is the last one published or will-published, by Lir's order.
TEST(ScenarioLiveness, DISABLED_AfterAHealEachTopicHoldsItsLastStatus_WaitsOnDagdaD6AndD5) {
  test::Servers cluster(3);
  ASSERT_TRUE(cluster.joined);
  Node a(cluster, 0, "a"), b(cluster, 1, "b"), c(cluster, 2, "c");
  (void)a.seen();
  cluster.nodes.network().partition({{0, 1}, {2}});
  a.client.send(status("a", "degraded"));
  c.client.send(status("c", "degraded"));
  EXPECT_EQ(b.seen()[topic_of("a")], "degraded");
  EXPECT_NE(b.seen()[topic_of("c")], "down");
  cluster.nodes.network().heal();
  cluster.world.run_until([] { return false; }, 800'000);
  test::StreamClient late(cluster.world, cluster.servers[1]->listener());
  late.connect();
  late.subscribe(statuses);
  std::map<std::string, std::string> retained;
  for (const auto& publish : late.publishes())
    if (publish.retain) retained[publish.topic] = std::string(publish.payload.begin(), publish.payload.end());
  EXPECT_EQ(retained[topic_of("a")], "degraded");
  EXPECT_EQ(retained[topic_of("c")], "degraded");
  EXPECT_EQ(retained[topic_of("b")], "ok");
}

}  // namespace
}  // namespace lir::scenario
