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
//            [--max-connections N]
//
// The limits default to the design's moderate set (section 4): 1 MiB
// packets, receive maximum 1024, 10,000 queued per offline session, session
// expiry capped at 7 days, 4 MiB queued for a slow client, 10,000 clients.
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
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

Options parse(int argc, char** argv) {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc) throw std::runtime_error(option + " needs a value");
      return argv[i];
    };
    if (option == "--version")
      out.version = true;
    else if (option == "--key")
      out.key = value();
    else if (option == "--state")
      out.state = value();
    else if (option == "--listen")
      out.listen = value();
    else if (option == "--advertise")
      out.advertise = value();
    else if (option == "--port")
      out.port = static_cast<uint16_t>(std::stoi(value()));
    else if (option == "--mqtt-listen")
      out.mqtt_listen = value();
    else if (option == "--mqtt-port")
      out.mqtt_port = static_cast<uint16_t>(std::stoi(value()));
    else if (option == "--bootstrap")
      out.bootstrap.push_back(endpoint(value()));
    else if (option == "--name")
      out.name = value();
    else if (option == "--domain")
      out.domain = value();
    else if (option == "--heartbeat-ms")
      out.heartbeat = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--status-ms")
      out.status = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--seconds")
      out.seconds = std::stoi(value());
    else if (option == "--log-level")
      out.log_level = dannan::status::parse_log_level(value());
    else if (option == "--mqtt-tls-port")
      out.mqtt_tls_port = std::stoi(value());
    else if (option == "--https-port")
      out.https_port = std::stoi(value());
    else if (option == "--ws-port")
      out.ws_port = std::stoi(value());
    else if (option == "--ws-listen")
      out.ws_listen = value();
    else if (option == "--wss-port")
      out.wss_port = std::stoi(value());
    else if (option == "--wss-listen")
      out.wss_listen = value();
    else if (option == "--max-packet-size")
      out.limits.maximum_packet_size = static_cast<uint32_t>(std::stoul(value()));
    else if (option == "--receive-maximum")
      out.limits.receive_maximum = static_cast<uint16_t>(std::stoul(value()));
    else if (option == "--max-queued")
      out.limits.maximum_queued = std::stoul(value());
    else if (option == "--max-session-expiry")
      out.limits.maximum_session_expiry = static_cast<uint32_t>(std::stoul(value()));
    else if (option == "--max-outbound-bytes")
      out.maximum_outbound_bytes = std::stoul(value());
    else if (option == "--max-connections")
      out.limits.maximum_connections = std::stoul(value());
    else if (option == "--no-system-topics") {
      const auto which = value();
      if (which == "nodes")
        out.system.nodes = false;
      else if (which == "cluster")
        out.system.cluster = false;
      else if (which == "clients")
        out.system.clients = false;
      else if (which == "roster")
        out.system.roster = false;
      else
        throw std::runtime_error("--no-system-topics: one of nodes, cluster, clients, roster; got " + which);
    } else if (option == "--allow-anonymous")
      out.allow_anonymous = true;
    else if (option == "--anonymous-scope")
      out.anonymous_scopes.push_back(value());
    else if (option == "--https-listen")
      out.https_listen = value();
    else if (option == "--mqtt-tls-listen")
      out.mqtt_tls_listen = value();
    else if (option == "--tls-client-ca")
      out.tls_client_ca = value();
    else if (option == "--tls-client-certificate") {
      const auto mode = value();
      if (mode != "required" && mode != "optional") throw std::runtime_error("--tls-client-certificate: required or optional, got " + mode);
      out.tls_client_certificate_required = mode == "required";
    } else if (option == "--tls-certificate")
      out.tls_certificate = value();
    else if (option == "--tls-key")
      out.tls_key = value();
    else if (option == "--http-port")
      out.http_port = std::stoi(value());
    else if (option == "--http-listen")
      out.http_listen = value();
    else if (option == "--session-standbys")
      out.session_standbys = static_cast<size_t>(std::stoul(value()));
    else if (option == "--log-format")
      out.log_format = dannan::status::parse_log_format(value());
    else
      throw std::runtime_error("unknown option " + option);
  }
  if (!out.version && (out.key.empty() || out.state.empty())) throw std::runtime_error("--key and --state are required");
  if ((out.mqtt_tls_port >= 0 || out.https_port >= 0) && (out.tls_certificate.empty() || out.tls_key.empty()))
    throw std::runtime_error("--mqtt-tls-port and --https-port need --tls-certificate and --tls-key");
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
