//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/websocket.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <deque>
#include <span>
#include <string>
#include <utility>

#include "dannan/listen/beast.h"
#include "lir/edge/edge.h"

namespace lir::edge {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;

namespace {

// The subprotocol to answer with, from the comma-separated list offered;
// empty for neither of MQTT's.
std::string choose(std::string_view offered) {
  bool mqtt31 = false;
  while (!offered.empty()) {
    const auto comma = offered.find(',');
    auto item = offered.substr(0, comma);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
    if (item == mqtt_subprotocol) return std::string(mqtt_subprotocol);
    if (item == mqtt31_subprotocol) mqtt31 = true;
    if (comma == std::string_view::npos) break;
    offered.remove_prefix(comma + 1);
  }
  return mqtt31 ? std::string(mqtt31_subprotocol) : std::string();
}

// One WebSocket client: the handshake, then its link fed each binary
// message and answering through it. Everything on the stream's strand.
class Session final : public contract::Connection, public std::enable_shared_from_this<Session> {
 public:
  explicit Session(std::shared_ptr<dannan::listen::Stream> stream) : _stream(std::move(stream)), _ws(*_stream), _peer(endpoint_string(_stream->peer())) {}

  void start(MakeLink make) {
    _make = std::move(make);
    http::async_read(*_stream, _buffer, _request, [self = shared_from_this()](beast::error_code error, std::size_t) { self->on_request(error); });
  }

  void send(mqtt::Bytes bytes) override {
    if (_closing) return;
    _outbound.push_back(std::move(bytes));
    if (!_writing) write_next();
  }
  void close() override {
    if (_closing) return;
    _closing = true;
    if (!_writing) shut();
  }
  std::string peer() const override { return _peer; }
  std::optional<contract::ClientCertificate> certificate() const override {
    const auto& presented = _stream->client_certificate();
    if (!presented || !presented->verified) return std::nullopt;
    return contract::ClientCertificate{presented->subject, presented->thumbprint};
  }
  std::string transport() const override { return _stream->is_tls() ? "wss" : "ws"; }

 private:
  void on_request(beast::error_code error) {
    if (error || !websocket::is_upgrade(_request)) return _stream->close();
    const auto chosen = choose(_request[http::field::sec_websocket_protocol]);
    if (chosen.empty()) return _stream->close();
    _ws.binary(true);
    _ws.set_option(
        websocket::stream_base::decorator([chosen](websocket::response_type& response) { response.set(http::field::sec_websocket_protocol, chosen); }));
    _ws.async_accept(_request, [self = shared_from_this()](beast::error_code accepted) { self->on_accept(accepted); });
  }

  void on_accept(beast::error_code error) {
    if (error) return _stream->close();
    _link = std::exchange(_make, {})(shared_from_this());
    read();
  }

  void read() {
    _ws.async_read(_message, [self = shared_from_this()](beast::error_code error, std::size_t) { self->on_read(error); });
  }

  void on_read(beast::error_code error) {
    // The client went, or sent other than a binary message [MQTT-6.0.0-1].
    if (error || !_ws.got_binary()) return end();
    const auto data = _message.cdata();
    _link->receive(std::span<const uint8_t>(static_cast<const uint8_t*>(data.data()), data.size()));
    _message.consume(_message.size());
    if (_closing) {
      // The link closed it: it holds nothing more of this connection.
      _link.reset();
      return;
    }
    read();
  }

  // The link drops what it holds; then the connection, once.
  void end() {
    if (_link) _link->closed();
    _link.reset();
    close();
  }

  void write_next() {
    if (_outbound.empty()) {
      _writing = false;
      if (_closing) shut();
      return;
    }
    _writing = true;
    auto bytes = std::make_shared<mqtt::Bytes>(std::move(_outbound.front()));
    _outbound.pop_front();
    _ws.async_write(boost::asio::buffer(*bytes), [self = shared_from_this(), bytes](beast::error_code error, std::size_t) {
      if (error) {
        self->_outbound.clear();
        self->_writing = false;
        self->_closing = true;
        self->_stream->close();
        return;
      }
      self->write_next();
    });
  }

  // The closing handshake, then the stream (within its close bound).
  void shut() {
    if (_shut) return;
    _shut = true;
    if (!_ws.is_open()) return _stream->close();
    _ws.async_close(websocket::close_code::normal, [self = shared_from_this()](beast::error_code) { self->_stream->close(); });
  }

  std::shared_ptr<dannan::listen::Stream> _stream;
  websocket::stream<dannan::listen::Stream&> _ws;
  const std::string _peer;
  MakeLink _make;
  std::shared_ptr<broker::Link> _link;
  beast::flat_buffer _buffer;
  http::request<http::string_body> _request;
  beast::flat_buffer _message;
  std::deque<mqtt::Bytes> _outbound;
  bool _writing = false;
  bool _closing = false;
  bool _shut = false;
};

}  // namespace

void serve_websocket(std::shared_ptr<dannan::listen::Stream> stream, MakeLink make) { std::make_shared<Session>(std::move(stream))->start(std::move(make)); }

}  // namespace lir::edge
