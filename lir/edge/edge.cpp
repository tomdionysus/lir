//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <array>
#include <atomic>
#include <boost/asio/buffer.hpp>
#include <span>
#include <utility>

#include "lir/edge/websocket.h"

namespace lir::edge {

namespace {

// Reads a stream into its link until either ends. Runs on the stream's
// strand, which is the link's.
class Reader : public std::enable_shared_from_this<Reader> {
 public:
  Reader(std::shared_ptr<dannan::listen::Stream> stream, std::shared_ptr<contract::Connection> connection, std::shared_ptr<broker::Link> link)
      : _stream(std::move(stream)), _connection(std::move(connection)), _link(std::move(link)) {}

  void read() {
    _stream->async_read_some(boost::asio::buffer(_buffer), [self = shared_from_this()](boost::system::error_code error, std::size_t bytes) {
      if (bytes > 0) self->_link->receive(std::span<const uint8_t>(self->_buffer.data(), bytes));
      if (error) {
        // The peer went, the link closed it, or the listener did: either way
        // the link drops what it holds, and the stream is closed once.
        self->_link->closed();
        self->_connection->close();
        return;
      }
      self->read();
    });
  }

 private:
  std::shared_ptr<dannan::listen::Stream> _stream;
  std::shared_ptr<contract::Connection> _connection;
  std::shared_ptr<broker::Link> _link;
  std::array<uint8_t, 16 * 1024> _buffer{};
};

}  // namespace

std::string endpoint_string(const boost::asio::ip::tcp::endpoint& endpoint) {
  const auto address = endpoint.address();
  const auto host = address.is_v6() ? "[" + address.to_string() + "]" : address.to_string();
  return host + ":" + std::to_string(endpoint.port());
}

StreamConnection::StreamConnection(std::shared_ptr<dannan::listen::Stream> stream) : _stream(std::move(stream)), _peer(endpoint_string(_stream->peer())) {}

void StreamConnection::send(mqtt::Bytes bytes) { _stream->send(std::string(bytes.begin(), bytes.end())); }

void StreamConnection::close() {
  // The contract's close is idempotent; the stream's is once.
  if (!_closed.exchange(true)) _stream->close();
}

std::string StreamConnection::peer() const { return _peer; }

std::optional<contract::ClientCertificate> StreamConnection::certificate() const {
  const auto& presented = _stream->client_certificate();
  if (!presented || !presented->verified) return std::nullopt;
  return contract::ClientCertificate{presented->subject, presented->thumbprint};
}

std::string StreamConnection::transport() const { return _stream->transport(); }

Edge::Edge(broker::Delivery& delivery, contract::Authenticator& authenticator, dannan::Time& time, dannan::status::ScopedLogger logger,
           broker::Link::Limits limits, system::Views* views)
    : _delivery(delivery), _authenticator(authenticator), _time(time), _logger(std::move(logger)), _limits(limits), _views(views) {}

dannan::listen::StreamListener::Accepted Edge::accepted() {
  return [this](std::shared_ptr<dannan::listen::Stream> stream) { serve(std::move(stream)); };
}

dannan::listen::StreamListener::Accepted Edge::accepted_websocket() {
  return [this](std::shared_ptr<dannan::listen::Stream> stream) {
    auto strand = stream->get_executor();
    serve_websocket(std::move(stream), [this, strand](std::shared_ptr<contract::Connection> connection) {
      return broker::Link::create(strand, _delivery, _authenticator, _time, std::move(connection), _logger, _limits, _views);
    });
  };
}

void Edge::serve(std::shared_ptr<dannan::listen::Stream> stream) {
  auto connection = std::make_shared<StreamConnection>(stream);
  auto link = broker::Link::create(stream->get_executor(), _delivery, _authenticator, _time, connection, _logger, _limits, _views);
  std::make_shared<Reader>(std::move(stream), std::move(connection), std::move(link))->read();
}

}  // namespace lir::edge
