//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <stdexcept>
#include <utility>

#include "dannan/json/json.h"
#include "lir/contract/endpoints.h"

namespace lir::edge {

namespace {

dannan::listen::ListenerConfig listener_config(const ServerConfig& config, std::string name, uint16_t port) {
  dannan::listen::ListenerConfig out;
  out.name = std::move(name);
  out.host = config.address;
  out.port = port;
  out.close_bound = config.close_bound;
  // A slow reader is closed past the design's 4 MiB a client.
  out.outbound_bound = 4u << 20;
  return out;
}

// Refused before anything is opened on the node.
ServerConfig checked(ServerConfig config) {
  if (config.wss_port && !config.tls) throw std::invalid_argument("lir::edge::Server: wss needs TLS");
  return config;
}

bool wildcard(const std::string& address) {
  boost::system::error_code error;
  const auto parsed = boost::asio::ip::make_address(address, error);
  return !error && parsed.is_unspecified();
}

// "host:port", an IPv6 host in brackets.
std::string host_port(const std::string& host, uint16_t port) {
  const bool v6 = host.find(':') != std::string::npos;
  return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

}  // namespace

Server::Server(boost::asio::io_context& io, dannan::Context& context, contract::Authenticator& authenticator, ServerConfig config,
               const dannan::contract::TimeSource* time)
    : _io(io),
      _context(context),
      _config(checked(std::move(config))),
      _delivery(context.channels()),
      _views(context, _config.figures_every),
      _edge(_delivery, authenticator, context.time(), context.logger("lir.link"), _config.limits, &_views),
      _ticker(std::make_shared<Ticker>()) {
  _ticker->server = this;
  std::shared_ptr<dannan::listen::Certificates> certificates;
  if (_config.tls) certificates = std::make_shared<dannan::listen::Certificates>(*_config.tls);
  const auto add = [&](std::string transport, std::string name, uint16_t port, bool tls) {
    auto listener = listener_config(_config, std::move(name), port);
    if (tls) {
      listener.certificates = certificates;
      listener.tls.client_certificates = _config.client_certificates;
    }
    _listeners[std::move(transport)] = std::make_unique<dannan::listen::StreamListener>(io, std::move(listener), time);
  };
  add("tcp", "mqtt", _config.port, false);
  if (_config.tls) add("tls", "mqtts", _config.tls_port, true);
  if (_config.ws_port) add("ws", "mqtt-ws", *_config.ws_port, false);
  if (_config.wss_port) add("wss", "mqtt-wss", *_config.wss_port, true);
}

Server::~Server() {
  {
    std::lock_guard lock(_ticker->mutex);
    _ticker->stopped = true;
  }
  for (auto& [transport, listener] : _listeners) listener->stop();
}

void Server::start() {
  contract::Endpoints endpoints;
  const auto host = advertised_host();
  for (auto& [transport, listener] : _listeners) {
    const bool websocket = transport == "ws" || transport == "wss";
    listener->start(_io.get_executor(), websocket ? _edge.accepted_websocket() : _edge.accepted());
    endpoints[transport] = host_port(host, listener->port());
  }
  _started = true;
  const auto text = contract::endpoints_advert(endpoints);
  _context.nodes().advertise(std::string(contract::endpoints_advert_key), dannan::net::Bytes(text.begin(), text.end()));
  _context.nodes().advertise(std::string(contract::lir_advert), {});
  tick();
}

void Server::tick() {
  dannan::json::object limits;
  limits["maximum_packet_size"] = static_cast<uint64_t>(_config.limits.maximum_packet_size);
  limits["maximum_qos"] = static_cast<uint64_t>(0);
  limits["retain_available"] = false;
  limits["session_expiry"] = static_cast<uint64_t>(0);
  const auto published = _views.published_count();
  const auto seconds = std::chrono::duration<double>(_config.figures_every).count();
  dannan::json::object figures;
  uint64_t connections = 0;
  for (const auto& [transport, listener] : _listeners) connections += listener->counters().open;
  figures["connections"] = connections;
  figures["published_per_second"] = seconds > 0 ? static_cast<double>(published - _ticker->published) / seconds : 0.0;
  figures["limits"] = std::move(limits);
  _ticker->published = published;
  const auto bytes = dannan::json::to_bytes(figures);
  if (std::string text(bytes.begin(), bytes.end()); text != _ticker->last) {
    _ticker->last = std::move(text);
    _context.nodes().advertise(std::string(system::figures_advert), dannan::net::Bytes(bytes.begin(), bytes.end()));
    _views.own_figures(mqtt::Bytes(bytes.begin(), bytes.end()));
  }
  _context.time().after(_config.figures_every, [weak = std::weak_ptr<Ticker>(_ticker)] {
    auto ticker = weak.lock();
    if (!ticker) return;
    std::lock_guard lock(ticker->mutex);
    if (!ticker->stopped) ticker->server->tick();
  });
}

void Server::stop(boost::asio::any_io_executor executor, std::function<void()> done) {
  if (_stopped || !_started) {
    _stopped = true;
    boost::asio::post(executor, std::move(done));
    return;
  }
  _stopped = true;
  {
    std::lock_guard lock(_ticker->mutex);
    _ticker->stopped = true;
  }
  // Withdrawn first: no client or node is sent here once it is going.
  _context.nodes().withdraw(std::string(system::figures_advert));
  _views.own_figures(std::nullopt);
  _context.nodes().withdraw(std::string(contract::lir_advert));
  _context.nodes().withdraw(std::string(contract::endpoints_advert_key));
  for (auto& [transport, listener] : _listeners) listener->stop_accepting();
  // Each listener's clients closed, then done once all have gone.
  auto left = std::make_shared<size_t>(_listeners.size());
  auto finished = std::make_shared<std::function<void()>>(std::move(done));
  for (auto& [transport, listener] : _listeners) {
    listener->close_all(executor, [left, finished] {
      if (--*left == 0) (*finished)();
    });
  }
}

uint16_t Server::port_of(std::string_view transport) const noexcept {
  const auto found = _listeners.find(transport);
  return found == _listeners.end() ? 0 : found->second->port();
}

dannan::listen::StreamListener* Server::listener_of(std::string_view transport) noexcept {
  const auto found = _listeners.find(transport);
  return found == _listeners.end() ? nullptr : found->second.get();
}

std::string Server::advertised_host() const {
  if (!_config.advertise.empty()) return _config.advertise;
  if (wildcard(_config.address)) return _context.nodes().self().host;
  return _config.address;
}

}  // namespace lir::edge
