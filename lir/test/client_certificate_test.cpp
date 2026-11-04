//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <mutex>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/edge/edge.h"
#include "lir/test/fixtures/mqtt_client.h"

// Client certificates (mutual TLS) on the MQTT TLS listener: verified
// against the configured CAs, required or optional, and the subject handed
// to the authenticator.
namespace lir::edge {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using mqtt::Version;
using tcp = asio::ip::tcp;

// Accepts everyone, remembering the last certificate subject it was shown.
class Recording final : public contract::Authenticator {
 public:
  contract::AuthDecision connect(const mqtt::Connect&, const contract::Peer& peer) override {
    std::lock_guard lock(_mutex);
    _seen = peer.certificate_subject;
    _asked = true;
    return contract::AuthDecision::accept(std::make_shared<contract::Unrestricted>());
  }
  contract::AuthDecision auth(const std::string&, const std::string&, const mqtt::Bytes&) override {
    return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  }
  std::pair<bool, std::optional<std::string>> seen() {
    std::lock_guard lock(_mutex);
    return {_asked, _seen};
  }

 private:
  std::mutex _mutex;
  bool _asked = false;
  std::optional<std::string> _seen;
};

class ClientCertificateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    openssl = LIR_OPENSSL;
    if (openssl.find("NOTFOUND") != std::string::npos) GTEST_SKIP() << "no openssl command";
    // The server's own certificate, the CA clients chain to, a client it
    // signed, and a client from a stranger CA.
    self_signed("server", "/CN=localhost", true);
    self_signed("ca", "/CN=lir test ca", false);
    self_signed("stranger", "/CN=stranger ca", false);
    signed_by("ca", "device", "/CN=device-17/O=lir");
    signed_by("stranger", "intruder", "/CN=intruder");
  }

  void run(std::vector<std::string> argv) {
    argv.insert(argv.begin(), openssl);
    dannan::test::ChildProcess make(argv, dir.path() / "openssl.log");
    ASSERT_EQ(make.wait(std::chrono::steady_clock::now() + 20s), 0) << argv[1];
  }
  std::string path(const std::string& name) { return (dir.path() / name).string(); }
  void self_signed(const std::string& name, const std::string& subject, bool server) {
    std::vector<std::string> argv{
        "req",   "-x509", "-newkey", "ec",   "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", path(name + ".key"), "-out", path(name + ".pem"),
        "-days", "1",     "-subj",   subject};
    if (server) {
      argv.push_back("-addext");
      argv.push_back("subjectAltName=IP:127.0.0.1");
    }
    run(argv);
  }
  void signed_by(const std::string& ca, const std::string& name, const std::string& subject) {
    run({"req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", path(name + ".key"), "-out", path(name + ".csr"), "-subj",
         subject});
    run({"x509", "-req", "-in", path(name + ".csr"), "-CA", path(ca + ".pem"), "-CAkey", path(ca + ".key"), "-CAcreateserial", "-out", path(name + ".pem"),
         "-days", "1"});
  }

  void start(bool required) {
    EdgeConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    TlsConfig tls;
    tls.listen_host = "127.0.0.1";
    tls.port = 0;
    tls.certificate_chain = path("server.pem");
    tls.private_key = path("server.key");
    tls.client_ca = path("ca.pem");
    tls.require_client_certificate = required;
    config.tls = tls;
    edge.emplace(io.context, config, dannan::contract::steady_time_source(), recording,
                 dannan::status::ScopedLogger{std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
    edge->start();
  }

  // Connects over TLS presenting `certificate` (none when empty) and sends
  // CONNECT: the CONNACK's reason, or nothing when the handshake or the
  // connection failed.
  std::optional<uint8_t> connect_as(const std::string& certificate) {
    asio::io_context client_io;
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.load_verify_file(path("server.pem"));
    if (!certificate.empty()) {
      context.use_certificate_chain_file(path(certificate + ".pem"));
      context.use_private_key_file(path(certificate + ".key"), asio::ssl::context::pem);
    }
    asio::ssl::stream<tcp::socket> tls(client_io, context);
    tls.set_verify_mode(asio::ssl::verify_peer);
    tls.lowest_layer().connect({asio::ip::make_address("127.0.0.1"), edge->tls_port()});
    // Every step async and bounded: a blocking Asio read retries past a
    // socket timeout.
    const auto bounded = [&client_io] {
      client_io.restart();
      client_io.run_for(3s);
      return client_io.stopped();
    };
    boost::system::error_code ec = asio::error::timed_out;
    tls.async_handshake(asio::ssl::stream_base::client, [&](const boost::system::error_code& e) { ec = e; });
    if (!bounded() || ec) return std::nullopt;
    const auto connect = mqtt::encode(mqtt::Connect{Version::v5, true, 0, "c", {}, {}, {}, {}}, Version::v5);
    ec = asio::error::timed_out;
    asio::async_write(tls, asio::buffer(connect), [&](const boost::system::error_code& e, size_t) { ec = e; });
    if (!bounded() || ec) return std::nullopt;
    // TLS 1.3 reports a refused certificate after the handshake, on the
    // first read.
    mqtt::Bytes buffer;
    std::array<uint8_t, 512> chunk{};
    for (;;) {
      if (const auto frame = mqtt::read_frame(buffer, UINT32_MAX); frame && buffer.size() >= frame->size()) {
        return std::get<mqtt::Connack>(mqtt::decode(buffer, Version::v5)).reason;
      }
      size_t got = 0;
      ec = asio::error::timed_out;
      tls.async_read_some(asio::buffer(chunk), [&](const boost::system::error_code& e, size_t n) {
        ec = e;
        got = n;
      });
      if (!bounded() || ec) return std::nullopt;
      buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got));
    }
  }

  std::string openssl;
  dannan::test::TempDir dir;
  test::Io io;
  Recording recording;
  std::optional<Edge> edge;
};

TEST_F(ClientCertificateTest, TheAuthenticatorSeesTheVerifiedSubject) {
  start(true);
  EXPECT_EQ(connect_as("device"), mqtt::reason::success);
  const auto [asked, subject] = recording.seen();
  ASSERT_TRUE(asked);
  ASSERT_TRUE(subject);
  EXPECT_NE(subject->find("CN=device-17"), std::string::npos) << *subject;
  EXPECT_NE(subject->find("O=lir"), std::string::npos) << *subject;
}

TEST_F(ClientCertificateTest, RequiredRefusesAClientWithoutOne) {
  start(true);
  EXPECT_FALSE(connect_as(""));
  EXPECT_FALSE(recording.seen().first);
}

TEST_F(ClientCertificateTest, ACertificateFromAnotherCaIsRefused) {
  start(false);
  EXPECT_FALSE(connect_as("intruder"));
  EXPECT_FALSE(recording.seen().first);
}

TEST_F(ClientCertificateTest, OptionalLetsAClientWithoutOneInWithNoSubject) {
  start(false);
  EXPECT_EQ(connect_as(""), mqtt::reason::success);
  const auto [asked, subject] = recording.seen();
  EXPECT_TRUE(asked);
  EXPECT_FALSE(subject);
}

TEST_F(ClientCertificateTest, APlainTcpClientHasNoSubject) {
  start(true);
  test::MqttClient plain(edge->port());
  plain.connect("plain");
  const auto [asked, subject] = recording.seen();
  EXPECT_TRUE(asked);
  EXPECT_FALSE(subject);
}

}  // namespace
}  // namespace lir::edge
