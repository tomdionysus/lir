# Lir design: the seven unwritten sections, Tom's answers

Asked in turn on 2026-11-04; all seven are answered. This file and
`2026-11-03-lir-plan.md` are the design and the source of truth; the starter Claude Doc
will be deleted.

## 1. MQTT front end, sessions and Lugh authorisation

- Topic permissions are scopes carried in the Lugh token (publish and subscribe topic
  filters), checked on each PUBLISH and SUBSCRIBE through Dannan `Identity::verify`. Lir
  keeps no ACL state.
- Credential: the token as the CONNECT password (username the principal) for every
  version; MQTT 5.0 clients may also use enhanced authentication, method `lugh`, to
  re-authenticate a long-lived connection with a fresh token.
- A connection whose token expires without a 5.0 re-authentication is sent DISCONNECT
  0x87 (Not authorized) at expiry; its session is kept for resume.
- Anonymous clients are refused by default; a node flag allows them with a configured
  scope set.

## 2. Failure, partition and lease fencing

- A QoS 1/2 publish on a side of a split that cannot reach k replicas: configurable per
  node, between degrading (write Persisted, PUBACK, count as under-replicated in
  `$lir/nodes/<id>`, re-replicate on heal) and refusing with 0x97 after a deadline.
  Default: degrade.
- A node is gone when Dannan's roster says it has left; Lir adds no failure timer.
- A client id connected on both sides of a healed split: the session with the higher
  takeover counter keeps its connection (node id breaks ties); the other gets DISCONNECT
  0x8E. Subscriptions and inflight sets merge by union: duplicates possible, loss not.
- QoS 2 dedup window: configurable, default 10 minutes.

## 3. System topics

- Beyond `$lir/nodes/<id>` and `$dannan/nodes/<id>`, each configurable and on by default:
  - `$lir/shards/<n>`, retained: holders, primary, lease epoch, retained count.
  - `$lir/clients/...`, events: connect and disconnect with client id, node, reason code,
    protocol version.
  - `$lir/cluster`, retained: node count, total connections, message rate, limit
    mismatches.
  - `$dannan/roster/...`, events: join, leave, incarnation change.
- Lir reserves only `$lir/` and `$dannan/`, which it alone writes. Any other `$` prefix
  is protected by token scopes only.
- Subscribing to `$lir/` and `$dannan/` is scope-controlled.

## 4. Budgets, limits and configuration

- Configuration from flags and a config file, flags overriding the file. The reader is
  Dannan's to own (rule 3): ask Dannan; Lir keeps flags until it is tagged.
- Limits are per node; a session moving to a node with smaller limits is clamped and
  told so in CONNACK; mismatches are reported in `$lir/cluster`.
- Default limits configurable, defaulting to the moderate set: maximum packet 1 MiB,
  receive maximum 1024, 10k queued messages per offline session, session expiry cap 7
  days, 4 MiB outbound per client before a slow client is closed, 10k connections per
  node.
- Past the memory budget: drop QoS 0 oldest-first and count it, then refuse new QoS 1/2
  publishes with 0x97, then refuse new connections with 0x89. Existing sessions are never
  evicted.

## 5. What Lir does not do

- Kept: bridging to external brokers; total order across topics or across publishers on
  one topic across a failover; public multi-tenant service.
- Added: federation across regions beyond what one Dannan cluster spans.
- Removed: "no durable log or replay". Message history is in scope.
- In scope, newly: MQTT over WebSockets (ws and wss, Boost.Beast), MQTT-SN, a rules
  engine, message history.

## 6. Build sequence

- Unblocked work, security first: move to Dannan 0.3.0; scope-based topic authorisation
  and anonymous refused by default; MQTT over WebSockets; configurable system topics;
  the config file once Dannan has it; client certificates. Then step 5 on Dagda and the
  Lugh plugin as they unblock.
- The first release, 0.1.0 on `main`, follows step 5 (persistence on Dagda).

## 7. Open decisions

Decided while writing this section:

- Message history: opt-in by topic filter, each a bounded log (count and age) in Dagda
  at Replicated, read by subscribing with an MQTT 5 user property such as
  `lir-replay=<since>`.
- Rules engine: declarative routes (topic filter, conditions on JSON payload fields,
  actions republish to a templated topic, drop or set properties), stored as retained
  messages under `$lir/rules/`. No embedded scripting language.
- MQTT-SN: 1.2 and 2.0, as a gateway in `lir-node`.
- Server Reference: implemented, configurable. Cluster nodes are reachable, so a node may
  move a client to another node as part of load balancing (CONNACK 0x9C Use another
  server, or DISCONNECT 0x9C or 0x9D with Server Reference, MQTT 5.0 clients only).
  Policy (Tom, 2026-11-04): at CONNECT only. A node with clearly more clients than the
  cluster average (a configurable threshold, for example 20% over) refuses a 5.0
  CONNECT with 0x9C naming the least-loaded node, from the figures the nodes already
  advertise for `$lir/cluster`; live connections are never moved; 3.x clients, which
  cannot be redirected, are always served.

Left open:

- The shard count S (256) is fixed when a cluster is created, for now; resharding a live
  cluster may come later.
- Ordering across origins on one topic over an epoch change may interleave differently;
  it may be strengthened later.
- The grammar of scopes in Lugh tokens is Lugh's to define; Lir waits for it
  (separation of concerns).

## Further decisions (Tom, 2026-11-04, asked in turn)

Rules engine:

- Form: each rule is a retained JSON message on `$lir/rules/<name>`, so MQTT is the API,
  rules replicate like any retained message, an empty publish deletes one, and topic
  scopes say who may write them. An SQL form may come later.
- Actions: republish (templated topic: `{1}` the first wildcard level, `{topic}` the
  whole; optional QoS, retain, payload template), drop, set properties (user properties,
  content type, expiry), and transform a JSON payload (pick, rename, add fields).
- Conditions: the topic filter; JSON payload fields by path (=, !=, <, >, exists, in);
  user properties, content type, QoS, retain flag and the publisher's client id;
  combined with all, any and not.
- Loops: a message a rule produces carries a hop count; rules apply to it again, and
  past 8 hops it is dropped and counted in `$lir/nodes`.
- Where: on the publishing node, before fan-out; every node holds every rule.

Memory (section 4):

- The budget is configurable, default 512 MiB; the thresholds are configurable, default
  80% (shed QoS 0), 90% (refuse QoS 1/2 publishes), 100% (refuse new clients).
- Lir's held bytes are also leased in Dannan's node-wide ledger, in 1 MiB chunks; a
  refused lease counts as 100% until room returns.

MQTT-SN:

- Gateway discovery (ADVERTISE broadcasts): configurable, off by default; SEARCHGW is
  always answered.
- Predefined topic ids: retained messages on `$lir/mqttsn/topics/<id>`, changeable at
  runtime and replicated.
- DTLS beside plain UDP, with the same certificate settings and client certificates.

