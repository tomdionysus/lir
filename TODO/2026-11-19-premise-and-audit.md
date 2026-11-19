# The premise, and the audit of what is built (2026-11-19)

The maintainer, 2026-11-19, after withdrawing Lir's guarded-publish design: "you need a
plan to keep what is good, and rewrite the rest. This is dangerous, because
the temptation will be to keep code which was written with a fundamentally
flawed premise."

This file is that plan. Nothing in it is done yet. It is the first work of
the next session.

## What went wrong

The guarded-publish design (2026-11-19, in the history at `9712275`) was
four workarounds for things Dannan and Dagda had not shipped, each put to
The maintainer as a choice his rules had already settled:

| The design | the maintainer's answer |
|---|---|
| A whole tree in one agreement register, to get round a one-name limit | Wait for multi-name commits |
| Agreed state in Lir's memory, or a file of Lir's own | Wait for Dagda |
| A fixed set of four nodes, moved by an operator | "Leaderless does not mean 'group of immovable leaders'." |
| Perhaps restart all four nodes together | "You need to replan." |

The maintainer: "The questions you're asking me mean you haven't understood what Lir is
supposed to do." The same habit of mind wrote the cluster that runs today,
so that code is suspect until it is shown otherwise.

## The goal

The maintainer, 2026-11-03 night: "It must expose a MQTT interface and support all MQTT
functionality. But, it needs to be distributed, multimaster, clustered, using
Dannan as the network layer."

## The premise

Five statements a piece of Lir can be checked against. The first four are
The maintainer's rules, with where each was given. The fifth is the Lir session's own
reading of them, and is marked so. The maintainer confirms or corrects this list before
any code is judged by it.

1. **No node is special.** Every node is a broker and any node does any
   operation. No node is picked out by configuration, by an operator, or by
   a rule such as "the lowest id". (the maintainer, 2026-11-03 night: multimaster;
   2026-11-19: "Leaderless does not mean 'group of immovable leaders'.")
2. **Lir replicates and persists nothing itself.** What must survive a node
   lives in Dagda. (The stack's rule 2.)
3. **Membership and placement are Dannan's.** Lir keeps no node list and
   chooses no replica set of its own. (The stack's rule 1; the maintainer, 2026-11-15:
   "that's what Dannan is FOR".)
4. **Nothing stands in for a primitive that has not shipped.** When Dannan
   or Dagda lacks something, Lir waits and tells that project what it needs.
   (the maintainer, 2026-11-15, on agreement; 2026-11-19, on the design above.)
5. **Nothing acknowledges a guarantee it does not have.** (The Lir session's
   reading, from the maintainer's durability decision of 2026-11-04: QoS 1 and 2 are
   Replicated.)

And one rule of operation, the maintainer's (2026-11-19): the cluster is never stopped
as a whole to change it.

## The question this plan cannot answer

The cluster that runs today is the "interim" the maintainer approved on 2026-11-04:
sessions mirrored to standbys now, Dagda later; retained values on every
node; PUBACK on acceptance. The Lir session does not know whether the maintainer sees
the flaw as that interim itself, or only as the habit of adding new
workarounds to it. The two lead to very different amounts of rewriting. It
is the first thing to settle, and it is asked once, plainly, with nothing
built meanwhile.

## The audit

Each component goes in one of three bins. The bin sets who has to prove
what.

**Bin 1: no cluster assumptions.** Kept, after a check that nothing from
the cluster has leaked in.
- `lir/mqtt/` (the codec), `lir/subs/` (the trie), `lir/sn/packet.*` (the
  MQTT-SN codec), `lir/auth/scopes.*`, `lir/rules/` (the compiler and its
  conditions and actions), `lir/shard/` (pure functions).

**Bin 2: one-node broker behaviour.** Kept only where a behaviour traces to
a numbered statement of the MQTT specification, not to what the cluster
happened to need.
- `lir/broker/` without its cluster side, `lir/edge/`, `lir/http/`,
  `lir/sn/gateway.*`, `lir/auth/tokens.*` and `lugh.*`, `lir/client/`,
  `lir/contract/`.

**Bin 3: anything that touches state on more than one node.** Assumed
wrong, and rewritten, unless the design statement it stands on can be
shown. Passing tests are not that proof.
- `lir/cluster/` whole, the broker's cluster side (`ClusterHooks`,
  `SessionImage`, `inject`, `apply_retained`, `import`, `adopt`,
  `surrender`), `lir/system/` (what it publishes about the cluster, and
  who publishes it), `tools/lir_node.cpp` where it assembles them.

## What is already suspected

Read from the record and confirmed present in the code on 2026-11-19; none
is yet judged.

| What | Where | Statement |
|---|---|---|
| Session standbys: Lir's own mirror of each session to other nodes, on its own route and its own placement key | `lir/cluster/cluster.cpp:392`, `flush_mirrors` at `413`; route `0x4C03` | 2, 3, 4 |
| Retained values held on every node, with a snapshot fetched by a node that joins | `lir/cluster/cluster.cpp`, route `0x4C00` | 2, 4 |
| PUBACK and PUBREC on acceptance at the publishing node | `lir/broker/broker.cpp` | 5 |
| "The Lir node with the lowest id" publishes `$lir/cluster` and the roster events | `lir/system/system.cpp:245`, `publisher()` | 1 |
| The MQTT-SN gateway drives OpenSSL's DTLS itself | `lir/sn/gateway.cpp` | 4 |
| Shared groups spread on Lir's own channel and handed out in turn by the publishing node | `lir/cluster/cluster.cpp`, `lir.groups`, route `0x4C02` | to be judged |
| Server Reference: a node counts the clients it has sent each other node | `lir/cluster/balance.*` | to be judged |
| `SequenceBlocks` and the stored entry forms, built ahead of Dagda | `lir/cluster/sequence.*`, `codec.*` | to be judged |

## Guards against keeping what should go

The Lir session wrote this code and its 361 tests pass. Both pull towards
keeping it. So:

1. **The premise is fixed first.** No code is read for the audit until the maintainer
   has confirmed the list above. An audit done first would shape the list
   to fit the code.
2. **Keeping is what needs a reason.** For bin 3 the written finding is "the
   design statement this stands on is ...", or it is rewritten.
3. **Tests do not argue for the code under them.** A test that pins an
   interim behaviour (standbys, PUBACK on acceptance, the lowest node) is
   rewritten to the right behaviour and its count kept. The standing rule
   that a test is never deleted is not a reason to keep the code a test
   exercises. This reading of the rule is the Lir session's and is put to
   The maintainer with the audit.
4. **The cluster is rewritten on a clean branch from the contracts**, not
   edited in place, which would keep its shape.
5. **A second reader.** A fresh agent given only the design, the premise
   and the code, and none of the Lir session's conclusions, audits bin 2
   and bin 3 as well. Where the two differ, it goes to the maintainer.
6. **Lines and tests kept are never reported as a success.**
7. **Report before fixing.** The audit's findings go to the maintainer whole before
   anything is deleted or rewritten.

## The order

1. Ask the maintainer the question above, and for his word on the premise.
2. The audit, by bin, written into this file as findings: kept and why, or
   rewritten and what the right form waits on.
3. The second reader's audit, and the differences.
4. To the maintainer: the findings and the rewrite's order.
5. Only then code, tests first.

What the rewrite will wait on is already known in part: Dagda's slices 1 to
4 (step 5, paused by the maintainer until Macha runs on Dagda), and Dannan's atomic
commit across names for anything guarded. Until then the right form of some
parts cannot be built, and by statement 4 nothing is built in its place.

## The standing cluster

It runs the code under audit and carries the claim board. It stays as it is
until the maintainer says otherwise. Nothing is added to it, and no new behaviour is
offered to the board from it.

## Guarded publish, as replanned

It comes after step 5, on three things Lir does not have and does not
build:
- a register per topic in Dannan's agreement, so a guard over a filter is
  one change naming several registers: Dannan's atomic commit across names
  (its plan, item 7);
- acceptor state on Dagda's node-local log
  (`../dannan/TODO/2026-11-15-agreement-design.md`, "Where a persisting
  store lives");
- replica sets from Dannan's placement, an agreed topic's set being its
  topic shard's replicas, moved by Dannan's `transfer()` as they move.

What the board asked for is
`../coordination/TODO/2026-11-15-claim-board-0.4.0.md`, part 1.
Needs to tell Dannan, not yet sent: the atomic commit, as a user beside
Mimir and Kvasir; a running cluster starting to use agreement one node at
a time (today the genesis set is node configuration, and a node whose
table differs is refused at join); and whose rule it is that replica sets
follow placement.
