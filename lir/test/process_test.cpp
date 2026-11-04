//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The process test: lir-node (LIR_NODE_BINARY) as a child, one and then two
// clustered, served to the mosquitto command-line clients, a third-party
// MQTT implementation. Skipped where mosquitto is not installed.

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace lir {
namespace {

using namespace std::chrono_literals;
using dannan::test::ChildProcess;
using Clock = std::chrono::steady_clock;

constexpr const char* mosquitto_pub = LIR_MOSQUITTO_PUB;
constexpr const char* mosquitto_sub = LIR_MOSQUITTO_SUB;

bool have_mosquitto() {
  return std::string(mosquitto_pub).find("NOTFOUND") == std::string::npos && std::string(mosquitto_sub).find("NOTFOUND") == std::string::npos;
}

struct NodeProcess {
  std::unique_ptr<ChildProcess> child;
  uint16_t port = 0;
  uint16_t mqtt_port = 0;

  // Anonymous clients are allowed unless `extra` says otherwise.
  NodeProcess(const std::filesystem::path& dir, const std::string& name, std::optional<uint16_t> bootstrap,
              const std::vector<std::string>& extra = {"--allow-anonymous"}) {
    std::vector<std::string> argv{LIR_NODE_BINARY,
                                  "--key",
                                  (dir / "cluster.key").string(),
                                  "--state",
                                  (dir / name).string(),
                                  "--listen",
                                  "127.0.0.1",
                                  "--port",
                                  "0",
                                  "--advertise",
                                  "127.0.0.1",
                                  "--mqtt-listen",
                                  "127.0.0.1",
                                  "--mqtt-port",
                                  "0",
                                  "--http-port",
                                  "0",
                                  "--heartbeat-ms",
                                  "200",
                                  "--status-ms",
                                  "60000"};
    argv.insert(argv.end(), extra.begin(), extra.end());
    if (bootstrap) {
      argv.push_back("--bootstrap");
      argv.push_back("127.0.0.1:" + std::to_string(*bootstrap));
    }
    child = std::make_unique<ChildProcess>(argv, dir / (name + ".log"));
    const auto line = child->line(Clock::now() + 10s);
    std::smatch match;
    if (!line || !std::regex_search(*line, match, std::regex(R"("port":(\d+),"mqtt_port":(\d+))"))) {
      ADD_FAILURE() << "lir-node did not report listening: " << line.value_or("(nothing)");
      return;
    }
    port = static_cast<uint16_t>(std::stoi(match[1]));
    mqtt_port = static_cast<uint16_t>(std::stoi(match[2]));
  }
};

// Runs a mosquitto client to its end: its exit status and what it printed.
std::pair<std::optional<int>, std::string> run(const std::filesystem::path& dir, std::vector<std::string> argv, std::chrono::seconds bound = 15s) {
  ChildProcess child(argv, dir / "client.log");
  std::string out;
  const auto deadline = Clock::now() + bound;
  while (auto line = child.line(deadline)) out += *line + "\n";
  return {child.wait(deadline), out};
}

class ProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!have_mosquitto()) GTEST_SKIP() << "mosquitto_pub and mosquitto_sub are not installed";
    std::ofstream(dir.path() / "cluster.key") << "lir process test";
  }
  std::vector<std::string> pub(uint16_t port, const std::string& topic, const std::string& message, const std::string& version, bool retain = false) {
    std::vector<std::string> argv{mosquitto_pub, "-h", "127.0.0.1", "-p", std::to_string(port), "-t", topic, "-m", message, "-q", "1", "-V", version};
    if (retain) argv.push_back("-r");
    return argv;
  }
  std::vector<std::string> sub(uint16_t port, const std::string& filter, const std::string& version, int wait_seconds) {
    return {mosquitto_sub, "-h", "127.0.0.1", "-p", std::to_string(port),        "-t", filter, "-C", "1", "-v", "-q",
            "1",           "-V", version,     "-W", std::to_string(wait_seconds)};
  }

  dannan::test::TempDir dir;
};

TEST_F(ProcessTest, MosquittoClientsAgainstOneNode) {
  NodeProcess node(dir.path(), "a", std::nullopt);
  ASSERT_NE(node.mqtt_port, 0);
  for (const auto* version : {"mqttv31", "mqttv311", "mqttv5"}) {
    const std::string topic = std::string("state/") + version;
    const auto [published, unused] = run(dir.path(), pub(node.mqtt_port, topic, "on", version, true));
    ASSERT_EQ(published, 0) << version;
    const auto [status, out] = run(dir.path(), sub(node.mqtt_port, "state/#", version, 5));
    EXPECT_EQ(status, 0) << version;
    EXPECT_NE(out.find("state/mqttv31 on"), std::string::npos) << out;
  }
}

TEST_F(ProcessTest, TwoNodesInTwoProcessesAreOneBroker) {
  NodeProcess first(dir.path(), "a", std::nullopt);
  ASSERT_NE(first.port, 0);
  NodeProcess second(dir.path(), "b", first.port);
  ASSERT_NE(second.mqtt_port, 0);

  // Live: a subscriber on the second node, publishes on the first until it
  // has one (the subscription's interest takes a gossip round to reach the
  // first node).
  ChildProcess listener(sub(second.mqtt_port, "room/+/temp", "mqttv5", 20), dir.path() / "listener.log");
  std::optional<int> status;
  for (int i = 0; i < 60 && !status; ++i) {
    ASSERT_EQ(run(dir.path(), pub(first.mqtt_port, "room/1/temp", "21.5", "mqttv311")).first, 0);
    status = listener.wait(Clock::now() + 300ms);
  }
  EXPECT_EQ(status, 0);
  EXPECT_EQ(listener.line(Clock::now() + 1s), "room/1/temp 21.5");

  // Retained: set on the second node, read back from the first.
  ASSERT_EQ(run(dir.path(), pub(second.mqtt_port, "door", "open", "mqttv5", true)).first, 0);
  std::optional<int> read;
  std::string out;
  for (int i = 0; i < 20 && read != 0; ++i) std::tie(read, out) = run(dir.path(), sub(first.mqtt_port, "door", "mqttv311", 1));
  EXPECT_EQ(read, 0);
  EXPECT_EQ(out, "door open\n");
}

TEST_F(ProcessTest, AnonymousClientsAreRefusedByDefault) {
  NodeProcess node(dir.path(), "a", std::nullopt, {});
  ASSERT_NE(node.mqtt_port, 0);
  for (const std::string version : {"mqttv31", "mqttv311", "mqttv5"}) {
    const auto [status, out] = run(dir.path(), pub(node.mqtt_port, "t", "x", version));
    EXPECT_NE(status, 0) << version << ": " << out;
  }
}

TEST_F(ProcessTest, TheConfigFileSetsTheNode) {
  // Anonymous clients allowed by the file alone, no flag.
  std::ofstream(dir.path() / "config.yaml") << "auth:\n  allow_anonymous: true\nsystem:\n  off: [roster]\n";
  NodeProcess node(dir.path(), "a", std::nullopt, {"--config", (dir.path() / "config.yaml").string()});
  ASSERT_NE(node.mqtt_port, 0);
  const auto [status, out] = run(dir.path(), pub(node.mqtt_port, "t", "x", "mqttv5"));
  EXPECT_EQ(status, 0) << out;
}

TEST_F(ProcessTest, ABadSettingIsRefusedNamingItsSource) {
  std::ofstream(dir.path() / "bad.yaml") << "limits:\n  receive_maximum: 0\n";
  ChildProcess child({LIR_NODE_BINARY, "--key", (dir.path() / "cluster.key").string(), "--state", (dir.path() / "bad").string(), "--config",
                      (dir.path() / "bad.yaml").string()},
                     dir.path() / "bad.log");
  EXPECT_EQ(child.wait(Clock::now() + 10s), 2);
  std::ifstream log(dir.path() / "bad.log");
  const std::string said((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  EXPECT_NE(said.find("bad.yaml"), std::string::npos) << said;
  EXPECT_NE(said.find("limits.receive_maximum"), std::string::npos) << said;
}

}  // namespace
}  // namespace lir
