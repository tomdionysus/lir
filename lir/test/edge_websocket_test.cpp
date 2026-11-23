//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "dannan/listen/beast.h"
#include "dannan/listen/connector.h"
#include "lir/client/roster.h"
#include "lir/contract/endpoints.h"
#include "lir/edge/websocket.h"
#include "lir/test/fixtures/servers.h"
#include "lir/test/fixtures/stream_client.h"

// MQTT over WebSocket (section 6), served by every node beside TCP: a Beast
// client inside the simulation's world, over an in-memory connection a
// node's ws listener serves as it would one over TCP.
namespace lir::edge {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace listen = dannan::listen;
using test::max_steps;

struct Cluster : test::Servers {
  explicit Cluster(size_t size) : Servers(size, 1, [](ServerConfig& config) { config.ws_port = 0; }) {}
};

// One WebSocket client: the handshake offering `subprotocols`, then MQTT
// packets each sent as one binary message unless the test says otherwise.
// What its reads and writes in flight use is shared with them, so the world
// may run on after the client has gone.
class WsClient {
 public:
  WsClient(Cluster& cluster, size_t node, std::string subprotocols = "mqtt") : _world(cluster.world), _in(std::make_shared<Inner>(cluster)) {
    auto injected = cluster.servers.at(node)->listener_of("ws")->inject(cluster.world.io.get_executor());
    _in->connector.adopt(cluster.world.io.get_executor(), std::move(injected.client), std::nullopt, std::chrono::seconds(10),
                         [in = _in](std::shared_ptr<listen::Stream> stream, std::string) { in->stream = std::move(stream); });
    _world.run_until([&] { return _in->stream != nullptr; }, max_steps);
    if (!_in->stream) return;
    _in->ws = std::make_unique<websocket::stream<listen::Stream&>>(*_in->stream);
    _in->ws->binary(true);
    _in->ws->set_option(websocket::stream_base::decorator([subprotocols](websocket::request_type& request) {
      if (!subprotocols.empty()) request.set(http::field::sec_websocket_protocol, subprotocols);
    }));
    _in->ws->async_handshake(_in->response, "localhost", "/mqtt", [in = _in](beast::error_code error) {
      in->handshaken = !error;
      in->answered = true;
    });
    _world.run_until([&] { return _in->answered; }, max_steps);
    if (_in->handshaken) read(_in);
  }

  bool handshaken() const { return _in->handshaken; }
  std::string subprotocol() const { return std::string(_in->response[http::field::sec_websocket_protocol]); }

  // `bytes` as one message, binary or text.
  void message(mqtt::Bytes bytes, bool binary = true) {
    _in->outbound.push_back({std::move(bytes), binary});
    if (!_in->writing) write_next(_in);
  }
  void send(const mqtt::Packet& packet) { message(mqtt::encode(packet, mqtt::Version::v5)); }

  std::optional<mqtt::Packet> next() {
    std::optional<mqtt::Packet> out;
    _world.run_until(
        [&] {
          auto& input = _in->input;
          const auto frame = mqtt::read_frame(input, 1u << 20);
          if (frame && input.size() >= frame->size()) {
            out = mqtt::decode(std::span<const uint8_t>(input.data(), frame->size()), mqtt::Version::v5);
            input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(frame->size()));
            return true;
          }
          return _in->ended;
        },
        max_steps);
    return out;
  }
  template <class T>
  T expect() {
    auto packet = next();
    EXPECT_TRUE(packet.has_value());
    const auto* out = packet ? std::get_if<T>(&*packet) : nullptr;
    EXPECT_NE(out, nullptr);
    return out ? *out : T{};
  }
  bool ends() {
    return _world.run_until([&] { return _in->ended; }, max_steps);
  }

 private:
  struct Inner {
    explicit Inner(Cluster& cluster) : connector(cluster.world.io.get_executor(), listen::ConnectorConfig{}, &cluster.time) {}
    listen::StreamConnector connector;
    std::shared_ptr<listen::Stream> stream;
    std::unique_ptr<websocket::stream<listen::Stream&>> ws;
    websocket::response_type response;
    beast::flat_buffer buffer;
    mqtt::Bytes input;
    std::deque<std::pair<mqtt::Bytes, bool>> outbound;
    bool writing = false;
    bool answered = false;
    bool handshaken = false;
    bool ended = false;
  };

  static void read(std::shared_ptr<Inner> in) {
    in->ws->async_read(in->buffer, [in](beast::error_code error, std::size_t) {
      if (error) {
        in->ended = true;
        return;
      }
      const auto data = in->buffer.cdata();
      const auto* bytes = static_cast<const uint8_t*>(data.data());
      in->input.insert(in->input.end(), bytes, bytes + data.size());
      in->buffer.consume(in->buffer.size());
      read(in);
    });
  }
  static void write_next(std::shared_ptr<Inner> in) {
    if (in->outbound.empty()) {
      in->writing = false;
      return;
    }
    in->writing = true;
    auto next = std::make_shared<std::pair<mqtt::Bytes, bool>>(std::move(in->outbound.front()));
    in->outbound.pop_front();
    in->ws->binary(next->second);
    in->ws->async_write(boost::asio::buffer(next->first), [in, next](beast::error_code, std::size_t) { write_next(in); });
  }

  dannan::test::sim::World& _world;
  std::shared_ptr<Inner> _in;
};

// Each node says where it serves WebSocket, beside TCP.
TEST(EdgeWebSocket, EveryNodeAdvertisesItsWebSocketEndpoint) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  const auto ports = std::vector<uint16_t>{cluster.servers[0]->port_of("ws"), cluster.servers[1]->port_of("ws")};
  EXPECT_TRUE(cluster.world.run_until(
      [&] {
        const auto found = client::lir_addresses(cluster.nodes.node(0).context().nodes().active(), "ws");
        return found.size() == 2 && ((found[0].port == ports[0] && found[1].port == ports[1]) || (found[0].port == ports[1] && found[1].port == ports[0]));
      },
      max_steps));
}

// The server selects "mqtt" [MQTT-6.0.0-4], and serves MQTT across the
// cluster over it: a client on WebSocket hears a publish from one on another
// node.
TEST(EdgeWebSocket, MQTT_6_0_0_4) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  WsClient subscriber(cluster, 0, "chat, mqtt");
  ASSERT_TRUE(subscriber.handshaken());
  EXPECT_EQ(subscriber.subprotocol(), "mqtt");
  subscriber.send(mqtt::Connect{});
  EXPECT_EQ(subscriber.expect<mqtt::Connack>().reason, mqtt::reason::success);
  mqtt::Subscribe subscribe;
  subscribe.packet_id = 1;
  subscribe.subscriptions.push_back({"sensors/#"});
  subscriber.send(subscribe);
  ASSERT_EQ(subscriber.expect<mqtt::Suback>().reasons, std::vector<uint8_t>{mqtt::reason::granted_qos_0});
  WsClient publisher(cluster, 1);
  ASSERT_TRUE(publisher.handshaken());
  publisher.send(mqtt::Connect{});
  publisher.expect<mqtt::Connack>();
  publisher.send(test::publish_of("sensors/kitchen", "21"));
  EXPECT_EQ(subscriber.expect<mqtt::Publish>().topic, "sensors/kitchen");
}

// An old client offering only "mqttv3.1" is answered with it.
TEST(EdgeWebSocket, AClientOfferingMqttv31IsAnsweredWithIt) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  WsClient client(cluster, 0, "mqttv3.1");
  ASSERT_TRUE(client.handshaken());
  EXPECT_EQ(client.subprotocol(), "mqttv3.1");
}

// A handshake that offers no MQTT subprotocol is refused.
TEST(EdgeWebSocket, AHandshakeWithoutMqttIsRefused) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  EXPECT_FALSE(WsClient(cluster, 0, "chat").handshaken());
  EXPECT_FALSE(WsClient(cluster, 0, "").handshaken());
}

// A message may carry part of a packet, or several [MQTT-6.0.0-2].
TEST(EdgeWebSocket, MQTT_6_0_0_2) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  WsClient client(cluster, 0);
  ASSERT_TRUE(client.handshaken());
  const auto connect = mqtt::encode(mqtt::Connect{}, mqtt::Version::v5);
  client.message(mqtt::Bytes(connect.begin(), connect.begin() + 3));
  client.message(mqtt::Bytes(connect.begin() + 3, connect.end()));
  EXPECT_EQ(client.expect<mqtt::Connack>().reason, mqtt::reason::success);
  auto two = mqtt::encode(mqtt::Pingreq{}, mqtt::Version::v5);
  const auto again = mqtt::encode(mqtt::Pingreq{}, mqtt::Version::v5);
  two.insert(two.end(), again.begin(), again.end());
  client.message(two);
  client.expect<mqtt::Pingresp>();
  client.expect<mqtt::Pingresp>();
}

// A text message ends the connection [MQTT-6.0.0-1].
TEST(EdgeWebSocket, MQTT_6_0_0_1) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  WsClient client(cluster, 0);
  ASSERT_TRUE(client.handshaken());
  client.send(mqtt::Connect{});
  client.expect<mqtt::Connack>();
  client.message(mqtt::encode(mqtt::Pingreq{}, mqtt::Version::v5), false);
  EXPECT_TRUE(client.ends());
}

// A protocol error over WebSocket ends the connection as over TCP: the link
// closes it, and its WebSocket goes with it.
TEST(EdgeWebSocket, AProtocolErrorEndsTheConnection) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  WsClient client(cluster, 0);
  ASSERT_TRUE(client.handshaken());
  client.send(mqtt::Pingreq{});
  EXPECT_TRUE(client.ends());
}

// A plain HTTP request on the WebSocket port, asking no upgrade, is closed.
TEST(EdgeWebSocket, ARequestWithoutAnUpgradeIsClosed) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  test::StreamClient plain(cluster.world, *cluster.servers[0]->listener_of("ws"));
  const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  plain.raw(mqtt::Bytes(request.begin(), request.end()));
  EXPECT_TRUE(plain.ends());
}

}  // namespace
}  // namespace lir::edge
