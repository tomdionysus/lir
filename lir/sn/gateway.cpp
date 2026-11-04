//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/sn/gateway.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <deque>
#include <map>

#include "dannan/identity/crypto.h"
#include "lir/mqtt/topic.h"
#include "lir/sn/packet.h"

namespace lir::sn {

namespace {

namespace asio = boost::asio;
using udp = asio::ip::udp;
using Clock = std::chrono::steady_clock;
using mqtt::Version;
namespace reason = mqtt::reason;

// The broker's side of one MQTT-SN client: MQTT packets in, handed to the
// gateway decoded.
class SnConnection final : public contract::Connection {
 public:
  explicit SnConnection(std::string peer, std::optional<std::string> subject = std::nullopt) : _peer(std::move(peer)), _subject(std::move(subject)) {}
  std::optional<std::string> certificate_subject() const override { return _subject; }
  void send(Bytes bytes) override {
    if (_closed || !on_packet) return;
    try {
      on_packet(mqtt::decode(bytes, Version::v5));
    } catch (const mqtt::MalformedPacket&) {
      // The broker sends nothing it cannot decode.
    }
  }
  void close() override {
    if (_closed) return;
    _closed = true;
    if (on_close) on_close();
  }
  std::string peer() const override { return _peer; }
  // Never redirected: the gateway is where the client's datagrams arrive.
  std::string transport() const override { return "udp"; }

  std::function<void(mqtt::Packet)> on_packet;
  std::function<void()> on_close;

 private:
  std::string _peer;
  std::optional<std::string> _subject;
  bool _closed = false;
};

// Where a client is: the datagram's source, and the forwarder's wrapping
// when it came encapsulated.
struct Address {
  udp::endpoint endpoint;
  std::optional<Bytes> node;
  uint8_t radius = 0;
  // Over DTLS.
  bool secure = false;
  std::string peer() const { return (secure ? "dtls:" : "") + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()); }
  std::string key() const {
    std::string out = peer();
    if (node) out += "/" + dannan::identity::hex(*node);
    return out;
  }
};

// Where the gateway's state hangs on its DTLS context, for the cookie
// callbacks: an index of its own, since Asio's context owns (and deletes)
// the app data slot.
int state_index() {
  static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  return index;
}

uint8_t sn_code(uint8_t mqtt_reason) {
  if (mqtt_reason < 0x80) return rc::accepted;
  if (mqtt_reason == reason::server_busy || mqtt_reason == reason::quota_exceeded || mqtt_reason == reason::server_unavailable) return rc::congestion;
  return rc::not_supported;
}

}  // namespace

struct Gateway::State : std::enable_shared_from_this<Gateway::State> {
  struct Will {
    std::string topic;
    int8_t qos = 0;
    bool retain = false;
    Bytes message;
  };

  struct Client {
    enum class Phase { will_topic, will_msg, connecting, active, asleep, awake };
    Address address;
    Phase phase = Phase::connecting;
    std::string client_id;
    bool clean = true;
    uint16_t duration = 0;
    std::optional<Will> will;
    std::shared_ptr<SnConnection> connection;
    std::shared_ptr<broker::Link> link;
    // Registered topic ids, both ways.
    std::map<std::string, uint16_t> ids;
    std::map<uint16_t, std::string> names;
    uint16_t next_id = 1;
    // The topic id of each publish the client sent, for its PUBACK.
    std::map<uint16_t, uint16_t> publish_topic;
    // The topic id each SUBACK reports.
    std::map<uint16_t, uint16_t> subscribe_topic;
    // Publishes waiting for the client's REGACK, by topic id; and the
    // REGISTER message ids the gateway sent.
    std::map<uint16_t, std::deque<Packet>> waiting;
    std::map<uint16_t, uint16_t> registering;
    uint16_t next_msg_id = 1;
    // Held while the client sleeps.
    std::deque<Packet> held;
    Clock::time_point last_heard;
    Clock::time_point sleep_deadline;
    Clock::time_point sleep_again_at;

    uint16_t id_for(const std::string& name) {
      if (const auto it = ids.find(name); it != ids.end()) return it->second;
      while (next_id == 0 || next_id == 0xFFFF || names.contains(next_id)) ++next_id;
      const uint16_t id = next_id++;
      ids[name] = id;
      names[id] = name;
      return id;
    }
  };

  // One DTLS peer: its session over datagram memory BIOs, fed from and
  // flushed to the DTLS socket.
  struct Dtls {
    SSL* ssl = nullptr;
    BIO* out = nullptr;
    Address address;
    Clock::time_point opened;
    Clock::time_point last;
    bool established = false;
    std::optional<std::string> subject;
    ~Dtls() {
      if (ssl) SSL_free(ssl);
    }
  };

  State(edge::Edge& e, GatewayConfig cfg, dannan::status::ScopedLogger log)
      : edge(e), config(std::move(cfg)), logger(std::move(log)), socket(e.strand()), dtls_socket(e.strand()), timer(e.strand()), advertise_timer(e.strand()) {}

  edge::Edge& edge;
  GatewayConfig config;
  dannan::status::ScopedLogger logger;
  udp::socket socket;
  udp::socket dtls_socket;
  std::unique_ptr<asio::ssl::context> dtls_context;
  dannan::identity::Bytes cookie_secret;
  std::array<uint8_t, 65536> dtls_buffer{};
  // Decrypted datagrams, and records to send: on the heap, never on an io
  // thread's small stack. Neither use nests within itself.
  std::vector<uint8_t> plain = std::vector<uint8_t>(65536);
  std::vector<uint8_t> record = std::vector<uint8_t>(65536);
  udp::endpoint dtls_sender;
  std::map<std::string, std::unique_ptr<Dtls>> sessions;
  std::atomic<uint16_t> dtls_port{0};
  asio::steady_timer timer;
  asio::steady_timer advertise_timer;
  std::atomic<uint16_t> port{0};
  // On the strand.
  bool running = false;
  std::array<uint8_t, 65536> buffer{};
  udp::endpoint sender;
  std::map<std::string, std::shared_ptr<Client>> clients;
  std::map<std::string, std::string> by_id;
  std::map<uint16_t, std::string> predefined;
  std::map<std::string, uint16_t> predefined_ids;
  GatewayStats stats;

  broker::Broker& broker() { return edge.broker(); }

  // ---- the wire

  void send_to(const Address& to, const Packet& packet) {
    auto bytes = std::make_shared<Bytes>(encode(packet));
    if (to.node) *bytes = encode(Encapsulated{to.radius, *to.node, *bytes});
    if (to.secure) return dtls_send(to, *bytes);
    socket.async_send_to(asio::buffer(*bytes), to.endpoint, [bytes](const boost::system::error_code&, size_t) {});
  }

  // ---- DTLS

  // The cookie a peer must echo (RFC 6347, 4.2.1): an HMAC of its address
  // under a secret made at start, so no state is held for a forged source.
  static int make_cookie(SSL* ssl, unsigned char* cookie, unsigned int* length) {
    const auto* state = static_cast<State*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), state_index()));
    const auto* peer = static_cast<Dtls*>(SSL_get_app_data(ssl));
    if (!state || !peer) return 0;
    const auto key = peer->address.peer();
    const auto mac = dannan::identity::hmac_sha256(state->cookie_secret, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
    std::copy(mac.begin(), mac.end(), cookie);
    *length = static_cast<unsigned int>(mac.size());
    return 1;
  }
  static int check_cookie(SSL* ssl, const unsigned char* cookie, unsigned int length) {
    unsigned char expected[64];
    unsigned int expected_length = 0;
    if (!make_cookie(ssl, expected, &expected_length) || length != expected_length) return 0;
    return CRYPTO_memcmp(cookie, expected, length) == 0 ? 1 : 0;
  }

  void dtls_receive() {
    dtls_socket.async_receive_from(asio::buffer(dtls_buffer), dtls_sender, [weak = weak_from_this()](const boost::system::error_code& ec, size_t got) {
      const auto s = weak.lock();
      if (!s || !s->running) return;
      if (!ec) s->dtls_datagram(std::span<const uint8_t>(s->dtls_buffer.data(), got), s->dtls_sender);
      s->dtls_receive();
    });
  }

  void dtls_datagram(std::span<const uint8_t> bytes, const udp::endpoint& from) {
    const Address address{from, std::nullopt, 0, true};
    const auto key = address.peer();
    auto it = sessions.find(key);
    if (it == sessions.end()) {
      size_t handshaking = 0;
      for (const auto& [k, session] : sessions) handshaking += session->established ? 0 : 1;
      if (handshaking >= config.maximum_handshakes) return;
      auto session = std::make_unique<Dtls>();
      session->ssl = SSL_new(dtls_context->native_handle());
      BIO* in = BIO_new(BIO_s_dgram_mem());
      session->out = BIO_new(BIO_s_dgram_mem());
      SSL_set_bio(session->ssl, in, session->out);
      SSL_set_accept_state(session->ssl);
      SSL_set_options(session->ssl, SSL_OP_COOKIE_EXCHANGE | SSL_OP_NO_QUERY_MTU);
      DTLS_set_link_mtu(session->ssl, 1400);
      session->address = address;
      session->opened = Clock::now();
      SSL_set_app_data(session->ssl, session.get());
      it = sessions.emplace(key, std::move(session)).first;
    }
    auto& session = *it->second;
    session.last = Clock::now();
    BIO_write(SSL_get_rbio(session.ssl), bytes.data(), static_cast<int>(bytes.size()));
    if (!session.established) {
      const int r = SSL_do_handshake(session.ssl);
      if (r == 1) {
        session.established = true;
        session.subject = edge::verified_subject(session.ssl);
        ++stats.dtls_sessions;
      } else {
        const int error = SSL_get_error(session.ssl, r);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
          ERR_clear_error();
          ++stats.dtls_failed;
          dtls_flush(session);
          sessions.erase(it);
          return;
        }
      }
    }
    if (session.established) {
      for (;;) {
        const int n = SSL_read(session.ssl, plain.data(), static_cast<int>(plain.size()));
        if (n > 0) {
          datagram(std::span<const uint8_t>(plain.data(), static_cast<size_t>(n)), address);
          // The datagram may have ended the session.
          if (!sessions.contains(key)) return;
          continue;
        }
        const int error = SSL_get_error(session.ssl, n);
        ERR_clear_error();
        if (error == SSL_ERROR_ZERO_RETURN || (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)) {
          // close_notify, or a fatal alert: the session and its client go.
          if (const auto client = clients.find(address.key()); client != clients.end()) end(client->second, error != SSL_ERROR_ZERO_RETURN, true);
          dtls_flush(session);
          sessions.erase(key);
          return;
        }
        break;
      }
    }
    dtls_flush(session);
  }

  void dtls_send(const Address& to, const Bytes& bytes) {
    const auto it = sessions.find(to.peer());
    if (it == sessions.end() || !it->second->established) return;
    SSL_write(it->second->ssl, bytes.data(), static_cast<int>(bytes.size()));
    dtls_flush(*it->second);
  }

  // Every record OpenSSL has written, one datagram each.
  void dtls_flush(Dtls& session) {
    for (;;) {
      const int n = BIO_read(session.out, record.data(), static_cast<int>(record.size()));
      if (n <= 0) break;
      auto copy = std::make_shared<Bytes>(record.begin(), record.begin() + n);
      dtls_socket.async_send_to(asio::buffer(*copy), session.address.endpoint, [copy](const boost::system::error_code&, size_t) {});
    }
  }

  // Retransmits a handshake's lost flight, and lets go of peers that never
  // finished one or have gone quiet with no client.
  void dtls_check() {
    const auto now = Clock::now();
    const auto handshake_timeout = config.dtls ? config.dtls->handshake_timeout : std::chrono::milliseconds(10000);
    std::vector<std::string> gone;
    for (auto& [key, session] : sessions) {
      if (!session->established) {
        if (now - session->opened > handshake_timeout) {
          ++stats.dtls_failed;
          gone.push_back(key);
          continue;
        }
        if (DTLSv1_handle_timeout(session->ssl) > 0) dtls_flush(*session);
      } else if (!clients.contains(session->address.key()) && now - session->last > std::chrono::minutes(10)) {
        gone.push_back(key);
      }
    }
    for (const auto& key : gone) sessions.erase(key);
  }

  void to_client(Client& c, Packet packet) {
    if (c.phase == Client::Phase::asleep) {
      if (c.held.size() >= config.maximum_held) {
        // The oldest QoS 0 publish goes first; failing that, the oldest.
        auto it = std::find_if(c.held.begin(), c.held.end(), [](const Packet& p) {
          const auto* pub = std::get_if<Publish>(&p);
          return pub && pub->flags.qos == 0;
        });
        c.held.erase(it != c.held.end() ? it : c.held.begin());
        ++stats.held_dropped;
      }
      c.held.push_back(std::move(packet));
      return;
    }
    send_to(c.address, packet);
  }

  void receive() {
    socket.async_receive_from(asio::buffer(buffer), sender, [weak = weak_from_this()](const boost::system::error_code& ec, size_t got) {
      const auto s = weak.lock();
      if (!s || !s->running) return;
      if (!ec) s->datagram(std::span<const uint8_t>(s->buffer.data(), got), s->sender);
      s->receive();
    });
  }

  void datagram(std::span<const uint8_t> bytes, const udp::endpoint& from) { datagram(bytes, Address{from, std::nullopt, 0, false}); }

  void datagram(std::span<const uint8_t> bytes, Address address) {
    ++stats.datagrams_received;
    Packet packet;
    try {
      packet = decode(bytes);
      if (const auto* e = std::get_if<Encapsulated>(&packet)) {
        address.node = e->wireless_node_id;
        address.radius = e->radius;
        packet = decode(e->packet);
        if (std::holds_alternative<Encapsulated>(packet)) throw Malformed("an encapsulation inside an encapsulation");
      }
    } catch (const Malformed&) {
      ++stats.malformed;
      return;
    }
    handle(address, std::move(packet));
  }

  // ---- from clients

  void handle(const Address& address, Packet packet) {
    if (std::holds_alternative<SearchGw>(packet)) return send_to(address, GwInfo{config.gateway_id, {}});
    if (std::holds_alternative<Advertise>(packet) || std::holds_alternative<GwInfo>(packet)) return;
    if (auto* c = std::get_if<Connect>(&packet)) return on_connect(address, std::move(*c));
    if (auto* p = std::get_if<Publish>(&packet); p && p->flags.qos == -1) return qos_minus_one(*p);
    auto client = find(address, packet);
    if (!client) {
      // Not connected here: told so.
      if (!std::holds_alternative<Disconnect>(packet)) send_to(address, Disconnect{});
      return;
    }
    auto& c = *client;
    c.last_heard = Clock::now();
    std::visit([&](auto& p) { on_packet(client, p); }, packet);
  }

  std::shared_ptr<Client> find(const Address& address, const Packet& packet) {
    if (const auto it = clients.find(address.key()); it != clients.end()) return it->second;
    // A sleeping client may wake from a new address, naming itself.
    if (const auto* ping = std::get_if<Pingreq>(&packet); ping && !ping->client_id.empty()) {
      if (const auto id = by_id.find(ping->client_id); id != by_id.end()) {
        const auto old = clients.find(id->second);
        if (old != clients.end() && old->second->phase == Client::Phase::asleep) {
          auto client = old->second;
          clients.erase(old);
          client->address = address;
          clients[address.key()] = client;
          id->second = address.key();
          return client;
        }
      }
    }
    return nullptr;
  }

  void on_connect(const Address& address, Connect connect) {
    if (connect.protocol_id != 0x01) return send_to(address, Connack{rc::not_supported});
    if (const auto it = clients.find(address.key()); it != clients.end()) end(it->second, false, true);
    auto client = std::make_shared<Client>();
    client->address = address;
    client->client_id = connect.client_id;
    client->clean = connect.flags.clean_session;
    client->duration = connect.duration;
    client->last_heard = Clock::now();
    clients[address.key()] = client;
    if (connect.flags.will) {
      client->phase = Client::Phase::will_topic;
      return send_to(address, WillTopicReq{});
    }
    link(client);
  }

  void link(const std::shared_ptr<Client>& client) {
    auto& c = *client;
    c.phase = Client::Phase::connecting;
    std::optional<std::string> subject;
    if (c.address.secure) {
      if (const auto session = sessions.find(c.address.peer()); session != sessions.end()) subject = session->second->subject;
    }
    c.connection = std::make_shared<SnConnection>(c.address.key(), subject);
    std::weak_ptr<State> weak_state = shared_from_this();
    std::weak_ptr<Client> weak_client = client;
    const auto connection = c.connection.get();
    c.connection->on_packet = [weak_state, weak_client, connection](mqtt::Packet p) {
      const auto s = weak_state.lock();
      const auto cl = weak_client.lock();
      if (s && cl && cl->connection.get() == connection) s->from_broker(cl, std::move(p));
    };
    c.connection->on_close = [weak_state, weak_client, connection] {
      const auto s = weak_state.lock();
      const auto cl = weak_client.lock();
      if (s && cl && cl->connection.get() == connection) s->broker_closed(cl);
    };
    c.link = broker().accept(c.connection);
    mqtt::Connect m;
    m.version = Version::v5;
    m.clean_start = c.clean;
    // The gateway keeps the client's keep alive and sleep itself.
    m.keep_alive = 0;
    m.client_id = c.client_id;
    // Kept through sleep; ended at a clean client's DISCONNECT.
    m.properties.session_expiry_interval = 0xFFFFFFFF;
    // What the broker sends ahead of the client's acknowledgements: the
    // rest it queues, and counts.
    m.properties.receive_maximum = 64;
    c.link->receive(mqtt::encode(m, Version::v5));
  }

  template <typename P>
  void on_packet(const std::shared_ptr<Client>& client, P& p) {
    auto& c = *client;
    using T = std::decay_t<P>;
    if constexpr (std::is_same_v<T, WillTopic>) {
      if (c.phase != Client::Phase::will_topic) return;
      if (p.topic.empty()) return link(client);
      c.will = Will{p.topic, std::max<int8_t>(p.flags.qos, 0), p.flags.retain, {}};
      c.phase = Client::Phase::will_msg;
      send_to(c.address, WillMsgReq{});
    } else if constexpr (std::is_same_v<T, WillMsg>) {
      if (c.phase != Client::Phase::will_msg) return;
      if (c.will) c.will->message = p.message;
      link(client);
    } else if constexpr (std::is_same_v<T, Register>) {
      if (!mqtt::valid_topic_name(p.topic)) return send_to(c.address, Regack{0, p.msg_id, rc::not_supported});
      send_to(c.address, Regack{c.id_for(p.topic), p.msg_id, rc::accepted});
    } else if constexpr (std::is_same_v<T, Regack>) {
      const auto it = c.registering.find(p.msg_id);
      if (it == c.registering.end()) return;
      const uint16_t id = it->second;
      c.registering.erase(it);
      auto waiting = std::move(c.waiting[id]);
      c.waiting.erase(id);
      if (p.code != rc::accepted) return;
      for (auto& queued : waiting) to_client(c, std::move(queued));
    } else if constexpr (std::is_same_v<T, Publish>) {
      if (!active(c)) return;
      const auto name = topic_name(c, p.flags.topic_id_type, p.topic_id);
      if (!name) return send_to(c.address, Puback{p.topic_id, p.msg_id, rc::invalid_topic_id});
      if (p.flags.qos > 0 && p.msg_id == 0) return send_to(c.address, Puback{p.topic_id, p.msg_id, rc::not_supported});
      mqtt::Publish m;
      m.topic = *name;
      m.qos = static_cast<mqtt::QoS>(p.flags.qos);
      m.retain = p.flags.retain;
      m.dup = p.flags.dup;
      m.packet_id = p.flags.qos > 0 ? p.msg_id : 0;
      m.payload = p.data;
      if (p.flags.qos > 0) c.publish_topic[p.msg_id] = p.topic_id;
      to_broker(c, m);
    } else if constexpr (std::is_same_v<T, Puback>) {
      if (p.code == rc::invalid_topic_id) {
        if (const auto it = c.names.find(p.topic_id); it != c.names.end()) {
          c.ids.erase(it->second);
          c.names.erase(it);
        }
      }
      to_broker(c, mqtt::Ack{mqtt::PacketType::puback, p.msg_id, reason::success, {}});
    } else if constexpr (std::is_same_v<T, PubFlow>) {
      const auto type = p.type == Type::pubrec ? mqtt::PacketType::pubrec : p.type == Type::pubrel ? mqtt::PacketType::pubrel : mqtt::PacketType::pubcomp;
      to_broker(c, mqtt::Ack{type, p.msg_id, reason::success, {}});
    } else if constexpr (std::is_same_v<T, Subscribe>) {
      if (!active(c)) return;
      std::optional<std::string> filter;
      uint16_t reported = 0;
      if (p.flags.topic_id_type == TopicIdType::normal) {
        filter = p.topic;
        if (!mqtt::has_wildcard(p.topic) && mqtt::valid_topic_name(p.topic)) reported = c.id_for(p.topic);
      } else {
        filter = topic_name(c, p.flags.topic_id_type, p.flags.topic_id_type == TopicIdType::short_name ? short_topic(p.topic) : p.topic_id);
        if (p.flags.topic_id_type == TopicIdType::predefined) reported = p.topic_id;
      }
      if (!filter) return send_to(c.address, Suback{p.flags, p.topic_id, p.msg_id, rc::invalid_topic_id});
      c.subscribe_topic[p.msg_id] = reported;
      to_broker(c, mqtt::Subscribe{p.msg_id, {}, {{*filter, static_cast<mqtt::QoS>(std::max<int8_t>(p.flags.qos, 0))}}});
    } else if constexpr (std::is_same_v<T, Unsubscribe>) {
      if (!active(c)) return;
      std::optional<std::string> filter = p.topic;
      if (p.flags.topic_id_type == TopicIdType::predefined) filter = topic_name(c, TopicIdType::predefined, p.topic_id);
      if (!filter) return send_to(c.address, Unsuback{p.msg_id});
      to_broker(c, mqtt::Unsubscribe{p.msg_id, {}, {*filter}});
    } else if constexpr (std::is_same_v<T, Pingreq>) {
      if (c.phase == Client::Phase::asleep || c.phase == Client::Phase::awake) {
        // Awake (section 6.14): what was held, then PINGRESP, then asleep
        // again after the grace.
        c.phase = Client::Phase::awake;
        auto held = std::move(c.held);
        c.held.clear();
        for (auto& packet : held) send_to(c.address, packet);
        c.sleep_again_at = Clock::now() + config.awake_grace;
      }
      send_to(c.address, Pingresp{});
    } else if constexpr (std::is_same_v<T, Disconnect>) {
      if (p.duration && *p.duration > 0 && c.link) {
        c.phase = Client::Phase::asleep;
        c.duration = *p.duration;
        c.sleep_deadline = Clock::now() + std::chrono::milliseconds(uint64_t{*p.duration} * 1500);
        send_to(c.address, Disconnect{});
        return;
      }
      send_to(c.address, Disconnect{});
      end(client, false, false);
    } else if constexpr (std::is_same_v<T, WillTopicUpd>) {
      if (p.topic.empty()) {
        c.will.reset();
      } else {
        if (!c.will) c.will = Will{};
        c.will->topic = p.topic;
        c.will->qos = std::max<int8_t>(p.flags.qos, 0);
        c.will->retain = p.flags.retain;
      }
      send_to(c.address, WillResp{Type::willtopicresp, rc::accepted});
    } else if constexpr (std::is_same_v<T, WillMsgUpd>) {
      if (c.will) c.will->message = p.message;
      send_to(c.address, WillResp{Type::willmsgresp, c.will ? rc::accepted : rc::not_supported});
    }
    // Anything else from a client (a gateway's packet) is ignored.
  }

  bool active(const Client& c) const { return c.phase == Client::Phase::active || c.phase == Client::Phase::awake || c.phase == Client::Phase::asleep; }

  std::optional<std::string> topic_name(const Client& c, TopicIdType type, uint16_t id) const {
    switch (type) {
      case TopicIdType::normal:
        if (const auto it = c.names.find(id); it != c.names.end()) return it->second;
        return std::nullopt;
      case TopicIdType::predefined:
        if (const auto it = predefined.find(id); it != predefined.end()) return it->second;
        return std::nullopt;
      case TopicIdType::short_name:
        return short_name(id);
    }
    return std::nullopt;
  }

  void to_broker(Client& c, const mqtt::Packet& packet) {
    if (c.link) c.link->receive(mqtt::encode(packet, Version::v5));
  }

  // A publish from a client that never connected (QoS -1, section 6.8):
  // a predefined or short topic, sent as an anonymous client's.
  void qos_minus_one(const Publish& p) {
    Client none;
    const auto name = topic_name(none, p.flags.topic_id_type, p.topic_id);
    if (!name || p.flags.topic_id_type == TopicIdType::normal) return;
    ++stats.qos_minus_one;
    auto connection = std::make_shared<SnConnection>("qos -1");
    bool accepted = false;
    connection->on_packet = [&accepted](mqtt::Packet packet) {
      if (const auto* ack = std::get_if<mqtt::Connack>(&packet)) accepted = ack->reason == reason::success;
    };
    auto l = broker().accept(connection);
    mqtt::Connect m;
    m.version = Version::v5;
    m.clean_start = true;
    l->receive(mqtt::encode(m, Version::v5));
    if (accepted) {
      mqtt::Publish publish;
      publish.topic = *name;
      publish.retain = p.flags.retain;
      publish.payload = p.data;
      l->receive(mqtt::encode(publish, Version::v5));
      l->receive(mqtt::encode(mqtt::Disconnect{}, Version::v5));
    }
    connection->on_packet = nullptr;
  }

  // ---- from the broker

  void from_broker(const std::shared_ptr<Client>& client, mqtt::Packet packet) {
    auto& c = *client;
    if (auto* ack = std::get_if<mqtt::Connack>(&packet)) {
      if (c.phase != Client::Phase::connecting) return;
      if (ack->reason != reason::success) {
        send_to(c.address, Connack{sn_code(ack->reason)});
        forget(client);
        return;
      }
      c.phase = Client::Phase::active;
      if (!c.client_id.empty()) by_id[c.client_id] = c.address.key();
      ++stats.clients_connected;
      return send_to(c.address, Connack{rc::accepted});
    }
    if (auto* publish = std::get_if<mqtt::Publish>(&packet)) return deliver(c, std::move(*publish));
    if (auto* ack = std::get_if<mqtt::Ack>(&packet)) {
      switch (ack->type) {
        case mqtt::PacketType::puback: {
          uint16_t id = 0;
          if (const auto it = c.publish_topic.find(ack->packet_id); it != c.publish_topic.end()) {
            id = it->second;
            c.publish_topic.erase(it);
          }
          return to_client(c, Puback{id, ack->packet_id, sn_code(ack->reason)});
        }
        case mqtt::PacketType::pubrec:
          if (ack->reason >= 0x80) {
            uint16_t id = 0;
            if (const auto it = c.publish_topic.find(ack->packet_id); it != c.publish_topic.end()) id = it->second;
            c.publish_topic.erase(ack->packet_id);
            return to_client(c, Puback{id, ack->packet_id, sn_code(ack->reason)});
          }
          return to_client(c, PubFlow{Type::pubrec, ack->packet_id});
        case mqtt::PacketType::pubrel:
          return to_client(c, PubFlow{Type::pubrel, ack->packet_id});
        case mqtt::PacketType::pubcomp:
          c.publish_topic.erase(ack->packet_id);
          return to_client(c, PubFlow{Type::pubcomp, ack->packet_id});
        default:
          return;
      }
    }
    if (auto* suback = std::get_if<mqtt::Suback>(&packet)) {
      const uint8_t r = suback->reasons.empty() ? reason::unspecified_error : suback->reasons.front();
      uint16_t id = 0;
      if (const auto it = c.subscribe_topic.find(suback->packet_id); it != c.subscribe_topic.end()) {
        id = it->second;
        c.subscribe_topic.erase(it);
      }
      Flags flags;
      flags.qos = r < 0x80 ? static_cast<int8_t>(r) : int8_t{0};
      return to_client(c, Suback{flags, r < 0x80 ? id : uint16_t{0}, suback->packet_id, sn_code(r)});
    }
    if (auto* unsuback = std::get_if<mqtt::Unsuback>(&packet)) return to_client(c, Unsuback{unsuback->packet_id});
    if (std::holds_alternative<mqtt::Disconnect>(packet)) {
      // The broker ended it (taken over, not authorized): the client is told.
      send_to(c.address, Disconnect{});
      forget(client);
    }
  }

  void deliver(Client& c, mqtt::Publish publish) {
    Publish p;
    p.flags.dup = publish.dup;
    p.flags.qos = static_cast<int8_t>(publish.qos);
    p.flags.retain = publish.retain;
    p.msg_id = publish.packet_id;
    p.data = std::move(publish.payload);
    const auto& name = publish.topic;
    if (name.size() == 2) {
      p.flags.topic_id_type = TopicIdType::short_name;
      p.topic_id = short_topic(name);
    } else if (const auto pre = predefined_ids.find(name); pre != predefined_ids.end()) {
      p.flags.topic_id_type = TopicIdType::predefined;
      p.topic_id = pre->second;
    } else if (const auto known = c.ids.find(name); known != c.ids.end() && !c.waiting.contains(known->second)) {
      p.topic_id = known->second;
    } else {
      // Registered with the client first (section 6.10); sent on its REGACK.
      const bool new_name = !c.ids.contains(name);
      p.topic_id = c.id_for(name);
      c.waiting[p.topic_id].push_back(std::move(p));
      if (new_name) {
        const uint16_t msg_id = c.next_msg_id++;
        if (c.next_msg_id == 0) c.next_msg_id = 1;
        c.registering[msg_id] = c.ids[name];
        to_client(c, Register{c.ids[name], msg_id, name});
      }
      return;
    }
    to_client(c, std::move(p));
  }

  void broker_closed(const std::shared_ptr<Client>& client) { forget(client); }

  // ---- endings

  // Ends a client: its will published when `lost`, its session ended when
  // it was clean.
  void end(const std::shared_ptr<Client>& client, bool lost, bool quietly) {
    auto& c = *client;
    if (lost && c.will && c.link) {
      mqtt::Publish w;
      w.topic = c.will->topic;
      w.qos = static_cast<mqtt::QoS>(c.will->qos);
      w.retain = c.will->retain;
      w.payload = c.will->message;
      w.packet_id = w.qos == mqtt::QoS::at_most_once ? 0 : 0xFFFF;
      to_broker(c, w);
      ++stats.wills_published;
    }
    if (c.link) {
      mqtt::Disconnect d;
      if (c.clean) d.properties.session_expiry_interval = 0;
      const auto l = std::move(c.link);
      if (c.connection) c.connection->on_packet = nullptr;
      l->receive(mqtt::encode(d, Version::v5));
    }
    (void)quietly;
    forget(client);
  }

  void forget(const std::shared_ptr<Client>& client) {
    auto& c = *client;
    if (c.connection) {
      c.connection->on_packet = nullptr;
      c.connection->on_close = nullptr;
    }
    c.link.reset();
    if (const auto it = by_id.find(c.client_id); it != by_id.end() && it->second == c.address.key()) by_id.erase(it);
    if (const auto it = clients.find(c.address.key()); it != clients.end() && it->second == client) clients.erase(it);
  }

  // ---- the poll

  void poll() {
    timer.expires_after(config.poll_every);
    timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      const auto s = weak.lock();
      if (ec || !s || !s->running) return;
      s->check();
      s->poll();
    });
  }

  void check() {
    refresh_predefined();
    if (dtls_context) dtls_check();
    const auto now = Clock::now();
    std::vector<std::shared_ptr<Client>> lost;
    for (const auto& [key, client] : clients) {
      auto& c = *client;
      switch (c.phase) {
        case Client::Phase::active:
          if (c.duration > 0 && now - c.last_heard > std::chrono::milliseconds(uint64_t{c.duration} * 1500)) lost.push_back(client);
          break;
        case Client::Phase::asleep:
          if (now > c.sleep_deadline) lost.push_back(client);
          break;
        case Client::Phase::awake:
          if (now >= c.sleep_again_at) {
            c.phase = Client::Phase::asleep;
            c.sleep_deadline = now + std::chrono::milliseconds(uint64_t{c.duration} * 1500);
          }
          break;
        default:
          // A client still giving its will, or connecting: lost when silent.
          if (now - c.last_heard > std::chrono::seconds(30)) lost.push_back(client);
          break;
      }
    }
    for (const auto& client : lost) {
      ++stats.clients_lost;
      end(client, true, true);
    }
  }

  void refresh_predefined() {
    predefined.clear();
    predefined_ids.clear();
    for (const auto& [topic, payload] : broker().retained_under(std::string(kPredefinedPrefix))) {
      const auto number = topic.substr(kPredefinedPrefix.size());
      uint32_t id = 0;
      bool ok = !number.empty() && number.size() <= 5;
      for (const char ch : number) {
        if (ch < '0' || ch > '9') ok = false;
        id = id * 10 + static_cast<uint32_t>(ch - '0');
      }
      const std::string name(payload.begin(), payload.end());
      if (!ok || id == 0 || id >= 0xFFFF || !mqtt::valid_topic_name(name)) continue;
      predefined[static_cast<uint16_t>(id)] = name;
      predefined_ids[name] = static_cast<uint16_t>(id);
    }
  }

  void advertise() {
    if (!config.advertise_every) return;
    advertise_timer.expires_after(*config.advertise_every);
    advertise_timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      const auto s = weak.lock();
      if (ec || !s || !s->running) return;
      const Address to{udp::endpoint(asio::ip::make_address(s->config.advertise_address), s->config.advertise_port), std::nullopt, 0};
      s->send_to(to, Advertise{s->config.gateway_id, static_cast<uint16_t>(std::min<int64_t>(s->config.advertise_every->count(), 0xFFFF))});
      s->advertise();
    });
  }
};

Gateway::Gateway(boost::asio::io_context&, edge::Edge& edge, GatewayConfig config, dannan::status::ScopedLogger logger)
    : _state(std::make_shared<State>(edge, std::move(config), std::move(logger))) {}

Gateway::~Gateway() {
  try {
    stop();
  } catch (const std::exception& e) {
    _state->logger.error(std::string("stopping the MQTT-SN gateway: ") + e.what());
  }
}

void Gateway::start() {
  auto& s = *_state;
  const udp::endpoint endpoint(asio::ip::make_address(s.config.listen_host), s.config.port);
  s.socket.open(endpoint.protocol());
  s.socket.set_option(udp::socket::reuse_address(true));
  if (s.config.advertise_every) s.socket.set_option(asio::socket_base::broadcast(true));
  s.socket.bind(endpoint);
  s.port = s.socket.local_endpoint().port();
  s.logger.info("MQTT-SN listening on " + s.config.listen_host + ":" + std::to_string(s.port.load()) + " (UDP)");
  if (const auto& dtls = s.config.dtls) {
    s.dtls_context = edge::server_context(*dtls, true);
    SSL_CTX_set_ex_data(s.dtls_context->native_handle(), state_index(), &s);
    SSL_CTX_set_cookie_generate_cb(s.dtls_context->native_handle(), &State::make_cookie);
    SSL_CTX_set_cookie_verify_cb(s.dtls_context->native_handle(), &State::check_cookie);
    s.cookie_secret = dannan::identity::random_bytes(32);
    const udp::endpoint dtls_endpoint(asio::ip::make_address(dtls->listen_host), dtls->port);
    s.dtls_socket.open(dtls_endpoint.protocol());
    s.dtls_socket.set_option(udp::socket::reuse_address(true));
    s.dtls_socket.bind(dtls_endpoint);
    s.dtls_port = s.dtls_socket.local_endpoint().port();
    s.logger.info("MQTT-SN over DTLS listening on " + dtls->listen_host + ":" + std::to_string(s.dtls_port.load()));
  }
  s.edge.call<bool>([st = _state](broker::Broker&) {
    st->running = true;
    st->refresh_predefined();
    st->receive();
    if (st->dtls_context) st->dtls_receive();
    st->poll();
    st->advertise();
    return true;
  });
}

void Gateway::stop() {
  _state->edge.call<bool>([st = _state](broker::Broker&) {
    if (!st->running) return false;
    st->running = false;
    auto clients = st->clients;
    for (const auto& [key, client] : clients) {
      st->send_to(client->address, Disconnect{});
      st->end(client, false, true);
    }
    for (auto& [key, session] : st->sessions) {
      if (session->established) SSL_shutdown(session->ssl);
      st->dtls_flush(*session);
    }
    st->sessions.clear();
    boost::system::error_code ignored;
    st->socket.close(ignored);
    st->dtls_socket.close(ignored);
    st->timer.cancel();
    st->advertise_timer.cancel();
    return true;
  });
}

uint16_t Gateway::port() const noexcept { return _state->port.load(); }

uint16_t Gateway::dtls_port() const noexcept { return _state->dtls_port.load(); }

GatewayStats Gateway::stats() {
  return _state->edge.call<GatewayStats>([st = _state](broker::Broker&) { return st->stats; });
}

}  // namespace lir::sn
