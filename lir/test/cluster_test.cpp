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
#include <future>
#include <memory>
#include <mutex>
#include <set>

#include "dannan/net/fanout.h"
#include "dannan/node.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/client/client.h"
#include "lir/system/ledger.h"
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

// Dannan sends a stream publish only to the nodes whose interest it
// already sees, and a joining node's snapshot is fetched when it is first
// seen serving Lir. So a node that serves Lir must already be subscribed
// to the retained and groups channels in every record that says so, or a
// retained value or a group published in between never reaches it.
TEST_F(ClusterTest, ANodeIsSeenServingLirOnlyWithItsChannelsSubscribed) {
  struct Seen {
    std::mutex mutex;
    size_t serving = 0;
    size_t without_interest = 0;
  };
  // Shared: a change already posted may still run after the unwatch.
  const auto seen = std::make_shared<Seen>();
  const auto watch = a->node->context().nodes().watch(boost::asio::make_strand(io.context), [seen](dannan::RosterChange change) {
    if (!change.node.advert("lir")) return;
    const bool retained = change.node.advert(dannan::net::interest_advert("lir.retained")).has_value();
    const bool groups = change.node.advert(dannan::net::interest_advert("lir.groups")).has_value();
    const std::lock_guard lock(seen->mutex);
    ++seen->serving;
    if (!retained || !groups) ++seen->without_interest;
  });
  auto d = start("d", {{"127.0.0.1", a->node->port()}});
  EXPECT_TRUE(eventually([&] { return a->sees(*d); }));
  a->node->context().nodes().unwatch(watch);
  const std::lock_guard lock(seen->mutex);
  EXPECT_GT(seen->serving, 0u);
  EXPECT_EQ(seen->without_interest, 0u);
}

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

// The maintainer, 2026-11-04: sessions are mirrored to a standby until they live in
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

// Dannan's partition reading reaches the broker: one node gone leaves a
// majority, two leave a minority, which degrades QoS 1 by default and says
// so in $lir/nodes with the ids it cannot reach.
TEST_F(ClusterTest, ANodeCutOffFromMostOfTheClusterReadsMinorityAndSaysSo) {
  using Reading = std::optional<dannan::membership::Partition>;
  const auto side = [](LirNode& node) { return node.on_broker<Reading>([](broker::Broker& broker) { return broker.partition(); }); };
  ASSERT_TRUE(eventually([&] {
    const auto p = side(*a);
    return p && p->majority && p->members == 3 && p->reachable == 3;
  }));
  c.reset();
  ASSERT_TRUE(eventually([&] { return side(*a)->reachable == 2; }, 15s));
  EXPECT_TRUE(side(*a)->majority);
  const auto b_id = dannan::identity::to_string(b->id());
  b.reset();
  ASSERT_TRUE(eventually([&] { return !side(*a)->majority; }, 15s));
  EXPECT_EQ(side(*a)->members, 3u);
  EXPECT_EQ(side(*a)->unreachable.size(), 2u);

  MqttClient pub(a->port());
  pub.connect("pub");
  pub.publish("t", "x", QoS::at_least_once);
  EXPECT_EQ(a->on_broker<uint64_t>([](broker::Broker& broker) { return broker.stats().under_replicated; }), 1u);
  const auto own = "$lir/nodes/" + dannan::identity::to_string(a->id());
  EXPECT_TRUE(eventually([&] {
    return a->on_broker<bool>([&](broker::Broker& broker) {
      for (const auto& m : broker.retained_entries()) {
        const std::string json(m.payload.begin(), m.payload.end());
        if (m.topic == own && json.find(R"("side":"minority")") != std::string::npos && json.find(b_id) != std::string::npos) return true;
      }
      return false;
    });
  }));
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

TEST_F(ClusterTest, ABusyNodeSendsNewClientsToAQuieterOne) {
  // Only b and c say where they serve, so only they can be chosen.
  b->cluster->advertise_endpoints();
  c->cluster->advertise_endpoints();
  const std::set<std::string> quieter{"127.0.0.1:" + std::to_string(b->port()), "127.0.0.1:" + std::to_string(c->port())};
  std::vector<std::unique_ptr<MqttClient>> held;
  std::optional<std::string> reference;
  for (int i = 0; i < 6 && !reference; ++i) {
    auto client = std::make_unique<MqttClient>(a->port());
    client->send(mqtt::Connect{Version::v5, true, 0, "busy-" + std::to_string(i), {}, {}, {}, {}});
    const auto ack = client->receive_a<mqtt::Connack>();
    ASSERT_TRUE(ack);
    if (ack->reason == mqtt::reason::use_another_server) {
      reference = ack->properties.server_reference;
    } else {
      held.push_back(std::move(client));
    }
    // Give b's and c's adverts time to be heard.
    if (i == 0) {
      ASSERT_TRUE(eventually([&] {
        for (const auto& record : a->node->context().nodes().active())
          if (record.id == c->id() && record.advert("lir.endpoints") && record.advert("lir.connections")) return true;
        return false;
      }));
    }
  }
  ASSERT_TRUE(reference);
  EXPECT_TRUE(quieter.contains(*reference)) << *reference;
  // A 3.x client is served however busy the node is.
  MqttClient old(a->port(), Version::v3_1_1);
  old.connect("old");
}

TEST_F(ClusterTest, HeldMessagesAreLeasedInTheNodesLedger) {
  system::LedgerLeases leases(a->node->context(), *a->edge, system::LedgerConfig{50ms, 1u << 20});
  leases.start();
  // The leases follow the bytes the broker holds, a chunk per MiB begun.
  const auto follows = [&] {
    const auto held = a->on_broker<uint64_t>([](broker::Broker& br) { return br.held_bytes(); });
    return leases.chunks() == (held + (1u << 20) - 1) / (1u << 20);
  };
  const auto set = [&](int i, size_t bytes) {
    a->on_broker<bool>([i, bytes](broker::Broker& br) {
      broker::Message m;
      m.topic = "big/" + std::to_string(i);
      m.payload = broker::Bytes(bytes, 'x');
      m.retain = true;
      br.publish(std::move(m));
      return true;
    });
  };
  const size_t before = leases.chunks();
  for (int i = 0; i < 3; ++i) set(i, 1u << 20);
  EXPECT_TRUE(eventually([&] { return follows() && leases.chunks() >= before + 3; }));
  for (int i = 0; i < 3; ++i) set(i, 0);
  EXPECT_TRUE(eventually([&] { return follows() && leases.chunks() <= before + 1; }));
  leases.stop();
  EXPECT_EQ(leases.chunks(), 0u);
}

TEST_F(ClusterTest, ARuleWrittenOnOneNodeAppliesOnEvery) {
  MqttClient admin(a->port());
  admin.connect("admin");
  admin.publish("$lir/rules/quiet", R"({"from": "noise/#", "do": {"drop": true}})", QoS::at_least_once, true);
  ASSERT_TRUE(eventually([&] { return b->on_broker<size_t>([](broker::Broker& broker) { return broker.rules(); }) == 1; }));
  MqttClient sub(b->port());
  sub.connect("sub");
  sub.subscribe("#", QoS::at_most_once);
  MqttClient pub(b->port());
  pub.connect("pub");
  pub.publish("noise/x", "dropped");
  pub.publish("signal/x", "kept");
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "signal/x");
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
  // The publishing node knows both members' nodes before anything is sent.
  ASSERT_TRUE(eventually([&] { return c->cluster->shared_members("workers", "jobs/+") == 2; }));
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

// lir::client finds Lir's nodes in its process's Dannan roster, the one list
// of them (the maintainer, 2026-11-15), and when its node goes it takes the next at once,
// well inside its backoff.
TEST_F(ClusterTest, AClientFindsLirsNodesInDannansRosterAndFailsOver) {
  for (auto* node : {a.get(), b.get(), c.get()}) node->cluster->advertise_endpoints();
  const auto& roster = a->node->context().nodes();
  ASSERT_TRUE(eventually([&] {
    size_t serving = 0;
    for (const auto& record : roster.active()) serving += record.advert("lir.endpoints") ? 1 : 0;
    return serving == 3;
  }));
  struct Seen {
    std::mutex mutex;
    size_t connected = 0;
  };
  auto seen = std::make_shared<Seen>();
  client::ClientConfig config;
  config.nodes = &roster;
  config.client_id = "roaming";
  config.backoff_first = 10s;
  config.connect_timeout = 2s;
  client::Client roaming(io.context.get_executor(), config, dannan::contract::steady_time_source(), a->node->context().logger("client"));
  roaming.on_state([seen](const client::StateChange& change) {
    std::lock_guard lock(seen->mutex);
    if (change.state == client::ConnectionState::connected) ++seen->connected;
  });
  const auto connected = [seen] {
    std::lock_guard lock(seen->mutex);
    return seen->connected;
  };
  const auto holder = [&]() -> LirNode* {
    for (auto* node : {a.get(), b.get(), c.get()}) {
      if (node->on_broker<size_t>([](broker::Broker& broker) { return broker.connections(); }) == 1) return node;
    }
    return nullptr;
  };
  roaming.start();
  ASSERT_TRUE(eventually([&] { return connected() == 1 && holder() != nullptr; }));
  LirNode* first = holder();
  first->edge->stop();
  ASSERT_TRUE(eventually([&] { return connected() == 2; }, 5s));
  ASSERT_TRUE(eventually([&] { return holder() != nullptr && holder() != first; }));
  std::promise<void> stopped;
  roaming.stop([&stopped] { stopped.set_value(); });
  EXPECT_EQ(stopped.get_future().wait_for(5s), std::future_status::ready);
}

}  // namespace
}  // namespace lir::cluster
