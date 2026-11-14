# Lir - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

## 2026-11-03, night: the project starts

- `git init`; basemind set up (`basemind.toml`, rules in the gitignored
  `CLAUDE.local.md`). The index is not built: the basemind MCP server
  was down this session.
- Dannan's conventions taken whole: `.clang-format`, `.gitignore`,
  `LICENSE`, `docs/principles-and-laws.md` (Macha's master, with Dannan's
  source note), `CLAUDE.md` and `CONTRIBUTING.md` in Dannan's form.
- The design rendered in `TODO/2026-11-03-lir-plan.md` (revision 7);
  findings against the stack in `ACTIVE.md`.

## 2026-11-03, night: step 1, wire and topics

Uncommitted (commit only when asked). Gate: `lir_tests` 73 passed, 0
failed; built with `-DLIR_WARNINGS_AS_ERRORS=ON`, AppleClang 17.

- Build: `CMakeLists.txt` in Dannan's form (`LIR_BUILD_TESTING`,
  `LIR_BUILD_FUZZING`, `LIR_WARNINGS_AS_ERRORS`, no presets); Dannan
  fetched from `../dannan` at `63c2f51` into the build tree, as Lugh
  does; library `lir`, version header generated.
- `lir/mqtt/wire.h`: MQTT's representations over Dannan's codec:
  variable byte integer (shortest form enforced), UTF-8 strings with the
  section 1.5.4 MUSTs, binary data; `MalformedPacket` carries the reason
  code a server sends, `UnsupportedVersion` the level.
- `lir/mqtt/properties.h`: every 5.0 property; where each may appear
  (table 2-4), repeats, and value ranges checked on decode and encode.
- `lir/mqtt/packet.h`: all fifteen packet types for 3.1 (MQIsdp, level
  3), 3.1.1 and 5.0; fixed-header flags, reserved bits, packet id 0,
  QoS 3, DUP at QoS 0, will flags, password without user name (3.x),
  per-packet reason code tables, 5.0 short forms, trailing bytes.
  `read_frame` sizes a packet from its fixed header and refuses one over
  the maximum before its body is held.
- `lir/mqtt/topic.h`: names, filters, `$share/<group>/<filter>`,
  matching with the `$` rule.
- `lir/subs/trie.h`: the subscription trie, checked against the
  reference matcher on random filters and names.
- `lir/shard/shard.h`: `shard_of`, `shard_of_filter` (shard-local
  filters under `ownership_prefix_levels`), the placement key.
- `lir/fuzz/mqtt_packet_fuzz.cpp`: decode, re-encode, decode equal.
  Not built here: this machine has no clang with libFuzzer (AppleClang
  only, no MacPorts LLVM).

## 2026-11-04: step 2, the broker on one node

Gate: `lir_tests` 125 passed, 0 failed (52 new); warnings as errors.

- `lir/contract/connection.h`: the transport under a client.
  `lir/contract/authenticator.h`: connect, enhanced authentication,
  publish and subscribe authorisation, synchronous by contract;
  `AllowAll`.
- `lir/broker/broker.h`: `Broker` and `Link`, confined to one strand,
  time injected, `poll()` for what time makes due. Covered by tests:
  CONNECT for all three versions, first packet must be CONNECT,
  unsupported level (return code 1), assigned client ids, 3.x id rules,
  second CONNECT, packets in any split, inbound maximum packet size;
  QoS 0/1/2 in both directions with QoS 2 dedup until PUBREL, PUBREL
  for an unknown id, "no matching subscribers", the client's and the
  server's receive maximum; wildcards and the `$` rule, overlapping
  subscriptions (one delivery, greatest QoS, every identifier), No
  Local, per-filter refusals, unsubscribe reasons, shared
  subscriptions (in turn, connected members first, no retained, No
  Local refused); retained messages (sent on subscribe, cleared by an
  empty payload, Retain As Published, Retain Handling 0/1/2);
  persistent sessions (QoS 1/2 queued offline, QoS 0 dropped, resend
  with DUP, clean start, session expiry, expiry 0 at disconnect, the
  DISCONNECT expiry rule), takeover, keep alive at 1.5 periods, connect
  timeout; wills (abnormal close, DISCONNECT 0x00 and 0x04, delay,
  cancelled by reconnecting, cut short by session end); inbound topic
  aliases, message expiry (counted down, queued messages dropped), 5.0
  properties forwarded, the client's maximum packet size, reserved
  `$lir/` and `$dannan/` prefixes, payload format checks, wildcard in a
  topic name; enhanced authentication and authorisation.
- Not done (step 2), all optional in the specification (MAY): topic aliases from
  server to client, Response Information, queueing QoS 0 for offline
  sessions. Server Reference (redirecting a client) waits for the
  cluster, where it means something.

## 2026-11-04: step 3, the edge

Gate: `lir_tests` 132 passed, 0 failed (7 new); warnings as errors.
Dannan pinned at `295ca4b` (channels, the membership feed, the
`dannan::dannan` alias). Changing the pin in `CMakeLists.txt` does not
reach an existing build tree, which caches it: reconfigure with
`-U LIR_DANNAN_COMMIT`.

- `lir/edge/edge.h`: the TCP listener, one connection per socket, the
  broker and its poll timer, all on one strand. A client with more than
  `maximum_outbound_bytes` queued is closed as an abnormal close (its
  will goes), never waited for. `stop()` closes the listener and every
  client; `post()` and `call()` reach the broker on its strand.
- `tools/lir_node.cpp`: `lir-node`, a Dannan node that advertises `lir`
  and serves MQTT; JSON lines on stdout as `dannan-node` writes them;
  `--version`.
- Tests over real sockets: connect and ping, publish across two sockets,
  a protocol error closing the socket, a dropped socket's will, stop
  closing every client, a client that does not read closed while the
  publisher carries on.
- Checked by hand with mosquitto 2 against `lir-node`: `mosquitto_pub`
  over 3.1, 3.1.1 and 5.0 at QoS 1 and 2, `mosquitto_sub` on a wildcard
  at QoS 1 (5.0) receiving both, a retained message read back over
  3.1.1; `lir-node` stopped cleanly. The automated process test with
  mosquitto waits for Dannan's `child_process.h` fixture (asked for;
  the Dannan session adds it after its 0.2.0 sanitizer run).
- Not done: TLS on the MQTT port.

## 2026-11-04: step 4, the cluster on Dannan (interim, before Dagda)

Gate: `lir_tests` 151 passed, 0 failed (19 new); the cluster tests
passed five repeats in a row. Dannan pinned at the `0.2.0` tag
(FetchContent of the tag; `-U LIR_DANNAN_COMMIT` on an old build tree).

- The broker gains a cluster side (`broker.h`): `ClusterHooks`
  (published, interest_changed, take), `inject`, `apply_retained` and
  `retained_entries` with `RetainedVersion` (Lamport, node), clearings
  kept as versions, `deliver_shared`, `surrender`, `SessionImage`,
  `import`, and an asynchronous CONNECT (`taking`) that waits while
  other nodes let go of the client, buffering what the client sends
  meanwhile. Alone, the broker behaves as before (125 tests unchanged).
- `lir/cluster/codec.h`: messages, snapshots, session images, groups,
  take and shared delivery, each with a format number, counts bounded
  before allocation, expiry carried as what is left.
- `lir/cluster/cluster.h`: channels `lir.notify` (QoS 0), `lir.seq`
  (QoS 1/2, deduplicated by origin, incarnation and sequence),
  `lir.retained` and `lir.groups`; subscriptions by the minimal set of
  literal filter prefixes; routes `0x4C00` snapshot (retained and
  groups for a node that joins or restarts), `0x4C01` session take,
  `0x4C02` shared delivery; the roster feed for peers.
- `lir-node` assembles node, edge and cluster; the cluster's routes are
  registered before the node starts.
- `lir/test/fixtures/mqtt_client.h`: the socket client, with reads
  bounded by `poll()`; Asio's blocking read retries a timed-out read for
  ever, which hung the first cluster run.
- Tests: QoS 0, 1 and 2 across nodes, once each; subscribers on three
  nodes each get one copy; retained values everywhere, cleared
  everywhere, the later value winning on every node, a late joiner
  catching up; a persistent session resumed on another node with what
  was queued; takeover across nodes; a will across nodes; a shared group
  across two nodes getting each of ten messages exactly once.
- `process_test.cpp` (Dannan's `child_process.h`): mosquitto 2 against
  one `lir-node` over 3.1, 3.1.1 and 5.0; two `lir-node` processes as one
  broker, live and retained.
- Interim and its limits are `ACTIVE.md`, questions 1 to 3.

## Findings resolved (2026-11-03 night to 2026-11-04)

The design predates the stack's rules; Lir's findings and their ends:
store nouns are Dagda's (stack document); multimaster with a per-shard
lease (the maintainer, 2026-11-04); no `lir.local` (the maintainer); authentication pluggable,
no token verification in Dannan (the maintainer); HTTP Lir's own (the maintainer); message
numbers free from 512 (the maintainer); `lir` advertised, one process per node
(the maintainer); k-of-N durability is Dagda's Replicated (the maintainer). The per-origin
sequence lives in a Dagda node-local space and never goes back (with
Dannan and Dagda; `durability_epoch` is unordered, for dedup only).

## 2026-11-04: session standbys (the maintainer's answer 2)

Gate: `lir_tests` 152 passed, 0 failed; cluster and process tests five
repeats clean.

- The broker marks persistent sessions (expiry above 0) as changed after
  every packet their link handles and at delivery, connect, disconnect,
  import and end; `take_changes()` hands them over as images, with those
  that ended or left; `adopt()` takes up a mirrored session offline.
- The cluster sends changed sessions to `session_standbys` other Lir
  nodes (rendezvous over the client id; default one, `lir-node
  --session-standbys N`, zero for none) at most every `mirror_every`
  (100 ms), on route `0x4C03` lir.session_mirror. A standby holds the
  image. When the owner leaves the roster or restarts, the first node in
  rendezvous order takes the session up offline (subscribed, queueing);
  the others note when the owner went and drop the copy once the
  session's expiry has run from then. A session take prefers a live
  session to a mirror, and every node drops its copy of a client that
  is taken.
- Limits, until step 5: a change made in the last `mirror_every` before
  a node dies is lost; a partition can leave a live session on one side
  and an adopted one on the other.
- Test: a persistent session survives its node being stopped: taken up
  by its standby, receiving a QoS 1 message published meanwhile, and
  resumed by its client on another node with that message.

## 2026-11-04: step 6, the system topics

Gate: `lir_tests` 154 passed, 0 failed; cluster and process tests three
repeats clean.

- `lir/system/system.h` (`lir::system`, a seventh namespace beside the
  six the maintainer kept): each node publishes, retained, as JSON with content
  type `application/json`, `$lir/nodes/<node id>` (connections,
  sessions, retained, accepted, refused, protocol errors, messages
  received, delivered and injected, wills, session counts, drops) and
  `$dannan/nodes/<node id>` (its record, path class, incarnation, peers
  and their latency), when they change, checked every 5 s; every node
  clears a node's topics when it leaves the roster. Clients may read
  them; publishing there is refused (5.0 reason 0x87).
- `lir-node` runs them.
- Tests: every node's topics retained on every node and readable by a
  client; a forged publish refused; a node's topics cleared when it
  leaves.

## 2026-11-04: step 6, HTTP publish and Server-Sent Events

Gate: `lir_tests` 163 passed, 0 failed; HTTP, cluster and process tests
three repeats clean.

- `lir/http/http.h` (`lir::http`): `POST /publish?topic=T[&qos][&retain]`
  and `GET /subscribe?filter=F[&filter=G]` as `text/event-stream`. Each
  request is an MQTT 5.0 client of the broker for its length, fed
  packets in process, so the authenticator, retained messages and the
  cluster apply as to any client. `Authorization: Bearer <token>` is the
  CONNECT password, for the authenticator (a Lugh plugin) to check.
  Only %XX is decoded in the query: '+' is the MQTT wildcard. Events
  carry the payload as text when it is UTF-8, else as hex (Dannan's
  `identity::hex`); retained first; a comment every 15 s; a stream not
  being read is closed past 4 MiB queued. Boost.Beast parses requests:
  it is part of Boost, already a dependency.
- MQTT refusals map to HTTP: 0x86 and 0x8C to 401, 0x87 and 0x8A to 403,
  busy and quota to 503, the rest to 400; 404, 405 and 413 for the
  request itself.
- `lir-node --http-port N|-1 --http-listen ADDR` (default 8083; -1 for
  none); the listening line reports `http_port`.
- Tests: publish reaching an MQTT subscriber with its content type;
  "matched" false with no subscriber; a retained HTTP publish read over
  MQTT; every refusal's status; an event stream getting a retained then
  a live message and its keep-alive; a closed stream leaving the broker;
  a bearer token as the CONNECT password. Checked by hand with curl
  against `lir-node`.
- A bug found on the way: an event stream's exchange was held only by
  weak references and went as soon as its headers were written; its
  pending read now holds it.

## 2026-11-04: TLS on the MQTT port (step 3's open item)

Gate: `lir_tests` 165 passed, 0 failed.

- `EdgeConfig::tls` (`TlsConfig`: listen address, port, PEM certificate
  chain and key, handshake timeout): a second listener for MQTT over TLS
  (1.2 and up), the same broker as the plain port. The connection is one
  template over a TCP socket or a TLS stream; a TLS client becomes a
  connection once its handshake is done, within the timeout.
- `lir-node --mqtt-tls-port N --tls-certificate PEM --tls-key PEM
  [--mqtt-tls-listen ADDR]`; the listening line reports `mqtt_tls_port`.
- Tests: a client verifying a self-signed certificate (made by the
  `openssl` command for the test) receives over TLS what a plain client
  publishes; plaintext on the TLS port gets nothing and leaves no
  connection. Checked by hand with `mosquitto_pub` and `mosquitto_sub
  --cafile` against `lir-node`.
- Not done: a TLS close_notify on close (the socket is closed); HTTPS on
  the HTTP port; client certificates.

## 2026-11-04: the specification's MAYs (all of MQTT, the maintainer)

Gate: `lir_tests` 169 passed, 0 failed (4 new).

- Topic aliases from server to client (5.0, 3.3.2.3.4): per connection,
  up to the client's Topic Alias Maximum, the topic the first time and
  the alias alone after; an alias counts only once a packet carrying it
  has gone (one over the client's maximum size is not sent and assigns
  nothing). Reset with each connection.
- Response Information (`Config::response_information`, "{client}" for
  the client id): sent in the CONNACK only when the client asks.
- `Config::queue_qos0_offline`: QoS 0 messages queued for an offline
  session (off by default: dropped and counted).
- With these, step 2's "not done" list is empty. Server Reference
  (redirecting a client) is not used: in a multimaster cluster every
  node serves every client.

## 2026-11-04: HTTPS

Gate: `lir_tests` 170 passed, 0 failed.

- `HttpConfig::tls` (an `edge::TlsConfig`): HTTPS on its own port beside
  plain HTTP, the same endpoints; the exchange is one template over a
  TCP socket or a TLS stream, as the edge's connection is.
- `lir-node --https-port N [--https-listen ADDR]` with `--tls-certificate`
  and `--tls-key`; the listening line reports `https_port`.
- Test: a verifying client publishes over HTTPS to an MQTT subscriber.
  Checked by hand with `curl --cacert` (a retained publish, then an
  event stream reading it back) against `lir-node`.

## 2026-11-04: the design's seven sections, the README, Dannan 0.3.0

- The maintainer answered the seven unwritten sections in turn:
  `TODO/2026-11-04-lir-sections.md`. The repositories are the sources of
  truth; the starter Claude Docs will be deleted (the maintainer), and the stack's
  sessions were told.
- `README.md` in the stack's form (why, what, the stack, how to use it);
  remote `origin` is `git@github.com:tomdionysus/lir.git`, not pushed.
- Built against Dannan `0.3.0` (`277adab`). Gate: `lir_tests` 170.

## 2026-11-04: topic scopes; anonymous refused by default

Gate: `lir_tests` 199 passed, 0 failed (29 new); the process tests ran
against mosquitto.

- The maintainer, 2026-11-04: Lir checks token scopes itself. Dannan 0.3.0's scope
  seam (`ScopeSource`, `Identity::verify`) answers only "may P write under
  prefix S", with data a Lugh source installs on a node; it is not used
  for MQTT.
- Contract: an accept carries a `contract::Grant` (may publish, with the
  retain flag; may subscribe; when it ends), held by the link and
  replaced on 5.0 re-authentication. The authenticator keeps no
  per-client state. `Unrestricted` and `Nothing` grants; `AllowAll`
  gives the first.
- Broker: publish, retain and subscribe checked against the grant; the
  will checked once the client is accepted, against pub or retain; a
  connection whose grant ends is sent DISCONNECT 0x87 and its session
  kept (design, section 1).
- `mqtt::covers(granted, requested)`: filter coverage with the '$' rule
  and "a/#" covering "a".
- `lir/auth/scopes.h`: the grammar Lugh proposed (`lir:pub|sub|retain:`
  percent-encoded filter, "{sub}" a whole level), `ScopeGrant`, and
  `AnonymousAuthenticator`: anonymous clients refused unless allowed,
  credentials refused until the Lugh plugin. The percent decoder moved
  here from `http` (one copy).
- `lir-node --allow-anonymous [--anonymous-scope S]...`: without a scope,
  anything but writing the system topics.
- The maintainer decided the grammar the same day, with Lir's review taken in
  (`../lugh/TODO/2026-11-04-oauth.md`, "Topic scopes, for Lir"):
  `{client}` dropped, retain its own action, Lir checks scopes itself.

## 2026-11-04: narrower grants drop subscriptions

Gate: `lir_tests` 202 passed, 0 failed (3 new).

- The maintainer, 2026-11-04 (via Lugh): a re-authentication whose grant no longer
  covers some subscriptions removes them from the session and sends
  DISCONNECT 0x87; the client resubscribes and sees SUBACK 0x87. A
  resumed session (any version, any node) keeps only what the new
  connection's grant allows. `Broker::prune`.

## 2026-11-04: MQTT over WebSocket

Gate: `lir_tests` 214 passed, 0 failed (12 new); the WebSocket and TLS
tests passed five runs in a row.

- In scope by the maintainer's answer to design section 5 (the specification's own
  transport, section 6). `EdgeConfig::websocket` (ws) and
  `EdgeConfig::secure_websocket` (wss, its own certificate and key), each
  on its own port beside TCP and TLS, the same broker.
- The upgrade (Boost.Beast, already linked for HTTP): any path; the
  server answers "mqtt" when offered, else "mqttv3.1" for a 3.1 client,
  serves a client that offers none, and refuses with 400 one that offers
  only others, or a request that is not an upgrade. MQTT goes in binary
  frames, one byte stream however the frames split it; a text frame
  closes the connection (section 6 MUST). One deadline covers the TLS
  handshake and the upgrade.
- The edge's connection is still one template, now over a socket, TLS,
  or a WebSocket over either.
- Fixed on the way: a handshake deadline that was already due when its
  handshake finished could close a stream that had moved into its
  connection (TLS too); the deadline now checks a settled flag.
- `lir-node --ws-port N [--ws-listen ADDR] --wss-port N [--wss-listen
  ADDR]`; the listening line reports `ws_port` and `wss_port`. Checked by
  hand: a raw upgrade to `lir-node` gets 101 with
  `Sec-WebSocket-Protocol: mqtt` and a CONNACK in a binary frame.

## 2026-11-04: the configurable system topics (design, section 3)

Gate: `lir_tests` 222 passed, 0 failed (8 new); the cluster and process
tests passed three runs in a row.

- `SystemConfig` switches `nodes`, `cluster`, `clients`, `roster`, all on
  by default; `lir-node --no-system-topics NAME`, repeatable.
- `$lir/cluster` (retained): nodes, connections, sessions, messages
  received a second, and the limits that differ between nodes (design
  section 4, "mismatch reported"). Each Lir node advertises its figures
  and limits in its Dannan record (`lir.connections`, `lir.sessions`,
  `lir.received_per_second`, `lir.limits`), changed only when they
  change; the Lir node with the lowest id publishes, so a subscriber sees
  one summary, not one per node.
- `$lir/clients/connected` and `/disconnected` (events): each node its own
  clients, from a new `Broker::observe` hook (`ClientEvent`: version,
  peer, user name, session present; by client, server or network, and
  the reason code). The hook posts the publish to the strand, never
  calling the broker back from inside it.
- `$dannan/roster/joined`, `/left`, `/changed` (events), published by the
  lowest Lir node only; an advert's change is not an event.
- `$lir/shards/<n>` is not built: shards have no holders, primary or
  lease until step 5, and the topic would report nothing true. It
  follows step 5.

## 2026-11-04: client certificates (mutual TLS)

Gate: `lir_tests` 227 passed, 0 failed (5 new).

- `TlsConfig::client_ca` and `require_client_certificate`: on every TLS
  listener (MQTT, wss, HTTPS), a client certificate verified against the
  named CAs, required (refused at the handshake without one) or
  optional. The context is built once, `edge::server_context`, which the
  HTTP server now uses too (it had its own copy).
- `contract::Connection::certificate_subject()` and `contract::Peer`:
  the authenticator's `connect` is given the peer's address and the
  verified subject (RFC 2253). A certificate grants nothing by itself:
  the anonymous authenticator treats a client with one and no credential
  as anonymous. Whether a subject should map to scopes is open (Lugh's
  devices are service principals with tokens).
- `lir-node --tls-client-ca PEM [--tls-client-certificate
  required|optional]`. Checked by hand: `mosquitto_pub --cert` connects
  to `lir-node` over TLS; without a certificate the connection is lost.
- Not done: HTTPS hands no subject to the authenticator (its exchanges
  are virtual clients).

## 2026-11-04: the moderate default limits (design, section 4)

Gate: `lir_tests` 230 passed, 0 failed (3 new).

- Defaults now the moderate set the maintainer chose: 1 MiB packets and receive
  maximum 1024 (as before), 10,000 queued per offline session (was 1000),
  session expiry capped at 7 days (was never), 4 MiB queued for a slow
  client (was 16 MiB), and `Config::maximum_connections` 10,000, past
  which a CONNECT is refused with 0x89 Server busy (3.x: return code 3);
  existing clients are never evicted.
- `lir-node --max-packet-size --receive-maximum --max-queued
  --max-session-expiry --max-outbound-bytes --max-connections`.
- Not done from section 4: the memory budget's order under pressure
  (drop QoS 0, then refuse QoS 1/2 with 0x97, then 0x89) needs a node
  memory budget, Dannan's `budget()`, which Lir has not declared yet;
  the config file waits on Dannan's reader.

## 2026-11-04: memory pressure (design, section 4)

Gate: `lir_tests` 234 passed, 0 failed (4 new).

- The broker counts the bytes of every message it holds (queued, in
  flight, retained) as they come and go; `recount_held_bytes()` walks
  them all, and a test checks the two agree through every flow.
- `Config::memory_budget` (512 MiB; `lir-node --memory-budget`). The
  design's order: at 80% QoS 0 is no longer queued and queued QoS 0 is
  shed oldest first at each poll; at 90% QoS 1/2 publishes are refused,
  PUBACK or PUBREC 0x97 (a 3.x client, which has no code for it, is
  closed and resends on return); at 100% new clients get CONNACK 0x89.
  Existing sessions are never evicted. Counted in `$lir/nodes/<id>`
  (`memory`: held, budget, pressure, refusals), and the budget is one of
  the limits `$lir/cluster` compares.
- `lir-node` declares Lir's unit of work (one maximum-size packet,
  foreground) to Dannan's `budget()`, which refuses to start Lir on a
  node that cannot hold one (the design's "Budget" row).
- Open for the maintainer: the 512 MiB default and the 80/90/100 thresholds are
  mine, not decided; and the held bytes are Lir's own count, not leases
  in Dannan's retained-memory ledger (that would make other systems'
  memory press on Lir too).

## 2026-11-04: Dannan 0.4.0; the config file; one percent decoder

Gate: `lir_tests` 239 passed, 0 failed (5 new).

- Built against Dannan `0.4.0` (`d657a9b`).
- Percent-decoding is Dannan's (`dannan::encoding::percent_decode`,
  strict), for the HTTP query and topic scopes; Lir's copy is deleted
  (rule 3; the maintainer). Scope filters are now decoded level by level: an encoded
  '/' (`%2F`) can no longer make a level boundary, and the subject is
  never decoded. Lugh's encoder output (Lugh `2c63171`) and the strings
  Lugh refuses are tests here.
- `lir-node` reads its settings through Dannan's reader
  (`dannan::config`): a YAML file (`--config`, `$LIR_CONFIG`,
  `/usr/local/etc/lir/config.yaml`), `secrets.yaml` beside it, `LIR_*`
  variables, then the flags. Every flag kept; `reader()` lists the keys.
  Process tests: a setting from the file alone takes effect; a bad one
  stops the node with status 2, naming the file and the key.

## 2026-11-04: Server Reference at CONNECT (the maintainer's policy, design section 7)

Gate: `lir_tests` 248 passed, 0 failed (9 new); the cluster and process
tests passed three runs in a row.

- `cluster/balance.h`, pure: a node over (1 + `rebalance_over`) times the
  cluster's average sends a new 5.0 client to the least-loaded node
  serving the same transport, if that node has at least two fewer
  clients; lowest id among equals. Default 20%; `lir-node
  --rebalance-over PERCENT|-1`.
- Each node advertises where it serves (`lir.endpoints`: tcp, tls, ws,
  wss as host:port, from its advertised host) once its edge is bound
  (`Cluster::advertise_endpoints`), and its client count
  (`lir.connections`, now clients past CONNECT). A node counts the
  clients it has sent each other node since that node's count last
  changed, so a burst is spread rather than all sent to one node.
- Broker: `ClusterHooks::redirect(transport)`; CONNACK 0x9C with Server
  Reference; counted (`connections_redirected`). 3.x clients and HTTP
  requests are never redirected. `Connection::transport()`.

## 2026-11-04: memory thresholds configurable; Dannan's ledger

Gate: `lir_tests` 251 passed, 0 failed (3 new).

- The maintainer: budget configurable (512 MiB), thresholds configurable (80, 90,
  100): `Config::shed_percent`, `refuse_publish_percent`,
  `refuse_connect_percent`; `lir-node --shed-percent
  --refuse-publish-percent --refuse-connect-percent`, refused unless they
  rise.
- The maintainer: Lir's held bytes are also leased in Dannan's ledger.
  `system::LedgerLeases` follows the broker's count each second in 1 MiB
  foreground leases; a refused lease sets `Broker::ledger_full`, which
  puts the pressure at refuse_connect until room returns. Test: three
  MiB retained on a node are leased and released as they come and go.
- The memory part of the "Open for the maintainer" note under memory pressure is
  settled by this.

## 2026-11-04: the rules engine (the maintainer's decisions, sections file)

Gate: `lir_tests` 270 passed, 0 failed (19 new); the cluster tests passed
twice more.

- `lir/rules/rules.h`, namespace `rules`: a rule is a retained JSON
  message on `$lir/rules/<name>`, compiled when its retained value lands
  on a node (locally or from another node), deleted by an empty one, and
  refused (counted, logged) when it cannot be compiled. Boost.JSON, used
  header-only and compiled once as Lugh does (Boost is already a
  dependency; nothing added).
- Conditions on topic, client, qos, retain, content_type, user.<name>,
  payload and payload.<path> (= != < <= > >= exists in; all, any, not).
  An absent value equals nothing: "!=" holds, the rest fail.
- Actions: republish (templated topic and payload, QoS, retain;
  transform: pick, rename, add), set (user properties, content type,
  expiry), drop. Rules apply in name order; set changes the message for
  later rules and for delivery.
- In the broker: applied in `route`, on the publishing node, before
  retaining and fan-out; a rule's own messages are never ruled on.
  Produced messages pass through the rules again, at most 8 hops
  (`Message::hops`, never carried between nodes). Writing `$lir/rules/`
  is the one reserved prefix a client may publish to: retained, and only
  with a grant to retain there. Counted in `$lir/nodes/<id>` (`rules`).
- Cluster test: a rule written on one node applies on another.

## 2026-11-04: MQTT-SN 1.2 (design sections 5 and 7; the maintainer's MQTT-SN decisions)

Gate: `lir_tests` 288 passed, 0 failed (18 new); the gateway tests passed
three runs in a row.

- `lir/sn/packet.h`, namespace `sn`: every MQTT-SN 1.2 packet, one-byte
  and three-byte lengths, the flags byte with QoS -1 and the three topic
  id types, and the encapsulated form (its length runs to the end of the
  node id; the relayed packet follows).
- `lir/sn/gateway.h`: UDP, on the edge's strand. Each client is a client
  of the broker (a connection like HTTP's), so sessions, the
  authenticator, the rules and the cluster apply. Topic ids registered
  per client both ways (the gateway REGISTERs a new name and sends the
  PUBLISH on the REGACK); short names; predefined ids from retained
  `$lir/mqttsn/topics/<id>` (the maintainer). The will is asked for, held by the
  gateway (so WILLTOPICUPD and WILLMSGUPD work) and published when the
  client is lost (keep alive or sleep overrun), not on an orderly
  DISCONNECT. Sleep: the broker connection stays, with receive maximum
  64, and the gateway holds what arrives (bounded, oldest QoS 0 first)
  until PINGREQ wakes the client, then PINGRESP, then asleep again after
  a grace. QoS -1 from unconnected clients goes in as an anonymous
  client's. Forwarders are answered through the forwarder. ADVERTISE is
  off unless configured (the maintainer); SEARCHGW is always answered.
- `lir-node --mqttsn-port --mqttsn-listen --mqttsn-gateway-id
  --mqttsn-advertise-seconds --mqttsn-advertise-address`; the listening
  line reports `mqttsn_port`. Checked by hand: a raw MQTT-SN client
  connects to `lir-node`, publishes on a short topic, and `mosquitto_sub`
  receives it.
- Not done: DTLS (next), MQTT-SN 2.0 (after checking the OASIS text).

## 2026-11-04: MQTT-SN over DTLS (the maintainer: DTLS beside plain UDP)

Gate: `lir_tests` 291 passed, 0 failed (3 new); the TLS, DTLS and
MQTT-SN tests passed three runs in a row.

- `GatewayConfig::dtls` (an `edge::TlsConfig`): DTLS 1.2 on its own UDP
  port. Asio has no DTLS, so the gateway drives OpenSSL itself: one
  session per peer over datagram memory BIOs (OpenSSL 3.2's
  `BIO_s_dgram_mem`, so records keep their boundaries), the stateless
  cookie exchange (an HMAC of the peer's address under a secret made at
  start, with Dannan's `hmac_sha256` and `random_bytes`), handshake
  retransmission at each poll, a cap on peers mid-handshake, and idle
  sessions let go. Decrypted datagrams take the plain path.
- Client certificates as on every TLS listener; the verified subject
  reaches the authenticator. `edge::server_context(tls, datagram)` and
  `edge::verified_subject` are shared, not copied.
- Found on the way: Asio's SSL context owns the `SSL_CTX` app data slot
  and deletes what is in it; the gateway's pointer now has its own
  ex-data index.
- `lir-node --mqttsn-dtls-port N [--mqttsn-dtls-listen ADDR]`, with
  `--tls-certificate`, `--tls-key` and `--tls-client-ca`.


## 2026-11-04: close_notify, and the HTTPS certificate subject

Gate: `lir_tests` 295 passed, 0 failed (4 new); the TLS, WebSocket,
client certificate, HTTP and process tests passed three runs in a row.

- A close the broker makes (DISCONNECT, a refusal, a server DISCONNECT)
  now ends the stream cleanly once what is queued is written: TLS sends
  close_notify (RFC 8446, section 6.1), a WebSocket a close frame (RFC
  6455, section 7, then close_notify under wss), each bounded by a
  2-second deadline. Abnormal closes (errors, an overfull queue, stop)
  stay immediate. HTTPS responses end with close_notify the same way.
- HTTPS hands the authenticator the verified client certificate subject,
  as the MQTT TLS port does; the HTTPS listener already asked for
  certificates when `client_ca` was set.
- MQTT-SN 2.0: OASIS has only working drafts (the newest 2025-10-19; CSD01
  of 2023-12 is superseded). Recommended to the maintainer to wait for an approved
  Committee Specification; recorded as waiting on OASIS.

## 2026-11-04: Dannan 0.4.1, a cluster flake, and the Lugh token authenticator

Gate: `lir_tests` 307 passed, 0 failed (12 new); the certificate,
TLS, WebSocket, HTTPS, MQTT-SN and token tests passed three runs in a
row, the cluster tests five.

- Dannan 0.4.1 (additive: readiness planes; Lir declares none).
- `ClusterTest.ASharedGroupAcrossNodesGetsEachMessageOnce` failed once
  under load: it waited on an empty groups advert and then slept 500 ms.
  It now waits on `Cluster::shared_members(group, filter)`, the nodes the
  publishing node knows to hold a member.
- `auth::TokenAuthenticator` (`lir/auth/tokens.h`): the token as the
  CONNECT password or the data of the MQTT 5 method "lugh" (which also
  re-authenticates); "lir" among the audiences or 0x87; a token that does
  not verify or has expired 0x86; a DPoP-bound token refused (MQTT cannot
  carry a proof); a certificate-bound token only from that certificate
  (RFC 8705 x5t#S256); the topic scopes with `{sub}` as the grant until
  exp; anonymous clients as before. Whether a token is genuine is
  `TokenVerifier`'s, which is Lugh's code: nothing in Lir parses JOSE.
- `contract::ClientCertificate` (subject and SHA-256 thumbprint) now
  travels from every TLS, HTTPS and DTLS listener to `Peer`, and
  `Authenticator::auth` is given the peer, so a re-authentication with a
  bound token is checked too.
- Not wired into `lir-node`: there is no verifier to run until the
  linking question is answered.

## 2026-11-13: Dannan and Dagda for all three systems

Gate: `lir_tests` 307, unchanged (no code changed; not rerun).

- The maintainer ruled that Dannan and Dagda serve Macha, Lugh and Lir as first-class
  users. Lir wrote its needs of both as behaviour and numbers
  (`TODO/2026-11-13-lir-needs.md`: Dannan N1 to N6, Dagda G1 to G9),
  answered Dagda's five questions from Lir's plan files (sizes there are
  the Lir session's estimates), and reviewed Dagda's design (R1 to R11).
  Every point landed in `../dagda/TODO/2026-11-13-three-systems-design.md`
  before the maintainer ruled; Lugh's review went in beside it.
- Found while answering: a second hot path beside the inflight log
  (each QoS 1/2 delivery changes a session's state, and offline sessions
  queue up to 10,000 messages); the stack document's "2-4 commits a shard"
  assumes a 250-500 ms batch, where a 10-20 ms batch gives about 27-32;
  Dagda's per-space adverts against Dannan's 64-advert cap; and a union
  merge that would bring back a removed QoS 2 message after a split
  (closed by tombstones for 10 minutes, a stated limit).
- The rulings are under Decisions, 2026-11-13: the log noun, the
  register map, per-space placement and k, contract v2 at slice 4, the
  10-minute QoS 2 limit, PUBACK p99 under 50 ms, Dannan's ABI promise, the
  partition reading (even is a minority), and a shared listener later.
- Lugh agreed that `lugh_client` hides where its signing keys come from
  and keeps HTTPS JWKS for a node outside Lugh's cluster.
- The Macha session confirmed that measuring the head commit rate early
  costs its port nothing (stage one does not touch heads).

## 2026-11-13: Dannan 0.5.6, and step 5 designed

Gate: `lir_tests` 307 passed, 0 failed, on Dannan 0.5.6 (Release, macOS,
warnings as errors, no warnings); the 44 cluster, process and shared-group
tests passed three runs in a row.

- Repinned from Dannan 0.4.1 to 0.5.6 (`feature/dannan-0.5`), ahead of
  Dagda 0.1.5 at the maintainer's word: 0.5 is the minor Dagda 0.1.5 builds on, and
  Lir links no Dagda yet. No source changed. Since 0.4.1 Dannan's Context
  changes are additive; a call is watched for progress only when it asks,
  and deadlines follow injected time. Neither moved a test.
- Step 5's design, `TODO/2026-11-13-step5-design.md`: direct fan-out kept,
  an inflight log per shard appended before PUBACK, session queues only for
  offline or saturated sessions, session state and retained in replicated
  spaces, the sequence reserved in blocks of 65,536, minority behaviour, and
  19 tests to write first, each when its Dagda slice lands. The maintainer answered its
  three questions; Dagda answered its four (any node appends directly to a
  log's replicas; a dot is the owner's, and an append's origin must be the
  appender, so a queue entry takes the subscriber node's own dot; acks and
  retires may name any origin's dots, and Lir restricts retiring itself).
- README status brought up to date, with the QoS 2 split limit stated.
- `develop` pushed to `origin` at the maintainer's word (its first push there).

## 2026-11-13: Dannan 0.6.0, and step 5's groundwork

Gate: `lir_tests` 323 passed, 0 failed, on Dannan 0.6.0 (Release, macOS,
warnings as errors, no warnings); the cluster, process and shared-group
tests passed three runs in a row. 16 new: 2 shard, 6 stored entries, 8
sequence blocks.

- Repinned to Dannan 0.6.0, the first with the ABI promise and the minor
  Dagda 0.1.5 builds on; no source changed.
- `shard::session_shard_of`: SHA-256("lir-sess:" || client id) mod S.
- `cluster::SequenceBlocks` over a `SequenceStore` seam: seq handed out from
  blocks whose upper bound is durable first, the next block reserved at half,
  one reservation in flight, nothing handed out past the durable bound, a
  restart starting at the bound, completions posted.
- `encode_inflight`/`encode_queued` and their decoders: Lir's log entries
  under their own format and kind. Found while writing them: the wire codec
  sends an expiry as what is left, which is wrong for anything that outlives
  the process, so a stored expiry is wall-clock time (`Clocks`).
- Two test cases first failed because a drained `io_context` stops itself;
  the tests restart it (`drain`). Test code only.
- A first GCC build on node-a was started before the Macha session's
  note that the machine was Dagda's, and stopped within minutes; both
  sessions were told.

## 2026-11-14: Linux arm64, and a standing cluster on the build nodes

Gate: `lir_tests` 323 passed, 0 failed, on node-c (arm64, Debian 13,
GCC 14, Boost 1.83, Release, warnings as errors, no warnings; -j1, nice 19,
throttled=0x0 throughout), and 323 on macOS.

- Lir's first GCC build. Boost's floor lowered to 1.83, as Dannan's and
  Dagda's. GCC 14 found two shadowed names in the broker, dangling elses
  around test macros and lambda parameters shadowing a fixture's node, all
  fixed; and a false positive on MQTT-SN framing's growing vector, twice
  (a reserve traded one warning for another), fixed by sizing the frame
  once. No behaviour changed.
- `deploy/test-cluster/`: a systemd unit, a node config template, an install
  script run from the workstation, and a README of the coordination topics. Three
  nodes joined, `$lir/cluster` reporting 3 with consistent limits.
- Trial across the three build nodes: QoS 2 from node-a to a subscriber on
  node-c, in order; a retained value published on node-b served from
  node-c; a persistent session leaving node-a and resuming on node-c
  with its three queued messages; with node-c stopped, the same session
  resuming on node-a from its standby with both messages queued since;
  node-c rejoining and serving retained values again.
- Announced to the Macha, Dagda, Dannan, Lugh, Corvus LoRa, Corvus
  Radioberry, AthenaSIP and Miach sessions.

## 2026-11-14: GET /retained, built on the build nodes

Gate: `lir_tests` 329 passed, 0 failed, on node-b (GCC 14, Release,
warnings as errors, no warnings, throttled=0x0). 6 new HTTP cases, and the
token case extended. From now on every Lir build and test run is on the
build nodes, never the workstation (the maintainer, 2026-11-14), taking turns with the other sessions
and claiming the machine on the board first.

- `GET /retained?filter=F[&filter=G]`: the retained messages the filters
  match, as one JSON array of the event stream's objects, then the
  connection closes. The snapshot is exact: the broker sends a
  subscription's retained messages inside the SUBSCRIBE, on the edge's
  strand, so nothing live can be among them. Past
  `HttpConfig::maximum_retained_bytes` (4 MiB) it is refused whole with 413.
- `message_json` is the one JSON form of a delivered message, shared by the
  event stream and the snapshot.
