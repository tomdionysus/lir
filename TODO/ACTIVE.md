# Lir - Active Work

Last updated: 2026-11-03, night. The repository has had `git init` and
holds no commits. Lir's plan is not set: the design is partly written and
the build sequence is one of its unwritten sections. `COMPLETED.md` is the
record.

The design is Tom's, 2026-11-03: "Lir: Distributed Event System on
Dannan", a Claude Doc and the master, rendered in
[`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md) (revision 7; seven
sections unwritten). Its predecessor, "Macha Events: Distributed Event
System with MQTT Interface" (Claude Doc, 2026-11-02), is not the design.
The Dannan plan's consumer table (`../dannan/TODO/2026-11-02-dannan-plan.md`,
"Event system" row) says what Dannan was designed to give an event system.

How the projects divide the work is `../dagda/TODO/2026-11-03-stack.md`,
the one copy; Lir is not yet in it (below).

## What Lir is (Tom, 2026-11-03 night)

"It must expose a MQTT interface and support all MQTT functionality. But,
it needs to be distributed, multimaster, clustered, using Dannan as the
network layer."

- All of MQTT: 5.0 and 3.1.1 (and 3.1, protocol level 3, unless Tom says
  otherwise), every packet and property, QoS 0/1/2, retained, wills
  (with delay), wildcards, shared subscriptions, persistent sessions and
  session expiry, message expiry, topic aliases, subscription
  identifiers and options (no local, retain as published, retain
  handling), request/response, flow control (receive maximum, maximum
  packet size), enhanced authentication, server reference. A feature
  left out is a finding, not a choice.
- Distributed and clustered: any node is the broker; a client connects,
  publishes, subscribes and resumes its session at any node.
- Multimaster: no node is the master and no step needs one. Any node
  accepts any client operation; mutable state is heads that merge after a
  partition rather than being refused.
- Dannan is the network layer (stack rule 1).

## Decisions

- (2026-11-03 night, Tom) Lir is its own project, with its own agent, and
  takes the stack's styles, conventions and layout (Dannan's).
- (2026-11-03 night, Tom) Macha is not modified until Tom says so.
- (2026-11-03 night, Tom, after the finding below) "Create a MQTT event
  system that is clusterable and distributed based on Dannan." Lir is
  written new; nothing is extracted from Macha.

## The plan

Steps, one session each, in order. Each lands on a `feature/` branch
off `develop` when Tom asks for commits.

1. **Wire and topics** (no dependency). Done 2026-11-03 night, gate
   `lir_tests` 73; see `COMPLETED.md`. CMake against a pinned Dannan
   commit (`63c2f51`, `feature/wire-v1`, as Lugh pins); `lir::mqtt`:
   every packet and property of MQTT 3.1, 3.1.1 and 5.0, encoded and
   decoded with every MUST of the specification checked; topic names,
   filters, `$share` parsing and matching; the subscription trie; the
   shard function. Fuzz target for the decoder. Gate: `lir_tests`.
2. **The broker on one node.** Done 2026-11-04, gate `lir_tests` 125;
   see `COMPLETED.md`. The protocol engine over an abstract
   connection, tested without sockets: connect and session takeover,
   clean start and session expiry, QoS 0/1/2 both ways, retained and
   retain handling, wills with delay, message expiry, topic aliases,
   subscription identifiers and options, shared subscriptions, receive
   maximum, maximum packet size, keep alive, enhanced authentication
   as an interface, server reference.
3. **The edge.** Asio TCP listener on the node's io context (TLS
   after), `lir-node`, a two-process test with a real MQTT client.
4. **The cluster on Dannan.** Shards and holders by Dannan placement
   over `lir`-advertising nodes; publish, deliver and subscribe over
   Dannan channels and routes; shard state replicated to holders in
   memory; sessions resumed on any node; the soft global wildcard
   index. Until Dagda has leases, a shard's primary is its first
   active holder in placement order (no epoch fence: the weakness is
   stated, and step 5 removes it). Compose bed.
5. **On Dagda.** Retained, inflight and sessions as head spaces with
   Lir's merges, leases with epochs, objects for payloads over 64 KiB,
   the node-local sequence. Waits on Dagda.
6. **System topics, HTTP, authentication.** `$dannan/` and `$lir/`;
   HTTP publish and SSE (finding 5); Lugh tokens and scopes (finding 4).

Until step 5 Lir persists nothing: a node's state is memory, held on
`R` holders, so a restart loses only what every holder lost. That keeps
rule 2 (nothing persisted outside Dagda).

## What Macha has (from the Macha session, 2026-11-03 night)

Macha has no general distributed event system: no MQTT, pub/sub,
topics, QoS, retained messages, wills, persistent subscriptions, or SSE
or events endpoint (confirmed by file names: `src/cluster/node_events.hpp`
is the only event file). The "Macha Events" doc (2026-11-02) was never
built. Three narrow mechanisms exist:

| Mechanism | Where | What it is | Home on the stack |
|---|---|---|---|
| NodeEvents | `src/cluster/node_events.hpp:31` (kinds at :12) | In-process, per node: a monotonic count per kind (storage, metadata, topology, claims); consumers wait on a condition and diff counts, so coalescing is by design. No payload, no queue, producer never calls out. Main consumer `service/maintenance.cpp` (14 sites). | Macha's own, or Dannan's `Context::events()`; not Lir |
| Cluster notices and gossip | `src/cluster/net.hpp:28` (metadata_notice 9, members 2, telemetry 24, session_sync 39, user_sync 42, session_retire 12, dial_request 43) | Fire-and-forget hints, no order or dedup; correctness never depends on them, a missed notice is recovered by pull | Dannan (membership, telemetry, notify channels); Dagda (head notices) |
| Observations | `src/observation.hpp:107` | Local measurement registry written as JSON lines to a local file; never reaches Status or the API | Dannan's observations |

Tests that pin them: `tests/test_foundations.cpp` (NodeEvents),
`test_contract.cpp`, `test_observation.cpp`, `test_rpc_cluster.cpp`,
`test_hydration_catalogue.cpp` (missed-notice recovery). Inventory rows
42, 49, 51, 88-92, 117-126 (`../dannan/TODO/2026-11-03-macha-function-inventory.md`).

The Macha session's warnings: keep "producer never calls out, consumers
coalesce"; never make Macha correctness depend on a notice's delivery;
the kinds are Macha-specific; `observations()` is a process singleton
shared by in-process test clusters; viewer traffic stays ahead of
loader and background (laws).

**Finding.** There is nothing in Macha for Lir to extract. Each of the
three mechanisms already has a home lower in the stack, and none is an
MQTT broker. Recommended: Lir is written new from its design; Macha's
mechanisms go where the stack puts them; "Macha uses Lir" means Macha
(and Dannan's Status) publishing state to Lir as an MQTT client, for
dashboards and automation, never as something Macha's correctness
rests on. For Tom.
- (2026-11-03 night, Tom) The requirement above.
- From the stack, binding Lir: Tom's rules 1 to 4; dependencies point
  down only; hosted systems never address one another; the async shape
  (deadline, caller's executor, completion posted, never inline); each
  Context versioned from 1, a mismatch refused.

## Findings against the stack

The doc was written before Tom's rules for the stack. Where they
differ, the rules win; each item is a recommendation for Tom.

1. **Store nouns are Dagda's.** The doc calls Head, Object, Claim,
   Lease and Ticket Dannan nouns ("written entirely against the Dannan
   API at seam 8"). Under rule 2 and the stack document they are
   Dagda's (`dagda::Context`). Recommended: Lir takes Node, Channel,
   placement, membership, Budget, Fault, Status and the scope check from
   `dannan::Context`, and Head, Object, Claim, Lease and Ticket from
   `dagda::Context`; Lir links both. The design is otherwise unchanged:
   the merges stay Lir's.
2. **Multimaster and the shard primary.** The doc gives each shard one
   primary, the holder of a Dagda lease, which fans out and stamps the
   epoch. Every node still accepts every client operation, there is no
   election, and in a partition each side's lease holder serves and the
   heads merge after. Recommended: that is multimaster as Tom means it
   (no master, no refusal under partition), with the per-shard lease as a
   fan-out coordinator rather than a write master. Tom to confirm, or say
   that any holder must fan out without a lease.
3. **`lir.local` and other hosted systems.** The doc has Macha, Lugh and
   others publish `$macha/`, `$lugh/` topics into Lir "over `lir.local`",
   a channel not in its channel table. Hosted systems never address one
   another (stack, Principle 4). Recommended: a hosted system that wants
   its state on the bus is an MQTT client of Lir like any other, over
   loopback, authenticated by Lugh like any other; no `lir.local`.
   `$dannan/` topics are Lir reading its own `dannan::Context` status,
   which needs no other system.
4. **Authentication.** The doc's edge does "Lugh scope checks". Lir never
   links Lugh. Recommended: MQTT CONNECT carries a Lugh-issued OAuth
   access token (password field, or MQTT 5 enhanced authentication),
   verified locally against Lugh's published signing keys; topic
   permissions are scopes checked by Dannan's `Identity::verify` /
   `contract::ScopeSource`. Token verification (JOSE, signature checks)
   is then needed by Lugh and Lir, so under rule 3 it moves down to
   Dannan beside the signing primitives. For Tom and the Lugh and Dannan
   sessions.
5. **HTTP publish and SSE.** The HTTP stack is Lugh's, and the stack
   document says a second project needing it is a question for Tom then.
   Lir is that second project. Recommended: the HTTP server moves down to
   Dannan's foundations when Lir's HTTP endpoint is built, not before;
   the endpoint is the last piece of Lir's plan.
6. **The per-origin sequence.** `seq` is "per-edge, persisted". Under
   rule 2 that persistence is Dagda's: a node-local space or a Dagda
   per-origin counter. For the Dagda session.
7. **Message numbers.** The stack's proposed blocks have none for the
   event system. Recommended: Lir 5120-6143, beside AthenaSIP's.
8. **The `lir` capability.** The stack's "one process per node" lists
   `lugh`, `macha`, `athena`; `lir` joins it.

## Open with Dagda (2026-11-03 night)

Dagda put Lir in the stack document (its findings 1-8 are item 11 there
for Tom) and read Lir's uses against the plan. Lir's answers, sent to
the Dagda session; 4, 5 and 7 change the doc's design and wait on Tom:

1. Durability per QoS path: QoS 0 waits on nothing. QoS 1/2 PUBACK
   waits on the acceptance certificate (plan 145) of the inflight commit
   holding the message; for an out-of-line payload, first the object's
   ticket (plan 132), then the commit that names it. A retained update
   waits on its commit's certificate for QoS 1/2. The doc's
   "Ticket" row reads "certificate" for commits.
2. Keys, not spaces: `lir/retained/<n>`, `lir/inflight/<n>`,
   `lir/sessions/<n>` for `n < S`, each `replicas: 3`, topics and client
   ids as Merkle keys (plan 149). That is 3 x S spaces (768 at S = 256);
   Dagda to say whether that is acceptable. Sessions are then placed by
   session shard, not by HRW per client id as the doc has it. A shard's
   space replicas must be Lir's shard holders. Dagda's reply: no bound
   on spaces in its design (one would come from its budgets; S stays
   Lir's knob); recommended both, `open()` taking Lir's salt as a Dannan
   `placement::Scheme` so Lir computes the same replica set, and the
   Heads noun returning a space's replica set as evidence, so Lir checks
   rather than assumes. Recommended, not yet designed.
3. The per-origin sequence lives in a Dagda node-local space and is
   monotonic across restarts. Dannan (2026-11-03 night):
   `Identity::durability_epoch` is a random 128-bit id, fresh per
   process, unordered: it says "same process", never "newer". So it is
   not part of the dot's order. The retained-set merge compares dots of
   one origin by `seq`, which must therefore never go back: on start a
   node takes `seq = max(persisted, highest seq of its own origin in
   any head it holds or fetches from replicas) + 1` (discipline 1,
   re-derive), so a node that lost its disk does not lose to its own
   old messages. The durability epoch rides in the envelope for dedup
   and diagnosis only. Proposed; for Tom.
4. The inflight log commits once per shard per tick (`batch_delay_ms`),
   a dot per Merkle key. The expected publish rate and size are Tom's to
   give (the doc's budgets section is unwritten).
5. No wildcard-index head (plan 289: derived indexes are rebuilt, not
   stored). The global wildcard index is soft state in memory on every
   node, announced over `lir.seq` by each session shard's primary and
   re-announced every `resync_ms`.

6. Durability flags (Tom's decision for Dagda, 2026-11-03 night): every
   Dagda write carries Ephemeral (queued), Persisted (acknowledged on
   one node) or Durable (on every replica), chosen by the caller; this
   replaces waiting on tickets and certificates by hand (item 1).
   Still open with Tom (`../dagda/TODO/2026-11-03-macha-store-requirements.md`
   section 8 item 1): a k-of-N level, what "acknowledged" means, and
   what Durable does with a replica down. Lir's answer, sent to Dagda:
   Lir needs k of N, with k = `write_copies` (default 2 of 3), for QoS
   1/2 PUBACK and QoS 1/2 retained updates; QoS 0 is Ephemeral; Lir uses
   neither Persisted (an acknowledged message dies with the accepting
   node) nor Durable (one dead holder stalls the shard). Acknowledged as
   synced to disk is fine. Short of k by the deadline: evidence of who
   acknowledged, write not undone; Lir sends no PUBACK (or 5.0 reason
   0x80), and the client's retry is absorbed by dot dedup.

## Waiting on Tom

1. Findings 1 to 8 above, and "Open with Dagda" 2, 3 and 5 (design
   changes), 4 (the expected rate) and 6 (a k-of-N durability level,
   which Lir's PUBACK needs).
2. The doc's unwritten sections, the build sequence among them; Lir's
   plan follows from that section.
3. The header block's second line. `CLAUDE.md` uses "Lir - The
   Distributed Event System" as a placeholder.
4. Versioning: Lir takes its own number from `0.1.0`, pinning a Dannan
   tag and a Dagda tag.
5. Namespaces `lir::<subsystem>`. Proposed from the design: `mqtt` (the
   codec and protocol state), `edge` (listener, client sessions),
   `shard` (placement, holders, leases, fan-out), `subs` (tries and
   subscription tables), `heads` (Lir's merges and codecs), `system`
   (the `$dannan/` and `$lir/` topics).

## What Lir waits on

- Dagda: heads, objects, claims, leases and tickets; nothing persistent
  in Lir can be built before Dagda's Context exists. The MQTT codec,
  tries and merge functions can, as pure code with tests. Dagda's first
  priority (Tom, 2026-11-03 night) is replacing Macha's DHT, so its step
  order follows Macha. Everything Lir needs is something Macha's store
  already does in some form, so it arrives with that extraction, except
  the node-local space (written for Lugh).
- Dannan: a tag carrying the Context and an install target, as for
  Dagda; `ScopeSource` and `Identity::verify`. On `feature/wire-v1`
  (`99b7fbb`, from the Dannan session, 2026-11-03 night):
  - There now: placement (`Nodes::placement(key, copies, scheme,
    only_advertising = "lir")`, `placement::rendezvous` over
    `placement::with_advert(nodes, "lir")`); `Budget::declare` and
    `reserve`; `supervise/` and `Context::faults()`; status, events,
    readiness, telemetry, diagnostics (the `$dannan/` topics'
    sources); the foundations (`identity/crypto.h`,
    `secure_memory.h`, `signing.h` with ES256 and Ed25519, `argon2.h`,
    `key_custody.h`, `durable_file.h`, `net/codec.h`, `contract/`,
    `status/log.h`, `dannan/test/fixtures/`). Nothing enforces ranges
    above message number 1023.
  - Channels landed at Dannan `2f16981` (head `1de966f`), API in
    `dannan/net/fanout.h` and `Context::channels()`: every node that
    publishes or subscribes opens the channel; subscriptions are
    node-local by topic prefix, gossiped as advert `dannan.ch.<channel>`
    (prefixes over 1 KiB coarsened to ""), so Lir subscribes to the
    literal part before an MQTT wildcard and filters itself; a stream
    gap is refilled from the sender's replay buffer, and what it no
    longer holds arrives as `ChannelMessage::lost`; every message
    carries its origin's incarnation, and a new one restarts the
    sequence. Step 4 moves Lir's pin to it. Formerly:
  - Being built next by Dannan: Channels with delivery contracts
    (Dannan plan, "Level 1"): `lir.notify` is a notify channel
    (CONTROL, at most once, dropped under pressure), `lir.seq` a stream
    (CONTROL, at least once, per-origin sequence, bounded replay from
    the sender), `lir.bulk` a bulk channel (DATA, backpressured). The
    plan's node-local topic subscription, forwarded only to nodes with
    subscribers, is Dannan's and Lir should use it rather than
    re-announce where it fits.
  - The membership feed landed at Dannan `f5dd71e`:
    `Nodes::watch(executor, handler)` returns an id, `Nodes::unwatch(id)`
    stops it; the handler gets `RosterChange {joined | left | changed,
    node}`, first every node active now, then each change, posted in
    order on a strand. A restart arrives as `changed` with a new
    `NodeRecord::incarnation` (the durability epoch, gossiped in the
    record); a node silent past `dead_after` arrives as `left` within a
    heartbeat. Lir's soft wildcard index drops an edge's announcements
    when its incarnation changes. Step 4 pins at or after `f5dd71e`.
- Lugh: issuing tokens an MQTT client can present.
