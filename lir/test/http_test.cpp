//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/http/http.h"

#include <gtest/gtest.h>
#include <poll.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/test/fixtures/mqtt_client.h"

namespace lir::http {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using mqtt::QoS;
using test::MqttClient;

// A plain HTTP client over a socket; reads give up after two seconds.
class HttpClient {
 public:
  explicit HttpClient(uint16_t port) { _socket.connect({asio::ip::make_address("127.0.0.1"), port}); }

  void send(const std::string& method, const std::string& target, const std::string& body = {}, const std::string& headers = {}) {
    const std::string request =
        method + " " + target + " HTTP/1.1\r\nHost: test\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
    asio::write(_socket, asio::buffer(request));
  }

  // Everything up to the server's close, or what came within the bound.
  std::string read_all() {
    std::string out;
    while (auto chunk = read_some()) out += *chunk;
    return out;
  }

  // Reads until `needle` has arrived; everything read so far.
  std::string read_until(const std::string& needle) {
    while (_seen.find(needle) == std::string::npos) {
      const auto chunk = read_some();
      if (!chunk) break;
      _seen += *chunk;
    }
    return _seen;
  }

 private:
  std::optional<std::string> read_some() {
    pollfd ready{_socket.native_handle(), POLLIN, 0};
    if (::poll(&ready, 1, 2000) <= 0) return std::nullopt;
    std::array<char, 8192> buffer{};
    boost::system::error_code ec;
    const size_t got = _socket.read_some(asio::buffer(buffer), ec);
    if (ec) return std::nullopt;
    return std::string(buffer.data(), got);
  }

  asio::io_context _io;
  tcp::socket _socket{_io};
  std::string _seen;
};

std::string request(uint16_t port, const std::string& method, const std::string& target, const std::string& body = {}, const std::string& headers = {}) {
  HttpClient client(port);
  client.send(method, target, body, headers);
  return client.read_all();
}

TEST(HttpTarget, PercentDecodingLeavesPlusAlone) {
  const auto t = parse_target("/subscribe?filter=room/+/temp&filter=a%2Fb%2B&x");
  EXPECT_EQ(t.path, "/subscribe");
  EXPECT_EQ(t.query.at("filter"), (std::vector<std::string>{"room/+/temp", "a/b+"}));
  EXPECT_EQ(t.query.at("x"), (std::vector<std::string>{""}));
  EXPECT_THROW(parse_target("/p?topic=%G1"), std::invalid_argument);
  EXPECT_THROW(parse_target("/p?topic=%2"), std::invalid_argument);
}

TEST(HttpEvents, TextOrHexAndEscaped) {
  mqtt::Publish p;
  p.topic = "a/\"b\"";
  p.payload = {'h', 'i', '\n'};
  EXPECT_EQ(sse_event(p), "event: message\ndata: {\"topic\":\"a/\\\"b\\\"\",\"qos\":0,\"retain\":false,\"payload\":\"hi\\u000a\"}\n\n");
  p.payload = {0xFF, 0x00};
  EXPECT_NE(sse_event(p).find("\"payload_hex\":\"ff00\""), std::string::npos);
}

class HttpTest : public ::testing::Test {
 protected:
  explicit HttpTest(contract::Authenticator* authenticator = nullptr)
      : edge(io.context, edge_config(), dannan::contract::steady_time_source(), authenticator ? *authenticator : allow, logger()),
        http(io.context, edge, http_config(), logger()) {
    edge.start();
    http.start();
  }
  static edge::EdgeConfig edge_config() {
    edge::EdgeConfig c;
    c.listen_host = "127.0.0.1";
    c.port = 0;
    return c;
  }
  static HttpConfig http_config() {
    HttpConfig c;
    c.listen_host = "127.0.0.1";
    c.port = 0;
    c.maximum_body = 1024;
    c.keepalive = 200ms;
    return c;
  }
  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  test::Io io;
  contract::AllowAll allow;
  edge::Edge edge;
  HttpServer http;
};

TEST_F(HttpTest, PublishReachesAnMqttSubscriber) {
  MqttClient sub(edge.port());
  sub.connect("sub");
  sub.subscribe("t/#", QoS::at_least_once);
  const auto response = request(http.port(), "POST", "/publish?topic=t/x&qos=1", "hello", "Content-Type: text/plain\r\n");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_NE(response.find("{\"accepted\":true,\"matched\":true}"), std::string::npos) << response;
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "t/x");
  EXPECT_EQ(std::string(got->payload.begin(), got->payload.end()), "hello");
  EXPECT_EQ(got->properties.content_type, "text/plain");
}

TEST_F(HttpTest, PublishSaysWhenNothingMatched) {
  const auto response = request(http.port(), "POST", "/publish?topic=nobody&qos=1", "x");
  EXPECT_NE(response.find("\"matched\":false"), std::string::npos) << response;
}

TEST_F(HttpTest, RetainedPublishIsReadByALaterMqttSubscriber) {
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=door&retain=1&qos=2", "open").starts_with("HTTP/1.1 200"));
  MqttClient sub(edge.port());
  sub.connect("sub");
  sub.subscribe("door", QoS::at_most_once);
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->retain);
}

TEST_F(HttpTest, RefusalsHaveTheirStatus) {
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=a/%2B", "x").starts_with("HTTP/1.1 400")) << "wildcard in a topic name";
  EXPECT_TRUE(request(http.port(), "POST", "/publish", "x").starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=t&qos=3", "x").starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=$lir/x&qos=1", "x").starts_with("HTTP/1.1 403"));
  EXPECT_TRUE(request(http.port(), "GET", "/publish?topic=t", "").starts_with("HTTP/1.1 405"));
  EXPECT_TRUE(request(http.port(), "GET", "/nowhere", "").starts_with("HTTP/1.1 404"));
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=t", std::string(2048, 'x')).starts_with("HTTP/1.1 413"));
  EXPECT_TRUE(request(http.port(), "GET", "/subscribe?filter=a/%23/b", "").starts_with("HTTP/1.1 400"));
}

TEST_F(HttpTest, EventStreamGetsRetainedThenLiveMessages) {
  MqttClient pub(edge.port());
  pub.connect("pub");
  pub.publish("room/0/temp", "19", QoS::at_least_once, true);
  HttpClient stream(http.port());
  stream.send("GET", "/subscribe?filter=room/+/temp");
  auto seen = stream.read_until("\"payload\":\"19\"");
  EXPECT_TRUE(seen.starts_with("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream")) << seen;
  EXPECT_NE(seen.find("\"retain\":true"), std::string::npos);
  pub.publish("room/1/temp", "21.5", QoS::at_least_once);
  seen = stream.read_until("\"payload\":\"21.5\"");
  EXPECT_NE(seen.find("{\"topic\":\"room/1/temp\",\"qos\":0,\"retain\":false,\"payload\":\"21.5\"}"), std::string::npos) << seen;
  // Kept alive with comments while quiet.
  EXPECT_NE(stream.read_until(":\n\n").find(":\n\n"), std::string::npos);
}

TEST_F(HttpTest, AStreamThatCloses_LeavesTheBroker) {
  {
    HttpClient stream(http.port());
    stream.send("GET", "/subscribe?filter=x");
    stream.read_until("text/event-stream");
    EXPECT_EQ(edge.call<size_t>([](broker::Broker& b) { return b.connections(); }), 1u);
  }
  EXPECT_TRUE(test::eventually([&] { return edge.call<size_t>([](broker::Broker& b) { return b.connections(); }) == 0; }, 3s));
}

class TokenOnly final : public contract::Authenticator {
 public:
  contract::AuthDecision connect(const mqtt::Connect& c) override {
    return c.password == mqtt::Bytes{'g', 'o', 'o', 'd'} ? contract::AuthDecision::accept(std::make_shared<contract::Unrestricted>())
                                                         : contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
  }
  contract::AuthDecision auth(const std::string&, const std::string&, const mqtt::Bytes&) override {
    return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  }
};

TokenOnly token_only;

class HttpAuthTest : public HttpTest {
 protected:
  HttpAuthTest() : HttpTest(&token_only) {}
};

TEST_F(HttpAuthTest, TheBearerTokenIsTheConnectPassword) {
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=t", "x").starts_with("HTTP/1.1 401"));
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=t", "x", "Authorization: Bearer bad\r\n").starts_with("HTTP/1.1 401"));
  EXPECT_TRUE(request(http.port(), "POST", "/publish?topic=t", "x", "Authorization: Bearer good\r\n").starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(request(http.port(), "GET", "/subscribe?filter=t", "").starts_with("HTTP/1.1 401"));
}

class HttpsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string openssl = LIR_OPENSSL;
    if (openssl.find("NOTFOUND") != std::string::npos) GTEST_SKIP() << "no openssl command";
    dannan::test::ChildProcess make(
        {openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", (dir.path() / "key.pem").string(), "-out",
         (dir.path() / "cert.pem").string(), "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"},
        dir.path() / "openssl.log");
    ASSERT_EQ(make.wait(std::chrono::steady_clock::now() + 20s), 0);
    edge::EdgeConfig edge_config;
    edge_config.listen_host = "127.0.0.1";
    edge_config.port = 0;
    edge.emplace(io.context, edge_config, dannan::contract::steady_time_source(), allow, logger());
    edge->start();
    HttpConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    config.tls = edge::TlsConfig{"127.0.0.1", 0, dir.path() / "cert.pem", dir.path() / "key.pem"};
    http.emplace(io.context, *edge, config, logger());
    http->start();
  }
  static dannan::status::ScopedLogger logger() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"}; }

  dannan::test::TempDir dir;
  test::Io io;
  contract::AllowAll allow;
  std::optional<edge::Edge> edge;
  std::optional<HttpServer> http;
};

TEST_F(HttpsTest, PublishOverHttpsReachesAnMqttSubscriber) {
  ASSERT_NE(http->tls_port(), 0);
  MqttClient sub(edge->port());
  sub.connect("sub");
  sub.subscribe("secure/#", QoS::at_most_once);

  asio::io_context client_io;
  asio::ssl::context context(asio::ssl::context::tls_client);
  context.load_verify_file((dir.path() / "cert.pem").string());
  asio::ssl::stream<tcp::socket> tls(client_io, context);
  tls.set_verify_mode(asio::ssl::verify_peer);
  tls.set_verify_callback(asio::ssl::host_name_verification("127.0.0.1"));
  tls.lowest_layer().connect({asio::ip::make_address("127.0.0.1"), http->tls_port()});
  tls.handshake(asio::ssl::stream_base::client);
  const std::string request = "POST /publish?topic=secure/door HTTP/1.1\r\nHost: test\r\nContent-Length: 4\r\n\r\nopen";
  asio::write(tls, asio::buffer(request));
  std::string response;
  boost::system::error_code ec;
  std::array<char, 4096> chunk{};
  while (!ec) {
    const size_t got = tls.read_some(asio::buffer(chunk), ec);
    response.append(chunk.data(), got);
  }
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "secure/door");
}

}  // namespace
}  // namespace lir::http
