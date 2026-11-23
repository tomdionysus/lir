//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "lir/broker/link.h"
#include "lir/test/fixtures/cluster.h"

// One client's protocol (the rewrite's step 2), each test named by the
// statement it proves: clients on Dannan's simulated nodes, connected through
// a fake transport, with publishes crossing the cluster on Delivery. One node
// is a cluster of one, never a mode.
namespace lir::broker {
namespace {

using namespace std::chrono_literals;
using mqtt::Version;
using test::max_steps;
using test::publish_of;
namespace reason = mqtt::reason;

class FakeConnection final : public contract::Connection {
 public:
  void send(mqtt::Bytes bytes) override {
    if (!closed) sent.push_back(std::move(bytes));
  }
  void close() override { closed = true; }
  std::string peer() const override { return "198.51.100.7:50000"; }
  std::optional<contract::ClientCertificate> certificate() const override { return presented; }

  std::vector<mqtt::Bytes> sent;
  bool closed = false;
  std::optional<contract::ClientCertificate> presented;
};

// Everything, except what is under "secret/"; it ends `lifetime` after it
// was given, when that is set.
class Limited final : public contract::Grant {
 public:
  explicit Limited(std::optional<dannan::contract::TimePoint> ends = {}) : _ends(ends) {}
  bool may_publish(std::string_view topic, bool) const override { return !topic.starts_with("secret/"); }
  bool may_subscribe(std::string_view filter) const override { return !filter.starts_with("secret/"); }
  std::optional<dannan::contract::TimePoint> expires() const override { return _ends; }

 private:
  std::optional<dannan::contract::TimePoint> _ends;
};

// A user name "bad" is refused; "limited" gets Limited; method "scram" is a
// two-step exchange: challenge {1}, answer {2}, accepted with {3}.
class TestAuthenticator final : public contract::Authenticator {
 public:
  contract::AuthDecision connect(const mqtt::Connect& connect, const contract::Peer& peer) override {
    seen_peer = peer.address;
    seen_subject = peer.certificate_subject;
    if (connect.properties.authentication_method == "scram") return contract::AuthDecision::challenge({1});
    // A challenge with no method named: the authenticator's mistake.
    if (connect.username == "challenge") return contract::AuthDecision::challenge({1});
    if (connect.username == "bad") return contract::AuthDecision::refuse(reason::bad_user_name_or_password);
    if (connect.username == "limited") return contract::AuthDecision::accept(std::make_shared<Limited>(ends));
    return contract::AuthDecision::accept(std::make_shared<contract::Unrestricted>());
  }
  contract::AuthDecision auth(const std::string&, const std::string& method, const mqtt::Bytes& data, const contract::Peer&) override {
    if (method != "scram" || data != mqtt::Bytes{2}) return contract::AuthDecision::refuse(reason::not_authorized);
    return contract::AuthDecision::accept(std::make_shared<Limited>(ends), mqtt::Bytes{3});
  }

  std::string seen_peer;
  std::optional<std::string> seen_subject;
  std::optional<dannan::contract::TimePoint> ends;
};

struct Cluster : test::Cluster {
  using test::Cluster::Cluster;
  TestAuthenticator authenticator;
};

// One client on node `node`: what it sends goes to its link on the link's
// strand; what the link sent back is read in order.
class Client {
 public:
  Client(Cluster& cluster, size_t node, Version version = Version::v5, Link::Limits limits = {},
         std::optional<contract::ClientCertificate> certificate = std::nullopt)
      : _cluster(cluster), _version(version), _strand(boost::asio::make_strand(cluster.world.io)), connection(std::make_shared<FakeConnection>()) {
    connection->presented = std::move(certificate);
    auto& context = cluster.nodes.node(node).context();
    link = Link::create(_strand, *cluster.deliveries.at(node), cluster.authenticator, context.time(), connection, context.logger("lir.link"), limits);
  }

  void raw(mqtt::Bytes bytes) {
    boost::asio::post(_strand, [link = link, bytes = std::move(bytes)] { link->receive(bytes); });
  }
  void send(const mqtt::Packet& packet) { raw(mqtt::encode(packet, _version)); }

  // The next packet the link sent, running the world until there is one.
  std::optional<mqtt::Packet> next() {
    if (!_cluster.world.run_until([&] { return connection->sent.size() > _read; }, max_steps)) return std::nullopt;
    return mqtt::decode(connection->sent[_read++], _version);
  }
  template <class T>
  T expect() {
    auto packet = next();
    EXPECT_TRUE(packet.has_value());
    if (!packet) return T{};
    const auto* out = std::get_if<T>(&*packet);
    EXPECT_NE(out, nullptr) << "packet type " << static_cast<int>(mqtt::type_of(*packet));
    return out ? *out : T{};
  }
  bool closes() {
    return _cluster.world.run_until([&] { return connection->closed; }, max_steps);
  }
  // Nothing more was sent once the world settles.
  bool quiet() {
    _cluster.settle();
    return connection->sent.size() == _read;
  }

  mqtt::Connack connect(mqtt::Connect connect = {}) {
    connect.version = _version;
    if (connect.client_id.empty() && _version == Version::v3_1) connect.client_id = "c";
    send(connect);
    return expect<mqtt::Connack>();
  }
  mqtt::Suback subscribe(std::vector<std::string> filters, uint16_t packet_id = 1) {
    mqtt::Subscribe subscribe;
    subscribe.packet_id = packet_id;
    for (auto& filter : filters) subscribe.subscriptions.push_back({std::move(filter)});
    send(subscribe);
    return expect<mqtt::Suback>();
  }
  // Each PUBLISH that arrives while the world settles.
  std::vector<mqtt::Publish> publishes() {
    _cluster.settle();
    std::vector<mqtt::Publish> out;
    while (connection->sent.size() > _read) {
      auto packet = mqtt::decode(connection->sent[_read++], _version);
      if (auto* publish = std::get_if<mqtt::Publish>(&packet)) out.push_back(*publish);
    }
    return out;
  }

 private:
  Cluster& _cluster;
  Version _version;
  boost::asio::strand<boost::asio::io_context::executor_type> _strand;
  size_t _read = 0;

 public:
  std::shared_ptr<FakeConnection> connection;
  std::shared_ptr<Link> link;
};

mqtt::Connect with_will(std::string topic, mqtt::QoS qos = mqtt::QoS::at_most_once, bool retain = false) {
  mqtt::Connect connect;
  connect.client_id = "willing";
  connect.will = mqtt::Will{std::move(topic), mqtt::Bytes{'g', 'o', 'n', 'e'}, qos, retain, {}};
  return connect;
}

// CONNECT

TEST(Link, MQTT_3_1_0_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.send(mqtt::Pingreq{});
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.connection->sent.empty());
}

TEST(Link, MQTT311_3_1_0_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  client.send(mqtt::Subscribe{1, {}, {{"a"}}});
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.connection->sent.empty());
}

TEST(Link, MQTT_3_1_0_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  EXPECT_EQ(client.connect().reason, reason::success);
  client.send(mqtt::Connect{});
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT311_3_1_0_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  mqtt::Connect connect;
  connect.client_id = "c";
  EXPECT_EQ(client.connect(connect).reason, 0);
  connect.version = Version::v3_1_1;
  client.send(connect);
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.quiet());
}

TEST(Link, MQTT311_3_1_2_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  // "MQTT" level 2, below 3.1.1's: return code 1, then close.
  client.raw({0x10, 0x0d, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x02, 0x02, 0x00, 0x3c, 0x00, 0x01, 'c'});
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, 1);
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT_3_1_2_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.raw({0x10, 0x0e, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x06, 0x02, 0x00, 0x3c, 0x00, 0x00, 0x01, 'c'});
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, reason::unsupported_protocol_version);
  EXPECT_TRUE(client.closes());
}

// What Lir offers until Dagda's nouns ship, stated in the CONNACK (the
// design, "What may be acknowledged").
TEST(Link, MQTT_3_2_2_9) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.clean_start = false;
  connect.properties.session_expiry_interval = 3600;
  const auto connack = client.connect(connect);
  EXPECT_EQ(connack.reason, reason::success);
  EXPECT_FALSE(connack.session_present);
  EXPECT_EQ(connack.properties.maximum_qos, 0);
  EXPECT_EQ(connack.properties.retain_available, 0);
  EXPECT_EQ(connack.properties.shared_subscription_available, 0);
  EXPECT_EQ(connack.properties.subscription_identifier_available, 0);
  EXPECT_EQ(connack.properties.session_expiry_interval, 0u);
  EXPECT_EQ(connack.properties.maximum_packet_size, Link::Limits{}.maximum_packet_size);
  EXPECT_EQ(connack.properties.topic_alias_maximum, Link::Limits{}.topic_alias_maximum);  // [MQTT-3.2.2-17]
  EXPECT_EQ(cluster.authenticator.seen_peer, "198.51.100.7:50000");
}

TEST(Link, MQTT_3_2_2_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  EXPECT_FALSE(client.connect(connect).session_present);
}

TEST(Link, MQTT_3_2_2_16) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  const auto connack = client.connect();
  ASSERT_TRUE(connack.properties.assigned_client_identifier.has_value());
  EXPECT_FALSE(connack.properties.assigned_client_identifier->empty());
  EXPECT_EQ(*connack.properties.assigned_client_identifier, client.link->client_id());
  Client other(cluster, 0);
  EXPECT_NE(other.connect().properties.assigned_client_identifier, connack.properties.assigned_client_identifier);
}

TEST(Link, MQTT311_3_1_3_8) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  mqtt::Connect connect;
  connect.clean_start = false;
  EXPECT_EQ(client.connect(connect).reason, 2);
  EXPECT_TRUE(client.closes());
  // With a clean session an empty identifier is assigned one.
  Client clean(cluster, 0, Version::v3_1_1);
  EXPECT_EQ(clean.connect().reason, 0);
  EXPECT_FALSE(clean.link->client_id().empty());
}

// MQTT 3.1: a client identifier of 1 to 23 characters; outside that, CONNACK
// return code 2.
TEST(Link, MQTT31_AClientIdentifierOutside1To23CharactersIsRefused) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  mqtt::Connect connect;
  connect.client_id = std::string(24, 'c');
  Client long_id(cluster, 0, Version::v3_1);
  EXPECT_EQ(long_id.connect(connect).reason, 2);
  EXPECT_TRUE(long_id.closes());
  connect.client_id = std::string(23, 'c');
  Client longest(cluster, 0, Version::v3_1);
  EXPECT_EQ(longest.connect(connect).reason, 0);
  Client empty(cluster, 0, Version::v3_1);
  connect.client_id.clear();
  // The helper names an empty 3.1 identifier "c"; sent as empty it is refused.
  connect.version = Version::v3_1;
  empty.send(connect);
  EXPECT_EQ(empty.expect<mqtt::Connack>().reason, 2);
}

TEST(Link, MQTT_3_2_2_7) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.username = "bad";
  const auto connack = client.connect(connect);
  EXPECT_EQ(connack.reason, reason::bad_user_name_or_password);
  EXPECT_FALSE(connack.session_present);  // [MQTT-3.2.2-6]
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT311_3_2_2_5) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.username = "bad";
  EXPECT_EQ(client.connect(connect).reason, 4);
  EXPECT_TRUE(client.closes());
}

TEST(Link, Connack3xCodesAreTheNearestToEachRefusal) {
  EXPECT_EQ(connack_code_3x(reason::success), 0);
  EXPECT_EQ(connack_code_3x(reason::unsupported_protocol_version), 1);
  EXPECT_EQ(connack_code_3x(reason::client_identifier_not_valid), 2);
  EXPECT_EQ(connack_code_3x(reason::server_unavailable), 3);
  EXPECT_EQ(connack_code_3x(reason::unspecified_error), 3);
  EXPECT_EQ(connack_code_3x(reason::bad_user_name_or_password), 4);
  EXPECT_EQ(connack_code_3x(reason::not_authorized), 5);
  EXPECT_EQ(connack_code_3x(reason::banned), 5);
}

TEST(Link, MQTT_3_2_2_12) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  EXPECT_EQ(client.connect(with_will("w", mqtt::QoS::at_least_once)).reason, reason::qos_not_supported);
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT_3_2_2_13) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  EXPECT_EQ(client.connect(with_will("w", mqtt::QoS::at_most_once, true)).reason, reason::retain_not_supported);
  EXPECT_TRUE(client.closes());
}

// Every will is refused until Dagda's leases let one outlive its node (the
// maintainer, 2026-11-22) [MQTT-3.1.2-7].
TEST(Link, EveryWillIsRefusedUntilDagdasLeases) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  EXPECT_EQ(client.connect(with_will("w")).reason, reason::implementation_specific_error);
  EXPECT_TRUE(client.closes());
  Client old(cluster, 0, Version::v3_1_1);
  EXPECT_EQ(old.connect(with_will("w")).reason, 3);
  EXPECT_TRUE(old.closes());
}

// Enhanced authentication

TEST(Link, MQTT_4_12_0_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.properties.authentication_method = "scram";
  client.send(connect);
  const auto challenge = client.expect<mqtt::Auth>();
  EXPECT_EQ(challenge.reason, reason::continue_authentication);
  EXPECT_EQ(challenge.properties.authentication_method, "scram");
  EXPECT_EQ(challenge.properties.authentication_data, mqtt::Bytes{1});
  mqtt::Auth answer;
  answer.reason = reason::continue_authentication;
  answer.properties.authentication_method = "scram";
  answer.properties.authentication_data = mqtt::Bytes{2};
  client.send(answer);
  const auto connack = client.expect<mqtt::Connack>();
  EXPECT_EQ(connack.reason, reason::success);
  EXPECT_EQ(connack.properties.authentication_method, "scram");
  EXPECT_EQ(connack.properties.authentication_data, mqtt::Bytes{3});
  // Re-authentication answers with AUTH 0x00.
  answer.reason = reason::reauthenticate;
  client.send(answer);
  const auto done = client.expect<mqtt::Auth>();
  EXPECT_EQ(done.reason, reason::success);
  EXPECT_EQ(done.properties.authentication_data, mqtt::Bytes{3});
}

TEST(Link, MQTT_3_1_2_30) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.properties.authentication_method = "scram";
  client.send(connect);
  (void)client.expect<mqtt::Auth>();
  client.send(mqtt::Pingreq{});
  // [MQTT-3.14.0-1]: before CONNACK the refusal is the CONNACK.
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
}

TEST(Link, AFailedExchangeIsRefused) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.properties.authentication_method = "scram";
  client.send(connect);
  (void)client.expect<mqtt::Auth>();
  mqtt::Auth answer;
  answer.reason = reason::continue_authentication;
  answer.properties.authentication_method = "scram";
  answer.properties.authentication_data = mqtt::Bytes{9};
  client.send(answer);
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, reason::not_authorized);
  EXPECT_TRUE(client.closes());
}

TEST(Link, AnAuthForAnotherMethodIsAProtocolError) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  mqtt::Auth auth;
  auth.reason = reason::reauthenticate;
  auth.properties.authentication_method = "scram";
  client.send(auth);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
}

TEST(Link, AnExpiredGrantEndsTheConnection) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  cluster.authenticator.ends = cluster.nodes.node(0).context().time().now() + 30s;
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.username = "limited";
  EXPECT_EQ(client.connect(connect).reason, reason::success);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::not_authorized);
  EXPECT_TRUE(client.closes());
}

// SUBSCRIBE and delivery across the cluster

TEST(Link, MQTT_3_8_4_1) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 2);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  const auto suback = subscriber.subscribe({"sensors/+/temperature"}, 7);
  EXPECT_EQ(suback.packet_id, 7);  // [MQTT-3.8.4-2]
  EXPECT_EQ(suback.reasons, std::vector<uint8_t>{reason::granted_qos_0});
  // Once SUBACK has gone, a publish anywhere reaches it.
  publisher.send(publish_of("sensors/kitchen/temperature", "21.5"));
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received.front(), publish_of("sensors/kitchen/temperature", "21.5"));
}

TEST(Link, APublishReachesA311SubscriberWithoutProperties) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1, Version::v3_1_1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  EXPECT_EQ(subscriber.subscribe({"t"}).reasons, std::vector<uint8_t>{0});
  auto publish = publish_of("t", "x");
  publish.properties.content_type = "text/plain";
  publish.properties.user_properties = {{"k", "v"}};
  publisher.send(publish);
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received.front(), publish_of("t", "x"));
}

TEST(Link, MQTT_3_2_2_10) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  mqtt::Subscribe subscribe;
  subscribe.packet_id = 1;
  subscribe.subscriptions = {{"a", mqtt::QoS::exactly_once}, {"b", mqtt::QoS::at_least_once}};
  client.send(subscribe);
  EXPECT_EQ(client.expect<mqtt::Suback>().reasons, (std::vector<uint8_t>{reason::granted_qos_0, reason::granted_qos_0}));
}

TEST(Link, MQTT_3_8_4_3) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"a/#"}, 1);
  EXPECT_EQ(subscriber.subscribe({"a/#"}, 2).reasons, std::vector<uint8_t>{reason::granted_qos_0});
  publisher.send(publish_of("a/b", "once"));
  EXPECT_EQ(subscriber.publishes().size(), 1u);
}

TEST(Link, EachFilterIsAnsweredForItself) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 1);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.username = "limited";
  client.connect(connect);
  mqtt::Subscribe subscribe;
  subscribe.packet_id = 3;
  subscribe.subscriptions = {{"ok"}, {"$share/g/t"}, {"a/#/b"}, {"secret/x"}, {"ok/2"}};
  client.send(subscribe);
  const auto suback = client.expect<mqtt::Suback>();
  EXPECT_EQ(suback.packet_id, 3);
  EXPECT_EQ(suback.reasons, (std::vector<uint8_t>{reason::granted_qos_0, reason::shared_subscriptions_not_supported, reason::topic_filter_invalid,
                                                  reason::not_authorized, reason::granted_qos_0}));
}

TEST(Link, ASubscriptionIdentifierIsNotSupported) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  mqtt::Subscribe subscribe;
  subscribe.packet_id = 1;
  subscribe.properties.subscription_identifiers = {5};
  subscribe.subscriptions = {{"a"}};
  client.send(subscribe);
  EXPECT_EQ(client.expect<mqtt::Suback>().reasons, std::vector<uint8_t>{reason::subscription_identifiers_not_supported});
}

TEST(Link, MQTT311_3_9_3_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  client.connect();
  EXPECT_EQ(client.subscribe({"a", "a/#/b"}).reasons, (std::vector<uint8_t>{0x00, 0x80}));
}

TEST(Link, A31FailureClosesTheConnection) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1);
  EXPECT_EQ(client.connect().reason, 0);
  client.send(mqtt::Subscribe{1, {}, {{"a/#/b"}}});
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.quiet());
}

TEST(Link, MQTT_3_8_3_3) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client local(cluster, 0);
  Client other(cluster, 1);
  mqtt::Connect connect;
  connect.client_id = "local";
  local.connect(connect);
  other.connect();
  mqtt::Subscribe subscribe;
  subscribe.packet_id = 1;
  subscribe.subscriptions = {{"t", mqtt::QoS::at_most_once, true}};
  local.send(subscribe);
  EXPECT_EQ(local.expect<mqtt::Suback>().reasons, std::vector<uint8_t>{reason::granted_qos_0});
  other.subscribe({"t"});
  // Its own publish does not come back; another client's does, and the
  // other subscriber gets both.
  local.send(publish_of("t", "mine"));
  other.send(publish_of("t", "theirs"));
  const auto to_local = local.publishes();
  ASSERT_EQ(to_local.size(), 1u);
  EXPECT_EQ(to_local.front().payload, (mqtt::Bytes{'t', 'h', 'e', 'i', 'r', 's'}));
  EXPECT_EQ(other.publishes().size(), 2u);
}

// UNSUBSCRIBE

TEST(Link, MQTT_3_10_4_2) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"t"});
  subscriber.send(mqtt::Unsubscribe{9, {}, {"t", "never"}});
  const auto unsuback = subscriber.expect<mqtt::Unsuback>();
  EXPECT_EQ(unsuback.packet_id, 9);  // [MQTT-3.10.4-4], [MQTT-3.10.4-5]
  EXPECT_EQ(unsuback.reasons, (std::vector<uint8_t>{reason::success, reason::no_subscription_existed}));
  publisher.send(publish_of("t", "x"));
  EXPECT_TRUE(subscriber.publishes().empty());
}

TEST(Link, MQTT311_3_10_4_5) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  client.connect();
  client.send(mqtt::Unsubscribe{4, {}, {"never"}});
  const auto unsuback = client.expect<mqtt::Unsuback>();
  EXPECT_EQ(unsuback.packet_id, 4);
  EXPECT_TRUE(unsuback.reasons.empty());
}

// PUBLISH

TEST(Link, APublishAboveQoS0IsRefused) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  auto publish = publish_of("t", "x");
  publish.qos = mqtt::QoS::at_least_once;
  publish.packet_id = 1;
  client.send(publish);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::qos_not_supported);
  EXPECT_TRUE(client.closes());
}

TEST(Link, A311PublishAboveQoS0Closes) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  client.connect();
  auto publish = publish_of("t", "x");
  publish.qos = mqtt::QoS::exactly_once;
  publish.packet_id = 1;
  client.send(publish);
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.quiet());
}

TEST(Link, MQTT_3_2_2_14) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  auto publish = publish_of("t", "x");
  publish.retain = true;
  client.send(publish);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::retain_not_supported);
  EXPECT_TRUE(client.closes());
}

TEST(Link, A311RetainedPublishCloses) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  client.connect();
  auto publish = publish_of("t", "x");
  publish.retain = true;
  client.send(publish);
  EXPECT_TRUE(client.closes());
}

// A node configured for no aliases says none in CONNACK, and refuses one.
TEST(Link, MQTT_3_2_2_18) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Link::Limits none;
  none.topic_alias_maximum = 0;
  Client client(cluster, 0, Version::v5, none);
  EXPECT_FALSE(client.connect().properties.topic_alias_maximum.has_value());
  auto publish = publish_of("t", "x");
  publish.properties.topic_alias = 1;
  client.send(publish);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::topic_alias_invalid);
  EXPECT_TRUE(client.closes());
}

mqtt::Publish aliased(std::string topic, uint16_t alias, std::string payload) {
  auto publish = publish_of(std::move(topic), std::move(payload));
  publish.properties.topic_alias = alias;
  return publish;
}

// CONNACK says the most aliases a client may set; one past it is invalid.
TEST(Link, MQTT_3_2_2_17) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  EXPECT_EQ(client.connect().properties.topic_alias_maximum, Link::Limits{}.topic_alias_maximum);
  client.send(aliased("t", Link::Limits{}.topic_alias_maximum + 1, "x"));
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::topic_alias_invalid);
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT_3_3_2_8) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  // Lir's encoder will not write alias 0, as a sender must not: the bytes
  // are alias 1's with its value changed.
  auto bytes = mqtt::encode(aliased("t", 1, "x"), Version::v5);
  for (size_t i = 0; i + 2 < bytes.size(); ++i) {
    if (bytes[i] == 0x23 && bytes[i + 1] == 0x00 && bytes[i + 2] == 0x01) {
      bytes[i + 2] = 0x00;
      break;
    }
  }
  client.raw(bytes);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::topic_alias_invalid);
  EXPECT_TRUE(client.closes());
}

// An alias set with a topic stands for it after, until set again; the
// publish crosses the cluster under its topic, never its alias.
TEST(Link, MQTT_3_3_2_12) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"#"});
  publisher.send(aliased("sensors/kitchen", 1, "a"));
  publisher.send(aliased("", 1, "b"));
  publisher.send(aliased("sensors/hall", 1, "c"));
  publisher.send(aliased("", 1, "d"));
  // Order is kept per topic, not across topics (MQTT 4.6), so each payload
  // is matched to its topic.
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 4u);
  const std::map<std::string, std::string> topic_of = {{"a", "sensors/kitchen"}, {"b", "sensors/kitchen"}, {"c", "sensors/hall"}, {"d", "sensors/hall"}};
  for (const auto& publish : received) {
    EXPECT_EQ(publish.topic, topic_of.at(std::string(publish.payload.begin(), publish.payload.end())));
    EXPECT_FALSE(publish.properties.topic_alias.has_value());
  }
  EXPECT_FALSE(publisher.connection->closed);
}

// Aliases are a connection's own: a new connection, even for the same
// client id, starts with none, and one never set is a protocol error.
TEST(Link, MQTT_3_3_2_7) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  mqtt::Connect connect;
  connect.client_id = "same";
  {
    Client first(cluster, 0);
    first.connect(connect);
    first.send(aliased("t", 1, "x"));
    first.send(mqtt::Disconnect{});
    ASSERT_TRUE(first.closes());
  }
  Client second(cluster, 0);
  second.connect(connect);
  second.send(aliased("", 1, "x"));
  EXPECT_EQ(second.expect<mqtt::Disconnect>().reason, reason::protocol_error);
  EXPECT_TRUE(second.closes());
}

TEST(Link, MQTT_3_3_4_6) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  auto publish = publish_of("t", "x");
  publish.properties.subscription_identifiers = {1};
  client.send(publish);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::protocol_error);
}

TEST(Link, APublishTheGrantRefusesGoesNowhere) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  mqtt::Connect connect;
  connect.client_id = "p";
  connect.username = "limited";
  publisher.connect(connect);
  subscriber.subscribe({"#"});
  publisher.send(publish_of("secret/x", "no"));
  publisher.send(publish_of("open", "yes"));
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received.front().topic, "open");
  EXPECT_FALSE(publisher.connection->closed);
}

// Only Lir writes `$lir/` and `$dannan/`, whatever the grant
// (TODO/2026-11-22-system-topics.md): a client's publish there reaches no
// one, while another `$` topic is the client's like any.
TEST(Link, AClientsPublishToASystemTopicReachesNoOne) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"$lir/#", "$dannan/#", "$other/#"});
  publisher.send(publish_of("$lir/cluster", "forged"));
  publisher.send(publish_of("$dannan/roster/joined", "forged"));
  publisher.send(publish_of("$other/x", "yes"));
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received.front().topic, "$other/x");
  EXPECT_FALSE(publisher.connection->closed);
}

TEST(Link, MQTT_3_2_2_15) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v5, Link::Limits{64, 5s});
  EXPECT_EQ(client.connect().properties.maximum_packet_size, 64u);
  client.send(publish_of("t", std::string(100, 'x')));
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::packet_too_large);
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT_3_1_2_25) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.properties.maximum_packet_size = 32;
  client.connect(connect);
  client.subscribe({"t"});
  client.send(publish_of("t", std::string(100, 'x')));
  client.send(publish_of("t", "small"));
  const auto received = client.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received.front().payload, (mqtt::Bytes{'s', 'm', 'a', 'l', 'l'}));
}

// PINGREQ, keep-alive and DISCONNECT

TEST(Link, MQTT_3_12_4_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.send(mqtt::Pingreq{});
  (void)client.expect<mqtt::Pingresp>();
}

TEST(Link, MQTT_3_1_2_22) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.keep_alive = 10;
  auto& time = cluster.nodes.node(0).context().time();
  client.connect(connect);
  const auto start = time.now();
  // Pinging every 9 s keeps it open past several keep-alives.
  for (int i = 0; i < 5; ++i) {
    const auto at = time.now() + 9s;
    ASSERT_TRUE(cluster.world.run_until([&] { return time.now() >= at; }, max_steps));
    client.send(mqtt::Pingreq{});
    (void)client.expect<mqtt::Pingresp>();
  }
  EXPECT_FALSE(client.connection->closed);
  const auto quiet_from = time.now();
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::keep_alive_timeout);
  EXPECT_TRUE(client.closes());
  // One and a half keep-alives of silence, and not much more.
  EXPECT_GE(time.now() - quiet_from, 15s);
  EXPECT_LT(time.now() - quiet_from, 16s);
  EXPECT_GT(time.now() - start, 45s);
}

TEST(Link, MQTT311_3_1_2_24) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v3_1_1);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.keep_alive = 2;
  client.connect(connect);
  EXPECT_TRUE(client.closes());
}

TEST(Link, MQTT_3_14_4_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.send(mqtt::Disconnect{});
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.quiet());
}

TEST(Link, AClosedLinkLeavesNothingSubscribed) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"t"});
  boost::asio::post(cluster.world.io, [link = subscriber.link] { link->closed(); });
  cluster.settle();
  publisher.send(publish_of("t", "x"));
  EXPECT_TRUE(subscriber.publishes().empty());
  EXPECT_FALSE(subscriber.link->connected());
}

TEST(Link, APacketSplitAcrossReadsIsWhole) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  const auto bytes = mqtt::encode(connect, Version::v5);
  const auto ping = mqtt::encode(mqtt::Pingreq{}, Version::v5);
  client.raw(mqtt::Bytes(bytes.begin(), bytes.begin() + 3));
  auto rest = mqtt::Bytes(bytes.begin() + 3, bytes.end());
  rest.insert(rest.end(), ping.begin(), ping.end());
  client.raw(rest);
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, reason::success);
  (void)client.expect<mqtt::Pingresp>();
}

TEST(Link, AMalformedPacketEndsTheConnection) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  // PINGREQ with a body.
  client.raw({0xC0, 0x01, 0x00});
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::malformed_packet);
  EXPECT_TRUE(client.closes());
}

// Conformance a QoS 0 server meets now, each statement by name.

// Every topic is an ordered topic [MQTT-4.6.0-6]: one client's messages on
// one topic reach a subscriber on another node in the order sent. Seen
// failing on notify ("at most once, no sequence"): 20 publishes arrived out
// of order; Lir's channel is Dannan 0.11.1's ordered delivery since.
TEST(Link, MQTT_4_6_0_6) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"ordered"});
  for (int i = 0; i < 20; ++i) publisher.send(publish_of("ordered", std::to_string(i)));
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 20u);
  for (size_t i = 0; i < received.size(); ++i) EXPECT_EQ(std::string(received[i].payload.begin(), received[i].payload.end()), std::to_string(i));
}

// Where links overtake, as no one session does, order still holds: what
// arrives is in the order sent, a late message dropped rather than delivered
// out of turn, as QoS 0 allows.
TEST(Link, AnOvertakenPublishIsDroppedRatherThanDeliveredOutOfOrder) {
  auto overtaking = test::Cluster::faults();
  overtaking.in_order = false;
  Cluster cluster(2, 1, overtaking);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"ordered"});
  for (int i = 0; i < 20; ++i) publisher.send(publish_of("ordered", std::to_string(i)));
  const auto received = subscriber.publishes();
  ASSERT_FALSE(received.empty());
  int last = -1;
  for (const auto& publish : received) {
    const int n = std::stoi(std::string(publish.payload.begin(), publish.payload.end()));
    EXPECT_GT(n, last);
    last = n;
  }
}

// What a publisher said of its message reaches every subscriber unaltered:
// its payload format, response topic, correlation data, user properties in
// their order, and content type.
TEST(Link, MQTT_3_3_2_4_and_15_to_18_and_20) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 1);
  Client publisher(cluster, 0);
  subscriber.connect();
  publisher.connect();
  subscriber.subscribe({"rpc/#"});
  auto publish = publish_of("rpc/request", "{}");
  publish.properties.payload_format_indicator = 1;
  publish.properties.response_topic = "rpc/reply/7";
  publish.properties.correlation_data = mqtt::Bytes{7, 7};
  publish.properties.user_properties = {{"b", "2"}, {"a", "1"}, {"b", "3"}};
  publish.properties.content_type = "application/json";
  publisher.send(publish);
  const auto received = subscriber.publishes();
  ASSERT_EQ(received.size(), 1u);
  const auto& properties = received[0].properties;
  EXPECT_EQ(properties.payload_format_indicator, 1);
  EXPECT_EQ(properties.response_topic, "rpc/reply/7");
  EXPECT_EQ(properties.correlation_data, (mqtt::Bytes{7, 7}));
  EXPECT_EQ(properties.user_properties, publish.properties.user_properties);
  EXPECT_EQ(properties.content_type, "application/json");
}

// The topic a subscriber is sent matches its filter [MQTT-3.3.2-3]; with
// overlapping subscriptions it has the message at least once [MQTT-3.3.4-2].
TEST(Link, MQTT_3_3_2_3) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.subscribe({"a/+"});
  client.send(publish_of("a/b", "x"));
  client.send(publish_of("c/d", "y"));
  const auto received = client.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].topic, "a/b");
}

TEST(Link, MQTT_3_3_4_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.subscribe({"a/+", "a/#"});
  client.send(publish_of("a/b", "x"));
  EXPECT_GE(client.publishes().size(), 1u);
}

// One SUBSCRIBE of several filters is answered as if each came alone, by one
// SUBACK with its packet id and a reason per filter, in order.
TEST(Link, MQTT_3_8_4_2_and_5_to_7) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  const auto suback = client.subscribe({"a/#", "a/#/b", "c"}, 77);
  EXPECT_EQ(suback.packet_id, 77);
  EXPECT_EQ(suback.reasons, (std::vector<uint8_t>{reason::granted_qos_0, reason::topic_filter_invalid, reason::granted_qos_0}));
}

TEST(Link, MQTT_3_9_3_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  EXPECT_EQ(client.subscribe({"x/#/y", "ok"}).reasons, (std::vector<uint8_t>{reason::topic_filter_invalid, reason::granted_qos_0}));
}

// UNSUBSCRIBE compares filters character by character, never by matching
// [MQTT-3.10.4-1]; it is answered by an UNSUBACK with its packet id even when
// nothing was removed [MQTT-3.10.4-4], [MQTT-3.10.4-5], one reason per
// filter in order [MQTT-3.11.3-1], several filters as if each came alone
// [MQTT-3.10.4-6].
TEST(Link, MQTT_3_10_4_1_and_4_to_6) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.subscribe({"a/+", "a/#"});
  mqtt::Unsubscribe unsubscribe;
  unsubscribe.packet_id = 9;
  unsubscribe.filters = {"a/b", "a/+", "a/+"};
  client.send(unsubscribe);
  const auto unsuback = client.expect<mqtt::Unsuback>();
  EXPECT_EQ(unsuback.packet_id, 9);
  EXPECT_EQ(unsuback.reasons, (std::vector<uint8_t>{reason::no_subscription_existed, reason::success, reason::no_subscription_existed}));
  // "a/#" is still there.
  client.send(publish_of("a/b", "x"));
  EXPECT_EQ(client.publishes().size(), 1u);
}

TEST(Link, MQTT_3_11_3_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.subscribe({"b"});
  mqtt::Unsubscribe unsubscribe;
  unsubscribe.packet_id = 3;
  unsubscribe.filters = {"a", "b"};
  client.send(unsubscribe);
  EXPECT_EQ(client.expect<mqtt::Unsuback>().reasons, (std::vector<uint8_t>{reason::no_subscription_existed, reason::success}));
}

// CONNACK is the first packet the server sends, and the only one: a second
// CONNECT is answered by DISCONNECT [MQTT-3.2.0-1], [MQTT-3.2.0-2].
TEST(Link, MQTT_3_2_0_1_and_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  EXPECT_EQ(client.connect().reason, reason::success);
  client.send(mqtt::Connect{});
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
}

// A refusing CONNACK closes the connection [MQTT-3.2.2-8].
TEST(Link, MQTT_3_2_2_8) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.username = "bad";
  EXPECT_EQ(client.connect(connect).reason, reason::bad_user_name_or_password);
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.quiet());
}

// Before CONNACK there is no DISCONNECT: a protocol error while
// authenticating is a refusing CONNACK [MQTT-3.14.0-1].
TEST(Link, MQTT_3_14_0_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.properties.authentication_method = "scram";
  client.send(connect);
  (void)client.expect<mqtt::Auth>();
  mqtt::Subscribe subscribe;
  subscribe.packet_id = 1;
  subscribe.subscriptions.push_back({"a"});
  client.send(subscribe);
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
}

// After its DISCONNECT the server sends nothing more and closes
// [MQTT-3.14.4-1], [MQTT-3.14.4-2]; a protocol error is that, with its reason
// [MQTT-4.13.1-1].
TEST(Link, MQTT_3_14_4_2_and_MQTT_4_13_1_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  client.subscribe({"a"});
  client.send(mqtt::Pingresp{});
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
  EXPECT_TRUE(client.quiet());
}

// Nothing larger than the client's Maximum Packet Size is sent to it
// [MQTT-3.1.2-24]; what fits still is.
TEST(Link, MQTT_3_1_2_24) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.properties.maximum_packet_size = 64;
  client.connect(connect);
  client.subscribe({"a"});
  client.send(publish_of("a", std::string(100, 'x')));
  client.send(publish_of("a", "small"));
  const auto received = client.publishes();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].payload.size(), 5u);
}

// A client that takes no aliases, or a maximum of them, is sent none: Lir
// sends no Topic Alias [MQTT-3.1.2-27], [MQTT-3.3.2-11].
TEST(Link, MQTT_3_1_2_27_and_MQTT_3_3_2_11) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.properties.topic_alias_maximum = 10;
  client.connect(connect);
  client.subscribe({"a"});
  client.send(publish_of("a", "1"));
  client.send(publish_of("a", "2"));
  for (const auto& publish : client.publishes()) {
    EXPECT_EQ(publish.topic, "a");
    EXPECT_FALSE(publish.properties.topic_alias.has_value());
  }
}

// A client that asks for no Response Information gets none
// [MQTT-3.1.2-28]; one that asks for no Problem Information is sent no Reason
// String or User Property past CONNACK and DISCONNECT [MQTT-3.1.2-29].
TEST(Link, MQTT_3_1_2_28_and_29) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.properties.request_response_information = 0;
  connect.properties.request_problem_information = 0;
  EXPECT_FALSE(client.connect(connect).properties.response_information.has_value());
  const auto suback = client.subscribe({"a/#/b"});
  EXPECT_FALSE(suback.properties.reason_string.has_value());
  EXPECT_TRUE(suback.properties.user_properties.empty());
}

// Without a Server Keep Alive in CONNACK, the client's own Keep Alive is
// the one held [MQTT-3.2.2-22].
TEST(Link, MQTT_3_2_2_22) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.keep_alive = 10;
  EXPECT_FALSE(client.connect(connect).properties.server_keep_alive.has_value());
  // Quiet for less than one and a half keep-alives: still open.
  const auto start = cluster.world.now_ms();
  cluster.world.run_until([&] { return cluster.world.now_ms() >= start + 10'000; }, max_steps);
  EXPECT_FALSE(client.connection->closed);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::keep_alive_timeout);
}

// With an Authentication Method in CONNECT, every AUTH and the CONNACK carry
// it [MQTT-4.12.0-5]; without one, none does and no AUTH is sent
// [MQTT-4.12.0-6].
TEST(Link, MQTT_4_12_0_5_and_6) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client plain(cluster, 0);
  EXPECT_FALSE(plain.connect().properties.authentication_method.has_value());
  Client scram(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "s";
  connect.properties.authentication_method = "scram";
  scram.send(connect);
  EXPECT_EQ(scram.expect<mqtt::Auth>().properties.authentication_method, "scram");
  mqtt::Auth answer;
  answer.reason = reason::continue_authentication;
  answer.properties.authentication_method = "scram";
  answer.properties.authentication_data = mqtt::Bytes{2};
  scram.send(answer);
  EXPECT_EQ(scram.expect<mqtt::Connack>().properties.authentication_method, "scram");
}

// A failed re-authentication ends the connection with DISCONNECT
// [MQTT-4.12.1-2].
TEST(Link, MQTT_4_12_1_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "r";
  connect.properties.authentication_method = "scram";
  client.send(connect);
  (void)client.expect<mqtt::Auth>();
  mqtt::Auth answer;
  answer.reason = reason::continue_authentication;
  answer.properties.authentication_method = "scram";
  answer.properties.authentication_data = mqtt::Bytes{2};
  client.send(answer);
  ASSERT_EQ(client.expect<mqtt::Connack>().reason, reason::success);
  mqtt::Auth again;
  again.reason = reason::reauthenticate;
  again.properties.authentication_method = "scram";
  again.properties.authentication_data = mqtt::Bytes{9};
  client.send(again);
  EXPECT_EQ(client.expect<mqtt::Disconnect>().reason, reason::not_authorized);
  EXPECT_TRUE(client.closes());
}

// A verified client certificate reaches the authenticator with the peer.
TEST(Link, AVerifiedCertificateReachesTheAuthenticator) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0, Version::v5, {}, contract::ClientCertificate{"CN=sensor-7", {}});
  client.connect();
  EXPECT_EQ(cluster.authenticator.seen_subject, "CN=sensor-7");
}

// An AUTH whose reason does not fit where the exchange is: a protocol error.
TEST(Link, AnAuthWithAReasonOutOfPlaceIsAProtocolError) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.properties.authentication_method = "scram";
  client.send(connect);
  (void)client.expect<mqtt::Auth>();
  mqtt::Auth wrong;
  wrong.reason = reason::reauthenticate;
  wrong.properties.authentication_method = "scram";
  client.send(wrong);
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, reason::protocol_error);
  EXPECT_TRUE(client.closes());
}

// An authenticator that challenges a CONNECT naming no method is refused for
// it, not followed.
TEST(Link, AChallengeToAConnectWithoutAMethodIsRefused) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.username = "challenge";
  EXPECT_EQ(client.connect(connect).reason, reason::unspecified_error);
  EXPECT_TRUE(client.closes());
}

// The same filter subscribed again before the cluster holds it: both
// SUBACKs wait for the one hold [MQTT-3.8.4-3].
TEST(Link, AFilterSubscribedTwiceBeforeItIsHeldIsAnsweredTwice) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  client.connect();
  mqtt::Subscribe first;
  first.packet_id = 1;
  first.subscriptions.push_back({"t"});
  auto second = first;
  second.packet_id = 2;
  client.send(first);
  client.send(second);
  const auto a = client.expect<mqtt::Suback>();
  const auto b = client.expect<mqtt::Suback>();
  EXPECT_EQ(a.reasons, std::vector<uint8_t>{reason::granted_qos_0});
  EXPECT_EQ(b.reasons, std::vector<uint8_t>{reason::granted_qos_0});
}

// A subscription the cluster cannot hold by its deadline is refused rather
// than granted: a publish made after the SUBACK might not reach it (the
// design, "What may be acknowledged").
TEST(Link, ASubscriptionTheClusterCannotHoldInTimeIsRefused) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  cluster.nodes.network().partition({{0}, {1}});
  Link::Limits limits;
  limits.subscribe_deadline = 1s;
  Client client(cluster, 0, Version::v5, limits);
  client.connect();
  EXPECT_EQ(client.subscribe({"t"}).reasons, std::vector<uint8_t>{reason::unspecified_error});
}

}  // namespace
}  // namespace lir::broker
