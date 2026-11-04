//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mutex>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/sn/gateway.h"
#include "lir/sn/packet.h"
#include "lir/test/fixtures/mqtt_client.h"

// MQTT-SN over DTLS 1.2: certificates made for the test, a client that
// verifies the gateway, and client certificates handed to the authenticator.
namespace lir::sn {
namespace {

using namespace std::chrono_literals;

Bytes bytes(const std::string& s) { return {s.begin(), s.end()}; }

// An OpenSSL DTLS client on a connected UDP socket; every read bounded.
class DtlsClient {
 public:
  DtlsClient(uint16_t port, const std::string& ca, const std::string& certificate = {}, const std::string& key = {}) {
    _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(_fd, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    _ctx = SSL_CTX_new(DTLS_client_method());
    SSL_CTX_load_verify_locations(_ctx, ca.c_str(), nullptr);
    SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER, nullptr);
    if (!certificate.empty()) {
      SSL_CTX_use_certificate_chain_file(_ctx, certificate.c_str());
      SSL_CTX_use_PrivateKey_file(_ctx, key.c_str(), SSL_FILETYPE_PEM);
    }
    BIO* bio = BIO_new_dgram(_fd, BIO_NOCLOSE);
    timeval bound{0, 500000};
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_RECV_TIMEOUT, 0, &bound);
    // OpenSSL 3 takes the peer as a BIO_ADDR, which it copies.
    BIO_ADDR* peer = BIO_ADDR_new();
    BIO_ADDR_rawmake(peer, AF_INET, &to.sin_addr, sizeof(to.sin_addr), to.sin_port);
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, peer);
    BIO_ADDR_free(peer);
    _ssl = SSL_new(_ctx);
    SSL_set_bio(_ssl, bio, bio);
  }
  ~DtlsClient() {
    SSL_free(_ssl);
    SSL_CTX_free(_ctx);
    ::close(_fd);
  }

  bool handshake() {
    for (int i = 0; i < 12; ++i) {
      const int r = SSL_connect(_ssl);
      if (r == 1) return true;
      const int error = SSL_get_error(_ssl, r);
      ERR_clear_error();
      if (error != SSL_ERROR_WANT_READ) return false;
      DTLSv1_handle_timeout(_ssl);
    }
    return false;
  }
  void send(const Packet& p) {
    const auto b = encode(p);
    SSL_write(_ssl, b.data(), static_cast<int>(b.size()));
  }
  std::optional<Packet> next() {
    std::array<uint8_t, 4096> buffer{};
    for (int i = 0; i < 6; ++i) {
      const int n = SSL_read(_ssl, buffer.data(), static_cast<int>(buffer.size()));
      if (n > 0) return decode(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)));
      ERR_clear_error();
    }
    return std::nullopt;
  }

 private:
  int _fd = -1;
  SSL_CTX* _ctx = nullptr;
  SSL* _ssl = nullptr;
};

class Recording final : public contract::Authenticator {
 public:
  contract::AuthDecision connect(const mqtt::Connect&, const contract::Peer& peer) override {
    std::lock_guard lock(mutex);
    subject = peer.certificate_subject;
    return contract::AuthDecision::accept(std::make_shared<contract::Unrestricted>());
  }
  contract::AuthDecision auth(const std::string&, const std::string&, const mqtt::Bytes&, const contract::Peer&) override {
    return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  }
  std::mutex mutex;
  std::optional<std::string> subject;
};

class SnDtlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    openssl = LIR_OPENSSL;
    if (openssl.find("NOTFOUND") != std::string::npos) GTEST_SKIP() << "no openssl command";
    run({"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", path("server.key"), "-out", path("server.pem"),
         "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"});
    run({"req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", path("ca.key"), "-out", path("ca.pem"), "-days", "1",
         "-subj", "/CN=sensors ca"});
    run({"req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", path("device.key"), "-out", path("device.csr"), "-subj",
         "/CN=sensor-9"});
    run({"x509", "-req", "-in", path("device.csr"), "-CA", path("ca.pem"), "-CAkey", path("ca.key"), "-CAcreateserial", "-out", path("device.pem"), "-days",
         "1"});
    edge::EdgeConfig ec;
    ec.listen_host = "127.0.0.1";
    ec.port = 0;
    edge.emplace(io.context, ec, dannan::contract::steady_time_source(), recording, logger());
    edge->start();
    GatewayConfig gc;
    gc.listen_host = "127.0.0.1";
    gc.port = 0;
    gc.poll_every = 100ms;
    edge::TlsConfig dtls;
    dtls.listen_host = "127.0.0.1";
    dtls.port = 0;
    dtls.certificate_chain = path("server.pem");
    dtls.private_key = path("server.key");
    dtls.client_ca = path("ca.pem");
    dtls.require_client_certificate = false;
    gc.dtls = dtls;
    gateway.emplace(io.context, *edge, gc, logger());
    gateway->start();
  }
  void TearDown() override {
    if (gateway) gateway->stop();
    if (edge) edge->stop();
  }
  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }
  void run(std::vector<std::string> argv) {
    argv.insert(argv.begin(), openssl);
    dannan::test::ChildProcess make(argv, dir.path() / "openssl.log");
    ASSERT_EQ(make.wait(std::chrono::steady_clock::now() + 20s), 0);
  }
  std::string path(const std::string& name) { return (dir.path() / name).string(); }

  std::string openssl;
  dannan::test::TempDir dir;
  test::Io io;
  Recording recording;
  std::optional<edge::Edge> edge;
  std::optional<Gateway> gateway;
};

TEST_F(SnDtlsTest, AVerifyingClientConnectsAndPublishes) {
  ASSERT_NE(gateway->dtls_port(), 0);
  test::MqttClient tcp(edge->port());
  tcp.connect("tcp");
  tcp.subscribe("ab", mqtt::QoS::at_most_once);
  DtlsClient c(gateway->dtls_port(), path("server.pem"));
  ASSERT_TRUE(c.handshake());
  Connect connect;
  connect.flags.clean_session = true;
  connect.duration = 60;
  connect.client_id = "secure-sensor";
  c.send(connect);
  const auto ack = c.next();
  ASSERT_TRUE(ack && std::holds_alternative<Connack>(*ack));
  EXPECT_EQ(std::get<Connack>(*ack).code, rc::accepted);
  Publish p;
  p.flags.topic_id_type = TopicIdType::short_name;
  p.topic_id = short_topic("ab");
  p.data = bytes("sealed");
  c.send(p);
  const auto got = tcp.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "sealed");
  {
    std::lock_guard lock(recording.mutex);
    EXPECT_FALSE(recording.subject);
  }
  EXPECT_EQ(gateway->stats().dtls_sessions, 1u);
}

TEST_F(SnDtlsTest, AClientCertificatesSubjectReachesTheAuthenticator) {
  DtlsClient c(gateway->dtls_port(), path("server.pem"), path("device.pem"), path("device.key"));
  ASSERT_TRUE(c.handshake());
  Connect connect;
  connect.flags.clean_session = true;
  connect.duration = 60;
  connect.client_id = "sensor-9";
  c.send(connect);
  ASSERT_TRUE(c.next());
  std::lock_guard lock(recording.mutex);
  ASSERT_TRUE(recording.subject);
  EXPECT_NE(recording.subject->find("CN=sensor-9"), std::string::npos) << *recording.subject;
}

TEST_F(SnDtlsTest, PlainMqttSnOnTheDtlsPortGetsNothing) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(gateway->dtls_port());
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Connect connect;
  connect.client_id = "plain";
  const auto b = encode(connect);
  ::sendto(fd, b.data(), b.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  timeval bound{0, 500000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
  std::array<uint8_t, 512> buffer{};
  const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
  ::close(fd);
  // Whatever came back (nothing, or a DTLS alert), it was no CONNACK.
  if (n > 0) EXPECT_THROW(decode(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n))), Malformed);
  EXPECT_EQ(edge->call<size_t>([](broker::Broker& b) { return b.connections(); }), 0u);
}

}  // namespace
}  // namespace lir::sn
