//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "dannan/net/codec.h"

// The client's way out: a TCP or TLS connection to the broker. A local seam
// until Dannan's stream connector (its 0.8 patch, the outbound side of
// dannan::listen) lands, shaped like it so the client moves onto it by
// replacing this file: connect within a deadline, a posted done(stream,
// why), the stream on the executor given, reads and writes on it.
namespace lir::client {

// The server's certificate is checked against `trust` (a PEM bundle; empty:
// the system's store) and its name against `server_name` (empty: the host
// connected to; an IP address is checked against the certificate's IP
// addresses and sent as no SNI). A client certificate is offered when both
// of its paths are given.
struct Tls {
  std::filesystem::path trust;
  std::string server_name;
  std::filesystem::path certificate_chain;
  std::filesystem::path private_key;
};

class Transport {
 public:
  virtual ~Transport() = default;
  // One read of whatever has arrived, into `buffer`; `done(error, bytes)`.
  // One at a time. On the executor connect() was given.
  virtual void read_some(std::span<uint8_t> buffer, std::function<void(boost::system::error_code, size_t)> done) = 0;
  // Writes all of `bytes`; one at a time. On the executor.
  virtual void write(dannan::net::Bytes bytes, std::function<void(boost::system::error_code)> done) = 0;
  // Ends the connection: TLS's close_notify within a bound, then the socket.
  // Pending reads and writes end with an error. On the executor.
  virtual void close() = 0;
};

// Resolves `host`, connects to each address in turn, sets TCP_NODELAY, and
// with `tls` takes the handshake, all within `deadline`. `done` is posted to
// `executor` once: the transport, living on `executor`, or none and why.
void connect(boost::asio::any_io_executor executor, const std::string& host, uint16_t port, const std::optional<Tls>& tls, std::chrono::milliseconds deadline,
             std::function<void(std::shared_ptr<Transport>, std::string)> done);

}  // namespace lir::client
