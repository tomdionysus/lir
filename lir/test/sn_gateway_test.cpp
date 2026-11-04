//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/ip/udp.hpp>

#include "lir/sn/gateway.h"
#include "lir/sn/packet.h"
#include "lir/test/fixtures/mqtt_client.h"

// The MQTT-SN 1.2 gateway over real UDP, beside an MQTT client on TCP: the
// same broker.
namespace lir::sn {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using udp = asio::ip::udp;
using test::MqttClient;

// An MQTT-SN client: datagrams out, the next packet in within a bound.
class SnClient {
 public:
  explicit SnClient(uint16_t port) : _socket(_io, udp::endpoint(udp::v4(), 0)), _gateway(asio::ip::make_address("127.0.0.1"), port) {}

  void send(const Packet& packet) { send_bytes(encode(packet)); }
  void send_bytes(const Bytes& bytes) { _socket.send_to(asio::buffer(bytes), _gateway); }

  std::optional<Packet> next(std::chrono::milliseconds bound = 2000ms) {
    size_t got = 0;
    boost::system::error_code result = asio::error::timed_out;
    _socket.async_receive_from(asio::buffer(_buffer), _from, [&](const boost::system::error_code& ec, size_t n) {
      result = ec;
      got = n;
    });
    _io.restart();
    _io.run_for(bound);
    if (!_io.stopped()) {
      _socket.cancel();
      _io.restart();
      _io.run_for(200ms);
      return std::nullopt;
    }
    if (result) return std::nullopt;
    return decode(std::span<const uint8_t>(_buffer.data(), got));
  }
  template <typename T>
  std::optional<T> next_a(std::chrono::milliseconds bound = 2000ms) {
    const auto p = next(bound);
    if (!p || !std::holds_alternative<T>(*p)) return std::nullopt;
    return std::get<T>(*p);
  }

  void connect(const std::string& id, uint16_t duration = 60, bool clean = true) {
    Connect c;
    c.flags.clean_session = clean;
    c.duration = duration;
    c.client_id = id;
    send(c);
    const auto ack = next_a<Connack>();
    ASSERT_TRUE(ack);
    ASSERT_EQ(ack->code, rc::accepted);
  }
  uint16_t register_topic(const std::string& topic, uint16_t msg_id = 1) {
    send(Register{0, msg_id, topic});
    const auto ack = next_a<Regack>();
    EXPECT_TRUE(ack);
    return ack ? ack->topic_id : 0;
  }

 private:
  asio::io_context _io;
  udp::socket _socket;
  udp::endpoint _gateway;
  udp::endpoint _from;
  std::array<uint8_t, 65536> _buffer{};
};

std::string text(const Bytes& b) { return {b.begin(), b.end()}; }
Bytes bytes(const std::string& s) { return {s.begin(), s.end()}; }

class SnGatewayTest : public ::testing::Test {
 protected:
  SnGatewayTest()
      : edge(io.context, edge_config(), dannan::contract::steady_time_source(), allow, logger()), gateway(io.context, edge, gateway_config(), logger()) {
    edge.start();
    gateway.start();
  }
  ~SnGatewayTest() override {
    gateway.stop();
    edge.stop();
  }
  static edge::EdgeConfig edge_config() {
    edge::EdgeConfig c;
    c.listen_host = "127.0.0.1";
    c.port = 0;
    return c;
  }
  static GatewayConfig gateway_config() {
    GatewayConfig c;
    c.listen_host = "127.0.0.1";
    c.port = 0;
    c.gateway_id = 9;
    c.poll_every = 100ms;
    c.awake_grace = 300ms;
    return c;
  }
  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  void predefine(uint16_t id, const std::string& topic) {
    edge.call<bool>([&](broker::Broker& b) {
      broker::Message m;
      m.topic = std::string(kPredefinedPrefix) + std::to_string(id);
      m.payload = bytes(topic);
      m.retain = true;
      b.publish(std::move(m));
      return true;
    });
    // The gateway reads them at its next poll.
    std::this_thread::sleep_for(250ms);
  }

  test::Io io;
  contract::AllowAll allow;
  edge::Edge edge;
  Gateway gateway;
};

TEST_F(SnGatewayTest, SearchGwIsAnsweredWithGwInfo) {
  SnClient c(gateway.port());
  c.send(SearchGw{1});
  const auto info = c.next_a<GwInfo>();
  ASSERT_TRUE(info);
  EXPECT_EQ(info->gateway_id, 9);
}

TEST_F(SnGatewayTest, APublishReachesATcpSubscriber) {
  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.subscribe("sensors/#", mqtt::QoS::at_least_once);
  SnClient c(gateway.port());
  c.connect("sn-1");
  const auto id = c.register_topic("sensors/temp");
  ASSERT_NE(id, 0);
  Publish p;
  p.flags.qos = 1;
  p.topic_id = id;
  p.msg_id = 7;
  p.data = bytes("21.5");
  c.send(p);
  const auto ack = c.next_a<Puback>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->msg_id, 7);
  EXPECT_EQ(ack->topic_id, id);
  EXPECT_EQ(ack->code, rc::accepted);
  const auto got = tcp.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "sensors/temp");
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "21.5");
}

TEST_F(SnGatewayTest, AnUnknownTopicIdIsRefused) {
  SnClient c(gateway.port());
  c.connect("sn-1");
  Publish p;
  p.topic_id = 99;
  c.send(p);
  const auto ack = c.next_a<Puback>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->code, rc::invalid_topic_id);
}

TEST_F(SnGatewayTest, ANewTopicIsRegisteredWithTheClientBeforeItsPublish) {
  SnClient c(gateway.port());
  c.connect("sn-1");
  Subscribe s;
  s.flags.qos = 1;
  s.msg_id = 3;
  s.topic = "news/#";
  c.send(s);
  const auto suback = c.next_a<Suback>();
  ASSERT_TRUE(suback);
  EXPECT_EQ(suback->code, rc::accepted);
  EXPECT_EQ(suback->flags.qos, 1);
  EXPECT_EQ(suback->topic_id, 0);  // a wildcard has no id

  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.publish("news/today", "hello", mqtt::QoS::at_least_once);
  const auto reg = c.next_a<Register>();
  ASSERT_TRUE(reg);
  EXPECT_EQ(reg->topic, "news/today");
  // Nothing more until the client acknowledges the id.
  EXPECT_FALSE(c.next(300ms));
  c.send(Regack{reg->topic_id, reg->msg_id, rc::accepted});
  const auto pub = c.next_a<Publish>();
  ASSERT_TRUE(pub);
  EXPECT_EQ(pub->topic_id, reg->topic_id);
  EXPECT_EQ(pub->flags.topic_id_type, TopicIdType::normal);
  EXPECT_EQ(text(pub->data), "hello");
  c.send(Puback{pub->topic_id, pub->msg_id, rc::accepted});
}

TEST_F(SnGatewayTest, ShortTopicNamesTravelAsTheirCharacters) {
  SnClient c(gateway.port());
  c.connect("sn-1");
  Subscribe s;
  s.flags.topic_id_type = TopicIdType::short_name;
  s.msg_id = 1;
  s.topic = "ab";
  c.send(s);
  ASSERT_TRUE(c.next_a<Suback>());
  Publish p;
  p.flags.topic_id_type = TopicIdType::short_name;
  p.topic_id = short_topic("ab");
  p.data = bytes("x");
  c.send(p);
  const auto got = c.next_a<Publish>();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->flags.topic_id_type, TopicIdType::short_name);
  EXPECT_EQ(short_name(got->topic_id), "ab");
}

TEST_F(SnGatewayTest, PredefinedTopicsAreRetainedMessages) {
  predefine(5, "alarms/door");
  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.subscribe("alarms/#", mqtt::QoS::at_most_once);
  SnClient c(gateway.port());
  c.connect("sn-1");
  Publish p;
  p.flags.topic_id_type = TopicIdType::predefined;
  p.topic_id = 5;
  p.data = bytes("open");
  c.send(p);
  const auto got = tcp.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "alarms/door");
}

TEST_F(SnGatewayTest, QoSMinusOneNeedsNoConnection) {
  predefine(6, "alarms/smoke");
  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.subscribe("alarms/#", mqtt::QoS::at_most_once);
  SnClient c(gateway.port());
  Publish p;
  p.flags.qos = -1;
  p.flags.topic_id_type = TopicIdType::predefined;
  p.topic_id = 6;
  p.data = bytes("fire");
  c.send(p);
  const auto got = tcp.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "alarms/smoke");
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "fire");
  EXPECT_EQ(gateway.stats().qos_minus_one, 1u);
}

TEST_F(SnGatewayTest, TheWillIsAskedForAndPublishedWhenTheClientIsLost) {
  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.subscribe("wills/#", mqtt::QoS::at_most_once);
  SnClient c(gateway.port());
  Connect connect;
  connect.flags.will = true;
  connect.flags.clean_session = true;
  connect.duration = 1;
  connect.client_id = "mortal";
  c.send(connect);
  ASSERT_TRUE(c.next_a<WillTopicReq>());
  Flags wf;
  wf.qos = 0;
  c.send(WillTopic{wf, "wills/mortal"});
  ASSERT_TRUE(c.next_a<WillMsgReq>());
  c.send(WillMsg{bytes("gone")});
  const auto ack = c.next_a<Connack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->code, rc::accepted);
  // Silent past one and a half keep alive periods.
  const auto got = tcp.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "wills/mortal");
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "gone");
  EXPECT_EQ(gateway.stats().wills_published, 1u);
}

TEST_F(SnGatewayTest, AnOrderlyDisconnectPublishesNoWill) {
  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.subscribe("wills/#", mqtt::QoS::at_most_once);
  SnClient c(gateway.port());
  Connect connect;
  connect.flags.will = true;
  connect.flags.clean_session = true;
  connect.duration = 1;
  connect.client_id = "polite";
  c.send(connect);
  ASSERT_TRUE(c.next_a<WillTopicReq>());
  c.send(WillTopic{Flags{}, "wills/polite"});
  ASSERT_TRUE(c.next_a<WillMsgReq>());
  c.send(WillMsg{bytes("gone")});
  ASSERT_TRUE(c.next_a<Connack>());
  c.send(Disconnect{});
  ASSERT_TRUE(c.next_a<Disconnect>());
  EXPECT_FALSE(tcp.next_message());
}

TEST_F(SnGatewayTest, ASleepingClientGetsWhatArrivedWhenItWakes) {
  SnClient c(gateway.port());
  c.connect("sleepy", 60, false);
  Subscribe s;
  s.msg_id = 1;
  s.topic = "ab";
  s.flags.topic_id_type = TopicIdType::short_name;
  c.send(s);
  ASSERT_TRUE(c.next_a<Suback>());
  c.send(Disconnect{600});
  ASSERT_TRUE(c.next_a<Disconnect>());

  MqttClient tcp(edge.port());
  tcp.connect("tcp");
  tcp.publish("ab", "one");
  tcp.publish("ab", "two");
  EXPECT_FALSE(c.next(300ms));
  c.send(Pingreq{"sleepy"});
  const auto first = c.next_a<Publish>();
  const auto second = c.next_a<Publish>();
  ASSERT_TRUE(first && second);
  EXPECT_EQ(text(first->data), "one");
  EXPECT_EQ(text(second->data), "two");
  EXPECT_TRUE(c.next_a<Pingresp>());
  // Back asleep after the grace: held again.
  std::this_thread::sleep_for(500ms);
  tcp.publish("ab", "three");
  EXPECT_FALSE(c.next(300ms));
  c.send(Pingreq{"sleepy"});
  const auto third = c.next_a<Publish>();
  ASSERT_TRUE(third);
  EXPECT_EQ(text(third->data), "three");
}

TEST_F(SnGatewayTest, AForwardersClientIsAnsweredThroughTheForwarder) {
  SnClient forwarder(gateway.port());
  Connect connect;
  connect.flags.clean_session = true;
  connect.duration = 60;
  connect.client_id = "far";
  forwarder.send(Encapsulated{0, bytes("node-7"), encode(connect)});
  const auto wrapped = forwarder.next_a<Encapsulated>();
  ASSERT_TRUE(wrapped);
  EXPECT_EQ(text(wrapped->wireless_node_id), "node-7");
  EXPECT_EQ(decode(wrapped->packet), Packet(Connack{rc::accepted}));
}

TEST_F(SnGatewayTest, AStrangerIsToldItIsNotConnected) {
  SnClient c(gateway.port());
  c.send(Pingreq{});
  EXPECT_TRUE(c.next_a<Disconnect>());
  c.send_bytes(Bytes{0x05, 0x99});
  EXPECT_FALSE(c.next(200ms));
  EXPECT_EQ(gateway.stats().malformed, 1u);
}

}  // namespace
}  // namespace lir::sn
