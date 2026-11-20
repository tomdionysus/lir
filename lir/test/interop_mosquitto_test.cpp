//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "lir/mqtt/packet.h"

// Lir's codec against a real MQTT client (the maintainer, 2026-11-20: "You will test
// with a real MQTT client"). The test is the server end of one connection:
// mosquitto_pub or mosquitto_sub connects to it, what the client sends is
// read with Lir's decoder, and Lir's encoder answers. The client finishing
// cleanly, and printing what Lir sent it, shows a real implementation reads
// Lir's bytes; the decoded packets show Lir reads the client's.
namespace lir::mqtt {
namespace {

using std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto patience = 10s;

// A closed peer must fail a send, not signal the test: Linux by flag, macOS by
// socket option (set at accept).
#ifdef MSG_NOSIGNAL
constexpr int no_sigpipe = MSG_NOSIGNAL;
#else
constexpr int no_sigpipe = 0;
#endif

// One listening socket on the loopback and the one connection it accepts.
class Server {
 public:
  Server() {
    _listen = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (_listen < 0 || ::bind(_listen, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(_listen, 1) != 0) {
      throw std::runtime_error("cannot listen on the loopback");
    }
    socklen_t size = sizeof(address);
    ::getsockname(_listen, reinterpret_cast<sockaddr*>(&address), &size);
    _port = ntohs(address.sin_port);
  }
  ~Server() {
    if (_connection >= 0) ::close(_connection);
    if (_listen >= 0) ::close(_listen);
  }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  uint16_t port() const { return _port; }

  void accept() {
    if (!ready(_listen)) throw std::runtime_error("no client connected");
    _connection = ::accept(_listen, nullptr, nullptr);
    if (_connection < 0) throw std::runtime_error("accept failed");
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(_connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  }

  // The next whole packet the client sent, decoded as `version`.
  Packet next(Version version) {
    for (;;) {
      if (const auto frame = read_frame(_buffer, 1 << 20); frame && _buffer.size() >= frame->size()) {
        const Bytes bytes(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(frame->size()));
        _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(frame->size()));
        ++_read;
        return decode(bytes, version);
      }
      if (!ready(_connection)) throw std::runtime_error("the client sent nothing more after " + std::to_string(_read) + " packets");
      uint8_t chunk[4096];
      const ssize_t got = ::recv(_connection, chunk, sizeof(chunk), 0);
      if (got <= 0) throw std::runtime_error("the client closed the connection after " + std::to_string(_read) + " packets");
      _buffer.insert(_buffer.end(), chunk, chunk + got);
    }
  }

  // The client's last word: a DISCONNECT, or nothing when it simply closes.
  std::optional<Packet> last(Version version) {
    if (_buffer.empty() && ready(_connection)) {
      uint8_t chunk[4096];
      const ssize_t got = ::recv(_connection, chunk, sizeof(chunk), 0);
      if (got == 0) return std::nullopt;
      if (got < 0) throw std::runtime_error("recv failed");
      _buffer.insert(_buffer.end(), chunk, chunk + got);
    }
    return next(version);
  }

  // What a broker does once the client's DISCONNECT is in: closes its end.
  void close() {
    if (_connection >= 0) ::close(_connection);
    _connection = -1;
  }

  void send(const Packet& packet, Version version) {
    const Bytes bytes = encode(packet, version);
    size_t sent = 0;
    while (sent < bytes.size()) {
      const ssize_t wrote = ::send(_connection, bytes.data() + sent, bytes.size() - sent, no_sigpipe);
      if (wrote <= 0) throw std::runtime_error("send failed");
      sent += static_cast<size_t>(wrote);
    }
  }

 private:
  static bool ready(int fd) {
    pollfd poll_fd{fd, POLLIN, 0};
    return ::poll(&poll_fd, 1, static_cast<int>(std::chrono::milliseconds(patience).count())) == 1;
  }

  int _listen = -1;
  int _connection = -1;
  uint16_t _port = 0;
  Bytes _buffer;
  size_t _read = 0;
};

// The client's own account (its stderr), for a failure's message: CI keeps no
// files.
std::string contents(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Runs a test's exchange; a failure inside it is reported with the client's
// output rather than as a bare exception.
template <class Body>
void exchange(const std::filesystem::path& log, Body&& body) {
  try {
    body();
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what() << "\nthe client said:\n" << contents(log);
  }
}

std::filesystem::path log_for(const std::string& name) {
  const std::filesystem::path dir = std::filesystem::path(DANNAN_TEST_TEMP_ROOT) / "interop";
  std::filesystem::create_directories(dir);
  return dir / (name + ".log");
}

std::string level(Version version) {
  switch (version) {
    case Version::v5:
      return "5";
    case Version::v3_1_1:
      return "311";
    default:
      return "31";
  }
}

Bytes bytes_of(const std::string& text) { return Bytes(text.begin(), text.end()); }

class MosquittoInterop : public ::testing::TestWithParam<Version> {};

INSTANTIATE_TEST_SUITE_P(Versions, MosquittoInterop, ::testing::Values(Version::v5, Version::v3_1_1, Version::v3_1),
                         [](const auto& version) { return "MQTT" + level(version.param); });

// mosquitto_pub: CONNECT with a will, credentials and keep alive, a retained
// QoS 1 PUBLISH, Lir's CONNACK and PUBACK, and a clean DISCONNECT.
TEST_P(MosquittoInterop, PublishAtQoS1WithAWill) {
  const Version version = GetParam();
  Server server;
  std::vector<std::string> argv = {LIR_MOSQUITTO_PUB,
                                   "-h",
                                   "127.0.0.1",
                                   "-p",
                                   std::to_string(server.port()),
                                   "-V",
                                   level(version),
                                   "-i",
                                   "lir-interop-pub",
                                   "-k",
                                   "30",
                                   "-u",
                                   "user",
                                   "-P",
                                   "secret",
                                   "-q",
                                   "1",
                                   "-r",
                                   "-t",
                                   "a/b",
                                   "-m",
                                   "hello",
                                   "--will-topic",
                                   "gone/pub",
                                   "--will-payload",
                                   "bye",
                                   "--will-qos",
                                   "1",
                                   "--will-retain"};
  if (version == Version::v5) {
    argv.insert(argv.end(),
                {"-D", "connect", "session-expiry-interval", "60", "-D", "publish", "user-property", "k", "v", "-D", "publish", "content-type", "text/plain"});
  }
  const auto log = log_for("pub-" + level(version));
  dannan::test::ChildProcess client(argv, log);
  exchange(log, [&] {
    server.accept();

    const auto connect = std::get<Connect>(server.next(version));
    EXPECT_EQ(connect.version, version);
    EXPECT_EQ(connect.client_id, "lir-interop-pub");
    EXPECT_EQ(connect.keep_alive, 30);
    EXPECT_EQ(connect.username, "user");
    EXPECT_EQ(connect.password, bytes_of("secret"));
    ASSERT_TRUE(connect.will.has_value());
    EXPECT_EQ(connect.will->topic, "gone/pub");
    EXPECT_EQ(connect.will->payload, bytes_of("bye"));
    EXPECT_EQ(connect.will->qos, QoS::at_least_once);
    EXPECT_TRUE(connect.will->retain);
    if (version == Version::v5) {
      EXPECT_EQ(connect.properties.session_expiry_interval, 60u);
    }
    server.send(Connack{false, reason::success, {}}, version);

    const auto publish = std::get<Publish>(server.next(version));
    EXPECT_EQ(publish.topic, "a/b");
    EXPECT_EQ(publish.payload, bytes_of("hello"));
    EXPECT_EQ(publish.qos, QoS::at_least_once);
    EXPECT_TRUE(publish.retain);
    EXPECT_FALSE(publish.dup);
    EXPECT_NE(publish.packet_id, 0);
    if (version == Version::v5) {
      EXPECT_EQ(publish.properties.content_type, "text/plain");
      EXPECT_EQ(publish.properties.user_properties, (std::vector<std::pair<std::string, std::string>>{{"k", "v"}}));
    }
    server.send(Ack{PacketType::puback, publish.packet_id, reason::success, {}}, version);

    // A client may end with DISCONNECT or simply close: mosquitto_pub on
    // Debian sends one, Homebrew's on macOS closes (CI run 44672174426).
    // Either way it read Lir's PUBACK, which its exit code says.
    if (const auto end = server.last(version)) {
      EXPECT_TRUE(std::holds_alternative<Disconnect>(*end));
    }
    server.close();
  });
  EXPECT_EQ(client.wait(steady_clock::now() + patience), 0) << "mosquitto_pub did not accept Lir's CONNACK and PUBACK; it said:\n" << contents(log);
}

// mosquitto_sub: a SUBSCRIBE with a wildcard, Lir's SUBACK, then a message
// Lir encodes delivered at the granted QoS (2 on 3.1.1 and 5.0: PUBLISH,
// PUBREC, PUBREL, PUBCOMP), which the client must print.
TEST_P(MosquittoInterop, SubscribeAndReceiveWhatLirSends) {
  const Version version = GetParam();
  const QoS qos = QoS::exactly_once;
  Server server;
  dannan::test::ChildProcess client({LIR_MOSQUITTO_SUB, "-h", "127.0.0.1", "-p", std::to_string(server.port()), "-V", level(version), "-i", "lir-interop-sub",
                                     "-q", "2", "-t", "a/+", "-C", "1", "-v"},
                                    log_for("sub-" + level(version)));
  const auto log = log_for("sub-" + level(version));
  exchange(log, [&] {
    server.accept();

    const auto connect = std::get<Connect>(server.next(version));
    EXPECT_EQ(connect.client_id, "lir-interop-sub");
    server.send(Connack{false, reason::success, {}}, version);

    const auto subscribe = std::get<Subscribe>(server.next(version));
    ASSERT_EQ(subscribe.subscriptions.size(), 1u);
    EXPECT_EQ(subscribe.subscriptions[0].filter, "a/+");
    EXPECT_EQ(subscribe.subscriptions[0].qos, qos);
    server.send(Suback{subscribe.packet_id, {}, {reason::granted_qos_2}}, version);

    Publish message{false, qos, false, "a/x", 7, {}, bytes_of("world")};
    if (version == Version::v5) message.properties.user_properties = {{"from", "lir"}};
    server.send(message, version);
    const auto pubrec = std::get<Ack>(server.next(version));
    EXPECT_EQ(pubrec.type, PacketType::pubrec);
    EXPECT_EQ(pubrec.packet_id, 7);
    server.send(Ack{PacketType::pubrel, 7, reason::success, {}}, version);
    const auto pubcomp = std::get<Ack>(server.next(version));
    EXPECT_EQ(pubcomp.type, PacketType::pubcomp);
    EXPECT_EQ(pubcomp.packet_id, 7);

    EXPECT_EQ(client.line(steady_clock::now() + patience), "a/x world");
    EXPECT_TRUE(std::holds_alternative<Disconnect>(server.next(version)));
    server.close();
  });
  EXPECT_EQ(client.wait(steady_clock::now() + patience), 0) << "mosquitto_sub did not accept Lir's packets; it said:\n" << contents(log);
}

// A refusal Lir encodes, read by the real client: it must give up rather than
// publish (MQTT 5.0's Not authorized, 3.x's return code 5).
TEST_P(MosquittoInterop, ARefusedConnectIsUnderstood) {
  const Version version = GetParam();
  Server server;
  dannan::test::ChildProcess client(
      {LIR_MOSQUITTO_PUB, "-h", "127.0.0.1", "-p", std::to_string(server.port()), "-V", level(version), "-i", "lir-interop-refused", "-t", "a/b", "-m", "x"},
      log_for("refused-" + level(version)));
  server.accept();
  EXPECT_TRUE(std::holds_alternative<Connect>(server.next(version)));
  server.send(Connack{false, version == Version::v5 ? reason::not_authorized : uint8_t{5}, {}}, version);
  const auto code = client.wait(steady_clock::now() + patience);
  ASSERT_TRUE(code.has_value());
  EXPECT_NE(*code, 0) << "mosquitto_pub carried on after a refused CONNECT";
}

}  // namespace
}  // namespace lir::mqtt
