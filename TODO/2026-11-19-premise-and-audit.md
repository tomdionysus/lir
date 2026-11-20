# The plan: Lir built on Dannan (2026-11-20)

Rewritten on 2026-11-20 after Tom's corrections. The first version, a plan
to audit the code piece by piece against five rules, is in the history at
`a0a6945`. Nothing here is done.

## The goal

Tom, 2026-11-03 night: "It must expose a MQTT interface and support all MQTT
functionality. But, it needs to be distributed, multimaster, clustered, using
Dannan as the network layer."

## Built on Dannan, not using Dannan

Tom, 2026-11-19 and 20: Lir is Dannan-based. Dannan has no special nodes, so
Lir has none. And more than "uses Dannan": "built on Dannan".

- **Using Dannan** is what was built. Lir is its own distributed system that
  calls Dannan as a library: a `cluster` subsystem with its own routes for
  snapshots, session take-over and session mirroring, its own placement key
  for standbys, its own de-duplication, its own rule for which node
  publishes the cluster's summary.
- **Built on Dannan** is the goal. Dannan is the distributed system, and
  Lir is MQTT expressed in its terms. Being leaderless, membership,
  placement, what happens in a split, how state moves as nodes come and go:
  none of these is Lir's to design. Lir inherits them by being made of
  Dannan's nouns, and Dagda's for what persists. Tom's design said so from
  the start: "It introduces no cluster-wide state of its own ... The table
  is the whole contract" (`2026-11-03-lir-plan.md`, "What Lir takes from
  Dannan").

What follows:

1. The question for every piece is "which Dannan or Dagda noun is this?" A
   piece whose answer is "none, it is Lir's own mechanism" is wrong by
   construction, however well it works and whatever its tests say.
2. A gap is never Lir's to fill. If MQTT needs something no noun provides,
   that is a need for Dannan or Dagda, built once there for every hosted
   system. Lir waits. No interim, no stopgap, and no such option is put to
   Tom.
3. Lir contains no kind of node that Dannan does not have. A design that
   needs a special node, a fixed set, or an operator to say where state
   lives has misread Dannan. The remedy is to read how Dannan already does
   it. (The withdrawn guarded-publish design read Dannan's agreement
   "genesis" as a permanent home. It is a starting point used once; slots
   then move with the roster.)
4. Nothing acknowledges a guarantee it does not have.
5. The cluster is never stopped as a whole to change it.

## The one real question

Tom, 2026-11-20: "what is required to implement full MQTT that Dannan does
not already provide, AND how would you do that building on top of Dannan?"

The Lir session's first answer follows, corrected on 2026-11-20 against the
contracts (step 1, below; the section "What the contracts hold" says what
was read).

**Per connection, with no cluster in it. Lir's own.** The codec; the
CONNECT, QoS 1 and QoS 2 exchanges; packet ids; keep-alive; receive
maximum; topic aliases; subscription options and identifiers; enhanced
authentication; topic filter matching (the trie). Dannan provides the
listeners under it.

**Across nodes:**

| MQTT needs | Built on | State |
|---|---|---|
| QoS 0 delivery to subscribers on other nodes | A `notify` channel (CONTROL, at most once, dropped under pressure), interest by prefix | Dannan has it |
| Order per publisher | QoS 0: `notify` has no sequence, but deliveries are posted in order per origin to a strand, with drops, which QoS 0 allows. QoS 1 and 2 while both nodes live: a `stream` channel's sequence per origin and destination, gaps refilled from a bounded replay buffer. A gap the buffer no longer holds, or the origin's restart (a new incarnation restarts the sequence), is reported as lost: past that, order and delivery are the log's. | Dannan has it, to the replay buffer's bound |
| Wildcard filters | Dannan routes by literal prefix; Lir's trie matches at the receiving node. A filter with no literal prefix (`#`, `+/x`) receives everything, and so does a node whose prefixes exceed the interest advert's 1 KiB: Dannan coarsens it to the empty prefix. Correct, not cheap. | Dannan has it |
| QoS 1 surviving a node's death | Dagda's log (design D9) | Waits on Dagda: not in its contract; slices 2 to 4 paused until Macha's cutover |
| QoS 2 exactly once | The log's retired dots, for the window | Waits on Dagda, as above |
| Retained messages, and wildcard reads of them | Dagda's register-map space (D6) and its keys-changed notice (`watch_keys`). The ordering stamp: Dannan `0.9.2` has `Context::hlc()`; D6 is written for (Lamport, node id). Which one is Tom's open question. | Waits on Dagda: today's notice (`Heads::watch`, `changed`) serves only every-node and node-local spaces |
| Session state resumable at any node | A Dagda `replicas(N)` head space (D1) with k-level commits (D2) | Waits on Dagda: `replicas: N` is refused today, naming L2 |
| System topics | Each node answers `$lir/...` for its own clients from its own view: the roster watch, adverts, `Telemetry`, the partition reading. Nothing is replicated, so no node is "the publisher". | Needs only Dannan; all present in `0.9.2` |
| Shared subscriptions | Corrected: Dannan's placement functions choose among nodes (`NodeRecord`s), not among a group's members. So the publishing node picks a node by `placement::rendezvous` over the nodes whose interest holds the group's prefix, keyed by the message, and that node gives it to one of its own members. Even across nodes, not across members; MQTT asks no fairness. A node the publish missed (`Published::missed`) gives way to the next in rendezvous order. No counters, no group state. | Needs only Dannan |
| A will when the client's node dies | The will is in the session's Dagda state. The trigger is Dannan's roster watch: every node sees a `left` once the node is unheard for `dead_after`. Every replica that sees it fires the will under one deterministic dot, so duplicates collapse. A split makes the other side "leave" while its clients are still connected there, so their wills fire: a guarantee question for Tom. | Waits on Dagda |

**Not known.** Tom: "It's OK to say 'I don't know'."

1. **One connection per client id.** A second CONNECT with an id must end
   the first, wherever it is (`[MQTT-3.1.4-3]`). Two nouns could carry it,
   and which is not known:
   - Dannan's agreement, a register per client id naming its holder. One
     holder exactly, but a minority side is refused, so no client could
     connect there: that contradicts "both sides of a split serve". Its
     acceptor state is the hosted system's to keep, so it waits on Dagda.
   - A channel the holding node subscribes to under the client id. Best
     effort, and both sides of a split hold the id. One prefix per
     connected client overflows the 1 KiB interest advert, which then
     reads as everything, so at any size every take-over goes to every
     node.
   Dagda's `Leases` are not a third: they live in one node's memory today.
2. **The subscriptions of an offline session.** While a client is away,
   something must match publishes against its filters and queue them. No
   node has the client, so whose interest is it? A candidate, unchecked
   with Dagda: the publishing node matches every session's filters (each
   node knowing them from the session space's key notices, as it knows
   retained topics) and appends to the session's queue under its own
   message dot. One appender per message, so nothing to de-duplicate, and
   Dagda's rule that a dot's origin is its appender holds. Its cost is
   every node holding every session's filters. Otherwise it is a noun
   that does not exist: an interest that outlives the node that declared
   it.
3. **Endings by time with nobody connected:** session expiry, will delay,
   expiry of a queued message. A queued message: a log entry carries its
   expiry (D9). A session: the register map's expiry per key (D6), but an
   expired key "is dropped with no commit", so nothing is told, and
   session expiry and will delay must each publish a will when they end.
   Dannan's only timer is `Time::after`, on one node. So a need, to Dagda
   or Dannan, not yet known which: something that acts when a key's time
   ends with no node holding the client.

## What the contracts hold (step 1, 2026-11-20)

Read whole: Dannan `0.9.2`'s `context.h`, `agree/agreement.h`,
`net/fanout.h` and `placement/placement.h` (the `../dannan` checkout is
another session's feature branch, older than the tag, so the tag was read);
Dagda's `context.h` at `0.3.2`, contract version 2 at slice 1; and Dagda's
`TODO/2026-11-13-three-systems-design.md` for what it has designed for Lir.

Dannan `0.9.2`, as it bears on MQTT:
- Channels: `notify`, `stream` and `bulk` as in the table. No exactly-once
  contract and no order across origins. One interest advert per channel,
  at most 1 KiB; a node carries at most 64 adverts of 1 KiB.
- Nodes: the roster watch (`joined`, `left` after `dead_after`,
  `changed`), adverts with a pushed form that returns who holds them, the
  partition reading against the agreed roster, `members()`, and placement
  as pure functions of a key and a list of nodes.
- Agreement: a register per name, leaderless, from any node; the acceptor
  state kept by the hosted system's `AcceptorStore`, in memory or durably;
  one name per change until Dannan's atomic commit (several throws); a
  strict read refused on a minority side; `Outcome::unknown` when a retry
  cannot tell whether the change took.
- `hlc()`: one hybrid logical clock per node, shared by every hosted
  system, never repeating across restarts.
- Nothing durable, and no timer across nodes.

Dagda's contract today holds Objects, Claims, Tickets, every-node and
node-local head spaces with the change notice, and Leases in one node's
memory. It does not hold any noun Lir's table waits on: `replicas(N)`
spaces (D1), k-level commits (D2), head-backed leases (D5), the register
map (D6), or logs (D9). Those are slices 2 to 4, paused by Tom until
Macha runs on Dagda.

Found on the way:
- Lir does not link Dagda at all today: nothing in its CMake names it,
  though `CLAUDE.md` says it links Dannan and Dagda.
- Dagda's design for Lir (a log per shard, a lease per shard whose holder
  Lir computes, a session space per shard with a take-over counter) was
  written from Lir's answers of 2026-11-13 and 14, before the premise.
  It is re-read at step 3 against the premise, not taken as given.

So the first answer to the question is short: topic matching, which is
Lir's and local, and possibly one new noun for item 2. Nearly everything
Lir built as its own mechanism has a home already, or waits for one in
Dagda.

## How much of MQTT Lir supports now

Not known as a measured figure, and that is itself a finding.

- By the record, on one node every feature of MQTT 3.1, 3.1.1 and 5.0 is
  built and has tests named for behaviours (`COMPLETED.md`, 2026-11-04,
  steps 2 and 3 and "the specification's MAYs"; 61 tests in
  `broker_test.cpp`).
- Lir's own rule is that "supports MQTT" is defined by named gates, the
  specification's conformance statements, each tested. **No such gate
  exists.** No source or test file names a conformance statement (the
  specification's `[MQTT-x.y.z-n]` identifiers), and no list maps
  statements to tests. So "all of MQTT" is felt, which is what the rule
  forbids, and the README's "each tested" is not shown.
- Across nodes the features work, but several do not give the guarantee
  MQTT states: a QoS 1 or 2 publish is acknowledged before anything but
  the publishing node holds it; a session's last 100 ms of change is lost
  with its node; retained values are lost if every node restarts.

## The work, in order

Nothing is built or deleted before step 5.

1. **Read the contracts.** Dannan's `Context` and Dagda's, whole, through
   basemind, and each project's capability statement. Correct every row of
   the table above against what is actually there. This is reading what
   they provide, not investigating their code.
2. **The conformance gate.** List every normative statement of MQTT 5.0
   and 3.1.1 (and what 3.1 differs in), each by its identifier, in one
   file. Against each: the test that proves it, or "none". Mark which
   statements bear on more than one node. This measures how much of MQTT
   Lir supports, on one node, and it is the gate the project's rule always
   asked for. It needs no build.
3. **The mapping.** For each statement that bears on more than one node,
   and each row of the table: the noun it is built on, or the need. Put
   the three unknowns, and anything else found, to the Dannan and Dagda
   sessions as needs, in MQTT's terms, without proposing their design.
4. **A second reader.** A fresh agent, given Tom's design, this file and
   the contracts, and none of Lir's conclusions, answers the one real
   question independently. The differences go to Tom.
5. **To Tom:** the conformance figure, the mapping, the needs, and what
   of the present code is the thin mapping and what is Lir's own
   mechanism.
6. **Then code**, tests first, on a clean branch from the contracts.
   What needs only Dannan can be built then. What waits on Dagda waits.

## Guards against keeping what should go

Tom, 2026-11-19: "the temptation will be to keep code which was written
with a fundamentally flawed premise." The Lir session wrote that code and
its 361 tests pass. Both pull towards keeping it.

1. `lir/cluster/` is not audited piece by piece for what can stay. It is
   Lir's own distributed system and by construction should mostly not
   exist. What replaces it is written from the mapping, and should be
   small enough that there is little in it to get wrong.
2. Tests do not argue for the code under them. A test that pins one of
   Lir's own mechanisms is rewritten to the behaviour MQTT requires, and
   its count kept. Whether "a test is never deleted" allows that is put
   to Tom with step 5.
3. Lines and tests kept are never reported as a success.
4. Findings go to Tom whole before anything is deleted or rewritten.

## What is already known to be Lir's own mechanism

Confirmed present on 2026-11-19. Each is Lir doing what a noun should do.

| What | Where |
|---|---|
| Session standbys: each session mirrored to other nodes on Lir's own route and placement key | `lir/cluster/cluster.cpp:392`, `flush_mirrors` at `413`; route `0x4C03` |
| Retained values on every node, with a snapshot fetched by a node that joins | `lir/cluster/cluster.cpp`; route `0x4C00` |
| Session take-over asked of every node | `lir/cluster/cluster.cpp`; route `0x4C01` |
| De-duplication of QoS 1 and 2 by origin, incarnation and sequence | `lir/cluster/cluster.cpp` |
| "The Lir node with the lowest id" publishes `$lir/cluster` and the roster events: a special node | `lir/system/system.cpp:245`, `publisher()` |
| Shared groups spread on `lir.groups` and handed out in turn | `lir/cluster/cluster.cpp`; route `0x4C02` |
| PUBACK and PUBREC on acceptance | `lir/broker/broker.cpp` |
| The MQTT-SN gateway drives DTLS itself | `lir/sn/gateway.cpp` |
| `SequenceBlocks` and the stored entry forms, built ahead of Dagda | `lir/cluster/sequence.*`, `codec.*` |

## The standing cluster

It runs that code and carries the claim board. It stays as it is until
Tom says otherwise. Nothing is added to it, and no new behaviour is
offered to the board from it.

## Guarded publish

Withdrawn (Tom, 2026-11-19; the design is at `9712275`). It is redrawn
only after the work above, as a question in the same form: which nouns is
it built on? What is known: a register per topic in Dannan's agreement, so
a guard over a filter needs Dannan's atomic commit across names; acceptor
state in Dagda; replica sets that follow Dannan's placement. What the
board asked for is
`../lir-develop-cluster/TODO/2026-11-15-claim-board-0.4.0.md`, part 1.
Whether a running cluster can start using agreement one node at a time is
a question for the Dannan session, not a gap Lir has shown.
