//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "dannan/status/log.h"
#include "lir/edge/edge.h"

// The MQTT-SN 1.2 gateway (design, sections 5 and 7; the maintainer 2026-11-04): a UDP
// port where sensor-network clients connect, each a client of the broker
// like any other, so sessions, retained messages, the rules, the cluster
// and the authenticator apply as they do over TCP. The gateway translates:
// topic ids to names (registered per client; predefined ids are retained
// messages on $lir/mqttsn/topics/<id> whose payload is the name; short
// names travel as their two characters), the will (asked for, held here,
// and published by the gateway when the client is lost), and sleep (the
// broker's connection stays; the gateway holds what arrives while the
// client sleeps and delivers it when it wakes with PINGREQ). QoS -1 publishes
// from unconnected clients go in as an anonymous client's. Encapsulated
// packets from forwarders are answered through the same forwarder.
// Everything runs on the edge's strand.
namespace lir::sn {

inline constexpr std::string_view kPredefinedPrefix = "$lir/mqttsn/topics/";

struct GatewayConfig {
  std::string listen_host = "0.0.0.0";
  // Zero: any; port() says which.
  uint16_t port = 1884;
  uint8_t gateway_id = 1;
  // ADVERTISE broadcasts (the maintainer: configurable, off): to this address and
  // port, every `advertise_every`. Nothing: none; SEARCHGW is always
  // answered.
  std::optional<std::chrono::seconds> advertise_every;
  std::string advertise_address = "255.255.255.255";
  uint16_t advertise_port = 1884;
  // How often clients' keep alive and sleep are checked.
  std::chrono::milliseconds poll_every{500};
  // Messages held for one sleeping client; the oldest QoS 0 goes first.
  size_t maximum_held = 1000;
  // A woken client is put back to sleep after this, once what was held is
  // sent (section 6.14: it returns to sleep after PINGRESP).
  std::chrono::milliseconds awake_grace{2000};
};

struct GatewayStats {
  uint64_t datagrams_received = 0;
  uint64_t malformed = 0;
  uint64_t clients_connected = 0;
  uint64_t clients_lost = 0;
  uint64_t wills_published = 0;
  uint64_t held_dropped = 0;
  uint64_t qos_minus_one = 0;
};

class Gateway {
 public:
  // The edge outlives the gateway.
  Gateway(boost::asio::io_context& io, edge::Edge& edge, GatewayConfig config, dannan::status::ScopedLogger logger);
  ~Gateway();
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  // Binds and listens; throws boost::system::system_error when it cannot.
  void start();
  // Disconnects every client and closes the port. Never from an io thread.
  void stop();
  uint16_t port() const noexcept;
  // Waits on the strand.
  GatewayStats stats();

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::sn
