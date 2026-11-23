# The rewrite: Lir as MQTT in Dannan's and Dagda's nouns (2026-11-20)

A draft for the maintainer, before any code. It is written from the premise and the
mapping (`2026-11-19-premise-and-audit.md`), the contract
(`../docs/contract.md`), the conformance gate (`../docs/conformance.md`), and
what the stack has settled through the coordinating session
(`../coordination/TODO/2026-11-20-cluster-stack-placement.md`, Lir's
items 13 to 20 and L1 to L8). Nothing of the withdrawn code binds it.

## What Lir is

One MQTT broker made of a soft, leaderless cluster. Every node runs the
same `lir-node`; there is no node with a role of its own. A client connects
to any node and sees one broker. Any node may vanish, stall or be cut off,
and what MQTT acknowledged survives it.

Lir writes only what makes it MQTT:
- the codec,
- each connection's protocol,
- topic matching,
- the merge of its session record,
- the composition of nouns below.

Everything a cluster needs comes from below: membership, placement,
transport, splits, clocks and leases from Dannan, persistence and
replication from Dagda. A piece with no noun under it is a need for Dannan
or Dagda, put through the coordinating session, and Lir waits for it.

## The pieces

| Piece | What it does | Built on | Stands |
|---|---|---|---|
| `lir::mqtt` | Encodes and decodes every packet of 5.0, 3.1.1 and 3.1; validates topics and filters; matches a filter against a topic | Lir's own (per connection, no cluster); the types of `docs/contract-v1/lir/mqtt/` kept | Can be built now |
| Connection | One client's protocol: CONNECT and AUTH, keep-alive, the QoS 1 and 2 exchanges, packet ids, receive maximum, topic aliases, subscription options and identifiers, DISCONNECT | Lir's own, over Dannan's `listen::Stream` (TCP, TLS; WebSocket through `listen/beast.h`), Dannan's listener limits and `Budget` | Can be built now |
| Authentication | Who may connect and what a connection may do | `lir::contract::Authenticator` and `Grant` (kept); the Lugh plugin on `lugh_client`; the scope grammar where L3 settles it | Can be built now; the grammar's home waits on Lugh (L3) |
| Delivery | A publish reaches every node with a matching subscriber; each node matches its own sessions' filters | Dannan channels: `notify` for QoS 0, `stream` for QoS 1 and 2. A node subscribes each filter's literal prefix, with the pushed form for the SUBACK hold | Dannan has it |
| Client id | One connection per client id; a second CONNECT anywhere ends the first | A Dagda head-backed lease per client id (D5): holder, epoch, since. The newcomer reaches the holder with `Routes::call`; a lower epoch is refused. In a split both sides hold it, and the older connection ends at the heal | Waits on Dagda D5 |
| Session | Subscriptions, options, will, expiry interval and the next packet id survive the connection and resume at any node | A Dagda `replicas(N)` head space keyed by client id, with k-level commits (D1, D2), and Lir's merge. While a client is connected its node holds a working copy; every change is committed before it is acknowledged | Waits on Dagda D1, D2 |
| In flight and queued | Inbound QoS 1 and 2 held until delivered; outbound unacknowledged state; messages queued for an offline session; QoS 2 exactly once; resend in first-sent order | Dagda's log (D9): the entry is the message, acks carry the stage, retire leaves a tombstone for the window. Which shape the offline queue takes is Dagda's answer, still owed | Waits on Dagda D9, and Dagda's answer |
| Retained | Last value per topic, cleared by an empty payload, sent on SUBSCRIBE as the options say; wildcard reads | Dagda's register map (D6) with expiry per key; each node's index of retained topics kept from its key notices; values read from holders. The order stamp is L1 | Waits on Dagda D6, and the maintainer on L1 |
| Wills and timed ends | A will published when its connection ends abnormally, after its delay, or when its session expires, with nobody connected | A Dagda lease per will or session (D5) with its deadline in the session space; the holder arms Dannan's `Time::after`; a take-over at epoch+1 re-arms it; the publish carries (key, epoch) so duplicates drop. At least once across a split | Waits on Dagda D5, D1 |
| Shared subscriptions | Each message to one member of a group | Dannan `Nodes::placement(message key, 1, scheme, "lir/share/<group>")`: one advert per group on the nodes holding members, then a local member. Past the 64-advert bound: L7 | Dannan has it up to the bound |
| Message expiry | Counts the time a message waited, wherever | Dannan `hlc()`; the log entry's expiry (D9) | The clock is there; the queue waits on D9 |
| System topics | `$lir/nodes/<id>`, `$dannan/nodes/<id>`, `$lir/cluster`, client and roster events | Each node answers its own local subscribers from its own view: the roster watch, adverts, telemetry, the partition reading. `$lir/cluster` is computed alike on every node from the same adverts, so no node is "the publisher" | Dannan has it |
| `lir::client` | The client library of the contract | Rewritten to `docs/contract-v1/lir/client/client.h`; Dannan's connector and Dannan's backoff (`listen/backoff.h`) | Can be built now |
| Later | HTTP publish and SSE, MQTT-SN, the rules engine, opt-in history, Server Reference | The design's sections; HTTP waits on the audit (Dannan's, if the maintainer rules); MQTT-SN DTLS on Dannan 0.10.0 | After the core |

## What may be acknowledged, and when

The rule is "nothing acknowledges a guarantee it does not have". Each
acknowledgement waits for the noun that makes it true:

- **CONNACK** says Session Present only from a session view that is caught
  up: Dagda's per-space caught-up signal, which the maintainer has ruled.
- **PUBACK, and PUBREC at QoS 2,** go once Dagda's log returns `replicated`
  for the message (k replicas synced).
- **SUBACK** goes once the subscription is committed in the session space
  and the interest is held by the nodes the hold waits for (L2).
- **UNSUBACK** goes once the removal is committed.

A wait that misses its deadline is answered with a failure reason (5.0), or
by closing the connection (3.x, which has no reason codes there). The
client retries, on this node or another.

Until a noun ships, Lir says so in MQTT's own terms rather than pretending:
- **Before Dagda's log:** CONNACK gives Maximum QoS 0 and SUBACK grants QoS
  0. A 3.x client that publishes at QoS 1 or 2 is disconnected, since 3.x
  cannot be told.
- **Before the register map:** Retain Available 0.
- **Before replicated spaces:** the Session Expiry Interval is answered as
  0. 3.1.1 has no way to refuse a persistent session, so a 3.1.1 client
  with CleanSession 0 is told Session Present 0 each time.

This is MQTT's own way for a server to state what it offers. It makes
partial Lir honest, not interim. Each limit lifts when its noun ships.

## Tests

Every test runs on a cluster: Dannan's deterministic simulation harness
(`dannan/test/fixtures/sim/`, `SimNodes` on a `World`), real Dannan nodes
on one thread under one seed, with message loss, delay, partitions and
heals, clock offset and drift, and restarts with state kept or wiped. One
node is a cluster of one, never a separate mode.

- Each conformance statement becomes a test named by its identifier. A
  client is connected to some node chosen by the seed, and what it sees is
  checked against the statement.
- Failure tests are written for what the goals say a user never sees: a
  node killed mid-QoS 2, a split and heal with clients on both sides, a
  retired node, a slow node, a clock off.
- Statements whose noun has not shipped are written now and disabled with
  the noun named, counted as waiting, never as passing.

Needed from Dannan, through the coordinating session: whether hosted
systems may build on its harness as an installed fixture, as Dagda's D13
is for Dagda. Lir's harness use would be a second user, which puts it in
Dannan by the rule.

## Order of work

1. `lir::mqtt` and its tests, from the statements of sections 1 to 3.
2. The connection, authentication and delivery: QoS 0 across nodes,
   wildcards, subscription options, aliases, keep-alive, enhanced auth. The
   system topics. All on the harness. Maximum QoS 0, no retain, no
   persistent sessions, as stated above.
3. `lir::client` to the contract, so Louhi can move its pin.
4. As Dagda ships each noun: client-id leases (D5), sessions (D1, D2), the
   log for QoS 1 and 2 (D9), retained (D6), wills and timed ends. Each
   lifts its stated limit.
5. The later pieces.

## The status and admin dashboard (the maintainer, 2026-11-20)

The maintainer, to Lir, Louhi, Mimir and Kvasir: "You will expose an optional
(configurable) Status/Admin dashboard, served via an API over http, written
in React. The base directory of this react build must be configurable, and
default to your own install. the http server must be SPA capable (routes
first, then all routes serve the SPA), this must also be configurable. You
will expose OpenAPI for the API. Styling and structure should be taken from
Lugh's admin interface. You will allow both a username/password and Lugh for
authentication for this."

Where each part lives, under the no-duplication rule (the coordinator,
2026-11-20):
- Static files, routes first and then the SPA fallback, all configurable:
  Dannan's HTTP core.
- Username and password: Dannan's credential helper (item 11).
- Lugh sign-in: `lugh_client`'s relying party.
- The shared React shell and its styling: settled (2026-11-20). Lugh
  carves `lugh/ui` ("@lugh/admin-ui"): styles, `AdminLayout`, `NavBar`,
  `Messages`, `Confirm`, `useAPI`, `useLoad`, `useAct`, `lib/format`, and
  a sign-in screen with both methods. Lir fetches Lugh at a tag and depends
  on `file:<fetched path>/ui`; there is no registry and nothing is
  published.
  - The stack: React 19, React Router 7 as a static SPA, Vite 7,
    TypeScript 5.9, plain CSS, Vitest.
  - Lir has asked Lugh whether the package will also carry a cursor-paged
    table, an SSE live hook, a detail panel and a payload viewer.

Lir's own part is its API, described in OpenAPI, and its screens:
- the nodes as Lir sees them (from the roster and the partition reading);
- clients and sessions;
- subscriptions and shared groups;
- retained topics;
- message rates;
- the conformance figures;
- for an admin: disconnecting a client, clearing a retained topic, and
  the limits.

It is built after the HTTP core ships (Dannan's second minor), on the same
configuration as the rest of `lir-node`.

## Open, and with whom

- Settled since this was drafted (the maintainer, 2026-11-20): L1, retained values
  order by (hlc stamp, node id); HTTP and JSON are Dannan's; plain
  clients find a live node through Dannan's DNS responder (L8). Dagda's
  answers on D6 are in `ACTIVE.md`.
- L1, the order stamp for retained values: settled, (hlc stamp, node id).
- L2, the SUBACK hold, and L7, groups past 64 adverts: with Dannan.
- L3, the scope grammar: with Lugh.
- L8, an address for plain clients that survives a node: none planned
  (2026-11-23: no cluster DNS in Dannan or Manannan); plain clients are
  given node addresses or the operator's own DNS, lir::client the roster.
- The offline queue's shape against D9: owed by Dagda.
- HTTP in Dannan: with the audit.
- The harness as an installed fixture: to ask Dannan.
