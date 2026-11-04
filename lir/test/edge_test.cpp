//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <thread>
#include <vector>

#include "lir/mqtt/packet.h"

namespace lir::edge {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using mqtt::QoS;
using mqtt::Version;

struct Io {
  asio::io_context context;
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  Io() {
    for (int i = 0; i < 4; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~Io() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
};

// A blocking MQTT client over a real socket; every read gives up after two
// seconds, so a test that waits wrongly fails rather than hangs.
class TcpClient {
 public:
  TcpClient(uint16_t port, Version version = Version::v5, int receive_buffer = 0) : _version(version) {
    _socket.open(tcp::v4());
    if (receive_buffer > 0) ::setsockopt(_socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    timeval timeout{2, 0};
    ::setsockopt(_socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    _socket.connect({asio::ip::make_address("127.0.0.1"), port});
  }

  void send(const mqtt::Packet& packet) { asio::write(_socket, asio::buffer(mqtt::encode(packet, _version))); }

  void connect(const std::string& id, std::optional<mqtt::Will> will = {}) {
    send(mqtt::Connect{_version, true, 0, id, std::move(will), {}, {}, {}});
    const auto ack = receive();
    ASSERT_TRUE(ack && std::holds_alternative<mqtt::Connack>(*ack));
    ASSERT_EQ(std::get<mqtt::Connack>(*ack).reason, mqtt::reason::success);
  }

  // The next packet, or nothing on close or timeout.
  std::optional<mqtt::Packet> receive() {
    for (;;) {
      if (const auto frame = mqtt::read_frame(_buffer, UINT32_MAX); frame && _buffer.size() >= frame->size()) {
        auto packet = mqtt::decode(std::span<const uint8_t>(_buffer).first(frame->size()), _version);
        _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(frame->size()));
        return packet;
      }
      if (!fill()) return std::nullopt;
    }
  }

  template <typename T>
  std::optional<T> receive_a() {
    auto packet = receive();
    if (!packet || !std::holds_alternative<T>(*packet)) return std::nullopt;
    return std::get<T>(std::move(*packet));
  }

  // True once the server has closed the socket (as opposed to a timeout).
  bool closed_by_server() {
    for (;;) {
      std::array<uint8_t, 65536> chunk{};
      boost::system::error_code ec;
      _socket.read_some(asio::buffer(chunk), ec);
      if (ec == asio::error::eof || ec == asio::error::connection_reset) return true;
      if (ec) return false;
    }
  }

  void abandon() {
    boost::system::error_code ignored;
    _socket.close(ignored);
  }

 private:
  bool fill() {
    std::array<uint8_t, 65536> chunk{};
    boost::system::error_code ec;
    const size_t got = _socket.read_some(asio::buffer(chunk), ec);
    if (ec) return false;
    _buffer.insert(_buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    return true;
  }

  asio::io_context _io;
  tcp::socket _socket{_io};
  Version _version;
  mqtt::Bytes _buffer;
};

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

  // Waits until the broker shows `wanted` connections, or two seconds.
  bool connections_reach(size_t wanted) {
    for (int i = 0; i < 200; ++i) {
      if (edge.call<size_t>([](broker::Broker& b) { return b.connections(); }) == wanted) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  Io io;
  contract::AllowAll allow;
  Edge edge;
};

mqtt::Publish publish(const std::string& topic, const std::string& text, QoS qos = QoS::at_most_once, uint16_t id = 0) {
  mqtt::Publish p;
  p.topic = topic;
  p.payload = mqtt::Bytes(text.begin(), text.end());
  p.qos = qos;
  p.packet_id = id;
  return p;
}

TEST_F(EdgeTest, ListensOnTheBoundPort) { EXPECT_NE(edge.port(), 0); }

TEST_F(EdgeTest, ConnectAndPing) {
  TcpClient client(edge.port());
  client.connect("a");
  client.send(mqtt::Pingreq{});
  EXPECT_TRUE(client.receive_a<mqtt::Pingresp>());
}

TEST_F(EdgeTest, PublishReachesAnotherSocket) {
  TcpClient sub(edge.port(), Version::v3_1_1);
  TcpClient pub(edge.port());
  sub.connect("sub");
  pub.connect("pub");
  sub.send(mqtt::Subscribe{1, {}, {{"room/+/temp", QoS::at_least_once}}});
  ASSERT_TRUE(sub.receive_a<mqtt::Suback>());
  pub.send(publish("room/1/temp", "21.5", QoS::at_least_once, 3));
  const auto ack = pub.receive_a<mqtt::Ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->packet_id, 3);
  const auto got = sub.receive_a<mqtt::Publish>();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "room/1/temp");
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "21.5");
}

TEST_F(EdgeTest, ProtocolErrorClosesTheSocket) {
  TcpClient client(edge.port());
  client.connect("a");
  client.send(mqtt::Connect{Version::v5, true, 0, "a", {}, {}, {}, {}});
  const auto disconnect = client.receive_a<mqtt::Disconnect>();
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, mqtt::reason::protocol_error);
  EXPECT_TRUE(client.closed_by_server());
  EXPECT_TRUE(connections_reach(0));
}

TEST_F(EdgeTest, DroppedSocketPublishesTheWill) {
  TcpClient watcher(edge.port());
  watcher.connect("watcher");
  watcher.send(mqtt::Subscribe{1, {}, {{"wills/#", QoS::at_most_once}}});
  ASSERT_TRUE(watcher.receive_a<mqtt::Suback>());
  {
    TcpClient dying(edge.port());
    mqtt::Will will;
    will.topic = "wills/dying";
    will.payload = {'x'};
    dying.connect("dying", will);
    dying.abandon();
  }
  const auto got = watcher.receive_a<mqtt::Publish>();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "wills/dying");
}

TEST_F(EdgeTest, StopClosesEveryClient) {
  TcpClient client(edge.port());
  client.connect("a");
  edge.stop();
  EXPECT_TRUE(client.closed_by_server());
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
  TcpClient slow(edge.port(), Version::v5, 4096);
  slow.connect("slow");
  slow.send(mqtt::Subscribe{1, {}, {{"flood", QoS::at_most_once}}});
  ASSERT_TRUE(slow.receive_a<mqtt::Suback>());
  TcpClient pub(edge.port());
  pub.connect("pub");
  const std::string big(60000, 'x');
  for (int i = 0; i < 400; ++i) pub.send(publish("flood", big));
  EXPECT_TRUE(connections_reach(1));
  // The publisher was never held up: it still gets its answers.
  pub.send(mqtt::Pingreq{});
  EXPECT_TRUE(pub.receive_a<mqtt::Pingresp>());
}

}  // namespace
}  // namespace lir::edge
