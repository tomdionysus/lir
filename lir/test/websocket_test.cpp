//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/edge/edge.h"
#include "lir/test/fixtures/mqtt_client.h"

// MQTT over WebSocket, as MQTT 5.0 and 3.1.1 section 6 says it is carried.
namespace lir::edge {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using mqtt::QoS;
using mqtt::Version;
using tcp = asio::ip::tcp;

// A WebSocket MQTT client over `Next` (a socket, or TLS over one). Every
// wait is bounded: the client's own io context runs for at most `bound`.
template <typename Next>
class WsClient {
 public:
  template <typename... Args>
  explicit WsClient(Args&&... args) : _ws(_io, std::forward<Args>(args)...) {}

  asio::io_context& io() { return _io; }
  websocket::stream<Next>& ws() { return _ws; }

  // The upgrade, offering `protocols` (comma-separated; empty: none). The
  // subprotocol the server chose, or the error.
  std::pair<boost::system::error_code, std::string> handshake(const std::string& protocols, const std::string& path = "/mqtt") {
    _ws.set_option(websocket::stream_base::decorator([protocols](websocket::request_type& request) {
      if (!protocols.empty()) request.set(http::field::sec_websocket_protocol, protocols);
    }));
    websocket::response_type response;
    boost::system::error_code result = asio::error::timed_out;
    _ws.async_handshake(response, "127.0.0.1", path, [&](const boost::system::error_code& ec) { result = ec; });
    run();
    _ws.binary(true);
    return {result, std::string(response[http::field::sec_websocket_protocol])};
  }

  bool send_bytes(const mqtt::Bytes& bytes, bool binary = true) {
    _ws.binary(binary);
    boost::system::error_code result = asio::error::timed_out;
    _ws.async_write(asio::buffer(bytes), [&](const boost::system::error_code& ec, size_t) { result = ec; });
    run();
    _ws.binary(true);
    return !result;
  }
  bool send(const mqtt::Packet& packet, Version version = Version::v5) { return send_bytes(mqtt::encode(packet, version)); }

  // The next packet, from however many frames it takes; nothing when the
  // connection closed or nothing came within the bound.
  std::optional<mqtt::Packet> next(Version version = Version::v5) {
    for (;;) {
      if (const auto frame = mqtt::read_frame(_pending, UINT32_MAX); frame && _pending.size() >= frame->size()) {
        const mqtt::Bytes one(_pending.begin(), _pending.begin() + static_cast<ptrdiff_t>(frame->size()));
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<ptrdiff_t>(frame->size()));
        return mqtt::decode(one, version);
      }
      beast::flat_buffer buffer;
      boost::system::error_code result = asio::error::timed_out;
      _ws.async_read(buffer, [&](const boost::system::error_code& ec, size_t) { result = ec; });
      run();
      if (result) {
        _closed = result != asio::error::timed_out;
        return std::nullopt;
      }
      _binary_frames += _ws.got_binary() ? 1 : 0;
      const auto* data = static_cast<const uint8_t*>(buffer.data().data());
      _pending.insert(_pending.end(), data, data + buffer.size());
    }
  }

  // Whether the server closed the connection within the bound.
  bool closed_by_server() {
    if (_closed) return true;
    next();
    return _closed;
  }
  int binary_frames() const { return _binary_frames; }

 private:
  void run(std::chrono::milliseconds bound = 3000ms) {
    _io.restart();
    _io.run_for(bound);
    if (!_io.stopped()) {
      // Timed out: abandon the operation so the next can start.
      boost::system::error_code ignored;
      beast::get_lowest_layer(_ws).cancel(ignored);
      _io.restart();
      _io.run_for(500ms);
    }
  }

  asio::io_context _io;
  websocket::stream<Next> _ws;
  mqtt::Bytes _pending;
  bool _closed = false;
  int _binary_frames = 0;
};

using PlainClient = WsClient<tcp::socket>;

class WebSocketTest : public ::testing::Test {
 protected:
  WebSocketTest() : edge(io.context, config(), dannan::contract::steady_time_source(), allow, logger()) { edge.start(); }

  static EdgeConfig config() {
    EdgeConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    config.websocket = WebSocketConfig{"127.0.0.1", 0};
    return config;
  }
  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  std::unique_ptr<PlainClient> client() {
    auto out = std::make_unique<PlainClient>();
    out->ws().next_layer().connect({asio::ip::make_address("127.0.0.1"), edge.websocket_port()});
    return out;
  }
  size_t connections() {
    return edge.call<size_t>([](broker::Broker& b) { return b.connections(); });
  }

  test::Io io;
  contract::AllowAll allow;
  Edge edge;
};

TEST_F(WebSocketTest, ListensOnItsOwnPort) {
  EXPECT_NE(edge.websocket_port(), 0);
  EXPECT_NE(edge.websocket_port(), edge.port());
  EXPECT_EQ(edge.secure_websocket_port(), 0);
}

TEST_F(WebSocketTest, TheServerChoosesTheMqttSubprotocol) {
  auto c = client();
  const auto [ec, chosen] = c->handshake("chat, mqtt");
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(chosen, "mqtt");
  ASSERT_TRUE(c->send(mqtt::Connect{Version::v5, true, 0, "ws", {}, {}, {}, {}}));
  const auto ack = c->next();
  ASSERT_TRUE(ack);
  EXPECT_EQ(std::get<mqtt::Connack>(*ack).reason, mqtt::reason::success);
  EXPECT_EQ(c->binary_frames(), 1);
}

TEST_F(WebSocketTest, AThreeOneClientGetsItsSubprotocol) {
  auto c = client();
  const auto [ec, chosen] = c->handshake("mqttv3.1");
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(chosen, "mqttv3.1");
  ASSERT_TRUE(c->send(mqtt::Connect{Version::v3_1, true, 0, "old", {}, {}, {}, {}}, Version::v3_1));
  const auto ack = c->next(Version::v3_1);
  ASSERT_TRUE(ack);
  EXPECT_EQ(std::get<mqtt::Connack>(*ack).reason, 0);
}

TEST_F(WebSocketTest, AClientOfferingNoSubprotocolIsServed) {
  auto c = client();
  const auto [ec, chosen] = c->handshake("");
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(chosen, "");
  ASSERT_TRUE(c->send(mqtt::Connect{Version::v3_1_1, true, 0, "plain", {}, {}, {}, {}}, Version::v3_1_1));
  EXPECT_TRUE(c->next(Version::v3_1_1));
}

TEST_F(WebSocketTest, OnlyOtherSubprotocolsAreRefused) {
  auto c = client();
  const auto [ec, chosen] = c->handshake("chat, superchat");
  EXPECT_TRUE(ec);
  EXPECT_TRUE(test::eventually([&] { return connections() == 0; }, 2s));
}

TEST_F(WebSocketTest, APlainHttpRequestGets400) {
  tcp::socket socket(io.context);
  socket.connect({asio::ip::make_address("127.0.0.1"), edge.websocket_port()});
  http::request<http::empty_body> request(http::verb::get, "/mqtt", 11);
  request.set(http::field::host, "127.0.0.1");
  http::write(socket, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);
  EXPECT_EQ(response.result(), http::status::bad_request);
}

TEST_F(WebSocketTest, ATextFrameClosesTheConnection) {
  auto c = client();
  ASSERT_FALSE(c->handshake("mqtt").first);
  ASSERT_TRUE(c->send(mqtt::Connect{Version::v5, true, 0, "t", {}, {}, {}, {}}));
  ASSERT_TRUE(c->next());
  const auto ping = mqtt::encode(mqtt::Pingreq{}, Version::v5);
  c->send_bytes(ping, false);
  EXPECT_TRUE(c->closed_by_server());
  EXPECT_TRUE(test::eventually([&] { return connections() == 0; }, 2s));
}

TEST_F(WebSocketTest, PacketsMayBeSplitAcrossFramesOrShareOne) {
  auto c = client();
  ASSERT_FALSE(c->handshake("mqtt").first);
  const auto connect = mqtt::encode(mqtt::Connect{Version::v5, true, 0, "split", {}, {}, {}, {}}, Version::v5);
  const size_t half = connect.size() / 2;
  ASSERT_TRUE(c->send_bytes(mqtt::Bytes(connect.begin(), connect.begin() + static_cast<ptrdiff_t>(half))));
  ASSERT_TRUE(c->send_bytes(mqtt::Bytes(connect.begin() + static_cast<ptrdiff_t>(half), connect.end())));
  ASSERT_TRUE(c->next());
  auto two = mqtt::encode(mqtt::Pingreq{}, Version::v5);
  const auto again = two;
  two.insert(two.end(), again.begin(), again.end());
  ASSERT_TRUE(c->send_bytes(two));
  for (int i = 0; i < 2; ++i) {
    const auto pong = c->next();
    ASSERT_TRUE(pong);
    EXPECT_TRUE(std::holds_alternative<mqtt::Pingresp>(*pong));
  }
}

TEST_F(WebSocketTest, WebSocketAndTcpClientsShareTheBroker) {
  auto c = client();
  ASSERT_FALSE(c->handshake("mqtt").first);
  ASSERT_TRUE(c->send(mqtt::Connect{Version::v5, true, 0, "browser", {}, {}, {}, {}}));
  ASSERT_TRUE(c->next());
  ASSERT_TRUE(c->send(mqtt::Subscribe{1, {}, {{"room/+/temp", QoS::at_most_once}}}));
  const auto suback = c->next();
  ASSERT_TRUE(suback && std::holds_alternative<mqtt::Suback>(*suback));

  test::MqttClient tcp_client(edge.port());
  tcp_client.connect("sensor");
  tcp_client.publish("room/1/temp", "21.5", QoS::at_least_once);
  const auto got = c->next();
  ASSERT_TRUE(got);
  const auto& publish = std::get<mqtt::Publish>(*got);
  EXPECT_EQ(publish.topic, "room/1/temp");
  EXPECT_EQ(std::string(publish.payload.begin(), publish.payload.end()), "21.5");
}

TEST_F(WebSocketTest, StopClosesWebSocketClients) {
  auto c = client();
  ASSERT_FALSE(c->handshake("mqtt").first);
  ASSERT_TRUE(c->send(mqtt::Connect{Version::v5, true, 0, "w", {}, {}, {}, {}}));
  ASSERT_TRUE(c->next());
  edge.stop();
  EXPECT_TRUE(c->closed_by_server());
}

// wss: a self-signed certificate made for the test and a client that
// verifies the server against it.
class SecureWebSocketTest : public ::testing::Test {
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
    config.secure_websocket = TlsConfig{"127.0.0.1", 0, dir.path() / "cert.pem", dir.path() / "key.pem"};
    edge.emplace(io.context, config, dannan::contract::steady_time_source(), allow,
                 dannan::status::ScopedLogger{std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
    edge->start();
  }

  dannan::test::TempDir dir;
  test::Io io;
  contract::AllowAll allow;
  std::optional<Edge> edge;
};

TEST_F(SecureWebSocketTest, AVerifyingClientConnectsAndReceives) {
  ASSERT_NE(edge->secure_websocket_port(), 0);
  EXPECT_EQ(edge->websocket_port(), 0);
  asio::ssl::context context(asio::ssl::context::tls_client);
  context.load_verify_file((dir.path() / "cert.pem").string());
  WsClient<asio::ssl::stream<tcp::socket>> c(context);
  auto& tls = c.ws().next_layer();
  tls.set_verify_mode(asio::ssl::verify_peer);
  tls.set_verify_callback(asio::ssl::host_name_verification("127.0.0.1"));
  tls.lowest_layer().connect({asio::ip::make_address("127.0.0.1"), edge->secure_websocket_port()});
  tls.handshake(asio::ssl::stream_base::client);
  const auto [ec, chosen] = c.handshake("mqtt");
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(chosen, "mqtt");
  ASSERT_TRUE(c.send(mqtt::Connect{Version::v5, true, 0, "secure", {}, {}, {}, {}}));
  ASSERT_TRUE(c.next());
  ASSERT_TRUE(c.send(mqtt::Subscribe{1, {}, {{"secret/#", QoS::at_most_once}}}));
  ASSERT_TRUE(c.next());

  test::MqttClient plain(edge->port());
  plain.connect("plain");
  plain.publish("secret/door", "open", QoS::at_least_once);
  const auto got = c.next();
  ASSERT_TRUE(got);
  EXPECT_EQ(std::get<mqtt::Publish>(*got).topic, "secret/door");
}

TEST_F(SecureWebSocketTest, PlaintextUpgradeOnTheSecurePortGetsNothing) {
  PlainClient c;
  c.ws().next_layer().connect({asio::ip::make_address("127.0.0.1"), edge->secure_websocket_port()});
  EXPECT_TRUE(c.handshake("mqtt").first);
  EXPECT_TRUE(test::eventually([&] { return edge->call<size_t>([](broker::Broker& b) { return b.connections(); }) == 0; }, 2s));
}

}  // namespace
}  // namespace lir::edge
