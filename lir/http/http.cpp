//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/http/http.h"

#include <array>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <deque>
#include <optional>
#include <stdexcept>

#include "dannan/identity/crypto.h"
#include "lir/mqtt/wire.h"
#include "lir/system/system.h"

namespace lir::http {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
using tcp = asio::ip::tcp;
using mqtt::Bytes;
using mqtt::Version;

std::string json_string(std::string_view text) { return "\"" + system::escaped(std::string(text)) + "\""; }

std::string json_error(std::string_view error, std::optional<uint8_t> reason = std::nullopt) {
  std::string out = "{\"error\":" + json_string(error);
  if (reason) out += ",\"reason\":" + std::to_string(*reason);
  return out + "}";
}

// What an MQTT refusal means over HTTP.
unsigned status_for(uint8_t reason) {
  switch (reason) {
    case mqtt::reason::bad_user_name_or_password:
    case mqtt::reason::bad_authentication_method:
      return 401;
    case mqtt::reason::not_authorized:
    case mqtt::reason::banned:
      return 403;
    case mqtt::reason::server_unavailable:
    case mqtt::reason::server_busy:
    case mqtt::reason::quota_exceeded:
      return 503;
    default:
      return 400;
  }
}

const char* status_text(unsigned status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Payload Too Large";
    default:
      return "Service Unavailable";
  }
}

// The broker's side of an HTTP request: an MQTT connection whose packets
// come straight back here, decoded.
class VirtualClient final : public contract::Connection {
 public:
  VirtualClient(std::string peer, std::optional<contract::ClientCertificate> certificate) : _peer(std::move(peer)), _certificate(std::move(certificate)) {}
  void send(Bytes bytes) override {
    if (_closed || !on_packet) return;
    try {
      on_packet(mqtt::decode(bytes, Version::v5));
    } catch (const mqtt::MalformedPacket&) {
      // The broker sends nothing it cannot decode; ignore rather than throw
      // into it.
    }
  }
  void close() override {
    if (_closed) return;
    _closed = true;
    if (on_close) on_close();
  }
  std::string peer() const override { return _peer; }
  std::optional<contract::ClientCertificate> certificate() const override { return _certificate; }
  // An HTTP request is served where it arrived, never redirected.
  std::string transport() const override { return "http"; }

  std::function<void(mqtt::Packet)> on_packet;
  std::function<void()> on_close;

 private:
  std::string _peer;
  std::optional<contract::ClientCertificate> _certificate;
  bool _closed = false;
};

std::optional<std::string> bearer(const bhttp::request<bhttp::string_body>& request) {
  const auto field = request[bhttp::field::authorization];
  if (!field.starts_with("Bearer ")) return std::nullopt;
  return std::string(field.substr(7));
}

mqtt::Connect connect_packet(const std::optional<std::string>& token) {
  mqtt::Connect c;
  c.version = Version::v5;
  c.clean_start = true;
  if (token) c.password = Bytes(token->begin(), token->end());
  return c;
}

}  // namespace

Target parse_target(std::string_view target) {
  Target out;
  const size_t question = target.find('?');
  out.path = percent_decode(target.substr(0, question));
  if (question == std::string_view::npos) return out;
  std::string_view rest = target.substr(question + 1);
  while (!rest.empty()) {
    const size_t amp = rest.find('&');
    const auto pair = rest.substr(0, amp);
    const size_t equals = pair.find('=');
    if (!pair.empty()) {
      out.query[percent_decode(pair.substr(0, equals))].push_back(equals == std::string_view::npos ? "" : percent_decode(pair.substr(equals + 1)));
    }
    if (amp == std::string_view::npos) break;
    rest = rest.substr(amp + 1);
  }
  return out;
}

std::string message_json(const mqtt::Publish& publish) {
  const std::string_view text(reinterpret_cast<const char*>(publish.payload.data()), publish.payload.size());
  std::string data = "{\"topic\":" + json_string(publish.topic) + ",\"qos\":" + std::to_string(static_cast<int>(publish.qos)) +
                     ",\"retain\":" + (publish.retain ? "true" : "false");
  if (mqtt::valid_utf8(text)) {
    data += ",\"payload\":" + json_string(text);
  } else {
    data += ",\"payload_hex\":\"" + dannan::identity::hex(publish.payload) + "\"";
  }
  return data + "}";
}

std::string sse_event(const mqtt::Publish& publish) { return "event: message\ndata: " + message_json(publish) + "\n\n"; }

// An exchange as the server holds it.
class ExchangeBase {
 public:
  virtual ~ExchangeBase() = default;
  virtual void terminate() = 0;
};

// One HTTP connection over a stream of Dannan's listeners (TCP, or TLS
// over it): a request, then a response or an event stream. The stream
// queues what is written, closes a reader too slow for the outbound bound,
// and writes what is queued before its clean close. Everything on the
// edge's strand.
class Exchange final : public ExchangeBase, public std::enable_shared_from_this<Exchange> {
 public:
  Exchange(std::shared_ptr<dannan::listen::Stream> stream, edge::Edge& edge, const HttpConfig& config, dannan::status::ScopedLogger logger,
           std::function<void(ExchangeBase*)> forget)
      : _stream(std::move(stream)), _edge(edge), _config(config), _logger(std::move(logger)), _timer(_stream->get_executor()), _forget(std::move(forget)) {
    const auto& remote = _stream->peer();
    _peer = "http " + remote.address().to_string() + ":" + std::to_string(remote.port());
    _certificate = edge::verified_certificate(*_stream);
  }

  void start() {
    _parser.emplace();
    _parser->body_limit(_config.maximum_body);
    _timer.expires_after(_config.read_timeout);
    _timer.async_wait([weak = this->weak_from_this()](const boost::system::error_code& ec) {
      if (const auto self = weak.lock(); self && !ec && !self->_streaming) self->terminate();
    });
    bhttp::async_read(*_stream, _buffer, *_parser, [self = this->shared_from_this()](const boost::system::error_code& ec, size_t) {
      self->_timer.cancel();
      if (ec == bhttp::error::body_limit) return self->respond(413, json_error("body over the limit"));
      if (ec) return self->terminate();
      self->handle(self->_parser->release());
    });
  }

  void terminate() override {
    if (_done) return;
    _done = true;
    _timer.cancel();
    _stream->abort();
    if (_client) _client->on_packet = nullptr;
    if (auto link = std::move(_link)) link->closed();
    _forget(this);
  }

 private:
  void handle(bhttp::request<bhttp::string_body> request) {
    Target target;
    try {
      target = parse_target(request.target());
    } catch (const std::invalid_argument& e) {
      return respond(400, json_error(e.what()));
    }
    if (target.path == "/publish") {
      if (request.method() != bhttp::verb::post) return respond(405, json_error("POST /publish"));
      return publish(request, target);
    }
    if (target.path == "/subscribe") {
      if (request.method() != bhttp::verb::get) return respond(405, json_error("GET /subscribe"));
      return subscribe(request, target);
    }
    if (target.path == "/retained") {
      if (request.method() != bhttp::verb::get) return respond(405, json_error("GET /retained"));
      return retained(request, target);
    }
    respond(404, json_error("no such endpoint"));
  }

  // Connects the virtual client; the CONNACK's refusal, if any.
  std::optional<uint8_t> connect(const bhttp::request<bhttp::string_body>& request, std::vector<mqtt::Packet>& got) {
    _client = std::make_shared<VirtualClient>(_peer, _certificate);
    _client->on_packet = [&got](mqtt::Packet p) { got.push_back(std::move(p)); };
    _link = _edge.broker().accept(_client);
    _link->receive(mqtt::encode(connect_packet(bearer(request)), Version::v5));
    for (const auto& p : got) {
      if (const auto* ack = std::get_if<mqtt::Connack>(&p)) return ack->reason == mqtt::reason::success ? std::nullopt : std::optional<uint8_t>(ack->reason);
    }
    return mqtt::reason::server_unavailable;
  }

  void end_client() {
    if (_client) _client->on_packet = nullptr;
    if (auto link = std::move(_link)) link->receive(mqtt::encode(mqtt::Disconnect{}, Version::v5));
  }

  void publish(const bhttp::request<bhttp::string_body>& request, const Target& target) {
    const auto topics = target.query.find("topic");
    if (topics == target.query.end() || topics->second.size() != 1) return respond(400, json_error("one topic is required"));
    int qos = 0;
    if (const auto q = target.query.find("qos"); q != target.query.end()) {
      if (q->second.size() != 1 || q->second[0].size() != 1 || q->second[0][0] < '0' || q->second[0][0] > '2')
        return respond(400, json_error("qos is 0, 1 or 2"));
      qos = q->second[0][0] - '0';
    }
    const auto retain_param = target.query.find("retain");
    const bool retain = retain_param != target.query.end() && !retain_param->second.empty() && retain_param->second[0] == "1";
    if (!mqtt::valid_utf8(topics->second[0])) return respond(400, json_error("topic is not UTF-8"));

    std::vector<mqtt::Packet> got;
    if (const auto refused = connect(request, got)) {
      end_client();
      return respond(status_for(*refused), json_error("refused", refused));
    }
    got.clear();
    mqtt::Publish p;
    p.topic = topics->second[0];
    p.qos = static_cast<mqtt::QoS>(qos);
    p.retain = retain;
    p.packet_id = qos > 0 ? 1 : 0;
    p.payload = Bytes(request.body().begin(), request.body().end());
    if (const auto type = request[bhttp::field::content_type]; !type.empty() && mqtt::valid_utf8(type)) p.properties.content_type = std::string(type);
    try {
      _link->receive(mqtt::encode(p, Version::v5));
    } catch (const std::invalid_argument& e) {
      end_client();
      return respond(400, json_error(e.what()));
    }
    std::optional<uint8_t> reason;
    for (const auto& packet : got) {
      if (const auto* d = std::get_if<mqtt::Disconnect>(&packet)) {
        _link.reset();
        _client->on_packet = nullptr;
        return respond(status_for(d->reason), json_error("refused", d->reason));
      }
      if (const auto* a = std::get_if<mqtt::Ack>(&packet)) reason = a->reason;
    }
    if (qos == 2 && reason && *reason < 0x80) _link->receive(mqtt::encode(mqtt::Ack{mqtt::PacketType::pubrel, 1, mqtt::reason::success, {}}, Version::v5));
    end_client();
    if (reason && *reason >= 0x80) return respond(status_for(*reason), json_error("refused", reason));
    std::string body = "{\"accepted\":true";
    if (reason) body += std::string(",\"matched\":") + (*reason == mqtt::reason::no_matching_subscribers ? "false" : "true");
    respond(200, body + "}");
  }

  void subscribe(const bhttp::request<bhttp::string_body>& request, const Target& target) {
    const auto filters = target.query.find("filter");
    if (filters == target.query.end() || filters->second.empty()) return respond(400, json_error("at least one filter is required"));
    for (const auto& f : filters->second) {
      if (!mqtt::valid_utf8(f)) return respond(400, json_error("filter is not UTF-8"));
    }
    std::vector<mqtt::Packet> got;
    if (const auto refused = connect(request, got)) {
      end_client();
      return respond(status_for(*refused), json_error("refused", refused));
    }
    // From here every packet the broker sends is the stream's.
    _client->on_packet = [weak = this->weak_from_this()](mqtt::Packet p) {
      if (const auto self = weak.lock()) self->on_stream_packet(std::move(p));
    };
    _client->on_close = [weak = this->weak_from_this(), executor = _stream->get_executor()] {
      asio::post(executor, [weak] {
        if (const auto self = weak.lock()) self->terminate();
      });
    };
    mqtt::Subscribe s{1, {}, {}};
    for (const auto& f : filters->second) s.subscriptions.push_back(mqtt::Subscription{f, mqtt::QoS::at_most_once});
    _link->receive(mqtt::encode(s, Version::v5));
    await_suback();
  }

  // The SUBACK's turn is over: what came before it and the retained
  // messages after it are pending, in order, and the stream begins.
  void start_stream() {
    if (_done || _ending || _streaming || !_suback) return;
    for (const uint8_t code : _suback->reasons) {
      if (code >= 0x80) {
        _pending.clear();
        end_client();
        return respond(status_for(code), json_error("a filter was refused", code));
      }
    }
    _streaming = true;
    write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
    for (auto& event : _pending) write(std::move(event));
    _pending.clear();
    keepalive();
    watch_close();
  }

  // A snapshot of the retained messages the filters match. The broker sends
  // a subscription's retained messages right after its SUBACK, in the same
  // turn on this strand. On a cluster node the SUBACK waits until the other
  // nodes hold the subscription (broker.h, ClusterHooks::interest_held), so
  // the answer waits for it. Live messages may come first (MQTT 5.0, 3.8.4),
  // but none carries the retain flag, so none is in the answer.
  void retained(const bhttp::request<bhttp::string_body>& request, const Target& target) {
    const auto filters = target.query.find("filter");
    if (filters == target.query.end() || filters->second.empty()) return respond(400, json_error("at least one filter is required"));
    for (const auto& f : filters->second) {
      if (!mqtt::valid_utf8(f)) return respond(400, json_error("filter is not UTF-8"));
    }
    std::vector<mqtt::Packet> got;
    if (const auto refused = connect(request, got)) {
      end_client();
      return respond(status_for(*refused), json_error("refused", refused));
    }
    // Everything after the CONNACK is the snapshot's. The answer goes once the
    // SUBACK's turn is over, so the retained messages after it are in.
    _client->on_packet = [weak = this->weak_from_this(), executor = _stream->get_executor()](mqtt::Packet p) {
      const auto self = weak.lock();
      if (!self) return;
      const bool acknowledged = std::holds_alternative<mqtt::Suback>(p);
      self->_snapshot.push_back(std::move(p));
      if (acknowledged) {
        asio::post(executor, [weak] {
          if (const auto s = weak.lock()) s->answer_retained();
        });
      }
    };
    mqtt::Subscribe s{1, {}, {}};
    for (const auto& f : filters->second) s.subscriptions.push_back(mqtt::Subscription{f, mqtt::QoS::at_most_once});
    try {
      _link->receive(mqtt::encode(s, Version::v5));
    } catch (const std::invalid_argument& e) {
      end_client();
      return respond(400, json_error(e.what()));
    }
    await_suback();
  }

  void answer_retained() {
    if (_done || _ending) return;
    std::string body = "[";
    for (const auto& packet : _snapshot) {
      if (const auto* ack = std::get_if<mqtt::Suback>(&packet)) {
        for (const uint8_t code : ack->reasons) {
          if (code >= 0x80) {
            end_client();
            return respond(status_for(code), json_error("a filter was refused", code));
          }
        }
      } else if (const auto* p = std::get_if<mqtt::Publish>(&packet); p && p->retain) {
        if (body.size() > 1) body += ",";
        body += message_json(*p);
        if (body.size() + 1 > _config.maximum_retained_bytes) {
          end_client();
          return respond(413, json_error("more retained messages than an answer holds: narrow the filter"));
        }
      }
    }
    end_client();
    respond(200, body + "]");
  }

  // The SUBACK is awaited for at most `read_timeout`. On a cluster node it
  // comes within the cluster's own deadline, missed peers or not, so this
  // bound is only for a broker that never answers. The wait holds the
  // exchange, which nothing else holds until the answer or the stream
  // begins; either cancels it.
  void await_suback() {
    _timer.expires_after(_config.read_timeout);
    _timer.async_wait([self = this->shared_from_this()](const boost::system::error_code& ec) {
      if (ec || self->_done || self->_ending || self->_streaming) return;
      self->_pending.clear();
      self->end_client();
      self->respond(503, json_error("the subscription was not acknowledged in time"));
    });
  }

  void on_stream_packet(mqtt::Packet packet) {
    if (auto* ack = std::get_if<mqtt::Suback>(&packet)) {
      _suback = std::move(*ack);
      asio::post(_stream->get_executor(), [weak = this->weak_from_this()] {
        if (const auto self = weak.lock()) self->start_stream();
      });
    } else if (const auto* p = std::get_if<mqtt::Publish>(&packet)) {
      auto event = sse_event(*p);
      if (_streaming) {
        write(std::move(event));
      } else {
        _pending.push_back(std::move(event));
      }
    }
  }

  void keepalive() {
    _timer.expires_after(_config.keepalive);
    _timer.async_wait([weak = this->weak_from_this()](const boost::system::error_code& ec) {
      const auto self = weak.lock();
      if (ec || !self || self->_done) return;
      self->write(":\n\n");
      self->keepalive();
    });
  }

  // A stream's client sends nothing more; a read ends when it goes.
  // The pending read is what keeps a stream's exchange alive: it ends
  // when the client goes or terminate() closes the socket.
  void watch_close() {
    _stream->async_read_some(asio::buffer(_scratch), [self = this->shared_from_this()](const boost::system::error_code& ec, size_t) {
      // The clean end, once begun, finishes the exchange; the read it
      // aborts must not.
      if (self->_done || self->_ending) return;
      if (ec) return self->terminate();
      self->watch_close();
    });
  }

  void respond(unsigned status, const std::string& body) {
    write("HTTP/1.1 " + std::to_string(status) + " " + status_text(status) +
          "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    end();
  }

  void write(std::string bytes) {
    if (_done || _ending) return;
    _stream->send(std::move(bytes));
  }

  // The response is queued: the stream writes it, then over TLS sends
  // close_notify (RFC 8446, section 6.1), within its close bound; then the
  // exchange ends.
  void end() {
    if (_done || _ending) return;
    _ending = true;
    _timer.cancel();
    _stream->when_finished([self = this->shared_from_this()] { self->terminate(); });
    _stream->close();
  }

  std::shared_ptr<dannan::listen::Stream> _stream;
  edge::Edge& _edge;
  const HttpConfig _config;
  dannan::status::ScopedLogger _logger;
  asio::steady_timer _timer;
  std::function<void(ExchangeBase*)> _forget;
  std::string _peer;
  std::optional<contract::ClientCertificate> _certificate;
  beast::flat_buffer _buffer;
  std::optional<bhttp::request_parser<bhttp::string_body>> _parser;
  std::shared_ptr<VirtualClient> _client;
  std::shared_ptr<broker::Link> _link;
  std::optional<mqtt::Suback> _suback;
  std::vector<mqtt::Packet> _snapshot;
  std::vector<std::string> _pending;
  std::array<uint8_t, 512> _scratch{};
  bool _streaming = false;
  bool _ending = false;
  bool _done = false;
};

struct HttpServer::State : std::enable_shared_from_this<HttpServer::State> {
  State(asio::io_context& io_context, edge::Edge& e, HttpConfig cfg, dannan::status::ScopedLogger log)
      : io(io_context), edge(e), config(std::move(cfg)), logger(std::move(log)) {}

  std::function<void(ExchangeBase*)> forgetter() {
    return [weak = weak_from_this()](ExchangeBase* gone) {
      if (const auto s = weak.lock()) s->exchanges.erase(gone);
    };
  }

  // A connection handed up by a listener, after its TLS handshake when it
  // has one. On the edge's strand.
  void begin(std::shared_ptr<dannan::listen::Stream> stream) {
    if (stopped || edge::presented_unverified(*stream)) return stream->abort();
    auto exchange = std::make_shared<Exchange>(std::move(stream), edge, config, logger, forgetter());
    exchanges.emplace(exchange.get(), exchange);
    exchange->start();
  }

  std::unique_ptr<dannan::listen::StreamListener> listen(dannan::listen::ListenerConfig settings) {
    auto listener = std::make_unique<dannan::listen::StreamListener>(io, std::move(settings));
    listener->start(edge.strand(), [weak = weak_from_this()](std::shared_ptr<dannan::listen::Stream> stream) {
      if (const auto s = weak.lock()) return s->begin(std::move(stream));
      stream->abort();
    });
    return listener;
  }

  std::vector<dannan::listen::StreamListener*> listeners() {
    std::vector<dannan::listen::StreamListener*> out;
    for (auto* each : {plain.get(), tls.get()}) {
      if (each) out.push_back(each);
    }
    return out;
  }

  asio::io_context& io;
  edge::Edge& edge;
  HttpConfig config;
  dannan::status::ScopedLogger logger;
  // Dannan's public listeners, on the edge's strand.
  std::unique_ptr<dannan::listen::StreamListener> plain;
  std::unique_ptr<dannan::listen::StreamListener> tls;
  std::map<ExchangeBase*, std::weak_ptr<ExchangeBase>> exchanges;
  bool started = false;
  bool stopped = false;
  std::atomic<uint16_t> port{0};
  std::atomic<uint16_t> tls_port{0};
};

HttpServer::HttpServer(asio::io_context& io, edge::Edge& edge, HttpConfig config, dannan::status::ScopedLogger logger)
    : _state(std::make_shared<State>(io, edge, std::move(config), std::move(logger))) {}

HttpServer::~HttpServer() {
  try {
    stop();
  } catch (const std::exception& e) {
    _state->logger.error(std::string("stopping HTTP: ") + e.what());
  }
}

void HttpServer::start() {
  auto& s = *_state;
  s.plain = s.listen(edge::listener_config("http", s.config.listen_host, s.config.port, s.config.maximum_outbound_bytes, nullptr));
  s.port = s.plain->port();
  if (const auto& tls = s.config.tls) {
    s.tls = s.listen(edge::listener_config("https", tls->listen_host, tls->port, s.config.maximum_outbound_bytes, &*tls));
    s.tls_port = s.tls->port();
    s.logger.info("HTTPS listening on " + tls->listen_host + ":" + std::to_string(s.tls_port.load()));
  }
  s.started = true;
  s.logger.info("HTTP listening on " + s.config.listen_host + ":" + std::to_string(s.port.load()));
}

void HttpServer::stop() {
  if (!_state->started) return;
  const bool first = _state->edge.call<bool>([s = _state](broker::Broker&) {
    if (s->stopped) return false;
    s->stopped = true;
    return true;
  });
  if (!first) return;
  // Waits on each listener's own strand: from this thread, never the edge's.
  for (auto* listener : _state->listeners()) listener->stop_accepting();
  _state->edge.call<bool>([s = _state](broker::Broker&) {
    std::vector<std::shared_ptr<ExchangeBase>> open;
    for (const auto& [raw, weak] : s->exchanges) {
      if (auto e = weak.lock()) open.push_back(std::move(e));
    }
    for (const auto& e : open) e->terminate();
    return true;
  });
  for (auto* listener : _state->listeners()) listener->close_all();
}

uint16_t HttpServer::port() const noexcept { return _state->port.load(); }

uint16_t HttpServer::tls_port() const noexcept { return _state->tls_port.load(); }

}  // namespace lir::http
