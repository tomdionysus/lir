//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <memory>
#include <string_view>

#include "dannan/listen/stream.h"
#include "lir/broker/link.h"

// MQTT over WebSocket (MQTT 5.0 and 3.1.1, section 6): Boost.Beast's WebSocket
// over a stream Dannan's listener accepted (dannan/listen/beast.h), plaintext
// (ws) or TLS (wss). Packets travel in binary messages, any number of them or
// parts of one to a message [MQTT-6.0.0-2]; anything else ends the
// connection [MQTT-6.0.0-1]. The server selects "mqtt", or "mqttv3.1" for an
// old client, from the subprotocols offered [MQTT-6.0.0-4]; a handshake that
// offers neither is refused.
namespace lir::edge {

inline constexpr std::string_view mqtt_subprotocol = "mqtt";
inline constexpr std::string_view mqtt31_subprotocol = "mqttv3.1";

// Takes the WebSocket handshake on `stream`, then makes its link with
// `make` (given the connection to answer through) and serves it. On the
// stream's strand, which becomes the link's.
using MakeLink = std::function<std::shared_ptr<broker::Link>(std::shared_ptr<contract::Connection>)>;
void serve_websocket(std::shared_ptr<dannan::listen::Stream> stream, MakeLink make);

}  // namespace lir::edge
