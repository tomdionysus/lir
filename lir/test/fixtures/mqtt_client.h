//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lir/mqtt/packet.h"

// Fixtures for tests that speak MQTT over real sockets: an io context with
// its own threads, a blocking client whose every read gives up after a
// bound (so a test that waits wrongly fails rather than hangs), and a wait
// for a condition.
namespace lir::test {

struct Io {
  boost::asio::io_context context;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{context.get_executor()};
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

inline bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds bound = std::chrono::seconds(10)) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return condition();
}

class MqttClient {
 public:
  using tcp = boost::asio::ip::tcp;

  MqttClient(uint16_t port, mqtt::Version version = mqtt::Version::v5, int receive_buffer = 0, int timeout_seconds = 2)
      : _version(version), _timeout_ms(timeout_seconds * 1000) {
    _socket.open(tcp::v4());
    if (receive_buffer > 0) ::setsockopt(_socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    _socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
  }

  void send(const mqtt::Packet& packet) { boost::asio::write(_socket, boost::asio::buffer(mqtt::encode(packet, _version))); }

  // CONNECT, and the CONNACK it must get.
  mqtt::Connack connect(const std::string& id, bool clean = true, std::optional<mqtt::Will> will = {}, mqtt::Properties properties = {}) {
    send(mqtt::Connect{_version, clean, 0, id, std::move(will), {}, {}, std::move(properties)});
    auto ack = receive_a<mqtt::Connack>();
    EXPECT_TRUE(ack);
    EXPECT_EQ(ack ? ack->reason : 0xFF, mqtt::reason::success);
    return ack.value_or(mqtt::Connack{});
  }

  void subscribe(const std::string& filter, mqtt::QoS qos = mqtt::QoS::at_least_once) {
    send(mqtt::Subscribe{_next_id++, {}, {{filter, qos}}});
    EXPECT_TRUE(receive_a<mqtt::Suback>());
  }

  void publish(const std::string& topic, const std::string& text, mqtt::QoS qos = mqtt::QoS::at_most_once, bool retain = false) {
    mqtt::Publish p;
    p.topic = topic;
    p.payload = mqtt::Bytes(text.begin(), text.end());
    p.qos = qos;
    p.retain = retain;
    p.packet_id = qos == mqtt::QoS::at_most_once ? 0 : _next_id++;
    send(p);
    if (qos == mqtt::QoS::at_least_once) {
      EXPECT_TRUE(receive_a<mqtt::Ack>());
    }
    if (qos == mqtt::QoS::exactly_once) {
      EXPECT_TRUE(receive_a<mqtt::Ack>());
      send(mqtt::Ack{mqtt::PacketType::pubrel, p.packet_id, mqtt::reason::success, {}});
      EXPECT_TRUE(receive_a<mqtt::Ack>());
    }
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

  // The next PUBLISH, acknowledged as its QoS asks.
  std::optional<mqtt::Publish> next_message() {
    auto p = receive_a<mqtt::Publish>();
    if (!p) return p;
    if (p->qos == mqtt::QoS::at_least_once) send(mqtt::Ack{mqtt::PacketType::puback, p->packet_id, mqtt::reason::success, {}});
    if (p->qos == mqtt::QoS::exactly_once) {
      send(mqtt::Ack{mqtt::PacketType::pubrec, p->packet_id, mqtt::reason::success, {}});
      if (!receive_a<mqtt::Ack>()) return std::nullopt;
      send(mqtt::Ack{mqtt::PacketType::pubcomp, p->packet_id, mqtt::reason::success, {}});
    }
    return p;
  }

  // True once the server has closed the socket (as opposed to a timeout).
  bool closed_by_server() {
    for (;;) {
      if (!readable()) return false;
      std::array<uint8_t, 65536> chunk{};
      boost::system::error_code ec;
      _socket.read_some(boost::asio::buffer(chunk), ec);
      if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) return true;
      if (ec) return false;
    }
  }

  void abandon() {
    boost::system::error_code ignored;
    _socket.close(ignored);
  }

 private:
  // Asio's blocking read retries a timed-out read for ever, so the bound is
  // a poll before each one.
  bool readable() {
    pollfd ready{_socket.native_handle(), POLLIN, 0};
    return ::poll(&ready, 1, _timeout_ms) > 0;
  }

  bool fill() {
    if (!readable()) return false;
    std::array<uint8_t, 65536> chunk{};
    boost::system::error_code ec;
    const size_t got = _socket.read_some(boost::asio::buffer(chunk), ec);
    if (ec) return false;
    _buffer.insert(_buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    return true;
  }

  boost::asio::io_context _io;
  tcp::socket _socket{_io};
  mqtt::Version _version;
  int _timeout_ms;
  mqtt::Bytes _buffer;
  uint16_t _next_id = 1;
};

}  // namespace lir::test
