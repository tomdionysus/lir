//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Lir's cluster in one process: three Dannan nodes over loopback, each
// with an edge and the cluster, and MQTT clients on real sockets. One
// broker, whichever node a client is connected to.

#include "lir/cluster/cluster.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

#include "dannan/net/fanout.h"
#include "dannan/node.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/system/system.h"
#include "lir/test/fixtures/mqtt_client.h"

namespace lir::cluster {
namespace {

using namespace std::chrono_literals;
using mqtt::QoS;
using mqtt::Version;
using test::eventually;
using test::MqttClient;

std::string text(const mqtt::Publish& p) { return {p.payload.begin(), p.payload.end()}; }

struct LirNode {
  std::unique_ptr<dannan::Node> node;
  std::unique_ptr<edge::Edge> edge;
  std::unique_ptr<Cluster> cluster;
  std::unique_ptr<system::SystemTopics> system;
  contract::AllowAll allow;

  LirNode(boost::asio::io_context& io, const std::filesystem::path& state, const dannan::identity::ClusterKey& key,
          std::vector<dannan::net::Endpoint> bootstrap) {
    dannan::NodeConfig config;
    config.state_path = state;
    config.key = key;
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = 0;
    config.bootstrap = std::move(bootstrap);
    config.inbound = dannan::reach::Inbound::yes;
    config.heartbeat = 100ms;
    config.dead_after = 2s;
    config.telemetry_interval = 100ms;
    config.connect_timeout = 1s;
    config.version = "lir-test";
    config.log = std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error);
    node = std::make_unique<dannan::Node>(io, config);
    edge::EdgeConfig edge_config;
    edge_config.listen_host = "127.0.0.1";
    edge_config.port = 0;
    edge = std::make_unique<edge::Edge>(io, edge_config, dannan::contract::steady_time_source(), allow, node->context().logger("lir"));
    cluster = std::make_unique<Cluster>(node->context(), *edge);
    node->start();
    cluster->start();
    edge->start();
    system = std::make_unique<system::SystemTopics>(node->context(), *edge, system::SystemConfig{200ms});
    system->start();
  }
  ~LirNode() {
    system->stop();
    edge->stop();
    cluster->stop();
    node->stop();
  }

  uint16_t port() const { return edge->port(); }
  dannan::identity::NodeId id() const { return node->context().identity().id(); }
  template <typename T>
  T on_broker(std::function<T(broker::Broker&)> fn) {
    return edge->call<T>(std::move(fn));
  }
  // Retained values on clients' topics, the system topics left out.
  size_t retained() {
    return on_broker<size_t>([](broker::Broker& b) {
      size_t count = 0;
      for (const auto& m : b.retained_entries()) count += !m.topic.starts_with("$") && !m.payload.empty() ? 1 : 0;
      return count;
    });
  }

  // Whether this node sees `other` serving Lir and, when `prefix` is given,
  // subscribed on `channel` to it.
  bool sees(const LirNode& other, const std::string& channel = {}, const std::string& prefix = {}) const {
    for (const auto& record : node->context().nodes().active()) {
      if (record.id != other.id() || !record.advert("lir")) continue;
      if (channel.empty()) return true;
      const auto interest = record.advert(dannan::net::interest_advert(channel));
      if (!interest) return false;
      for (const auto& p : dannan::net::decode_interest(*interest)) {
        if (p == prefix) return true;
      }
      return false;
    }
    return false;
  }
};

class ClusterTest : public ::testing::Test {
 protected:
  ClusterTest() {
    std::ofstream(dir.path() / "cluster.key") << "lir cluster test";
    key = dannan::identity::load_cluster_key(dir.path() / "cluster.key");
    a = start("a", {});
    b = start("b", {{"127.0.0.1", a->node->port()}});
    c = start("c", {{"127.0.0.1", a->node->port()}});
    EXPECT_TRUE(eventually([&] { return formed({a.get(), b.get(), c.get()}); }));
  }

  std::unique_ptr<LirNode> start(const std::string& name, std::vector<dannan::net::Endpoint> bootstrap) {
    return std::make_unique<LirNode>(io.context, dir.path() / name, key, std::move(bootstrap));
  }

  static bool formed(const std::vector<LirNode*>& nodes) {
    for (const auto* x : nodes) {
      for (const auto* y : nodes) {
        if (x != y && !x->sees(*y)) return false;
      }
    }
    return true;
  }

  test::Io io;
  dannan::test::TempDir dir;
  dannan::identity::ClusterKey key;
  std::unique_ptr<LirNode> a;
  std::unique_ptr<LirNode> b;
  std::unique_ptr<LirNode> c;
};

TEST_F(ClusterTest, QoS1PublishReachesASubscriberOnAnotherNode) {
  MqttClient sub(b->port());
  sub.connect("sub");
  sub.subscribe("room/+/temp", QoS::at_least_once);
  ASSERT_TRUE(eventually([&] { return a->sees(*b, "lir.seq", "room/"); }));
  MqttClient pub(a->port(), Version::v3_1_1);
  pub.connect("pub");
  pub.publish("room/1/temp", "21.5", QoS::at_least_once);
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "room/1/temp");
  EXPECT_EQ(text(*got), "21.5");
  EXPECT_EQ(got->qos, QoS::at_least_once);
}

TEST_F(ClusterTest, QoS0AndQoS2CrossNodesOnceEach) {
  MqttClient sub(c->port());
  sub.connect("sub");
  sub.subscribe("t", QoS::exactly_once);
  ASSERT_TRUE(eventually([&] { return a->sees(*c, "lir.notify", "t") && a->sees(*c, "lir.seq", "t"); }));
  MqttClient pub(a->port());
  pub.connect("pub");
  pub.publish("t", "zero", QoS::at_most_once);
  pub.publish("t", "two", QoS::exactly_once);
  std::vector<std::string> got;
  while (auto p = sub.next_message()) got.push_back(text(*p));
  EXPECT_EQ(got, (std::vector<std::string>{"zero", "two"}));
}

TEST_F(ClusterTest, SubscribersOnEveryNodeEachGetOneCopy) {
  MqttClient sa(a->port()), sb(b->port()), sc(c->port());
  sa.connect("sa");
  sb.connect("sb");
  sc.connect("sc");
  for (auto* s : {&sa, &sb, &sc}) s->subscribe("all/#", QoS::at_least_once);
  ASSERT_TRUE(eventually([&] { return b->sees(*a, "lir.seq", "all/") && b->sees(*c, "lir.seq", "all/"); }));
  MqttClient pub(b->port());
  pub.connect("pub");
  pub.publish("all/x", "hello", QoS::at_least_once);
  for (auto* s : {&sa, &sb, &sc}) {
    const auto got = s->next_message();
    ASSERT_TRUE(got);
    EXPECT_EQ(text(*got), "hello");
    EXPECT_FALSE(s->receive_a<mqtt::Publish>());
  }
}

TEST_F(ClusterTest, RetainedValuesAreEverywhereAndClearedEverywhere) {
  MqttClient pub(a->port());
  pub.connect("pub");
  pub.publish("state/door", "open", QoS::at_least_once, true);
  ASSERT_TRUE(eventually([&] { return b->retained() == 1 && c->retained() == 1; }));
  MqttClient sub(c->port());
  sub.connect("sub");
  sub.subscribe("state/#", QoS::at_most_once);
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->retain);
  EXPECT_EQ(text(*got), "open");
  MqttClient clearer(b->port());
  clearer.connect("clearer");
  clearer.publish("state/door", "", QoS::at_least_once, true);
  EXPECT_TRUE(eventually([&] { return a->retained() == 0 && b->retained() == 0 && c->retained() == 0; }));
}

TEST_F(ClusterTest, TheLaterRetainedValueWinsOnEveryNode) {
  MqttClient first(a->port()), second(c->port());
  first.connect("first");
  second.connect("second");
  first.publish("v", "1", QoS::at_least_once, true);
  ASSERT_TRUE(eventually([&] { return c->retained() == 1; }));
  second.publish("v", "2", QoS::at_least_once, true);
  for (auto* node : {a.get(), b.get(), c.get()}) {
    ASSERT_TRUE(eventually([&] {
      return node->on_broker<std::string>([](broker::Broker& broker) {
        for (const auto& m : broker.retained_entries())
          if (m.topic == "v") return std::string(m.payload.begin(), m.payload.end());
        return std::string();
      }) == "2";
    }));
  }
}

TEST_F(ClusterTest, ANodeThatJoinsLateCatchesUpOnRetained) {
  MqttClient pub(a->port());
  pub.connect("pub");
  pub.publish("late/a", "x", QoS::at_least_once, true);
  pub.publish("late/b", "y", QoS::at_least_once, true);
  ASSERT_TRUE(eventually([&] { return b->retained() == 2; }));
  auto d = start("d", {{"127.0.0.1", b->node->port()}});
  EXPECT_TRUE(eventually([&] { return d->retained() == 2; }));
}

TEST_F(ClusterTest, APersistentSessionResumesOnAnotherNodeWithWhatWasQueued) {
  {
    MqttClient first(a->port(), Version::v3_1_1);
    first.connect("roamer", false);
    first.subscribe("inbox", QoS::at_least_once);
    first.send(mqtt::Disconnect{});
  }
  ASSERT_TRUE(eventually([&] { return c->sees(*a, "lir.seq", "inbox"); }));
  MqttClient pub(c->port());
  pub.connect("pub");
  pub.publish("inbox", "while you were away", QoS::at_least_once);
  ASSERT_TRUE(eventually([&] { return a->on_broker<uint64_t>([](broker::Broker& broker) { return broker.stats().messages_injected; }) >= 1; }));
  MqttClient again(b->port(), Version::v3_1_1);
  const auto ack = again.connect("roamer", false);
  EXPECT_TRUE(ack.session_present);
  const auto got = again.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(text(*got), "while you were away");
  // The session left the node it was on.
  EXPECT_EQ(a->on_broker<size_t>([](broker::Broker& broker) { return broker.sessions(); }), 0u);
}

// Tom, 2026-11-04: sessions are mirrored to a standby until they live in
// Dagda, so one node's loss does not lose them.
TEST_F(ClusterTest, APersistentSessionSurvivesItsNodeGoing) {
  mqtt::Properties keep;
  keep.session_expiry_interval = 600;
  {
    MqttClient first(a->port());
    first.connect("survivor", false, std::nullopt, keep);
    first.subscribe("inbox", QoS::at_least_once);
    first.send(mqtt::Disconnect{});
  }
  // The mirror goes to the standby within a mirror interval or two.
  std::this_thread::sleep_for(500ms);
  a.reset();
  const auto holds = [](LirNode& node) { return node.on_broker<bool>([](broker::Broker& broker) { return broker.local_filters().contains("inbox"); }); };
  ASSERT_TRUE(eventually([&] { return holds(*b) || holds(*c); }, 15s));
  LirNode& keeper = holds(*b) ? *b : *c;
  LirNode& other = &keeper == b.get() ? *c : *b;
  ASSERT_TRUE(eventually([&] { return other.sees(keeper, "lir.seq", "inbox"); }));
  MqttClient pub(other.port());
  pub.connect("pub");
  pub.publish("inbox", "kept for you", QoS::at_least_once);
  ASSERT_TRUE(eventually([&] { return keeper.on_broker<uint64_t>([](broker::Broker& broker) { return broker.stats().messages_injected; }) >= 1; }));
  MqttClient again(other.port());
  const auto ack = again.connect("survivor", false, std::nullopt, keep);
  EXPECT_TRUE(ack.session_present);
  const auto got = again.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(text(*got), "kept for you");
}

TEST_F(ClusterTest, EveryNodesSystemTopicsAreRetainedEverywhere) {
  MqttClient watcher(b->port());
  watcher.connect("watcher");
  for (const auto* prefix : {"$lir/nodes/", "$dannan/nodes/"}) {
    std::set<std::string> nodes;
    ASSERT_TRUE(eventually([&] {
      nodes.clear();
      for (const auto& m : b->on_broker<std::vector<broker::Message>>([](broker::Broker& broker) { return broker.retained_entries(); })) {
        if (m.topic.starts_with(prefix) && !m.payload.empty()) nodes.insert(m.topic.substr(std::string(prefix).size()));
      }
      return nodes.size() == 3;
    }));
    EXPECT_TRUE(nodes.contains(dannan::identity::to_string(a->id())));
  }
  watcher.subscribe("$lir/nodes/" + dannan::identity::to_string(c->id()), QoS::at_most_once);
  const auto got = watcher.next_message();
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->retain);
  EXPECT_EQ(got->properties.content_type, "application/json");
  EXPECT_NE(text(*got).find("\"connections\":"), std::string::npos);
  // Clients may read the system topics, never write them.
  watcher.send([] {
    mqtt::Publish p;
    p.topic = "$lir/nodes/forged";
    p.qos = QoS::at_least_once;
    p.packet_id = 77;
    return p;
  }());
  const auto refused = watcher.receive_a<mqtt::Ack>();
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->reason, mqtt::reason::not_authorized);
}

TEST_F(ClusterTest, ANodeThatLeavesHasItsSystemTopicsCleared) {
  const auto gone = dannan::identity::to_string(c->id());
  const auto has = [&](LirNode& node) {
    return node.on_broker<bool>([&](broker::Broker& broker) {
      for (const auto& m : broker.retained_entries())
        if (m.topic == "$lir/nodes/" + gone && !m.payload.empty()) return true;
      return false;
    });
  };
  ASSERT_TRUE(eventually([&] { return has(*a); }));
  c.reset();
  EXPECT_TRUE(eventually([&] { return !has(*a) && !has(*b); }, 15s));
}

// The next message on `topic` whose payload contains `needle`, within a
// bound; others are skipped.
std::optional<std::string> next_containing(MqttClient& client, const std::string& topic, const std::string& needle, std::chrono::seconds bound = 10s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    const auto got = client.next_message();
    if (!got) continue;
    const auto payload = text(*got);
    if (got->topic == topic && payload.find(needle) != std::string::npos) return payload;
  }
  return std::nullopt;
}

TEST_F(ClusterTest, ClientEventsReachEveryNode) {
  MqttClient watcher(b->port());
  watcher.connect("watcher");
  watcher.subscribe("$lir/clients/#", QoS::at_most_once);
  ASSERT_TRUE(eventually([&] { return a->sees(*b, "lir.notify", "$lir/clients/"); }));
  {
    MqttClient visitor(a->port(), Version::v3_1_1);
    visitor.connect("visitor");
    visitor.send(mqtt::Disconnect{});
  }
  const auto node = "\"node\":\"" + dannan::identity::to_string(a->id()) + "\"";
  const auto connected = next_containing(watcher, "$lir/clients/connected", "\"client\":\"visitor\"");
  ASSERT_TRUE(connected);
  EXPECT_NE(connected->find(node), std::string::npos) << *connected;
  EXPECT_NE(connected->find("\"version\":\"3.1.1\""), std::string::npos) << *connected;
  const auto disconnected = next_containing(watcher, "$lir/clients/disconnected", "\"client\":\"visitor\"");
  ASSERT_TRUE(disconnected);
  EXPECT_NE(disconnected->find("\"by\":\"client\",\"reason\":0"), std::string::npos) << *disconnected;
}

TEST_F(ClusterTest, TheLowestNodePublishesTheClusterSummary) {
  std::vector<std::string> ids{dannan::identity::to_string(a->id()), dannan::identity::to_string(b->id()), dannan::identity::to_string(c->id())};
  const auto lowest = *std::min_element(ids.begin(), ids.end());
  MqttClient one(a->port());
  one.connect("one");
  MqttClient two(c->port());
  two.connect("two");
  const auto summary = [&](LirNode& node) {
    return node.on_broker<std::string>([](broker::Broker& broker) {
      for (const auto& m : broker.retained_entries())
        if (m.topic == "$lir/cluster") return std::string(m.payload.begin(), m.payload.end());
      return std::string();
    });
  };
  EXPECT_TRUE(eventually(
      [&] {
        const auto s = summary(*b);
        return s.find("\"publisher\":\"" + lowest + "\"") != std::string::npos && s.find("\"nodes\":3,\"connections\":2") != std::string::npos &&
               s.find("\"consistent\":true") != std::string::npos;
      },
      15s))
      << summary(*b);
}

TEST_F(ClusterTest, ANodeLeavingIsOneRosterEvent) {
  MqttClient watcher(a->port());
  watcher.connect("watcher");
  watcher.subscribe("$dannan/roster/#", QoS::at_most_once);
  ASSERT_TRUE(eventually([&] { return b->sees(*a, "lir.notify", "$dannan/roster/"); }));
  const auto gone = "\"id\":\"" + dannan::identity::to_string(c->id()) + "\"";
  c.reset();
  ASSERT_TRUE(next_containing(watcher, "$dannan/roster/left", gone, 15s));
  // Only the lowest remaining node published it.
  EXPECT_FALSE(next_containing(watcher, "$dannan/roster/left", gone, 2s));
}

TEST_F(ClusterTest, ConnectingOnAnotherNodeTakesTheClientOver) {
  MqttClient first(a->port());
  first.connect("same");
  MqttClient second(b->port());
  second.connect("same");
  const auto disconnect = first.receive_a<mqtt::Disconnect>();
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, mqtt::reason::session_taken_over);
  EXPECT_TRUE(first.closed_by_server());
}

TEST_F(ClusterTest, AWillReachesSubscribersOnOtherNodes) {
  MqttClient watcher(b->port());
  watcher.connect("watcher");
  watcher.subscribe("wills/#", QoS::at_most_once);
  ASSERT_TRUE(eventually([&] { return a->sees(*b, "lir.notify", "wills/"); }));
  {
    MqttClient dying(a->port());
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

TEST_F(ClusterTest, ASharedGroupAcrossNodesGetsEachMessageOnce) {
  MqttClient wa(a->port()), wb(b->port());
  wa.connect("worker-a");
  wb.connect("worker-b");
  wa.subscribe("$share/workers/jobs/+", QoS::at_least_once);
  wb.subscribe("$share/workers/jobs/+", QoS::at_least_once);
  // Give the groups' gossip a moment to reach the publishing node.
  ASSERT_TRUE(eventually([&] { return c->sees(*a, "lir.groups", "") && c->sees(*b, "lir.groups", ""); }));
  std::this_thread::sleep_for(500ms);
  MqttClient pub(c->port());
  pub.connect("pub");
  constexpr int sent = 10;
  for (int i = 0; i < sent; ++i) pub.publish("jobs/" + std::to_string(i), "job", QoS::at_least_once);
  int at_a = 0;
  int at_b = 0;
  while (wa.next_message()) ++at_a;
  while (wb.next_message()) ++at_b;
  EXPECT_EQ(at_a + at_b, sent);
  EXPECT_GT(at_a, 0);
  EXPECT_GT(at_b, 0);
}

}  // namespace
}  // namespace lir::cluster
