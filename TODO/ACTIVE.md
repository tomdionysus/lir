# Lir - Active Work

Last updated: 2026-11-04. Steps 1 to 4 are on `develop` (step 4 on
`feature/cluster`, merged); gate `lir_tests` 151. `COMPLETED.md` is the
record. "Resume here": the questions for Tom below, then step 5 (waits on
Dagda) or step 6.

The design is Tom's, 2026-11-03: "Lir: Distributed Event System on
Dannan", a Claude Doc and the master, rendered in
[`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md) (revision 7; seven
sections unwritten). How the projects divide the work is
`../dagda/TODO/2026-11-03-stack.md`, the one copy, which has Lir in it
("Lir on Dagda", and Tom's "Decided (Tom, 2026-11-04)").

## What Lir is

Tom, 2026-11-03 night: "It must expose a MQTT interface and support all
MQTT functionality. But, it needs to be distributed, multimaster,
clustered, using Dannan as the network layer." Then: "Create a MQTT event
system that is clusterable and distributed based on Dannan." Lir is
written new; Macha has no event system to extract (`COMPLETED.md`,
2026-11-03 night).

- All of MQTT 3.1, 3.1.1 and 5.0. A feature left out is a finding.
- Distributed and clustered: a client connects, publishes, subscribes and
  resumes its session at any node.
- Multimaster: any node accepts any client operation; both sides of a
  split serve and merge after.
- Dannan is the network layer; what Lir persists lives in Dagda.

## Decisions

- (2026-11-04, Tom; stack document "Decided", and `../dannan/TODO/ACTIVE.md`)
  - Dagda durability levels: Ephemeral (memory), Persisted (disk, one
    node), Replicated (disk, k nodes per space, default 2), Durable (all
    available replicas). Lir: QoS 0 Ephemeral, QoS 1/2 Replicated.
  - No `lir.local`: other systems are ordinary MQTT clients of Lir.
  - Multimaster as Lir described it: any node accepts any operation, "the
    per-shard lease coordinates fan-out", both sides of a split serve and
    merge. (Step 4's interim fans out without it: question 1 below.)
  - Dagda's `open()` takes Lir's placement salt; Dagda reports each
    space's replica set.
  - Authentication is pluggable (`contract::Authenticator`); a Lugh plugin
    depends on Lugh. Token verification does not move into Dannan.
  - HTTP is each hosted system's own; Lir writes its own publish and SSE.
  - Message types from 512, registered freely (Lir: `0x4C00`-`0x4C02`).
  - One process per node: `lir-node` is a node and advertises `lir`.
  - Process-level tests are GoogleTest only, no Python.
  - Dannan `0.2.0` is cut; Lir builds against the tag.
- (2026-11-03 night, Tom) Lir is its own project and agent, with
  Dannan's styles, conventions and layout. Macha is not modified until
  Tom says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

## The plan

1. **Wire and topics.** Done (73).
2. **The broker on one node.** Done (125).
3. **The edge.** Done (132; the mosquitto process test landed in step 4).
   Open: TLS on the MQTT port.
4. **The cluster on Dannan.** Done as an interim (151): see
   `COMPLETED.md` and question 1.
5. **On Dagda.** Retained, inflight and sessions as head spaces with
   Lir's merges, Replicated writes for QoS 1/2, leases, objects for
   payloads over 64 KiB, the node-local sequence. Waits on Dagda, whose
   first priority is Macha's DHT.
6. **System topics, HTTP, authentication.** `$dannan/` and `$lir/`
   retained topics from the Context's status; HTTP publish and SSE; a
   Lugh authentication plugin. No dependency but Lugh's tokens for the
   last.

## Questions for Tom

1. **Fan-out without a shard primary (step 4).** Dannan's channels now
   route by gossiped prefix interest, so an edge publishes straight to
   the edges with matching subscribers: one hop, no primary, no lease.
   The design (and Tom's 2026-11-04 decision) has each shard's lease
   holder fan out. Recommended: keep direct fan-out for delivery, which
   needs no coordinator and loses nothing on a split, and use per-shard
   leases only where one writer matters (the inflight log and retained
   heads on Dagda, step 5). Or: route every publish through the shard
   primary as designed.
2. **Interim state, until step 5.** Every node holds every retained value
   (spread on `lir.retained`, fetched whole from each node that joins),
   not R shard holders; a session lives on the node its client last
   used and moves on connect (`lir.session_take`); PUBACK means accepted
   and queued for the other nodes, not replicated. A node's crash loses
   its sessions and what it had not yet sent. Accept as interim?
3. **Retained ordering.** The design's merge orders a topic's values by
   dot, "lowest origin byte-wise" across origins, so a later value from
   a higher node id loses to an earlier one. Step 4 uses a Lamport
   counter, then the node id: a value set after another node's value was
   seen always wins. Recommended for the design too.
4. **From earlier, still open:** the expected publish rate and size (for
   step 5's batching); the per-shard storage layout and in-memory
   wildcard index (stack document, "yours to bring to Tom"); the
   monotonic per-origin sequence on restart; the header tagline ("Lir -
   The Distributed Event System" is a placeholder); namespaces
   (`mqtt`, `subs`, `shard`, `broker`, `edge`, `cluster` so far).
5. **The doc's seven unwritten sections.** Writing them is Tom's; Lir
   can draft them into the doc on request.

## What Lir waits on

- Dagda: head spaces with owner merges, Replicated writes, leases,
  objects and claims, a node-local space (step 5).
- Lugh: tokens an MQTT client can present (step 6's plugin).
- Dannan: nothing now. `ScopeSource` and `Identity::verify` when Lir
  checks topic scopes.
