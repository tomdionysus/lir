//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/broker/broker.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "lir/auth/scopes.h"

namespace lir::broker {
namespace {

using namespace std::chrono_literals;
using mqtt::PacketType;
using mqtt::QoS;
using mqtt::Version;
namespace reason = mqtt::reason;

class FakeConnection final : public contract::Connection {
 public:
  void send(Bytes bytes) override {
    if (!closed) sent.push_back(std::move(bytes));
  }
  void close() override { closed = true; }
  std::string peer() const override { return "test"; }
  std::vector<Bytes> sent;
  bool closed = false;
};

Message msg(std::string topic, Bytes payload, QoS qos, bool retain = false) {
  Message m;
  m.topic = std::move(topic);
  m.payload = std::move(payload);
  m.qos = qos;
  m.retain = retain;
  return m;
}

struct Client {
  std::shared_ptr<FakeConnection> connection;
  std::shared_ptr<Link> link;
  Version version;

  void send(const mqtt::Packet& packet) { link->receive(mqtt::encode(packet, version)); }
  std::vector<mqtt::Packet> take() {
    std::vector<mqtt::Packet> out;
    for (const auto& bytes : connection->sent) out.push_back(mqtt::decode(bytes, version));
    connection->sent.clear();
    return out;
  }
  template <typename T>
  std::vector<T> take_all() {
    std::vector<T> out;
    for (auto& packet : take()) {
      if (auto* p = std::get_if<T>(&packet)) out.push_back(std::move(*p));
    }
    return out;
  }
  // The first packet of type T, leaving every other packet for the test.
  template <typename T>
  T take_first() {
    for (auto it = connection->sent.begin(); it != connection->sent.end(); ++it) {
      auto packet = mqtt::decode(*it, version);
      if (auto* p = std::get_if<T>(&packet)) {
        connection->sent.erase(it);
        return std::move(*p);
      }
    }
    ADD_FAILURE() << "no packet of the given type";
    return T{};
  }
  template <typename T>
  T take_one() {
    auto packets = take();
    EXPECT_EQ(packets.size(), 1u);
    if (packets.empty() || !std::holds_alternative<T>(packets.front())) {
      ADD_FAILURE() << "expected one packet of the given type";
      return T{};
    }
    return std::get<T>(std::move(packets.front()));
  }
  bool closed() const { return connection->closed; }
};

class BrokerTest : public ::testing::Test {
 protected:
  explicit BrokerTest(Config config = {}) : broker(std::move(config), time, auth, logger()) {}

  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  Client open(Version version = Version::v5) {
    auto connection = std::make_shared<FakeConnection>();
    return Client{connection, broker.accept(connection), version};
  }

  Client connect(const std::string& id, Version version = Version::v5, bool clean = true, mqtt::Properties properties = {}, std::optional<mqtt::Will> will = {},
                 uint16_t keep_alive = 0) {
    auto client = open(version);
    mqtt::Connect c;
    c.version = version;
    c.clean_start = clean;
    c.client_id = id;
    c.keep_alive = keep_alive;
    c.properties = std::move(properties);
    c.will = std::move(will);
    client.send(c);
    last_connack = client.take_first<mqtt::Connack>();
    EXPECT_EQ(last_connack.reason, reason::success);
    return client;
  }

  void subscribe(Client& client, const std::string& filter, QoS qos = QoS::at_least_once, mqtt::Subscription extra = {}) {
    extra.filter = filter;
    extra.qos = qos;
    client.send(mqtt::Subscribe{next_id++, {}, {extra}});
    client.take_first<mqtt::Suback>();
  }

  static mqtt::Publish message(const std::string& topic, const std::string& text, QoS qos = QoS::at_most_once, bool retain = false, uint16_t id = 0) {
    mqtt::Publish p;
    p.topic = topic;
    p.payload = Bytes(text.begin(), text.end());
    p.qos = qos;
    p.retain = retain;
    p.packet_id = qos == QoS::at_most_once ? 0 : (id ? id : 1);
    return p;
  }

  static std::string text(const mqtt::Publish& p) { return {p.payload.begin(), p.payload.end()}; }

  void advance(std::chrono::milliseconds by) {
    time.advance(by);
    broker.poll();
  }

  // Declared before the broker, which is built on them.
  dannan::test::SteppedTime time;
  contract::AllowAll auth;
  Broker broker;
  mqtt::Connack last_connack;
  uint16_t next_id = 100;
};

// ---- connecting

TEST_F(BrokerTest, ConnectsEveryVersion) {
  for (const auto version : {Version::v3_1, Version::v3_1_1, Version::v5}) {
    auto client = connect("c-" + std::to_string(static_cast<int>(version)), version);
    EXPECT_FALSE(last_connack.session_present);
    EXPECT_FALSE(client.closed());
  }
  EXPECT_EQ(broker.connections(), 3u);
}

TEST_F(BrokerTest, FirstPacketMustBeConnect) {
  auto client = open();
  client.send(mqtt::Pingreq{});
  EXPECT_TRUE(client.closed());
  EXPECT_TRUE(client.connection->sent.empty());
  EXPECT_EQ(broker.connections(), 0u);
}

TEST_F(BrokerTest, UnsupportedLevelGetsReturnCodeOne) {
  auto client = open();
  client.link->receive(Bytes{0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x07, 0x02, 0x00, 0x3C, 0x00, 0x01, 'c'});
  ASSERT_EQ(client.connection->sent.size(), 1u);
  EXPECT_EQ(client.connection->sent[0], (Bytes{0x20, 0x02, 0x00, 0x01}));
  EXPECT_TRUE(client.closed());
}

TEST_F(BrokerTest, EmptyClientIdIsAssignedOnV5) {
  connect("", Version::v5);
  ASSERT_TRUE(last_connack.properties.assigned_client_identifier);
  EXPECT_TRUE(last_connack.properties.assigned_client_identifier->starts_with("lir-"));
}

TEST_F(BrokerTest, EmptyClientIdWithoutCleanSessionRefusedOn311) {
  auto client = open(Version::v3_1_1);
  client.send(mqtt::Connect{Version::v3_1_1, false, 0, "", {}, {}, {}, {}});
  EXPECT_EQ(client.take_one<mqtt::Connack>().reason, 0x02);
  EXPECT_TRUE(client.closed());
}

TEST_F(BrokerTest, SecondConnectIsProtocolError) {
  auto client = connect("a");
  client.send(mqtt::Connect{Version::v5, true, 0, "a", {}, {}, {}, {}});
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closed());
}

TEST_F(BrokerTest, PingIsAnswered) {
  auto client = connect("a", Version::v3_1_1);
  client.send(mqtt::Pingreq{});
  client.take_one<mqtt::Pingresp>();
}

TEST_F(BrokerTest, PacketsArriveInAnySplit) {
  auto client = open(Version::v3_1_1);
  const Bytes bytes = mqtt::encode(mqtt::Connect{Version::v3_1_1, true, 0, "split", {}, {}, {}, {}}, Version::v3_1_1);
  for (const uint8_t b : bytes) client.link->receive(std::span<const uint8_t>(&b, 1));
  EXPECT_EQ(client.take_one<mqtt::Connack>().reason, reason::success);
}

TEST_F(BrokerTest, OversizedPacketIsRefused) {
  auto client = connect("a");
  auto big = message("t", std::string(2 << 20, 'x'));
  client.send(big);
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::packet_too_large);
  EXPECT_TRUE(client.closed());
}

// ---- QoS flows

TEST_F(BrokerTest, QoS0Delivered) {
  auto sub = connect("sub");
  auto pub = connect("pub");
  subscribe(sub, "a/b", QoS::at_most_once);
  pub.send(message("a/b", "hello"));
  const auto got = sub.take_one<mqtt::Publish>();
  EXPECT_EQ(got.topic, "a/b");
  EXPECT_EQ(text(got), "hello");
  EXPECT_EQ(got.qos, QoS::at_most_once);
  EXPECT_TRUE(pub.take().empty());
}

TEST_F(BrokerTest, QoS1AckedAndDeliveredAtTheLesserQoS) {
  auto sub1 = connect("sub1");
  auto sub0 = connect("sub0");
  auto pub = connect("pub");
  subscribe(sub1, "t", QoS::exactly_once);
  subscribe(sub0, "t", QoS::at_most_once);
  pub.send(message("t", "x", QoS::at_least_once, false, 7));
  const auto ack = pub.take_one<mqtt::Ack>();
  EXPECT_EQ(ack.type, PacketType::puback);
  EXPECT_EQ(ack.packet_id, 7);
  const auto at1 = sub1.take_one<mqtt::Publish>();
  EXPECT_EQ(at1.qos, QoS::at_least_once);
  EXPECT_NE(at1.packet_id, 0);
  EXPECT_EQ(sub0.take_one<mqtt::Publish>().qos, QoS::at_most_once);
  sub1.send(mqtt::Ack{PacketType::puback, at1.packet_id, reason::success, {}});
  EXPECT_TRUE(sub1.take().empty());
}

TEST_F(BrokerTest, QoS2BothWaysAndNoDuplicateDelivery) {
  auto sub = connect("sub");
  auto pub = connect("pub");
  subscribe(sub, "t", QoS::exactly_once);
  const auto publish = message("t", "once", QoS::exactly_once, false, 9);
  pub.send(publish);
  EXPECT_EQ(pub.take_one<mqtt::Ack>().type, PacketType::pubrec);
  // The publisher resends before PUBREL: answered, not delivered again.
  auto again = publish;
  again.dup = true;
  pub.send(again);
  EXPECT_EQ(pub.take_one<mqtt::Ack>().type, PacketType::pubrec);
  pub.send(mqtt::Ack{PacketType::pubrel, 9, reason::success, {}});
  const auto comp = pub.take_one<mqtt::Ack>();
  EXPECT_EQ(comp.type, PacketType::pubcomp);
  EXPECT_EQ(comp.reason, reason::success);

  const auto delivered = sub.take_all<mqtt::Publish>();
  ASSERT_EQ(delivered.size(), 1u);
  EXPECT_EQ(delivered[0].qos, QoS::exactly_once);
  const uint16_t id = delivered[0].packet_id;
  sub.send(mqtt::Ack{PacketType::pubrec, id, reason::success, {}});
  EXPECT_EQ(sub.take_one<mqtt::Ack>().type, PacketType::pubrel);
  sub.send(mqtt::Ack{PacketType::pubcomp, id, reason::success, {}});
  EXPECT_TRUE(sub.take().empty());
}

TEST_F(BrokerTest, PubrelForUnknownIdIsAnsweredNotFound) {
  auto client = connect("a");
  client.send(mqtt::Ack{PacketType::pubrel, 44, reason::success, {}});
  EXPECT_EQ(client.take_one<mqtt::Ack>().reason, reason::packet_identifier_not_found);
}

TEST_F(BrokerTest, NoMatchingSubscribersIsSaidOnV5) {
  auto pub = connect("pub");
  pub.send(message("nobody", "x", QoS::at_least_once));
  EXPECT_EQ(pub.take_one<mqtt::Ack>().reason, reason::no_matching_subscribers);
}

TEST_F(BrokerTest, ClientReceiveMaximumHoldsTheRest) {
  mqtt::Properties props;
  props.receive_maximum = 1;
  auto sub = connect("sub", Version::v5, true, props);
  auto pub = connect("pub");
  subscribe(sub, "t", QoS::at_least_once);
  pub.send(message("t", "1", QoS::at_least_once, false, 1));
  pub.send(message("t", "2", QoS::at_least_once, false, 2));
  const auto first = sub.take_all<mqtt::Publish>();
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(text(first[0]), "1");
  sub.send(mqtt::Ack{PacketType::puback, first[0].packet_id, reason::success, {}});
  const auto second = sub.take_all<mqtt::Publish>();
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(text(second[0]), "2");
}

TEST_F(BrokerTest, ServerReceiveMaximumIsEnforced) {
  Config config;
  config.receive_maximum = 1;
  Broker small(config, time, auth, logger());
  auto connection = std::make_shared<FakeConnection>();
  Client client{connection, small.accept(connection), Version::v5};
  client.send(mqtt::Connect{Version::v5, true, 0, "a", {}, {}, {}, {}});
  client.take();
  client.send(message("t", "1", QoS::exactly_once, false, 1));
  client.send(message("t", "2", QoS::exactly_once, false, 2));
  const auto packets = client.take();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(std::get<mqtt::Disconnect>(packets[1]).reason, reason::receive_maximum_exceeded);
}

// ---- subscriptions and matching

TEST_F(BrokerTest, WildcardsAndDollarTopics) {
  auto sub = connect("sub");
  subscribe(sub, "#", QoS::at_most_once);
  broker.publish(msg("$SYS/x", {}, QoS::at_most_once));
  EXPECT_TRUE(sub.take().empty());
  broker.publish(msg("a/b/c", {}, QoS::at_most_once));
  EXPECT_EQ(sub.take_one<mqtt::Publish>().topic, "a/b/c");
}

TEST_F(BrokerTest, OverlappingSubscriptionsDeliverOnceAtTheGreatestQoSWithEveryIdentifier) {
  auto sub = connect("sub");
  sub.send(mqtt::Subscribe{1, {}, {{"a/+", QoS::at_most_once}}});
  sub.take();
  mqtt::Subscribe s{2, {}, {{"a/#", QoS::at_least_once}}};
  s.properties.subscription_identifiers = {5};
  sub.send(s);
  sub.take();
  mqtt::Subscribe s2{3, {}, {{"a/b", QoS::at_most_once}}};
  s2.properties.subscription_identifiers = {3};
  sub.send(s2);
  sub.take();
  broker.publish(msg("a/b", {}, QoS::exactly_once));
  const auto got = sub.take_one<mqtt::Publish>();
  EXPECT_EQ(got.qos, QoS::at_least_once);
  EXPECT_EQ(got.properties.subscription_identifiers, (std::vector<uint32_t>{3, 5}));
}

TEST_F(BrokerTest, NoLocal) {
  auto client = connect("self");
  mqtt::Subscription options;
  options.no_local = true;
  subscribe(client, "t", QoS::at_most_once, options);
  client.send(message("t", "mine"));
  EXPECT_TRUE(client.take().empty());
}

TEST_F(BrokerTest, InvalidFilterRefusedPerFilter) {
  auto client = connect("a");
  client.send(mqtt::Subscribe{1, {}, {{"a/#/b", QoS::at_most_once}, {"ok", QoS::exactly_once}}});
  const auto ack = client.take_one<mqtt::Suback>();
  EXPECT_EQ(ack.reasons, (std::vector<uint8_t>{reason::topic_filter_invalid, reason::granted_qos_2}));
  auto old = connect("b", Version::v3_1_1);
  old.send(mqtt::Subscribe{1, {}, {{"a/#/b", QoS::at_most_once}}});
  EXPECT_EQ(old.take_one<mqtt::Suback>().reasons, (std::vector<uint8_t>{0x80}));
}

TEST_F(BrokerTest, UnsubscribeSaysWhetherThereWasOne) {
  auto client = connect("a");
  subscribe(client, "t");
  client.send(mqtt::Unsubscribe{1, {}, {"t", "never"}});
  EXPECT_EQ(client.take_one<mqtt::Unsuback>().reasons, (std::vector<uint8_t>{reason::success, reason::no_subscription_existed}));
  broker.publish(msg("t", {}, QoS::at_most_once));
  EXPECT_TRUE(client.take().empty());
}

TEST_F(BrokerTest, SharedSubscriptionsTakeTurns) {
  auto a = connect("a");
  auto b = connect("b");
  subscribe(a, "$share/g/jobs/+");
  subscribe(b, "$share/g/jobs/+");
  for (int i = 0; i < 4; ++i) broker.publish(msg("jobs/x", {}, QoS::at_most_once));
  EXPECT_EQ(a.take_all<mqtt::Publish>().size(), 2u);
  EXPECT_EQ(b.take_all<mqtt::Publish>().size(), 2u);
}

TEST_F(BrokerTest, SharedSubscriptionPrefersConnectedMembers) {
  auto a = connect("a", Version::v5, false, [] {
    mqtt::Properties p;
    p.session_expiry_interval = 600;
    return p;
  }());
  auto b = connect("b");
  subscribe(a, "$share/g/t");
  subscribe(b, "$share/g/t");
  a.link->closed();
  for (int i = 0; i < 3; ++i) broker.publish(msg("t", {}, QoS::at_least_once));
  EXPECT_EQ(b.take_all<mqtt::Publish>().size(), 3u);
}

TEST_F(BrokerTest, SharedSubscriptionWithNoLocalIsProtocolError) {
  auto client = connect("a");
  mqtt::Subscribe s{1, {}, {{"$share/g/t", QoS::at_most_once, true, false, 0}}};
  client.send(s);
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::protocol_error);
}

// ---- retained messages

TEST_F(BrokerTest, RetainedSentOnSubscribeAndClearedByEmptyPayload) {
  auto pub = connect("pub");
  pub.send(message("state", "on", QoS::at_least_once, true));
  pub.take();
  EXPECT_EQ(broker.retained(), 1u);
  auto sub = connect("sub");
  subscribe(sub, "state", QoS::at_most_once);
  const auto got = sub.take_one<mqtt::Publish>();
  EXPECT_TRUE(got.retain);
  EXPECT_EQ(text(got), "on");
  EXPECT_EQ(got.qos, QoS::at_most_once);
  pub.send(message("state", "", QoS::at_most_once, true));
  EXPECT_EQ(broker.retained(), 0u);
  // The clearing message itself is delivered to current subscribers.
  EXPECT_EQ(sub.take_all<mqtt::Publish>().size(), 1u);
}

TEST_F(BrokerTest, RetainFlagOnLiveDeliveryFollowsRetainAsPublished) {
  auto plain = connect("plain");
  auto rap = connect("rap");
  subscribe(plain, "t", QoS::at_most_once);
  mqtt::Subscription options;
  options.retain_as_published = true;
  subscribe(rap, "t", QoS::at_most_once, options);
  broker.publish(msg("t", Bytes{'x'}, QoS::at_most_once, true));
  EXPECT_FALSE(plain.take_one<mqtt::Publish>().retain);
  EXPECT_TRUE(rap.take_one<mqtt::Publish>().retain);
}

TEST_F(BrokerTest, RetainHandling) {
  broker.publish(msg("r", Bytes{'x'}, QoS::at_most_once, true));
  auto client = connect("a");
  mqtt::Subscription never;
  never.retain_handling = 2;
  subscribe(client, "r", QoS::at_most_once, never);
  EXPECT_TRUE(client.take().empty());
  client.send(mqtt::Subscribe{1, {}, {{"r2/#", QoS::at_most_once, false, false, 1}}});
  client.take();
  broker.publish(msg("r2/a", Bytes{'y'}, QoS::at_most_once, true));
  client.take();
  // Resubscribing to an existing filter with handling 1 sends nothing.
  client.send(mqtt::Subscribe{2, {}, {{"r2/#", QoS::at_most_once, false, false, 1}}});
  EXPECT_EQ(client.take_all<mqtt::Publish>().size(), 0u);
}

TEST_F(BrokerTest, SharedSubscriptionGetsNoRetained) {
  broker.publish(msg("r", Bytes{'x'}, QoS::at_most_once, true));
  auto client = connect("a");
  subscribe(client, "$share/g/r");
  EXPECT_TRUE(client.take().empty());
}

// ---- sessions

TEST_F(BrokerTest, PersistentSessionQueuesQoS1AndDropsQoS0WhileOffline) {
  auto sub = connect("sub", Version::v3_1_1, false);
  subscribe(sub, "t", QoS::at_least_once);
  sub.link->closed();
  broker.publish(msg("t", Bytes{'1'}, QoS::at_least_once));
  broker.publish(msg("t", Bytes{'0'}, QoS::at_most_once));
  EXPECT_EQ(broker.stats().dropped_offline_qos0, 1u);
  auto back = connect("sub", Version::v3_1_1, false);
  EXPECT_TRUE(last_connack.session_present);
  const auto got = back.take_all<mqtt::Publish>();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(text(got[0]), "1");
}

TEST_F(BrokerTest, UnacknowledgedDeliveryIsResentWithDupOnReconnect) {
  auto sub = connect("sub", Version::v3_1_1, false);
  subscribe(sub, "t", QoS::at_least_once);
  broker.publish(msg("t", Bytes{'x'}, QoS::at_least_once));
  const auto first = sub.take_one<mqtt::Publish>();
  EXPECT_FALSE(first.dup);
  sub.link->closed();
  auto back = connect("sub", Version::v3_1_1, false);
  const auto again = back.take_one<mqtt::Publish>();
  EXPECT_TRUE(again.dup);
  EXPECT_EQ(again.packet_id, first.packet_id);
}

TEST_F(BrokerTest, CleanStartDiscardsTheSession) {
  auto sub = connect("sub", Version::v3_1_1, false);
  subscribe(sub, "t");
  sub.link->closed();
  auto back = connect("sub", Version::v3_1_1, true);
  EXPECT_FALSE(last_connack.session_present);
  broker.publish(msg("t", {}, QoS::at_least_once));
  EXPECT_TRUE(back.take().empty());
}

TEST_F(BrokerTest, SessionExpiresAfterItsInterval) {
  mqtt::Properties props;
  props.session_expiry_interval = 10;
  auto client = connect("a", Version::v5, false, props);
  subscribe(client, "t");
  client.link->closed();
  advance(9s);
  EXPECT_EQ(broker.sessions(), 1u);
  advance(2s);
  EXPECT_EQ(broker.sessions(), 0u);
  connect("a", Version::v5, false, props);
  EXPECT_FALSE(last_connack.session_present);
}

TEST_F(BrokerTest, SessionWithExpiryZeroEndsAtDisconnect) {
  auto client = connect("a");
  client.send(mqtt::Disconnect{});
  EXPECT_TRUE(client.closed());
  EXPECT_EQ(broker.sessions(), 0u);
}

TEST_F(BrokerTest, DisconnectMayNotRaiseExpiryFromZero) {
  auto client = connect("a");
  mqtt::Disconnect d;
  d.properties.session_expiry_interval = 60;
  client.send(d);
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::protocol_error);
}

TEST_F(BrokerTest, TakeoverClosesTheOldConnection) {
  auto first = connect("same");
  auto second = connect("same");
  EXPECT_EQ(first.take_one<mqtt::Disconnect>().reason, reason::session_taken_over);
  EXPECT_TRUE(first.closed());
  EXPECT_FALSE(second.closed());
  EXPECT_EQ(broker.connections(), 1u);
}

TEST_F(BrokerTest, KeepAliveTimeoutClosesAfterOneAndAHalfPeriods) {
  auto client = connect("a", Version::v5, true, {}, std::nullopt, 10);
  advance(14s);
  EXPECT_FALSE(client.closed());
  advance(2s);
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::keep_alive_timeout);
  EXPECT_TRUE(client.closed());
}

TEST_F(BrokerTest, ConnectTimeout) {
  auto client = open();
  advance(11s);
  EXPECT_TRUE(client.closed());
}

// ---- wills

mqtt::Will will_of(const std::string& topic, uint32_t delay = 0) {
  mqtt::Will w;
  w.topic = topic;
  w.payload = Bytes{'g', 'o', 'n', 'e'};
  if (delay) w.properties.will_delay_interval = delay;
  return w;
}

TEST_F(BrokerTest, WillPublishedOnAbnormalClose) {
  auto watcher = connect("watcher");
  subscribe(watcher, "wills/#", QoS::at_most_once);
  auto client = connect("a", Version::v5, true, {}, will_of("wills/a"));
  client.link->closed();
  EXPECT_EQ(watcher.take_one<mqtt::Publish>().topic, "wills/a");
}

TEST_F(BrokerTest, NormalDisconnectDiscardsTheWillAnd0x04SendsIt) {
  auto watcher = connect("watcher");
  subscribe(watcher, "wills/#", QoS::at_most_once);
  auto quiet = connect("a", Version::v5, true, {}, will_of("wills/a"));
  quiet.send(mqtt::Disconnect{});
  EXPECT_TRUE(watcher.take().empty());
  auto loud = connect("b", Version::v5, true, {}, will_of("wills/b"));
  loud.send(mqtt::Disconnect{reason::disconnect_with_will, {}});
  EXPECT_EQ(watcher.take_one<mqtt::Publish>().topic, "wills/b");
}

TEST_F(BrokerTest, DelayedWillIsCancelledByReconnecting) {
  auto watcher = connect("watcher");
  subscribe(watcher, "wills/#", QoS::at_most_once);
  mqtt::Properties props;
  props.session_expiry_interval = 600;
  auto client = connect("a", Version::v5, false, props, will_of("wills/a", 30));
  client.link->closed();
  advance(10s);
  connect("a", Version::v5, false, props);
  advance(60s);
  EXPECT_TRUE(watcher.take().empty());
}

TEST_F(BrokerTest, DelayedWillIsSentWhenTheDelayPasses) {
  auto watcher = connect("watcher");
  subscribe(watcher, "wills/#", QoS::at_most_once);
  mqtt::Properties props;
  props.session_expiry_interval = 600;
  auto client = connect("a", Version::v5, false, props, will_of("wills/a", 30));
  client.link->closed();
  advance(29s);
  EXPECT_TRUE(watcher.take().empty());
  advance(2s);
  EXPECT_EQ(watcher.take_one<mqtt::Publish>().topic, "wills/a");
}

TEST_F(BrokerTest, WillDelayIsCutShortBySessionEnd) {
  auto watcher = connect("watcher");
  subscribe(watcher, "wills/#", QoS::at_most_once);
  mqtt::Properties props;
  props.session_expiry_interval = 5;
  auto client = connect("a", Version::v5, false, props, will_of("wills/a", 30));
  client.link->closed();
  advance(6s);
  EXPECT_EQ(watcher.take_all<mqtt::Publish>().size(), 1u);
}

// ---- 5.0 features

TEST_F(BrokerTest, InboundTopicAliases) {
  auto sub = connect("sub");
  auto pub = connect("pub");
  subscribe(sub, "long/topic/name", QoS::at_most_once);
  auto first = message("long/topic/name", "1");
  first.properties.topic_alias = 1;
  pub.send(first);
  mqtt::Publish by_alias = message("", "2");
  by_alias.topic.clear();
  by_alias.properties.topic_alias = 1;
  pub.send(by_alias);
  const auto got = sub.take_all<mqtt::Publish>();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1].topic, "long/topic/name");
  EXPECT_FALSE(got[1].properties.topic_alias);
}

TEST_F(BrokerTest, UnknownOrOutOfRangeAliasDisconnects) {
  auto a = connect("a");
  mqtt::Publish unknown = message("x", "1");
  unknown.topic.clear();
  unknown.properties.topic_alias = 2;
  a.send(unknown);
  EXPECT_EQ(a.take_one<mqtt::Disconnect>().reason, reason::protocol_error);
  auto b = connect("b");
  auto big = message("x", "1");
  big.properties.topic_alias = 1000;
  b.send(big);
  EXPECT_EQ(b.take_one<mqtt::Disconnect>().reason, reason::topic_alias_invalid);
}

TEST_F(BrokerTest, MessageExpiryCountsDownAndDropsQueued) {
  mqtt::Properties props;
  props.session_expiry_interval = 600;
  auto sub = connect("sub", Version::v5, false, props);
  subscribe(sub, "t");
  auto pub = connect("pub");
  auto timed = message("t", "x", QoS::at_least_once);
  timed.properties.message_expiry_interval = 10;
  pub.send(timed);
  auto live = sub.take_all<mqtt::Publish>();
  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(live[0].properties.message_expiry_interval, 10u);
  sub.send(mqtt::Ack{PacketType::puback, live[0].packet_id, reason::success, {}});
  sub.link->closed();
  pub.send(timed);
  advance(11s);
  connect("sub", Version::v5, false, props);
  EXPECT_EQ(broker.stats().dropped_expired, 1u);
}

TEST_F(BrokerTest, PropertiesAreForwarded) {
  auto sub = connect("sub");
  auto pub = connect("pub");
  subscribe(sub, "req", QoS::at_most_once);
  auto request = message("req", "{}");
  request.properties.content_type = "application/json";
  request.properties.response_topic = "reply/1";
  request.properties.correlation_data = Bytes{1, 2};
  request.properties.user_properties = {{"k", "v"}};
  request.properties.payload_format_indicator = 1;
  pub.send(request);
  const auto got = sub.take_one<mqtt::Publish>();
  EXPECT_EQ(got.properties.content_type, "application/json");
  EXPECT_EQ(got.properties.response_topic, "reply/1");
  EXPECT_EQ(got.properties.correlation_data, (Bytes{1, 2}));
  EXPECT_EQ(got.properties.user_properties, request.properties.user_properties);
  EXPECT_EQ(got.properties.payload_format_indicator, 1);
}

TEST_F(BrokerTest, MessageOverTheClientsMaximumIsDropped) {
  mqtt::Properties props;
  props.maximum_packet_size = 64;
  auto sub = connect("sub", Version::v5, true, props);
  subscribe(sub, "t", QoS::at_most_once);
  broker.publish(msg("t", Bytes(200, 'x'), QoS::at_most_once));
  EXPECT_TRUE(sub.take().empty());
  EXPECT_EQ(broker.stats().dropped_too_large, 1u);
}

TEST_F(BrokerTest, ReservedTopicsAreSubscribeOnly) {
  auto client = connect("a");
  client.send(message("$lir/nodes", "x", QoS::at_least_once));
  EXPECT_EQ(client.take_one<mqtt::Ack>().reason, reason::not_authorized);
}

TEST_F(BrokerTest, InvalidUtf8PayloadWithFormatIndicatorIsRefused) {
  auto client = connect("a");
  auto p = message("t", "\xFF", QoS::at_least_once);
  p.properties.payload_format_indicator = 1;
  client.send(p);
  EXPECT_EQ(client.take_one<mqtt::Ack>().reason, reason::payload_format_invalid);
}

TEST_F(BrokerTest, WildcardInPublishTopicDisconnects) {
  auto client = connect("a");
  client.send(message("a/+", "x"));
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::topic_name_invalid);
}

// ---- what the specification leaves to the server (MAY)

TEST_F(BrokerTest, OutboundTopicAliasesWithinTheClientsMaximum) {
  mqtt::Properties props;
  props.topic_alias_maximum = 1;
  auto sub = connect("sub", Version::v5, true, props);
  subscribe(sub, "#", QoS::at_most_once);
  for (const auto* topic : {"long/topic", "long/topic", "other"}) broker.publish(msg(topic, {}, QoS::at_most_once));
  const auto got = sub.take_all<mqtt::Publish>();
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].topic, "long/topic");
  EXPECT_EQ(got[0].properties.topic_alias, 1);
  EXPECT_EQ(got[1].topic, "");
  EXPECT_EQ(got[1].properties.topic_alias, 1);
  // The maximum is one: a new topic goes in full, with no alias.
  EXPECT_EQ(got[2].topic, "other");
  EXPECT_FALSE(got[2].properties.topic_alias);
}

TEST_F(BrokerTest, NoAliasesForAClientThatAllowsNone) {
  auto sub = connect("sub");
  subscribe(sub, "t", QoS::at_most_once);
  broker.publish(msg("t", {}, QoS::at_most_once));
  broker.publish(msg("t", {}, QoS::at_most_once));
  for (const auto& p : sub.take_all<mqtt::Publish>()) {
    EXPECT_EQ(p.topic, "t");
    EXPECT_FALSE(p.properties.topic_alias);
  }
}

class BrokerOptionsTest : public BrokerTest {
 protected:
  BrokerOptionsTest() : BrokerTest(options()) {}
  static Config options() {
    Config c;
    c.response_information = "reply/{client}/";
    c.queue_qos0_offline = true;
    return c;
  }
};

TEST_F(BrokerOptionsTest, ResponseInformationOnlyWhenAskedFor) {
  mqtt::Properties ask;
  ask.request_response_information = 1;
  connect("asker", Version::v5, true, ask);
  EXPECT_EQ(last_connack.properties.response_information, "reply/asker/");
  connect("quiet");
  EXPECT_FALSE(last_connack.properties.response_information);
}

TEST_F(BrokerOptionsTest, QoS0CanBeQueuedForAnOfflineSession) {
  auto sub = connect("sub", Version::v3_1_1, false);
  subscribe(sub, "t", QoS::at_most_once);
  sub.link->closed();
  broker.publish(msg("t", Bytes{'0'}, QoS::at_most_once));
  auto back = connect("sub", Version::v3_1_1, false);
  EXPECT_EQ(back.take_all<mqtt::Publish>().size(), 1u);
}

// ---- authentication

class ChallengeOnce final : public contract::Authenticator {
 public:
  contract::AuthDecision connect(const mqtt::Connect&) override { return contract::AuthDecision::challenge(Bytes{'?'}); }
  contract::AuthDecision auth(const std::string&, const std::string&, const Bytes& data) override {
    return data == Bytes{'!'} ? contract::AuthDecision::accept(std::make_shared<NotSecret>()) : contract::AuthDecision::refuse(reason::not_authorized);
  }

 private:
  class NotSecret final : public contract::Grant {
   public:
    bool may_publish(std::string_view, bool) const override { return true; }
    bool may_subscribe(std::string_view filter) const override { return filter != "secret"; }
  };
};

TEST(BrokerAuth, EnhancedAuthenticationExchange) {
  dannan::test::SteppedTime time;
  ChallengeOnce challenger;
  Broker broker({}, time, challenger, {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
  auto connection = std::make_shared<FakeConnection>();
  Client client{connection, broker.accept(connection), Version::v5};
  mqtt::Connect c{Version::v5, true, 0, "a", {}, {}, {}, {}};
  c.properties.authentication_method = "TEST";
  client.send(c);
  const auto challenge = client.take_one<mqtt::Auth>();
  EXPECT_EQ(challenge.reason, reason::continue_authentication);
  EXPECT_EQ(challenge.properties.authentication_data, (Bytes{'?'}));
  mqtt::Auth answer{reason::continue_authentication, {}};
  answer.properties.authentication_method = "TEST";
  answer.properties.authentication_data = Bytes{'!'};
  client.send(answer);
  const auto ack = client.take_one<mqtt::Connack>();
  EXPECT_EQ(ack.reason, reason::success);
  EXPECT_EQ(ack.properties.authentication_method, "TEST");
  client.send(mqtt::Subscribe{1, {}, {{"secret", QoS::at_most_once}}});
  EXPECT_EQ(client.take_one<mqtt::Suback>().reasons, (std::vector<uint8_t>{reason::not_authorized}));
}

TEST(BrokerAuth, AllowAllRefusesAnUnknownMethod) {
  dannan::test::SteppedTime time;
  contract::AllowAll allow;
  Broker broker({}, time, allow, {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
  auto connection = std::make_shared<FakeConnection>();
  Client client{connection, broker.accept(connection), Version::v5};
  mqtt::Connect c{Version::v5, true, 0, "a", {}, {}, {}, {}};
  c.properties.authentication_method = "SCRAM";
  client.send(c);
  EXPECT_EQ(client.take_one<mqtt::Connack>().reason, reason::bad_authentication_method);
  EXPECT_TRUE(client.closed());
}

// ---- topic scopes (design, section 1)

// Grants scopes good for `lifetime` from each CONNECT or AUTH; a 5.0
// client naming the method "TEST" may re-authenticate with data "more" to
// get `renewed` instead.
class Scoped final : public contract::Authenticator {
 public:
  Scoped(const dannan::test::SteppedTime& time, auth::TopicScopes scopes, std::chrono::seconds lifetime)
      : _time(time), _scopes(std::move(scopes)), _lifetime(lifetime) {}
  contract::AuthDecision connect(const mqtt::Connect&) override { return grant(_scopes); }
  contract::AuthDecision auth(const std::string&, const std::string&, const Bytes& data) override {
    return data == Bytes{'m', 'o', 'r', 'e'} ? grant(renewed) : contract::AuthDecision::refuse(reason::not_authorized);
  }
  auth::TopicScopes renewed;

 private:
  contract::AuthDecision grant(const auth::TopicScopes& scopes) {
    return contract::AuthDecision::accept(std::make_shared<auth::ScopeGrant>(scopes, _time.now() + _lifetime));
  }
  const dannan::test::SteppedTime& _time;
  auth::TopicScopes _scopes;
  std::chrono::seconds _lifetime;
};

class BrokerScopes : public ::testing::Test {
 protected:
  BrokerScopes() : scoped(time, {{"open/#"}, {"open/#", "$lir/#"}, {"status/+"}}, 60s), broker({}, time, scoped, logger()) {}

  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  Client open(Version version = Version::v5) {
    auto connection = std::make_shared<FakeConnection>();
    return Client{connection, broker.accept(connection), version};
  }
  mqtt::Connack connect(Client& client, const std::string& id, std::optional<mqtt::Will> will = {}, bool clean = true, bool method = false) {
    mqtt::Connect c{client.version, clean, 0, id, std::move(will), {}, {}, {}};
    if (client.version == Version::v5 && !clean) c.properties.session_expiry_interval = 3600;
    if (method) c.properties.authentication_method = "TEST";
    client.send(c);
    return client.take_first<mqtt::Connack>();
  }
  static mqtt::Publish publish(const std::string& topic, bool retain = false, QoS qos = QoS::at_least_once) {
    mqtt::Publish p;
    p.topic = topic;
    p.payload = Bytes{'x'};
    p.qos = qos;
    p.retain = retain;
    p.packet_id = qos == QoS::at_most_once ? 0 : 1;
    return p;
  }
  void advance(std::chrono::milliseconds by) {
    time.advance(by);
    broker.poll();
  }

  dannan::test::SteppedTime time;
  Scoped scoped;
  Broker broker;
};

TEST_F(BrokerScopes, PublishOutsideTheScopesIsRefusedAndNotDelivered) {
  auto sub = open();
  connect(sub, "sub");
  sub.send(mqtt::Subscribe{1, {}, {{"open/#", QoS::at_least_once}}});
  sub.take_first<mqtt::Suback>();
  auto pub = open();
  connect(pub, "pub");
  pub.send(publish("closed/a"));
  EXPECT_EQ(pub.take_one<mqtt::Ack>().reason, reason::not_authorized);
  // A refused publish reaches no one, even a subscriber it would match.
  pub.send(publish("open/a", true));
  EXPECT_EQ(pub.take_one<mqtt::Ack>().reason, reason::not_authorized);
  EXPECT_TRUE(sub.take_all<mqtt::Publish>().empty());
  pub.send(publish("open/a"));
  pub.take_one<mqtt::Ack>();
  EXPECT_EQ(sub.take_all<mqtt::Publish>().size(), 1u);
}

TEST_F(BrokerScopes, ThreeOneOneIsToldSuccessButNothingIsDelivered) {
  auto sub = open();
  connect(sub, "sub");
  sub.send(mqtt::Subscribe{1, {}, {{"open/#", QoS::at_least_once}}});
  sub.take_first<mqtt::Suback>();
  auto pub = open(Version::v3_1_1);
  connect(pub, "pub");
  pub.send(publish("open/a", true));
  EXPECT_EQ(pub.take_one<mqtt::Ack>().reason, reason::success);
  EXPECT_TRUE(sub.take_all<mqtt::Publish>().empty());
}

TEST_F(BrokerScopes, RetainNeedsTheRetainAction) {
  auto pub = open();
  connect(pub, "pub");
  pub.send(publish("open/a", true));
  EXPECT_EQ(pub.take_one<mqtt::Ack>().reason, reason::not_authorized);
  pub.send(publish("status/a", true));
  pub.take_one<mqtt::Ack>();
  EXPECT_EQ(broker.retained(), 1u);
  pub.send(publish("status/a", false));
  EXPECT_EQ(pub.take_one<mqtt::Ack>().reason, reason::not_authorized);
}

TEST_F(BrokerScopes, SubscribeNeedsCoverage) {
  auto sub = open();
  connect(sub, "sub");
  sub.send(mqtt::Subscribe{
      1, {}, {{"open/+", QoS::at_most_once}, {"#", QoS::at_most_once}, {"$share/g/open/x", QoS::at_most_once}, {"$lir/nodes/+", QoS::at_most_once}}});
  EXPECT_EQ(sub.take_one<mqtt::Suback>().reasons, (std::vector<uint8_t>{0, reason::not_authorized, 0, 0}));
}

TEST_F(BrokerScopes, AWillOutsideTheScopesRefusesTheConnection) {
  for (const bool retain : {false, true}) {
    auto client = open();
    mqtt::Will will;
    will.topic = retain ? "open/gone" : "closed/gone";
    will.retain = retain;
    EXPECT_EQ(connect(client, "w", will).reason, reason::not_authorized) << retain;
    EXPECT_TRUE(client.closed());
  }
  auto client = open();
  mqtt::Will will;
  will.topic = "open/gone";
  EXPECT_EQ(connect(client, "w", will).reason, reason::success);
}

TEST_F(BrokerScopes, TheConnectionClosesWhenTheGrantEndsAndTheSessionStays) {
  auto client = open();
  connect(client, "c", {}, false);
  advance(59s);
  EXPECT_FALSE(client.closed());
  advance(2s);
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::not_authorized);
  EXPECT_TRUE(client.closed());
  auto again = open();
  EXPECT_TRUE(connect(again, "c", {}, false).session_present);
}

TEST_F(BrokerScopes, ReauthenticationReplacesTheGrant) {
  scoped.renewed = {{"other/#"}, {}, {}};
  auto client = open();
  connect(client, "c", {}, true, true);
  advance(50s);
  mqtt::Auth again{reason::reauthenticate, {}};
  again.properties.authentication_method = "TEST";
  again.properties.authentication_data = Bytes{'m', 'o', 'r', 'e'};
  client.send(again);
  EXPECT_EQ(client.take_one<mqtt::Auth>().reason, reason::success);
  // The old grant would have ended at 60 s; the new one runs to 110 s.
  advance(20s);
  EXPECT_FALSE(client.closed());
  client.send(publish("open/a"));
  EXPECT_EQ(client.take_one<mqtt::Ack>().reason, reason::not_authorized);
  client.send(publish("other/a"));
  EXPECT_EQ(client.take_one<mqtt::Ack>().reason, reason::no_matching_subscribers);
  advance(41s);
  EXPECT_EQ(client.take_one<mqtt::Disconnect>().reason, reason::not_authorized);
}

TEST_F(BrokerScopes, NarrowerReauthenticationDropsSubscriptionsAndDisconnects) {
  scoped.renewed = {{}, {"open/a/#"}, {}};
  auto client = open();
  connect(client, "c", {}, false, true);
  client.send(mqtt::Subscribe{1, {}, {{"open/#", QoS::at_most_once}, {"open/a/b", QoS::at_most_once}}});
  client.take_first<mqtt::Suback>();
  mqtt::Auth again{reason::reauthenticate, {}};
  again.properties.authentication_method = "TEST";
  again.properties.authentication_data = Bytes{'m', 'o', 'r', 'e'};
  client.send(again);
  const auto packets = client.take();
  ASSERT_FALSE(packets.empty());
  ASSERT_TRUE(std::holds_alternative<mqtt::Disconnect>(packets.back()));
  EXPECT_EQ(std::get<mqtt::Disconnect>(packets.back()).reason, reason::not_authorized);
  EXPECT_TRUE(client.closed());

  // The covered subscription stayed: a publish reaches the resumed session.
  auto back = open();
  EXPECT_TRUE(connect(back, "c", {}, false).session_present);
  auto pub = open();
  connect(pub, "pub");
  pub.send(publish("open/a/b", false, QoS::at_most_once));
  pub.send(publish("open/x", false, QoS::at_most_once));
  const auto got = back.take_all<mqtt::Publish>();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].topic, "open/a/b");
}

TEST_F(BrokerScopes, AnEqualOrWiderReauthenticationKeepsTheConnection) {
  scoped.renewed = {{}, {"#"}, {}};
  auto client = open();
  connect(client, "c", {}, true, true);
  client.send(mqtt::Subscribe{1, {}, {{"open/#", QoS::at_most_once}}});
  client.take_first<mqtt::Suback>();
  mqtt::Auth again{reason::reauthenticate, {}};
  again.properties.authentication_method = "TEST";
  again.properties.authentication_data = Bytes{'m', 'o', 'r', 'e'};
  client.send(again);
  EXPECT_EQ(client.take_one<mqtt::Auth>().reason, reason::success);
  EXPECT_FALSE(client.closed());
}

class BrokerScopesAtReconnect : public ::testing::Test {
 protected:
  // The node's grant for each new connection is set by the test.
  class Switchable final : public contract::Authenticator {
   public:
    contract::AuthDecision connect(const mqtt::Connect&) override { return contract::AuthDecision::accept(std::make_shared<auth::ScopeGrant>(scopes)); }
    contract::AuthDecision auth(const std::string&, const std::string&, const Bytes&) override {
      return contract::AuthDecision::refuse(reason::bad_authentication_method);
    }
    auth::TopicScopes scopes;
  };
  BrokerScopesAtReconnect() : broker({}, time, switchable, {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}) {}
  Client open(Version version) {
    auto connection = std::make_shared<FakeConnection>();
    return Client{connection, broker.accept(connection), version};
  }
  dannan::test::SteppedTime time;
  Switchable switchable;
  Broker broker;
};

TEST_F(BrokerScopesAtReconnect, AResumedThreeOneOneSessionKeepsOnlyWhatTheNewGrantAllows) {
  switchable.scopes = {{"#"}, {"#"}, {}};
  auto client = open(Version::v3_1_1);
  client.send(mqtt::Connect{Version::v3_1_1, false, 0, "c", {}, {}, {}, {}});
  client.take_first<mqtt::Connack>();
  client.send(mqtt::Subscribe{1, {}, {{"a/#", QoS::at_most_once}, {"b/#", QoS::at_most_once}}});
  client.take_first<mqtt::Suback>();
  client.link->closed();

  switchable.scopes = {{"#"}, {"a/#"}, {}};
  auto back = open(Version::v3_1_1);
  back.send(mqtt::Connect{Version::v3_1_1, false, 0, "c", {}, {}, {}, {}});
  EXPECT_TRUE(back.take_first<mqtt::Connack>().session_present);
  broker.publish(msg("a/1", Bytes{'1'}, QoS::at_most_once));
  broker.publish(msg("b/1", Bytes{'2'}, QoS::at_most_once));
  const auto got = back.take_all<mqtt::Publish>();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].topic, "a/1");
}

}  // namespace
}  // namespace lir::broker
