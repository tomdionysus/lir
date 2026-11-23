//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/write.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "dannan/listen/listener.h"
#include "dannan/listen/memory.h"
#include "dannan/test/fixtures/sim/world.h"
#include "lir/mqtt/packet.h"
#include "lir/test/fixtures/cluster.h"

// An MQTT client inside the simulation's world: the other end of a connection
// a node's listener serves in memory (StreamListener::inject), exactly as over
// TCP. Packets go whole; what arrives is framed and decoded in order.
namespace lir::test {

class StreamClient {
 public:
  StreamClient(dannan::test::sim::World& world, dannan::listen::StreamListener& listener, mqtt::Version version = mqtt::Version::v5,
               dannan::listen::MemoryOptions options = {})
      : _world(world), _version(version) {
    auto injected = listener.inject(world.io.get_executor(), options);
    _state->carrier = std::shared_ptr<dannan::listen::Carrier>(std::move(injected.client));
    link = injected.link;
    read(_state);
  }
  // Its end closed as it goes: a read still waiting (on a stalled link, say)
  // ends, and with it what the read held. Moved, not copied, so no copy
  // closes a connection another still uses.
  ~StreamClient() {
    if (!_state || !_state->carrier) return;
    boost::system::error_code ignored;
    _state->carrier->close(ignored);
  }
  StreamClient(const StreamClient&) = delete;
  StreamClient& operator=(const StreamClient&) = delete;
  StreamClient(StreamClient&&) = default;
  StreamClient& operator=(StreamClient&&) = delete;

  void send(const mqtt::Packet& packet) { raw(mqtt::encode(packet, _version)); }
  // Bytes as they are, MQTT or not.
  void raw(mqtt::Bytes data) {
    auto bytes = std::make_shared<mqtt::Bytes>(std::move(data));
    boost::asio::async_write(*_state->carrier, boost::asio::buffer(*bytes), [bytes, state = _state](boost::system::error_code, std::size_t) {});
  }

  // The next packet, running the world until one is whole; nothing once the
  // stream has ended with none.
  std::optional<mqtt::Packet> next() {
    std::optional<mqtt::Packet> out;
    _world.run_until([&] { return (out = take()).has_value() || _state->ended; }, max_steps);
    return out;
  }
  // The next packet of type T. A PUBLISH that comes first, as one may before
  // the SUBACK of its subscription (MQTT 5.0, 3.8.4), is kept for
  // publishes() rather than taken for it.
  template <class T>
  T expect() {
    for (;;) {
      auto packet = next();
      EXPECT_TRUE(packet.has_value());
      if (!packet) return T{};
      if constexpr (!std::is_same_v<T, mqtt::Publish>) {
        if (auto* early = std::get_if<mqtt::Publish>(&*packet)) {
          _early.push_back(std::move(*early));
          continue;
        }
      }
      const auto* out = std::get_if<T>(&*packet);
      EXPECT_NE(out, nullptr) << "packet type " << static_cast<int>(mqtt::type_of(*packet));
      return out ? *out : T{};
    }
  }
  // The server ended the stream.
  bool ends() {
    return _world.run_until([&] { return _state->ended; }, max_steps);
  }
  // Every PUBLISH that arrives while the world runs `steps` more.
  std::vector<mqtt::Publish> publishes(uint64_t steps = 200'000) {
    _world.run_until([] { return false; }, steps);
    std::vector<mqtt::Publish> out = std::exchange(_early, {});
    while (auto packet = take())
      if (auto* publish = std::get_if<mqtt::Publish>(&*packet)) out.push_back(*publish);
    return out;
  }

  mqtt::Connack connect(mqtt::Connect connect = {}) {
    connect.version = _version;
    send(connect);
    return expect<mqtt::Connack>();
  }
  mqtt::Suback subscribe(mqtt::Subscription subscription, uint16_t packet_id = 1) {
    mqtt::Subscribe subscribe;
    subscribe.packet_id = packet_id;
    subscribe.subscriptions.push_back(std::move(subscription));
    send(subscribe);
    return expect<mqtt::Suback>();
  }
  mqtt::Suback subscribe(std::string filter) { return subscribe(mqtt::Subscription{std::move(filter)}); }

  std::shared_ptr<dannan::listen::MemoryLink> link;

 private:
  // Held by the read and write in flight, which may outlive the client.
  struct State {
    std::shared_ptr<dannan::listen::Carrier> carrier;
    std::array<uint8_t, 4096> buffer{};
    mqtt::Bytes input;
    bool ended = false;
  };
  static void read(std::shared_ptr<State> state) {
    state->carrier->async_read_some(boost::asio::buffer(state->buffer), [state](boost::system::error_code error, std::size_t bytes) {
      state->input.insert(state->input.end(), state->buffer.begin(), state->buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
      if (error) {
        state->ended = true;
        return;
      }
      read(state);
    });
  }
  // A whole packet already arrived, if there is one.
  std::optional<mqtt::Packet> take() {
    auto& input = _state->input;
    const auto frame = mqtt::read_frame(input, 1u << 20);
    if (!frame || input.size() < frame->size()) return std::nullopt;
    auto out = mqtt::decode(std::span<const uint8_t>(input.data(), frame->size()), _version);
    input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(frame->size()));
    return out;
  }

  dannan::test::sim::World& _world;
  mqtt::Version _version;
  std::shared_ptr<State> _state = std::make_shared<State>();
  // PUBLISHes that came before the packet expect() waited for.
  std::vector<mqtt::Publish> _early;
};

}  // namespace lir::test
