# Lir - Active Work

Last updated: 2026-11-04. Steps 1 to 4 are on `develop` (step 4 on
`feature/cluster`, merged); gate `lir_tests` 151. `COMPLETED.md` is the
record. "Resume here": step 6, HTTP publish and SSE.

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
   `COMPLETED.md`. Sessions mirrored to configurable standbys (answer
   2): done (152).
5. **On Dagda.** Retained, inflight and sessions as head spaces with
   Lir's merges, Replicated writes for QoS 1/2, leases, objects for
   payloads over 64 KiB, the node-local sequence. Waits on Dagda, whose
   first priority is Macha's DHT.
6. **System topics, HTTP, authentication.** System topics done (154). `$dannan/` and `$lir/`
   retained topics from the Context's status; HTTP publish and SSE; a
   Lugh authentication plugin. No dependency but Lugh's tokens for the
   last.

## Tom's answers (2026-11-04, asked in turn)

1. Fan-out: direct, on Dannan's channels. Per-shard leases only where
   one writer matters: the inflight log and retained heads on Dagda
   (step 5).
2. Interim: sessions are replicated now, before Dagda: each persistent
   session mirrored in memory to standby nodes chosen by Dannan's
   placement, updated as it changes, taken from a standby when its node
   is gone. The number of standbys is configurable (Lir's default: one,
   two copies, as Dagda's k = 2). Retained everywhere and PUBACK on
   acceptance stay interim until step 5.
3. Retained ordering: the Lamport counter, then the node id, in the
   design too.
4. Load: up to about 10,000 messages a second across the cluster;
   messages mostly under 1 KiB; 64 KiB inline limit kept.
5. Header tagline: "Lir - The Message Broker".
6. Storage layout on Dagda approved: per-shard spaces for retained,
   inflight and sessions (3 x 256), writes batched per shard, the
   wildcard index in memory.
7. Namespaces kept: `mqtt`, `subs`, `shard`, `broker`, `edge`, `cluster`.
8. Next: session replication, then step 6 (system topics, then HTTP
   publish and SSE).
9. Still Tom's: the design doc's seven unwritten sections.

## What Lir waits on

- Dagda: head spaces with owner merges, Replicated writes, leases,
  objects and claims, a node-local space (step 5).
- Lugh: tokens an MQTT client can present (step 6's plugin).
- Dannan: nothing now. `ScopeSource` and `Identity::verify` when Lir
  checks topic scopes.
