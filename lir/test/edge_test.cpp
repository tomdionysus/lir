//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <gtest/gtest.h>

#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
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

// A stopping node tells a 5.0 client why before it closes (DISCONNECT 0x8B,
// Server shutting down, section 3.14.2.1), so the client can go elsewhere at
// once; a 3.x client, which has no server DISCONNECT, is closed.
TEST_F(EdgeTest, StoppingTellsA5ClientTheServerIsShuttingDown) {
  MqttClient five(edge.port());
  five.connect("five");
  MqttClient old(edge.port(), Version::v3_1_1);
  old.connect("old");
  edge.stop();
  const auto disconnect = five.receive_a<mqtt::Disconnect>();
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, mqtt::reason::server_shutting_down);
  EXPECT_TRUE(five.closed_by_server());
  EXPECT_TRUE(old.closed_by_server());
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

// MQTT over TLS: a self-signed certificate made for the test, a client
// that verifies the server against it, and the same broker as the plain
// port.
class TlsEdgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string openssl = LIR_OPENSSL;
    if (openssl.find("NOTFOUND") != std::string::npos) GTEST_SKIP() << "no openssl command";
    dannan::test::ChildProcess make(
        {openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", (dir.path() / "key.pem").string(), "-out",
         (dir.path() / "cert.pem").string(), "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"},
        dir.path() / "openssl.log");
    ASSERT_EQ(make.wait(std::chrono::steady_clock::now() + 20s), 0);
    EdgeConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    config.tls = TlsConfig{"127.0.0.1", 0, dir.path() / "cert.pem", dir.path() / "key.pem"};
    edge.emplace(io.context, config, dannan::contract::steady_time_source(), allow,
                 dannan::status::ScopedLogger{std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
    edge->start();
  }

  dannan::test::TempDir dir;
  test::Io io;
  contract::AllowAll allow;
  std::optional<Edge> edge;
};

TEST_F(TlsEdgeTest, AVerifyingClientConnectsAndReceives) {
  ASSERT_NE(edge->tls_port(), 0);
  boost::asio::io_context client_io;
  boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
  context.load_verify_file((dir.path() / "cert.pem").string());
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls(client_io, context);
  tls.set_verify_mode(boost::asio::ssl::verify_peer);
  tls.set_verify_callback(boost::asio::ssl::host_name_verification("127.0.0.1"));
  tls.lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), edge->tls_port()});
  tls.handshake(boost::asio::ssl::stream_base::client);

  const auto exchange = [&tls](const mqtt::Packet& packet) { boost::asio::write(tls, boost::asio::buffer(mqtt::encode(packet, Version::v5))); };
  const auto next = [&tls]() -> mqtt::Packet {
    mqtt::Bytes buffer;
    for (;;) {
      if (const auto frame = mqtt::read_frame(buffer, UINT32_MAX); frame && buffer.size() >= frame->size()) return mqtt::decode(buffer, Version::v5);
      std::array<uint8_t, 1> byte{};
      boost::asio::read(tls, boost::asio::buffer(byte));
      buffer.push_back(byte[0]);
    }
  };
  exchange(mqtt::Connect{Version::v5, true, 0, "secure", {}, {}, {}, {}});
  ASSERT_EQ(std::get<mqtt::Connack>(next()).reason, mqtt::reason::success);
  exchange(mqtt::Subscribe{1, {}, {{"secret/#", QoS::at_most_once}}});
  ASSERT_TRUE(std::holds_alternative<mqtt::Suback>(next()));

  MqttClient plain(edge->port());
  plain.connect("plain");
  plain.publish("secret/door", "open", QoS::at_least_once);
  const auto got = std::get<mqtt::Publish>(next());
  EXPECT_EQ(got.topic, "secret/door");
  EXPECT_EQ(std::string(got.payload.begin(), got.payload.end()), "open");
}

// A close the broker makes ends the TLS session with close_notify (RFC 8446,
// section 6.1), so the client reads a clean end, not a truncation.
TEST_F(TlsEdgeTest, TheBrokersCloseSendsCloseNotify) {
  boost::asio::io_context client_io;
  boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
  context.load_verify_file((dir.path() / "cert.pem").string());
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls(client_io, context);
  tls.set_verify_mode(boost::asio::ssl::verify_peer);
  tls.lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), edge->tls_port()});
  tls.handshake(boost::asio::ssl::stream_base::client);
  boost::asio::write(tls, boost::asio::buffer(mqtt::encode(mqtt::Connect{Version::v5, true, 0, "leaving", {}, {}, {}, {}}, Version::v5)));
  boost::asio::write(tls, boost::asio::buffer(mqtt::encode(mqtt::Disconnect{}, Version::v5)));
  std::array<uint8_t, 256> buffer{};
  boost::system::error_code ec;
  while (!ec) tls.read_some(boost::asio::buffer(buffer), ec);
  EXPECT_EQ(ec, boost::asio::error::eof) << ec.message();
}

TEST_F(TlsEdgeTest, PlaintextOnTheTlsPortGetsNothing) {
  MqttClient wrong(edge->tls_port(), Version::v5, 0, 1);
  wrong.send(mqtt::Connect{Version::v5, true, 0, "oops", {}, {}, {}, {}});
  EXPECT_FALSE(wrong.receive_a<mqtt::Connack>());
  EXPECT_TRUE(test::eventually([&] { return edge->call<size_t>([](broker::Broker& b) { return b.connections(); }) == 0; }, 2s));
}

}  // namespace
}  // namespace lir::edge
