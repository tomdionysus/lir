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

#include "dannan/config/node_settings.h"
#include "lir/edge/settings.h"

// lir-node's settings (Dannan's docs/config.md): its listeners named as every
// program of the stack names its own, Lir's own keys, and Lir's refusals, in
// the reader's words. The reader is built as Dannan's node-program helper
// builds it: Dannan's node settings, then Lir's.
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

config::Settings read_raw(const std::vector<std::string>& arguments, const Variables& variables = {}) {
  config::Reader reader;
  reader.environment = &variables;
  config::add_node_settings(reader, settings_prefix);
  add_settings(reader);
  return reader.read(arguments);
}

LirSettings read(std::vector<std::string> arguments, const Variables& variables = {}) { return lir_settings(read_raw(arguments, variables)); }

// A cluster key, which Dannan's settings read: any file that is not empty.
std::string key_path() {
  const auto dir = std::filesystem::path(DANNAN_TEST_TEMP_ROOT) / "edge-settings";
  std::filesystem::create_directories(dir);
  const auto path = dir / "cluster.key";
  std::ofstream(path) << "a cluster key for the settings tests\n";
  return path.string();
}

TEST(EdgeSettings, TheDefaultsServeMqttAndRefuseAnonymousClients) {
  const auto settings = read({});
  EXPECT_EQ(settings.server.mqtt.address, "0.0.0.0");
  EXPECT_EQ(settings.server.mqtt.port, 1883);
  EXPECT_FALSE(settings.server.mqtts);
  EXPECT_FALSE(settings.server.ws);
  EXPECT_FALSE(settings.server.wss);
  EXPECT_FALSE(settings.anonymous);
}

// A listener is named as every program names its own; the flag and the
// variable are made from the key, and the flag wins.
TEST(EdgeSettings, AListenerIsNamedAsTheStacksAre) {
  Variables variables;
  variables.values["LIR_MQTT_LISTEN_PORT"] = "1884";
  variables.values["LIR_MQTT_MAXIMUM_PACKET_SIZE"] = "4096";
  EXPECT_EQ(read({}, variables).server.mqtt.port, 1884);
  EXPECT_EQ(read({}, variables).server.limits.maximum_packet_size, 4096u);
  EXPECT_EQ(read({"--mqtt-listen-port", "1885"}, variables).server.mqtt.port, 1885);
  EXPECT_EQ(read({"--mqtt-listen-address", "127.0.0.1"}).server.mqtt.address, "127.0.0.1");
}

// Dannan's node settings are under cluster.*, with Lir's prefix.
TEST(EdgeSettings, TheNodesDannanSettingsAreUnderCluster) {
  Variables variables;
  variables.values["LIR_CLUSTER_LISTEN_PORT"] = "7500";
  const auto raw = read_raw({"--cluster-name", "node-a", "--cluster-state", "/var/lib/lir", "--cluster-key", key_path()}, variables);
  const auto node = config::node_config(raw);
  EXPECT_EQ(node.port, 7500);
  EXPECT_EQ(node.node_name, "node-a");
  EXPECT_NO_THROW(require_node(raw, node));
}

TEST(EdgeSettings, ANodeNeedsAStateAndAWayToProveItself) {
  const auto keyed = read_raw({"--cluster-key", key_path()});
  EXPECT_THROW(require_node(keyed, config::node_config(keyed)), config::ConfigError);
  const auto stated = read_raw({"--cluster-state", "/s"});
  EXPECT_THROW(require_node(stated, config::node_config(stated)), config::ConfigError);
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

// mqtts and wss are served when given their pair, at their default ports
// unless told; mqtt and ws take none.
TEST(EdgeSettings, TlsListenersAreServedWithTheirPair) {
  const auto settings = read({"--mqtts-tls-certificate", "/c.pem", "--mqtts-tls-key", "/k.pem", "--wss-tls-certificate", "/c.pem", "--wss-tls-key", "/k.pem",
                              "--wss-listen-port", "9443", "--mqtt-client-certificates", "optional"});
  ASSERT_TRUE(settings.server.mqtts);
  ASSERT_TRUE(settings.server.mqtts->tls);
  EXPECT_EQ(settings.server.mqtts->tls->chain, "/c.pem");
  EXPECT_EQ(settings.server.mqtts->port, 8883);
  ASSERT_TRUE(settings.server.wss);
  EXPECT_EQ(settings.server.wss->port, 9443);
  EXPECT_EQ(settings.server.client_certificates, dannan::listen::ClientCertificates::optional);
  EXPECT_THROW(read({"--mqtts-tls-certificate", "/c.pem"}), config::ConfigError);
  EXPECT_THROW(read({"--mqtts-listen-port", "8884"}), config::ConfigError);
  EXPECT_THROW(read({"--mqtt-tls-certificate", "/c.pem", "--mqtt-tls-key", "/k.pem"}), config::ConfigError);
  EXPECT_THROW(read({"--mqtt-client-certificates", "sometimes"}), config::ConfigError);
}

TEST(EdgeSettings, WebSocketIsServedWhenGivenAPort) {
  EXPECT_FALSE(read({}).server.ws);
  const auto settings = read({"--ws-listen-port", "8081"});
  ASSERT_TRUE(settings.server.ws);
  EXPECT_EQ(settings.server.ws->port, 8081);
  EXPECT_THROW(read({"--ws-listen-port", "8081", "--ws-tls-certificate", "/c.pem", "--ws-tls-key", "/k.pem"}), config::ConfigError);
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
