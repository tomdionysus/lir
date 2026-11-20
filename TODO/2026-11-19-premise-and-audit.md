# The plan: Lir built on Dannan (2026-11-20)

Rewritten on 2026-11-20 after the maintainer's corrections. The first version, a plan
to audit the code piece by piece against five rules, is in the history at
`a0a6945`. Nothing here is done.

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

The Lir session's first answer follows. **It is not yet checked against
Dannan's and Dagda's contracts**: it comes from Lir's records and Dannan's
agreement header. Step 1 of the work below checks every row.

**Per connection, with no cluster in it. Lir's own.** The codec; the
CONNECT, QoS 1 and QoS 2 exchanges; packet ids; keep-alive; receive
maximum; topic aliases; subscription options and identifiers; enhanced
authentication; topic filter matching (the trie). Dannan provides the
listeners under it.

**Across nodes:**

| MQTT needs | Built on | State |
|---|---|---|
| QoS 0 delivery to subscribers on other nodes | An at-most-once channel, interest by prefix | Dannan has it |
| Order per publisher | The channel's per-origin sequence | Dannan has it |
| Wildcard filters | Dannan routes by literal prefix; Lir's trie matches at the receiving node. A filter with no literal prefix (`#`, `+/x`) receives everything. Correct, not cheap. | Dannan has it |
| QoS 1 surviving a node's death | Dagda's log | Waits on Dagda |
| QoS 2 exactly once | The log's retired dots, for the window | Waits on Dagda |
| Retained messages, and wildcard reads of them | Dagda's register-map space and its keys-changed notice | Waits on Dagda |
| Session state resumable at any node | A Dagda space keyed by client id, placed by Dannan | Waits on Dagda |
| System topics | Each node answers `$lir/...` for its own clients from its own view of Dannan's roster and adverts. Nothing is replicated, so no node is "the publisher". | To check; needs only Dannan |
| Shared subscriptions | The publishing node picks the member with Dannan's placement function over the group's members, keyed by the message. No counters, no group state. | To check; needs only Dannan |
| A will when the client's node dies | The will is in the session's Dagda state. Every replica that sees the node leave fires it under one deterministic dot, so duplicates collapse. No node is chosen. | To check; waits on Dagda |

**Not known.** the maintainer: "It's OK to say 'I don't know'."

1. **One connection per client id.** A second CONNECT with an id must end
   the first, wherever it is. Perhaps a channel named for the client id
   that the holding node subscribes to; perhaps a Dagda lease on the id.
   Not known which. In a split both sides hold the id whatever is chosen.
2. **The subscriptions of an offline session.** While a client is away,
   something must match publishes against its filters and queue them. No
   node has the client, so whose interest is it? If the session's replicas
   hold it, several nodes append the same message, and it is not known how
   that de-duplicates under Dagda's rule that a dot's origin is its
   appender. This may be a noun that does not exist: an interest that
   outlives the node that declared it.
3. **Endings by time with nobody connected:** session expiry, will delay,
   expiry of a queued message. Dagda's expiry per key may cover the first.
   The other two are not known.

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

Withdrawn (the maintainer, 2026-11-19; the design is at `9712275`). It is redrawn
only after the work above, as a question in the same form: which nouns is
it built on? What is known: a register per topic in Dannan's agreement, so
a guard over a filter needs Dannan's atomic commit across names; acceptor
state in Dagda; replica sets that follow Dannan's placement. What the
board asked for is
`../coordination/TODO/2026-11-15-claim-board-0.4.0.md`, part 1.
Whether a running cluster can start using agreement one node at a time is
a question for the Dannan session, not a gap Lir has shown.
