//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/status/log.h"
#include "lir/auth/scopes.h"
#include "lir/edge/edge.h"

// HTTP publish and Server-Sent Events subscribe on the same bus, for
// browsers and curl (design, "In scope"; HTTP is Lir's own, the maintainer
// 2026-11-04). Each request is an MQTT 5.0 client of the broker for its
// length, fed packets in process, so the authenticator, retained messages,
// No Local and the cluster apply exactly as they do to any client:
//
//   POST /publish?topic=T[&qos=0|1|2][&retain=1]   body: the payload
//     200 {"accepted":true,"matched":true|false}; 400 for a bad topic or
//     QoS; 401/403 when the authenticator refuses.
//   GET /subscribe?filter=F[&filter=G...]          text/event-stream
//     each message as "event: message" with data
//     {"topic","qos","retain","payload"} (UTF-8 text) or "payload_hex";
//     retained messages first; a comment every `keepalive`.
//
// "Authorization: Bearer <token>" is passed as the CONNECT password. In
// the query, only %XX is decoded: '+' is MQTT's wildcard, never a space.
// Requests run on the edge's strand; one request per connection.
namespace lir::http {

struct HttpConfig {
  std::string listen_host = "0.0.0.0";
  // Zero: any; port() says which.
  uint16_t port = 8083;
  size_t maximum_body = 1u << 20;
  std::chrono::milliseconds keepalive{15000};
  // A request not complete within this is closed.
  std::chrono::milliseconds read_timeout{30000};
  // Bytes queued for one event stream before it is closed as too slow.
  size_t maximum_outbound_bytes = 4u << 20;
  // HTTPS on its own port, beside the plain one. Nothing: none.
  std::optional<edge::TlsConfig> tls;
};

// The path and its query parameters, %XX-decoded. Throws
// std::invalid_argument for a malformed escape.
struct Target {
  std::string path;
  std::map<std::string, std::vector<std::string>> query;
};
Target parse_target(std::string_view target);
// One decoder for the query and the scope grammar (auth/scopes.h).
using auth::percent_decode;

// One SSE event for a delivered message.
std::string sse_event(const mqtt::Publish& publish);

class HttpServer {
 public:
  // The edge outlives the server.
  HttpServer(boost::asio::io_context& io, edge::Edge& edge, HttpConfig config, dannan::status::ScopedLogger logger);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Binds and listens; throws boost::system::system_error when it cannot.
  void start();
  // Closes the listener and every stream. Never from an io thread.
  void stop();
  uint16_t port() const noexcept;
  // Zero without an HTTPS listener.
  uint16_t tls_port() const noexcept;

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::http
