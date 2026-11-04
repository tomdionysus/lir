//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// lir-node: one node of a Lir cluster. A Dannan node (the cluster's
// network) that advertises "lir" and serves MQTT on its own port. It
// reports what it does as one JSON object per line on stdout:
//
//   {"event":"listening","id":"...","port":N,"mqtt_port":M,"mqtt_tls_port":T,"ws_port":W,"wss_port":S,
//    "http_port":H,"https_port":P}                     (zero: that listener is off)
//   {"event":"status","connections":N,"sessions":N,"retained":N,...}   every --status-ms
//   {"event":"stopped"}
//
// It runs for --seconds (0: until SIGTERM or SIGINT).
//
// Every setting is also a key in a YAML file (--config PATH, $LIR_CONFIG, or
// /usr/local/etc/lir/config.yaml when it exists), in secrets.yaml beside it,
// or a LIR_* variable, the flags winning: `--mqtt-port 1883` is
// `mqtt: {port: 1883}` and LIR_MQTT_PORT (reader() below lists them all).
//
//   lir-node --key FILE --state DIR [--port N] [--listen ADDR]
//            [--mqtt-port N] [--mqtt-listen ADDR] [--advertise HOST]
//            [--bootstrap HOST:PORT]... [--name NAME] [--domain NAME]
//            [--heartbeat-ms N] [--status-ms N] [--seconds N]
//            [--log-level LEVEL] [--log-format text|json]
//            [--session-standbys N] [--http-port N|-1] [--http-listen ADDR]
//            [--mqtt-tls-port N --tls-certificate PEM --tls-key PEM]
//            [--mqtt-tls-listen ADDR] [--https-port N] [--https-listen ADDR]
//            [--tls-client-ca PEM [--tls-client-certificate required|optional]]
//            [--ws-port N] [--ws-listen ADDR] [--wss-port N] [--wss-listen ADDR]
//            [--allow-anonymous [--anonymous-scope SCOPE]...]
//            [--no-system-topics nodes|cluster|clients|roster]...
//            [--max-packet-size BYTES] [--receive-maximum N] [--max-queued N]
//            [--max-session-expiry SECONDS] [--max-outbound-bytes BYTES]
//            [--max-connections N] [--memory-budget BYTES]
//
// The limits default to the design's moderate set (section 4): 1 MiB
// packets, receive maximum 1024, 10,000 queued per offline session, session
// expiry capped at 7 days, 4 MiB queued for a slow client, 10,000 clients;
// 512 MiB of held messages before the node gives way (QoS 0 shed at 80%,
// QoS 1/2 refused at 90%, new clients refused at 100%).
//   lir-node --version
//
// Clients are refused unless --allow-anonymous is given (design, section
// 1); until the Lugh plugin there is nothing to check a credential against,
// so a client presenting one is refused too. An anonymous client may do
// what the --anonymous-scope values say ("lir:pub:F", "lir:sub:F",
// "lir:retain:F"; auth/scopes.h), or anything but write the system topics
// when none is given.

#include <signal.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/node.h"
#include "lir/auth/scopes.h"
#include "lir/cluster/cluster.h"
#include "lir/edge/edge.h"
#include "lir/http/http.h"
#include "lir/system/system.h"
#include "lir/version.h"

namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;

std::atomic<bool> stopping{false};
std::mutex output;

void say(const std::string& line) {
  std::lock_guard lock(output);
  std::cout << line << std::endl;
}

struct Options {
  std::string key;
  std::string state;
  std::string listen = "0.0.0.0";
  std::string advertise;
  uint16_t port = 7437;
  std::string mqtt_listen = "0.0.0.0";
  uint16_t mqtt_port = 1883;
  std::vector<dannan::net::Endpoint> bootstrap;
  std::string name;
  std::string domain;
  std::chrono::milliseconds heartbeat{5000};
  std::chrono::milliseconds status{1000};
  int seconds = 0;
  dannan::status::LogLevel log_level = dannan::status::LogLevel::info;
  dannan::status::LogFormat log_format = dannan::status::LogFormat::text;
  bool version = false;
  size_t session_standbys = 1;
  std::string http_listen = "0.0.0.0";
  // -1: no HTTP endpoint.
  int http_port = 8083;
  // -1: no TLS listener.
  int mqtt_tls_port = -1;
  std::string mqtt_tls_listen = "0.0.0.0";
  std::string tls_certificate;
  std::string tls_key;
  // Client certificates on every TLS listener: the CAs they chain to, and
  // whether one is required.
  std::string tls_client_ca;
  bool tls_client_certificate_required = true;
  // -1: no HTTPS listener. Uses --tls-certificate and --tls-key.
  int https_port = -1;
  std::string https_listen = "0.0.0.0";
  // -1: no MQTT over WebSocket listener.
  int ws_port = -1;
  std::string ws_listen = "0.0.0.0";
  // -1: none. Uses --tls-certificate and --tls-key.
  int wss_port = -1;
  std::string wss_listen = "0.0.0.0";
  lir::system::SystemConfig system;
  lir::broker::Config limits;
  size_t maximum_outbound_bytes = lir::edge::EdgeConfig{}.maximum_outbound_bytes;
  bool allow_anonymous = false;
  std::vector<std::string> anonymous_scopes;
};

dannan::net::Endpoint endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("expected HOST:PORT, got " + text);
  return {text.substr(0, colon), static_cast<uint16_t>(std::stoi(text.substr(colon + 1)))};
}

lir::edge::TlsConfig tls_config(const Options& options, const std::string& listen, int port) {
  lir::edge::TlsConfig tls;
  tls.listen_host = listen;
  tls.port = static_cast<uint16_t>(port);
  tls.certificate_chain = options.tls_certificate;
  tls.private_key = options.tls_key;
  tls.client_ca = options.tls_client_ca;
  tls.require_client_certificate = options.tls_client_certificate_required;
  return tls;
}

// Every setting, its flag, and its variable (LIR_ and the key in capitals),
// read through the stack's one reader (dannan/config/config.h): defaults,
// the file (--config, $LIR_CONFIG, then /usr/local/etc/lir/config.yaml if
// it exists), secrets.yaml beside it, the environment, then the flags.
dannan::config::Reader reader() {
  using Kind = dannan::config::Setting::Kind;
  dannan::config::Reader out;
  out.config_variable = "LIR_CONFIG";
  out.default_path = "/usr/local/etc/lir/config.yaml";
  const auto add = [&](std::string key, Kind kind, std::string flag, std::string what) {
    std::string variable = "LIR_";
    for (const char c : key) variable += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out.settings.push_back({std::move(key), kind, std::move(flag), std::move(variable), std::move(what)});
  };
  add("command.version", Kind::toggle, "--version", "print the version and stop");
  add("cluster.key", Kind::value, "--key", "the cluster key file");
  add("cluster.bootstrap", Kind::list, "--bootstrap", "HOST:PORT");
  add("cluster.session_standbys", Kind::value, "--session-standbys", "a count, 0 to 16");
  add("node.state", Kind::value, "--state", "the state directory");
  add("node.name", Kind::value, "--name", "a name");
  add("node.domain", Kind::value, "--domain", "a failure domain");
  add("node.listen", Kind::value, "--listen", "an address");
  add("node.port", Kind::value, "--port", "a port, 0 to 65535");
  add("node.advertise", Kind::value, "--advertise", "a host");
  add("timing.heartbeat_ms", Kind::value, "--heartbeat-ms", "milliseconds");
  add("timing.status_ms", Kind::value, "--status-ms", "milliseconds");
  add("timing.seconds", Kind::value, "--seconds", "seconds to run, 0 until stopped");
  add("log.level", Kind::value, "--log-level", "a log level");
  add("log.format", Kind::value, "--log-format", "text or json");
  add("mqtt.listen", Kind::value, "--mqtt-listen", "an address");
  add("mqtt.port", Kind::value, "--mqtt-port", "a port, 0 to 65535");
  add("mqtt.tls_listen", Kind::value, "--mqtt-tls-listen", "an address");
  add("mqtt.tls_port", Kind::value, "--mqtt-tls-port", "a port, 0 to 65535, or -1 for none");
  add("websocket.listen", Kind::value, "--ws-listen", "an address");
  add("websocket.port", Kind::value, "--ws-port", "a port, 0 to 65535, or -1 for none");
  add("websocket.tls_listen", Kind::value, "--wss-listen", "an address");
  add("websocket.tls_port", Kind::value, "--wss-port", "a port, 0 to 65535, or -1 for none");
  add("http.listen", Kind::value, "--http-listen", "an address");
  add("http.port", Kind::value, "--http-port", "a port, 0 to 65535, or -1 for none");
  add("http.tls_listen", Kind::value, "--https-listen", "an address");
  add("http.tls_port", Kind::value, "--https-port", "a port, 0 to 65535, or -1 for none");
  add("tls.certificate", Kind::value, "--tls-certificate", "a PEM certificate chain");
  add("tls.key", Kind::value, "--tls-key", "a PEM private key");
  add("tls.client_ca", Kind::value, "--tls-client-ca", "a PEM file of client CAs");
  add("tls.client_certificate", Kind::value, "--tls-client-certificate", "required or optional");
  add("auth.allow_anonymous", Kind::toggle, "--allow-anonymous", "whether anonymous clients are served");
  add("auth.anonymous_scopes", Kind::list, "--anonymous-scope", "a topic scope, lir:pub|sub|retain:FILTER");
  add("system.off", Kind::list, "--no-system-topics", "nodes, cluster, clients or roster");
  add("limits.max_packet_size", Kind::value, "--max-packet-size", "bytes, 16 to 268435455");
  add("limits.receive_maximum", Kind::value, "--receive-maximum", "a count, 1 to 65535");
  add("limits.max_queued", Kind::value, "--max-queued", "a count");
  add("limits.max_session_expiry", Kind::value, "--max-session-expiry", "seconds");
  add("limits.max_outbound_bytes", Kind::value, "--max-outbound-bytes", "bytes");
  add("limits.max_connections", Kind::value, "--max-connections", "a count");
  add("limits.memory_budget", Kind::value, "--memory-budget", "bytes, 0 for none");
  return out;
}

// A port that may be -1 (none).
int port_or_none(const dannan::config::Settings& settings, const std::string& key, int otherwise) {
  const auto text = settings.text(key);
  if (!text) return otherwise;
  if (*text == "-1") return -1;
  return static_cast<int>(settings.number(key, 0, 65535).value_or(0));
}

Options parse(int argc, char** argv) {
  const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
  if (!settings.positional.empty()) throw std::runtime_error("unexpected argument " + settings.positional.front());
  for (const auto& warning : settings.warnings) std::cerr << "lir-node: " << warning << "\n";
  Options out;
  out.version = settings.toggle("command.version").value_or(false);
  if (out.version) return out;
  const auto ms = [&](const std::string& key, std::chrono::milliseconds otherwise) {
    const auto value = settings.number(key, 1, 86400000);
    return value ? std::chrono::milliseconds(*value) : otherwise;
  };
  out.key = settings.path_of("cluster.key").value_or("");
  for (const auto& seed : settings.list("cluster.bootstrap")) out.bootstrap.push_back(endpoint(seed));
  out.session_standbys = settings.number("cluster.session_standbys", 0, 16).value_or(out.session_standbys);
  out.state = settings.path_of("node.state").value_or("");
  out.name = settings.text("node.name").value_or("");
  out.domain = settings.text("node.domain").value_or("");
  out.listen = settings.text("node.listen").value_or(out.listen);
  out.port = static_cast<uint16_t>(settings.number("node.port", 0, 65535).value_or(out.port));
  out.advertise = settings.text("node.advertise").value_or("");
  out.heartbeat = ms("timing.heartbeat_ms", out.heartbeat);
  out.status = ms("timing.status_ms", out.status);
  out.seconds = static_cast<int>(settings.number("timing.seconds", 0, 31536000).value_or(0));
  if (const auto level = settings.text("log.level")) out.log_level = dannan::status::parse_log_level(*level);
  if (const auto format = settings.choice("log.format", {"text", "json"})) out.log_format = dannan::status::parse_log_format(*format);
  out.mqtt_listen = settings.text("mqtt.listen").value_or(out.mqtt_listen);
  out.mqtt_port = static_cast<uint16_t>(settings.number("mqtt.port", 0, 65535).value_or(out.mqtt_port));
  out.mqtt_tls_listen = settings.text("mqtt.tls_listen").value_or(out.mqtt_tls_listen);
  out.mqtt_tls_port = port_or_none(settings, "mqtt.tls_port", out.mqtt_tls_port);
  out.ws_listen = settings.text("websocket.listen").value_or(out.ws_listen);
  out.ws_port = port_or_none(settings, "websocket.port", out.ws_port);
  out.wss_listen = settings.text("websocket.tls_listen").value_or(out.wss_listen);
  out.wss_port = port_or_none(settings, "websocket.tls_port", out.wss_port);
  out.http_listen = settings.text("http.listen").value_or(out.http_listen);
  out.http_port = port_or_none(settings, "http.port", out.http_port);
  out.https_listen = settings.text("http.tls_listen").value_or(out.https_listen);
  out.https_port = port_or_none(settings, "http.tls_port", out.https_port);
  out.tls_certificate = settings.path_of("tls.certificate").value_or("");
  out.tls_key = settings.path_of("tls.key").value_or("");
  out.tls_client_ca = settings.path_of("tls.client_ca").value_or("");
  if (const auto mode = settings.choice("tls.client_certificate", {"required", "optional"})) out.tls_client_certificate_required = *mode == "required";
  out.allow_anonymous = settings.toggle("auth.allow_anonymous").value_or(false);
  out.anonymous_scopes = settings.list("auth.anonymous_scopes");
  for (const auto& off : settings.list("system.off")) {
    if (off == "nodes")
      out.system.nodes = false;
    else if (off == "cluster")
      out.system.cluster = false;
    else if (off == "clients")
      out.system.clients = false;
    else if (off == "roster")
      out.system.roster = false;
    else
      throw settings.refuse("system.off", "nodes, cluster, clients or roster");
  }
  auto& limits = out.limits;
  limits.maximum_packet_size = static_cast<uint32_t>(settings.number("limits.max_packet_size", 16, 268435455).value_or(limits.maximum_packet_size));
  limits.receive_maximum = static_cast<uint16_t>(settings.number("limits.receive_maximum", 1, 65535).value_or(limits.receive_maximum));
  limits.maximum_queued = settings.number("limits.max_queued", 1, UINT32_MAX).value_or(limits.maximum_queued);
  limits.maximum_session_expiry = static_cast<uint32_t>(settings.number("limits.max_session_expiry", 0, UINT32_MAX).value_or(limits.maximum_session_expiry));
  out.maximum_outbound_bytes = settings.number("limits.max_outbound_bytes", 1024, UINT64_MAX).value_or(out.maximum_outbound_bytes);
  limits.maximum_connections = settings.number("limits.max_connections", 1, UINT32_MAX).value_or(limits.maximum_connections);
  limits.memory_budget = settings.number("limits.memory_budget", 0, UINT64_MAX).value_or(limits.memory_budget);
  if (out.key.empty() || out.state.empty()) throw std::runtime_error("--key and --state are required");
  if ((out.mqtt_tls_port >= 0 || out.https_port >= 0 || out.wss_port >= 0) && (out.tls_certificate.empty() || out.tls_key.empty()))
    throw std::runtime_error("--mqtt-tls-port, --wss-port and --https-port need --tls-certificate and --tls-key");
  return out;
}

std::string status_json(lir::edge::Edge& edge) {
  struct Figures {
    size_t connections = 0;
    size_t sessions = 0;
    size_t retained = 0;
    lir::broker::Stats stats;
  };
  const auto f =
      edge.call<Figures>([](lir::broker::Broker& broker) { return Figures{broker.connections(), broker.sessions(), broker.retained(), broker.stats()}; });
  std::ostringstream out;
  out << "{\"event\":\"status\",\"connections\":" << f.connections << ",\"sessions\":" << f.sessions << ",\"retained\":" << f.retained
      << ",\"accepted\":" << f.stats.connections_accepted << ",\"refused\":" << f.stats.connections_refused
      << ",\"protocol_errors\":" << f.stats.protocol_errors << ",\"received\":" << f.stats.messages_received << ",\"delivered\":" << f.stats.messages_delivered
      << "}";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    options = parse(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "lir-node: " << error.what() << "\n";
    return 2;
  }
  if (options.version) {
    std::cout << lir::kVersion << "\n";
    return 0;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });

  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { io.run(); });

  int status = 0;
  try {
    dannan::NodeConfig config;
    config.state_path = options.state;
    config.key = dannan::identity::load_cluster_key(options.key);
    config.listen_host = options.listen;
    config.advertise_host = options.advertise;
    config.port = options.port;
    config.bootstrap = options.bootstrap;
    config.node_name = options.name;
    config.failure_domain = options.domain;
    config.heartbeat = options.heartbeat;
    config.dead_after = options.heartbeat * 6;
    config.telemetry_interval = options.heartbeat;
    config.version = std::string("lir-node ") + lir::kVersion;
    config.log = dannan::status::console_sink(options.log_format, options.log_level);
    dannan::Node node(io, config);
    // Lir's unit of work: one packet of the largest size it accepts. Dannan
    // refuses to start Lir on a node whose budget cannot hold one.
    node.context().budget().declare(dannan::sched::WorkClass::foreground, options.limits.maximum_packet_size);
    lir::edge::EdgeConfig edge_config;
    edge_config.listen_host = options.mqtt_listen;
    edge_config.port = options.mqtt_port;
    edge_config.broker = options.limits;
    edge_config.maximum_outbound_bytes = options.maximum_outbound_bytes;
    if (options.mqtt_tls_port >= 0) {
      edge_config.tls = tls_config(options, options.mqtt_tls_listen, options.mqtt_tls_port);
    }
    if (options.ws_port >= 0) edge_config.websocket = lir::edge::WebSocketConfig{options.ws_listen, static_cast<uint16_t>(options.ws_port)};
    if (options.wss_port >= 0) {
      edge_config.secure_websocket = tls_config(options, options.wss_listen, options.wss_port);
    }
    std::optional<lir::auth::TopicScopes> anonymous;
    if (options.allow_anonymous) {
      if (options.anonymous_scopes.empty()) {
        anonymous = lir::auth::default_anonymous_scopes();
      } else {
        auto parsed = lir::auth::parse_scopes(options.anonymous_scopes);
        if (!parsed.refused.empty()) throw std::runtime_error("--anonymous-scope: not a topic scope: " + parsed.refused.front());
        anonymous = std::move(parsed.scopes);
      }
    }
    lir::auth::AnonymousAuthenticator authenticator(std::move(anonymous));
    lir::edge::Edge edge(io, edge_config, dannan::contract::steady_time_source(), authenticator, node.context().logger("lir"));
    // The cluster's routes are registered before the node starts.
    lir::cluster::ClusterConfig cluster_config;
    cluster_config.session_standbys = options.session_standbys;
    lir::cluster::Cluster cluster(node.context(), edge, cluster_config);
    node.start();
    cluster.start();
    edge.start();
    lir::system::SystemTopics system(node.context(), edge, options.system);
    system.start();
    std::optional<lir::http::HttpServer> http;
    if (options.http_port >= 0) {
      lir::http::HttpConfig http_config;
      http_config.listen_host = options.http_listen;
      http_config.port = static_cast<uint16_t>(options.http_port);
      if (options.https_port >= 0) {
        http_config.tls = tls_config(options, options.https_listen, options.https_port);
      }
      http.emplace(io, edge, http_config, node.context().logger("lir.http"));
      http->start();
    }
    say("{\"event\":\"listening\",\"id\":\"" + dannan::identity::to_string(node.context().identity().id()) + "\",\"port\":" + std::to_string(node.port()) +
        ",\"mqtt_port\":" + std::to_string(edge.port()) + ",\"mqtt_tls_port\":" + std::to_string(edge.tls_port()) +
        ",\"ws_port\":" + std::to_string(edge.websocket_port()) + ",\"wss_port\":" + std::to_string(edge.secure_websocket_port()) +
        ",\"http_port\":" + std::to_string(http ? http->port() : 0) + ",\"https_port\":" + std::to_string(http ? http->tls_port() : 0) + "}");

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    auto next_status = std::chrono::steady_clock::now() + options.status;
    while (!stopping && (options.seconds == 0 || std::chrono::steady_clock::now() < until)) {
      if (std::chrono::steady_clock::now() >= next_status) {
        say(status_json(edge));
        next_status += options.status;
      }
      std::this_thread::sleep_for(50ms);
    }
    if (http) http->stop();
    system.stop();
    edge.stop();
    cluster.stop();
    node.stop();
  } catch (const std::exception& error) {
    std::cerr << "lir-node: " << error.what() << "\n";
    status = 1;
  }
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  say("{\"event\":\"stopped\"}");
  return status;
}
