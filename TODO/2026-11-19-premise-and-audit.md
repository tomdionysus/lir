# The plan: Lir built on Dannan (2026-11-20)

Rewritten on 2026-11-20 after the maintainer's corrections. The first version, a plan
to audit the code piece by piece against five rules, is in the history at
`d005e79`. Nothing here is done.

## The goal

The maintainer, 2026-11-03 night: "It must expose a MQTT interface and support all MQTT
functionality. But, it needs to be distributed, multimaster, clustered, using
Dannan as the network layer."

## Built on Dannan, not using Dannan

The maintainer, 2026-11-19 and 20: Lir is Dannan-based. Dannan has no special nodes, so
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
  Dannan's nouns, and Dagda's for what persists. The maintainer's design said so from
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
   The maintainer.
3. Lir contains no kind of node that Dannan does not have. A design that
   needs a special node, a fixed set, or an operator to say where state
   lives has misread Dannan. The remedy is to read how Dannan already does
   it. (The withdrawn guarded-publish design read Dannan's agreement
   "genesis" as a permanent home. It is a starting point used once; slots
   then move with the roster.)
4. Nothing acknowledges a guarantee it does not have.
5. The cluster is never stopped as a whole to change it.

## The one real question

The maintainer, 2026-11-20: "what is required to implement full MQTT that Dannan does
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
| Retained messages, and wildcard reads of them | Dagda's register-map space (D6) and its keys-changed notice (`watch_keys`). The ordering stamp: Dannan `0.9.2` has `Context::hlc()`; D6 is written for (Lamport, node id). Which one is the maintainer's open question. | Waits on Dagda: today's notice (`Heads::watch`, `changed`) serves only every-node and node-local spaces |
| Session state resumable at any node | A Dagda `replicas(N)` head space (D1) with k-level commits (D2) | Waits on Dagda: `replicas: N` is refused today, naming L2 |
| System topics | Each node answers `$lir/...` for its own clients from its own view: the roster watch, adverts, `Telemetry`, the partition reading. Nothing is replicated, so no node is "the publisher". | Needs only Dannan; all present in `0.9.2` |
| Shared subscriptions | Corrected: Dannan's placement functions choose among nodes (`NodeRecord`s), not among a group's members. So the publishing node picks a node by `placement::rendezvous` over the nodes whose interest holds the group's prefix, keyed by the message, and that node gives it to one of its own members. Even across nodes, not across members; MQTT asks no fairness. A node the publish missed (`Published::missed`) gives way to the next in rendezvous order. No counters, no group state. | Needs only Dannan |
| A will when the client's node dies | The will is in the session's Dagda state. The trigger is Dannan's roster watch: every node sees a `left` once the node is unheard for `dead_after`. Every replica that sees it fires the will under one deterministic dot, so duplicates collapse. A split makes the other side "leave" while its clients are still connected there, so their wills fire: a guarantee question for the maintainer. | Waits on Dagda |

**Not known.** the maintainer: "It's OK to say 'I don't know'."

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

   **Dannan's answer (2026-11-20):** the noun is Dagda's store lease,
   whose design names "MQTT client-ID uniqueness" as a user
   (`../dannan/TODO/2026-11-02-dannan-plan.md:171`, shape at `:161-165`,
   `:219`): `acquire` returns holder, epoch and since; a newcomer asks
   who holds the id and reaches that node with `Routes::call`; every
   action carries the epoch and a lower one is refused. In a split both
   sides serve: the side without the holder takes the lease at epoch+1
   after the absence window, so two holders coexist, and at heal the
   higher epoch wins and the other connection is ended. So under a split
   `[MQTT-3.1.4-3]` is met at heal, not at the second CONNECT, which no
   design without a majority can better. Head-backed leases are Dagda's
   D5, not shipped. This retires unknown 1 into "waits on Dagda", with a
   stated limit for the maintainer.
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

   **Settled with Dagda (2026-11-20):** the candidate fits D9. The node
   where a PUBLISH lands matches every session's filters and appends under
   its own message dot to the queue log's k replicas; a retried publish
   lands once. The queue log is per shard, the stream key the client id.
   Since one message often matches several offline sessions in a shard,
   D9's identity becomes (stream, dot), and acks and retire name both.
   Reconnect reads the stream, then acks and retires. Every node keeps
   every session's filters from the session space's key notices and
   paged reads at join; that cost goes into Dagda's slice 0 measurement
   (Lir's working figure: 40,000 sessions on 4 nodes, a third offline,
   about 5 filters each). If too large, the fallback is matching by the
   shard's lease holder, a duty that moves, one hop more.
3. **Endings by time with nobody connected:** session expiry, will delay,
   expiry of a queued message. A queued message: a log entry carries its
   expiry (D9). A session: the register map's expiry per key (D6), but an
   expired key "is dropped with no commit", so nothing is told, and
   session expiry and will delay must each publish a will when they end.
   Dannan's only timer is `Time::after`, on one node. So a need, to Dagda
   or Dannan, not yet known which: something that acts when a key's time
   ends with no node holding the client.

   **Dannan's answer (2026-11-20):** nothing in either design covers it.
   It could be composed of existing nouns: a lease per will, whose holder
   arms `Time::after` at a deadline kept in a head space, re-armed by a
   take-over at epoch+1, the publish carrying the epoch. In a split that
   is at least once, not once. Dannan has put to the maintainer whether that is
   acceptable and whose noun it is, recommending the lease composition,
   and will relay his answer.

   **Dagda's answer (2026-11-20), under the maintainer's no-duplication rule** (a
   piece goes into a shared dependency only when two or more projects
   need it): Louhi and Lugh do not need it, so the action is Lir's to
   compose, not a Dagda noun. Lir builds it from nouns only: a Dagda
   head-backed lease per will or session (D5, which Lir, Lugh and Louhi
   all need), its deadline in a Dagda head space, the holder arming
   Dannan's `Time::after`, a take-over at epoch+1 re-arming it, and the
   publish carrying (key, epoch) so a duplicate is dropped. At least once
   across a split. For the maintainer: whether a composition of nouns, with no
   state or mechanism of Lir's own, meets the premise's "a gap is never
   Lir's to fill". Lir reads it as yes. It waits on D5 either way.

## The mapping (step 3, 2026-11-20)

The gate marks 108 statements (70 of 5.0, 38 of 3.1.1) as bearing on more
than one node. They fall into twelve needs. Each is MQTT's need, the noun
it is built on, and where that noun stands. Identifiers are 5.0's unless
marked 3.1.1.

| # | MQTT needs | Statements | Built on | Stands |
|---|---|---|---|---|
| 1 | A session's state survives its connection and resumes at any node; Clean Start discards it; Session Present says which | 3.1.2-4 to -6, 3.1.2-23, 3.2.2-2, -3, 4.1.0-1, -2, 4.2.0-1; 3.1.1: 3.1.2-4 to -6, 3.2.2-2, -3, 4.1.0-1 | Dagda `replicas(N)` head space (D1) with k-level commits (D2), keyed by client id; Lir's merge | Waits on Dagda |
| 2 | In-flight QoS 1 and 2 state: unacknowledged until acked, resent with DUP in first-sent order on reconnect, wherever it reconnects; packet ids not reused while in flight; QoS 2's exactly-once on receipt | 2.2.1-4, 3.3.1-1, 4.3.2-3, -4, 4.3.3-3 to -10, 4.4.0-1, 4.6.0-1 (3.1.1); 3.1.1: 2.3.1-4, 3.3.1-1, 4.3.2-1, 4.3.3-1, 4.4.0-1 | Dagda log (D9): the entry is the message, its acks carry the stage (PUBLISH sent with its id, PUBREC, PUBREL, PUBCOMP); first-sent order is the log's per-origin order; QoS 2 receipt is the retired dot within the window | Waits on Dagda |
| 3 | Messages matching an offline session's subscriptions are queued for it | 4.5.0-1; 3.1.1: 3.1.2-5, 4.5.0-1 | Dagda log, a stream per client id. Who appends is unknown 2 | Waits on Dagda, and unknown 2 |
| 4 | Retained messages: replaced, cleared by an empty payload, sent on SUBSCRIBE by Retain Handling, RETAIN flag by Retain As Published | 3.3.1-5 to -13, 3.8.4-4; 3.1.1: 3.3.1-5 to -8, -10 to -12 | Dagda register map (D6), its keys-changed notice for each node's index of retained topics, values read from holders at SUBSCRIBE | Waits on Dagda |
| 5 | A message reaches every matching subscriber on every node, its properties unaltered and in order, the topic matching the filter; overlapping subscriptions of one session by its own rule; granted QoS caps delivery | 3.3.2-3 to -5, -15 to -18, -20, 3.3.4-1 to -5, 3.8.4-8; 3.1.1: 3.3.2-3, 3.8.4-6 | Dannan channels (`notify` for QoS 0, `stream` for QoS 1 and 2 while both nodes live) carrying the message whole; matching and the overlap rule run where the session is, since one session's subscriptions are on one node | Dannan has it |
| 6 | Message Expiry counts the time the message waited, wherever it waited | 3.3.2-5, -6, 4.3.3-7 | Dannan `hlc()` gives every node one time to measure against; the log entry carries the expiry (D9) | Dannan has the clock; the queue waits on Dagda |
| 7 | Order per publisher, per topic, across nodes | 4.6.0-5, -6; 3.1.1: 4.6.0-5, -6 | Live: the `stream` channel's per-origin sequence. Queued: the log's per-origin order | Dannan has it to the replay buffer's bound; the rest waits on Dagda |
| 8 | One connection per client id: a second CONNECT ends the first, wherever it is | 3.1.3-2, 3.1.4-3, -4; 3.1.1: 3.1.3-2, 3.1.4-2, -3 | Unknown 1 | Not known |
| 9 | An assigned client id is unique across the cluster | 3.1.3-6, -7, 3.2.2-16 | Dannan's node id with its sealed counter (`Identity::advance_counter`), or an `hlc()` stamp: unique with no word between nodes | Dannan has it |
| 10 | Wills: stored with the session, published when the connection ends (as retained if asked), after the Will Delay unless the session resumes, discarded on a normal DISCONNECT, user properties in order | 3.1.2-7, -8, -10, -14, -15, 3.1.3-9, -10, 3.14.4-3; 3.1.1: 3.1.2-8, -10, -12, -16, -17, 3.14.4-3 | The will in the session's state (need 1); published by the connection's node when the connection ends; by the session's replicas when the node dies (the roster's `left`); a delay or a session expiry with nobody connected is unknown 3 | Waits on Dagda, and unknown 3 |
| 11 | SUBACK once subscribed; after UNSUBSCRIBE no new messages, in-flight ones finished | 3.8.4-1, 3.10.4-2, -3; 3.1.1: 3.8.4-1, 3.10.4-1 to -3 | SUBACK on Dannan's pushed interest (decided 2026-11-16); unsubscribe withdraws interest, and what still arrives is filtered where the session is; in-flight ones are need 2 | Dannan has it; in-flight waits on Dagda |
| 12 | Shared subscriptions: each member at its own QoS; a QoS 2 delivery finished with the member that started it; not sent to another member once that session ends; a refusal (0x80 or above) discards | 4.8.2-3 to -6 | The node by `placement::rendezvous` over the nodes with interest in the group (step 1); the member's in-flight state is need 2 | Dannan has the choice while each node's interest fits its 1 KiB advert; past it, not known (step 4); the rest waits on Dagda |

Three statements were marked "many" by the mappers but need no node but
one: 3.3.4-6 (refusing a Subscription Identifier on an inbound PUBLISH),
and 3.1.1's 4.8.0-2 (closing on a transient error) and 3.10.4-1
(comparing filters character by character). They stay in the gate as
marked; the mapping counts them as local.

What this says:
- **Needs only Dannan, and present in `0.9.2`:** 5, 9, the choice in 12,
  the SUBACK and UNSUBSCRIBE of 11, live ordering in 7, the clock in 6.
  These can be built on the clean branch at step 6 without waiting.
- **Waits on Dagda's slices 2 to 4:** 1, 2, 3, 4, 10, and the queued
  parts of 6, 7, 11 and 12. That is every guarantee MQTT makes past one
  connection's life.
- **Not known, the needs to state to Dannan and Dagda:** unknown 1 (8),
  unknown 2 (who appends to an offline session's queue, 3), unknown 3
  (something acts when a session's or a will's time ends with nobody
  connected, 10).
- Nothing in the table is Lir's own mechanism. What Lir writes is the
  codec, the per-connection exchanges, matching, the overlap rule, and
  the merge of its session space.

## The second reader (step 4, 2026-11-20)

A fresh agent, given the maintainer's design and the contracts and none of Lir's
conclusions, answered the question in
[`2026-11-20-second-reader.md`](2026-11-20-second-reader.md). It agrees
on the shape: transport, matching, the SUBACK hold, live order and the
clock need only Dannan; everything MQTT keeps past a connection waits on
Dagda's unshipped nouns. Where it differs:

1. **Shared subscriptions are not as easy as the mapping says.** It
   finds no one-of-group noun. Its point that holds: choosing a node by
   rendezvous over the nodes with interest in the group works only while
   each node's interest fits the 1 KiB advert. Past that the advert reads
   as everything, every node seems to have members, and the choice can
   land where there are none. The mapping's need 12 is corrected to
   "needs only Dannan while interest fits the advert; past it, not
   known", and that is a need to state to Dannan.
2. **Order when a publisher moves.** A client that reconnects on another
   node publishes from a new origin, and nothing orders its last messages
   through the old node before its first through the new. Not in the
   mapping; added as a fourth unknown. (Its other ordering point, order
   across publishers on one topic, is not what `[MQTT-4.6.0-6]` asks:
   MQTT's order is per publisher.)
3. **Wills when a node dies:** "left the roster" cannot tell dead from
   cut off. The same as the mapping's split question for the maintainer.
4. **Retained order after a heal:** last writer by `hlc()` converges, but
   whether that is MQTT's "last" is the maintainer's (already waiting on him), and
   the clock's bound on how far ahead a stamp may run is not resolved.
5. **Offline queues:** it adds a second shape (a log per topic, each
   session reading from its own position) beside the mapping's (the
   publisher appends to each session's queue). Dagda is answering which
   fits.
6. **Agreement's acceptor state and who moves its slots** as the roster
   changes. The first is answered (Dagda's next release brings a durable
   `AcceptorStore`, relayed by Louhi). The second matters only if Lir
   uses agreement; Dannan's answer to unknown 1 is a lease instead.

It also lists where the maintainer's design of 2026-11-03 disagrees with the rules:
shard primaries and a per-shard lease, a wildcard index Lir keeps and
replicates itself, the interim mirrors and PUBACK on acceptance, the
take-over counter in Lir's merge, round-robin at a primary, a Lamport
counter where Dannan ships a hybrid clock, QoS 2 bounded by a
de-duplication window, and epoch fencing written by Lir. These go to the maintainer
whole: each is either a noun now (lease, register map, `hlc()`) or Lir's
own mechanism.

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
map (D6), or logs (D9). Those are slices 2 to 4, paused by the maintainer until
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

Measured on 2026-11-20 (step 2): [`docs/conformance.md`](../docs/conformance.md).
Of MQTT 5.0's 252 statements 84 are tested, and of 3.1.1's 139, 51; of
the 108 that bear on more than one node, a handful are tested across
nodes. What follows is what was written before the measurement.

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
4. **A second reader.** A fresh agent, given the maintainer's design, this file and
   the contracts, and none of Lir's conclusions, answers the one real
   question independently. The differences go to the maintainer.
5. **To the maintainer:** the conformance figure, the mapping, the needs, and what
   of the present code is the thin mapping and what is Lir's own
   mechanism.
6. **Then code**, tests first, on a clean branch from the contracts.
   What needs only Dannan can be built then. What waits on Dagda waits.

## Guards against keeping what should go

The maintainer, 2026-11-19: "the temptation will be to keep code which was written
with a fundamentally flawed premise." The Lir session wrote that code and
its 361 tests pass. Both pull towards keeping it.

1. `lir/cluster/` is not audited piece by piece for what can stay. It is
   Lir's own distributed system and by construction should mostly not
   exist. What replaces it is written from the mapping, and should be
   small enough that there is little in it to get wrong.
2. Tests do not argue for the code under them. A test that pins one of
   Lir's own mechanisms is rewritten to the behaviour MQTT requires, and
   its count kept. Whether "a test is never deleted" allows that is put
   to the maintainer with step 5.
3. Lines and tests kept are never reported as a success.
4. Findings go to the maintainer whole before anything is deleted or rewritten.

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
The maintainer says otherwise. Nothing is added to it, and no new behaviour is
offered to the board from it.

## Guarded publish

Withdrawn (the maintainer, 2026-11-19; the design is at `003906a`). It is redrawn
only after the work above, as a question in the same form: which nouns is
it built on? What is known: a register per topic in Dannan's agreement, so
a guard over a filter needs Dannan's atomic commit across names; acceptor
state in Dagda; replica sets that follow Dannan's placement. What the
board asked for is
`../coordination/TODO/2026-11-15-claim-board-0.4.0.md`, part 1.
Whether a running cluster can start using agreement one node at a time is
a question for the Dannan session, not a gap Lir has shown.
