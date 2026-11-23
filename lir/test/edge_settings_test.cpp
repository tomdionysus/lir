//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "lir/edge/settings.h"

// lir-node's settings (Dannan's docs/config.md): the stack's names, Dannan's
// node settings under cluster.*, and Lir's own refusals, in the reader's
// words.
namespace lir::edge {
namespace {

namespace config = dannan::config;

class Variables final : public config::Environment {
 public:
  std::optional<std::string> get(const std::string& name) const override {
    const auto found = values.find(name);
    return found == values.end() ? std::nullopt : std::optional<std::string>(found->second);
  }
  std::map<std::string, std::string> values;
};

// A cluster key, which Dannan's settings read: any file that is not empty.
std::string key_path() {
  const auto dir = std::filesystem::path(DANNAN_TEST_TEMP_ROOT) / "edge-settings";
  std::filesystem::create_directories(dir);
  const auto path = dir / "cluster.key";
  std::ofstream(path) << "a cluster key for the settings tests\n";
  return path.string();
}

NodeSettings read(std::vector<std::string> arguments, const Variables& variables = {}) {
  // What a node needs from Dannan's keys to start at all.
  const std::vector<std::string> base = {"--cluster-state", "/var/lib/lir", "--cluster-key", key_path()};
  arguments.insert(arguments.begin(), base.begin(), base.end());
  return node_settings(node_reader(&variables).read(arguments));
}

TEST(EdgeSettings, TheDefaultsServeMqttAndRefuseAnonymousClients) {
  const auto settings = read({});
  EXPECT_EQ(settings.server.address, "0.0.0.0");
  EXPECT_EQ(settings.server.port, 1883);
  EXPECT_FALSE(settings.server.tls);
  EXPECT_FALSE(settings.anonymous);
  EXPECT_EQ(settings.node.state_path, "/var/lib/lir");
}

// The flag and the variable are made from the key; the flag wins.
TEST(EdgeSettings, AKeyIsAFlagAndAVariable) {
  Variables variables;
  variables.values["LIR_MQTT_PORT"] = "1884";
  variables.values["LIR_MQTT_MAXIMUM_PACKET_SIZE"] = "4096";
  EXPECT_EQ(read({}, variables).server.port, 1884);
  EXPECT_EQ(read({}, variables).server.limits.maximum_packet_size, 4096u);
  EXPECT_EQ(read({"--mqtt-port", "1885"}, variables).server.port, 1885);
}

// Dannan's node settings are under cluster.*, with Lir's prefix.
TEST(EdgeSettings, TheNodesDannanSettingsAreUnderCluster) {
  Variables variables;
  variables.values["LIR_CLUSTER_LISTEN_PORT"] = "7500";
  const auto settings = read({"--cluster-name", "node-a"}, variables);
  EXPECT_EQ(settings.node.port, 7500);
  EXPECT_EQ(settings.node.node_name, "node-a");
}

TEST(EdgeSettings, ANodeNeedsAStateAndAWayToProveItself) {
  EXPECT_THROW(node_settings(node_reader().read({"--cluster-key", key_path()})), config::ConfigError);
  EXPECT_THROW(node_settings(node_reader().read({"--cluster-state", "/s"})), config::ConfigError);
}

TEST(EdgeSettings, AnonymousClientsAreHeldToTheirFilters) {
  const auto settings = read({"--anonymous-allow", "--anonymous-subscribe", "public/#", "--anonymous-publish", "public/in"});
  ASSERT_TRUE(settings.anonymous);
  EXPECT_EQ(settings.anonymous->subscribe, std::vector<std::string>{"public/#"});
  EXPECT_EQ(settings.anonymous->publish, std::vector<std::string>{"public/in"});
  // Allowed with no filters: connected, and held to nothing.
  EXPECT_TRUE(read({"--anonymous-allow"}).anonymous);
}

TEST(EdgeSettings, AnonymousFiltersWithoutAllowAreRefused) { EXPECT_THROW(read({"--anonymous-subscribe", "public/#"}), config::ConfigError); }

TEST(EdgeSettings, AnInvalidAnonymousFilterStopsTheStart) { EXPECT_THROW(read({"--anonymous-allow", "--anonymous-subscribe", "a/#/b"}), config::ConfigError); }

TEST(EdgeSettings, TlsNeedsACertificateAndItsKey) {
  const auto settings = read({"--mqtt-tls-certificate", "/c.pem", "--mqtt-tls-key", "/k.pem", "--mqtt-tls-client-certificates", "optional"});
  ASSERT_TRUE(settings.server.tls);
  EXPECT_EQ(settings.server.tls->chain, "/c.pem");
  EXPECT_EQ(settings.server.tls_port, 8883);
  EXPECT_EQ(settings.server.client_certificates, dannan::listen::ClientCertificates::optional);
  EXPECT_THROW(read({"--mqtt-tls-certificate", "/c.pem"}), config::ConfigError);
  EXPECT_THROW(read({"--mqtt-tls-port", "8884"}), config::ConfigError);
  EXPECT_THROW(read({"--mqtt-tls-certificate", "/c.pem", "--mqtt-tls-key", "/k.pem", "--mqtt-tls-client-certificates", "sometimes"}), config::ConfigError);
}

TEST(EdgeSettings, TheLogTakesTheStacksSharedKeys) {
  const auto settings = read({"--log-level", "debug", "--log-format", "json"});
  EXPECT_EQ(settings.log_level, dannan::status::LogLevel::debug);
  EXPECT_EQ(settings.log_format, dannan::status::LogFormat::json);
  EXPECT_THROW(read({"--log-level", "loud"}), config::ConfigError);
  EXPECT_THROW(read({"--log-format", "xml"}), config::ConfigError);
}

TEST(EdgeSettings, LughSettingsNeedThePluginAndAnAudience) {
#ifdef LIR_WITH_LUGH
  EXPECT_THROW(read({"--lugh-issuers", "https://lugh.example"}), config::ConfigError);
  const auto settings = read({"--lugh-issuers", "https://lugh.example", "--lugh-audience", "lir"});
  EXPECT_EQ(settings.lugh_issuers, std::vector<std::string>{"https://lugh.example"});
#else
  EXPECT_THROW(read({"--lugh-issuers", "https://lugh.example", "--lugh-audience", "lir"}), config::ConfigError);
#endif
}

}  // namespace
}  // namespace lir::edge
