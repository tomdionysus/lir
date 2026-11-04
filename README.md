# Lir - The Message Broker

Lir is the event system for a cluster of trusted machines. It is a C++20 library and a
node binary that turn a set of nodes into one MQTT broker: connect to any node, publish
or subscribe on any node, drop the connection and resume the session on another, and the
cluster behaves as a single broker. It speaks MQTT 3.1, 3.1.1 and 5.0 in full, over TCP,
TLS and WebSocket, with an HTTP publish and Server-Sent Events endpoint beside it for browsers and
curl. It runs on [Dannan](https://github.com/tomdionysus/dannan), the network, and
persists in [Dagda](https://github.com/tomdionysus/dagda), the store.

## Why it exists

Publish and subscribe is how the systems in a cluster tell each other, and the people
watching them, that something happened: a file arrived, a call was answered, a node went
away. The usual answer is one MQTT broker on one machine, and that is what AthenaSIP (a
SIP server) leaned on. It works until that machine is rebooted, filled or cut off, and
then every system that publishes or listens stops at once. Clustered brokers exist, but
they bring their own membership, their own transport and their own idea of which node is
in charge, beside the cluster that is already there.

Lir is the broker written once for this stack:

- **No broker machine.** Every node is a broker. There is no leader, no coordinator and
  no node whose loss stops the bus. Both sides of a network split keep serving, and merge
  when it heals.
- **All of MQTT.** Every client library and tool that speaks MQTT works unchanged: QoS 0,
  1 and 2, retained messages, wills, wildcards, shared subscriptions, persistent sessions,
  topic aliases, message expiry, request and response, and enhanced authentication.
  "Supports MQTT" is defined by the specification's conformance statements, each tested.
- **On the cluster that is already there.** Membership, reachability through NATs, node
  identity, placement and the wire between nodes are Dannan's; what must survive a node
  is Dagda's. Lir adds only what makes it a broker, and has no copy of anything below it.
- **The cluster's own state as topics.** Each node publishes its own state and Dannan's
  view of the cluster as retained messages, so a dashboard or a script needs nothing but
  an MQTT client.

## What it does

- **MQTT front end.** A codec for every packet and property of MQTT 3.1, 3.1.1 and 5.0,
  written here, refusing every malformed packet the specification says must be refused.
  Listeners on plain TCP, TLS, WebSocket and WebSocket over TLS, each on its own port;
  over WebSocket, MQTT travels in binary frames with the `mqtt` subprotocol, as the
  specification's section 6 says, so browser MQTT libraries connect directly.
- **The broker.** Sessions, QoS 0, 1 and 2 with the full PUBREC, PUBREL and PUBCOMP
  exchange, retained messages, wills with delay, `$share` groups, No Local, Retain As
  Published, subscription identifiers, topic aliases in both directions, Receive Maximum,
  maximum packet size, message and session expiry, and optional queueing of QoS 0 for
  offline sessions. Slow clients are closed, never allowed to grow the node's memory.
- **The cluster.** Publishes fan out directly to the nodes with matching subscribers over
  Dannan's channels; a node learns who is subscribed from the subscriptions each node
  announces. A client that reconnects elsewhere has its session moved to the new node.
  Each persistent session is mirrored to standby nodes (one by default) chosen by
  Dannan's placement, and a standby takes the session over if its node is lost. A node
  with clearly more clients than the cluster's average (20% over, `--rebalance-over`)
  sends a new MQTT 5.0 client to the least-loaded node instead, with Server Reference;
  live connections are never moved, and 3.x clients, which cannot be redirected, are
  always served. Retained
  messages are ordered by a Lamport counter, then the node id, so every node agrees on
  the winner whatever order they arrive in.
- **System topics**, JSON, each switchable and on by default. Clients may read them,
  never write them:
  - `$lir/nodes/<node id>`, retained: connections, sessions, retained count, message
    counters; `$dannan/nodes/<node id>`, retained: the node's record, its peers and their
    latency. Cleared when the node leaves.
  - `$lir/cluster`, retained: nodes, connections, sessions, messages received a second, and
    whether every node runs the same limits, from figures each node carries in its Dannan
    record.
  - `$lir/clients/connected` and `$lir/clients/disconnected`: each client's connection
    and how it ended (by the client, the server or the network, with the reason code).
  - `$dannan/roster/joined`, `/left` and `/changed`: the cluster's membership, once per
    change.
- **HTTP.** `POST /publish?topic=T` publishes the body; `GET /subscribe?filter=F` streams
  matching messages as Server-Sent Events, retained messages first. Each request is a
  short-lived MQTT client of the broker, so authentication, retained messages and the
  cluster apply exactly as they do to any client. A bearer token is passed to the
  authenticator as the CONNECT password. HTTPS on its own port.
- **Rules.** Declarative routes, stored as retained JSON on `$lir/rules/<name>`, so MQTT
  is their API and they replicate to every node: match a topic filter and conditions on
  JSON payload fields, properties and the publisher, then republish to a templated topic
  (reshaping the payload if asked), set properties, or drop. Applied where a message is
  published, before it reaches anyone; chains stop at 8 hops.
- **Authentication and topic scopes.** A pluggable `Authenticator` decides CONNECT
  and enhanced authentication, and gives each connection a grant: what it may
  publish, retain and subscribe to, as topic filters (`lir:pub:F`, `lir:retain:F`,
  `lir:sub:F`, the grammar proposed for Lugh's tokens). Every publish, will and
  subscribe is checked against it, and the connection is closed when the
  credential behind it ends. Anonymous clients are refused unless a node allows
  them.

## The stack

Lir is one of a stack of projects, each its own library and repository, written to the
same conventions. No code is duplicated between them: a piece needed by more than one
lives in exactly one.

```
   Lugh            Lir             Macha, AthenaSIP, ...      hosted systems
     |               |                  |
   Dagda: the store  (domains, sealing, objects, heads, leases)
     |
   Dannan: the network  (reachability, routes, channels, membership,
                         identity, placement, work classes, status)
```

- **[Dannan](https://github.com/tomdionysus/dannan)** is the network: reachability
  through NATs, routes and channels between nodes, membership, identity, placement, work
  classes, logging and the shared primitives. Lir uses it for every byte between nodes.
- **[Dagda](https://github.com/tomdionysus/dagda)** is the store: objects, claims, leases
  and heads with merges their owner supplies, at a durability level each write chooses.
  Lir keeps its retained messages, in-flight messages and sessions there.
- **[Lugh](https://github.com/tomdionysus/lugh)** is the directory and sign-on. Lir never
  links it; to Lir it is an identity provider, and a Lugh authenticator plugin turns a
  Lugh token into a client's topic scopes.

Hosted systems never address one another. Other systems use Lir as any MQTT client
would.

## How to use it

### Build

A C++20 compiler, CMake 3.24 or later, Boost 1.88 (Asio and Beast), OpenSSL 3, and
GoogleTest for the tests. Dannan is fetched from `../dannan` at the pinned release (0.4.0),
so clone it beside this repository; building Dannan needs yaml-cpp.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIR_BUILD_TESTING=ON
cmake --build build -j8
./build/lir_tests
```

Pass `-DCMAKE_PREFIX_PATH` where Boost and OpenSSL are not on the default path (on macOS
with MacPorts: `"/opt/local/libexec/boost/1.88;/opt/local"`). The process tests use
`mosquitto_pub` and `mosquitto_sub` when they are installed.

### Run a node

`lir-node` is one node of a cluster: a Dannan node that advertises `lir` and serves MQTT.
It reports what it does as one JSON object per line.

```
printf '%s' 'a long shared secret' > cluster.key
./build/lir-node --key cluster.key --state ./a --advertise 127.0.0.1 --allow-anonymous
#   {"event":"listening","id":"...","port":7437,"mqtt_port":1883,"http_port":8083,...}
```

Clients are refused unless `--allow-anonymous` is given; until the Lugh
authenticator there is nothing to check a credential against. An anonymous client
may publish, retain and subscribe anywhere and read the system topics, or only
what `--anonymous-scope` allows, repeated:

```
./build/lir-node ... --allow-anonymous \
  --anonymous-scope 'lir:sub:sensors/#' --anonymous-scope 'lir:pub:sensors/#'
```

Any MQTT client can now use the node:

```
mosquitto_sub -h 127.0.0.1 -p 1883 -t 'sensors/#' -V mqttv5 &
mosquitto_pub -h 127.0.0.1 -p 1883 -t sensors/room1/temp -m 21.5 -q 1 -r
```

### Configure

Every flag is also a key in a YAML file and a `LIR_*` variable, read by the stack's one
settings reader (Dannan's): the file (`--config PATH`, `$LIR_CONFIG`, or
`/usr/local/etc/lir/config.yaml` when it exists), then `secrets.yaml` beside it, then the
environment, then the flags, each overriding the one before. `--mqtt-port 1883` is
`mqtt: {port: 1883}` and `LIR_MQTT_PORT`:

```yaml
cluster:
  key: /etc/lir/cluster.key
  bootstrap: [10.0.0.11:7437]
node:
  state: /var/lib/lir
  advertise: 10.0.0.10
mqtt:
  port: 1883
websocket:
  port: 8080
auth:
  allow_anonymous: true
  anonymous_scopes: ["lir:sub:sensors/#", "lir:pub:sensors/#"]
limits:
  max_connections: 20000
```

A value that cannot be used is refused at start, naming the file, variable or flag that
gave it. `tools/lir_node.cpp`, `reader()`, lists every key.

### Run a cluster

Start more nodes with the first as a bootstrap seed. The seed is not a master: once
joined, every node is equal.

```
./build/lir-node --key cluster.key --state ./b --advertise 127.0.0.1 --port 7438 \
  --mqtt-port 1884 --http-port 8084 --bootstrap 127.0.0.1:7437 --allow-anonymous
```

A subscriber on one node receives what is published on the other, retained messages are
on both, and a client with a persistent session can reconnect to either.
`--session-standbys N` sets how many other nodes mirror each session.

### Limits

Each node enforces its own limits, sized by default for about 10,000 messages a second
across a cluster: 1 MiB packets (`--max-packet-size`), a receive maximum of 1024
(`--receive-maximum`), 10,000 messages queued per offline session (`--max-queued`),
session expiry capped at 7 days (`--max-session-expiry`), 4 MiB queued for a slow client
before it is closed (`--max-outbound-bytes`), and 10,000 clients, past which a new one is
told the server is busy (`--max-connections`). Under memory pressure a node gives way
in order rather than failing: past 80% of its budget for held messages
(`--memory-budget`, 512 MiB; `--shed-percent`) it stops queueing QoS 0 and sheds what
is queued, oldest first; past 90% (`--refuse-publish-percent`) it refuses QoS 1 and 2
publishes (0x97); at 100% (`--refuse-connect-percent`) it refuses new clients. The
bytes are also leased from the node's Dannan memory ledger, which every system on the
node shares, so when the node as a whole runs short Lir refuses new clients too. Existing sessions are never evicted. `$lir/nodes/<id>` reports the bytes held
and the pressure, and `$lir/cluster` says whether every node runs the same limits.

### Rules

A rule is a retained message on `$lir/rules/<name>`; an empty retained publish deletes
it. Writing one needs a grant to retain there (`lir:retain:$lir/rules/#`):

```
mosquitto_pub ... -r -t '$lir/rules/hot-rooms' -m '{
  "from": "sensors/+/temp",
  "where": {"payload.celsius": {">": 30}},
  "do": [{"republish": {"topic": "alerts/{1}/hot", "qos": 1,
                        "transform": {"pick": ["celsius"], "add": {"room": "{1}"}}}}]}'
```

Conditions test `topic`, `client`, `qos`, `retain`, `content_type`, `user.<name>` and
`payload.<path>` with `=`, `!=`, `<`, `<=`, `>`, `>=`, `exists` and `in`, combined with
`all`, `any` and `not`. Actions are `republish` (topic templates `{1}`..`{9}`,
`{topic}`, `{client}`, `{payload.<path>}`; `payload` or `transform` with `pick`,
`rename`, `add`), `set` (`user`, `content_type`, `expiry`) and `drop`. `lir/rules/rules.h`
is the reference. A rule that cannot be compiled is refused and counted in
`$lir/nodes/<id>`.

### MQTT over WebSocket

```
./build/lir-node ... --ws-port 8080
./build/lir-node ... --tls-certificate node.pem --tls-key node.key --wss-port 8081
```

A browser MQTT library (MQTT.js, Paho) connects to `ws://host:8080/mqtt` or
`wss://host:8081/mqtt`; any path is served.

### Publish and subscribe over HTTP

```
curl -N 'http://127.0.0.1:8083/subscribe?filter=sensors/%23'
curl -X POST --data '22.0' 'http://127.0.0.1:8083/publish?topic=sensors/room1/temp&qos=1'
```

In the query only `%XX` is decoded; `+` is MQTT's wildcard, never a space.
`--http-port -1` turns HTTP off.

### TLS

Give the node a certificate chain and key, and a port for each listener that should use
them:

```
./build/lir-node ... --tls-certificate node.pem --tls-key node.key \
  --mqtt-tls-port 8883 --https-port 8443
mosquitto_sub -h node.example -p 8883 --cafile ca.pem -t '$lir/#'
```

To ask clients for certificates (mutual TLS) on every TLS listener, name the CAs they
must chain to; a client without one is refused, unless certificates are optional:

```
./build/lir-node ... --tls-client-ca clients-ca.pem [--tls-client-certificate optional]
mosquitto_sub ... --cafile ca.pem --cert device.pem --key device.key -t 'sensors/#'
```

The verified subject is handed to the authenticator.

### Embed it

The library is the same pieces `lir-node` puts together: a `dannan::Node`, then
`lir::edge::Edge` (the listeners and the broker), `lir::cluster::Cluster` (registered
before the Dannan node starts), `lir::system::SystemTopics` and, if wanted,
`lir::http::HttpServer`. `tools/lir_node.cpp` is the worked example, in the order to
start and stop them.

## Repository layout

```
lir/auth/       topic scopes, grants and the node's anonymous authenticator
lir/mqtt/       the MQTT codec: wire, properties, packets, topics
lir/subs/       the subscription trie
lir/shard/      topic shards and ownership prefixes
lir/contract/   the Connection and Authenticator contracts
lir/broker/     the broker: sessions, QoS, retained, wills, shared groups
lir/edge/       the TCP and TLS listeners
lir/cluster/    the cluster on Dannan and its codec
lir/rules/      the rules engine
lir/system/     the system topics and the memory ledger
lir/http/       HTTP publish and SSE
lir/test/       GoogleTest, one binary (lir_tests); fixtures in lir/test/fixtures/
lir/fuzz/       libFuzzer targets for untrusted bytes
tools/          lir-node
docs/           the principles and laws shared with the stack
TODO/           now and next (ACTIVE.md), the design, and what shipped (COMPLETED.md)
```

Conventions, shared with the stack: [CONTRIBUTING.md](CONTRIBUTING.md).

## Status

In development; no release is cut yet. The first, 0.1.0, follows persistence on Dagda.
Built and tested (270 tests): all of MQTT 3.1, 3.1.1 and 5.0 on one node, over TCP, TLS,
WebSocket and WebSocket over TLS; the cluster on Dannan with sessions that move and are mirrored to standbys; system
topics; HTTP and HTTPS publish and SSE; topic scopes, with anonymous clients
refused by default; client certificates. The cluster is an interim one until Dagda:
retained messages are held in memory on every node and a QoS 1 or 2 publish is
acknowledged on acceptance, not on replication. Next: then persistence on Dagda and the Lugh
authenticator.

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
