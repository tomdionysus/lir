# Guarded publish: agreed trees on Dannan's agreement (design, 2026-11-19)

A design for the maintainer. Nothing here is built. The claim board asked for it
(`../coordination/TODO/2026-11-15-claim-board-0.4.0.md`, part 1), and
Lir answered on 2026-11-15 that it would come on Dannan's agreement, which
0.9.0 shipped. `ACTIVE.md` ("Next", 4) says to put the design to the maintainer first.
It is written against Dannan 0.9.2's `dannan/agree/agreement.h`, read, not
yet called from Lir.

## What the board asked for

- `GET /retained` gives each topic the revision of its last write, the
  same on every node, and an `as_of` for the answer.
- `POST /publish` takes `{"unchanged": {"filter": F, "since": R}}`. It is
  applied only if nothing under F was written or cleared after R, checked
  where the write is applied, so it is linearisable. Otherwise 409, with
  the current revision.
- A clear is a write, with a revision.

## What Dannan gives

Dannan's agreement is CASPaxos: one linearisable register per name,
changed by a function of its current value, committed by a majority of
the slot's replica set. It has no log. In 0.9.2:
- A change names **one** name (several only for a read), until Dannan's
  plan item 7, the atomic commit across names.
- Every name is in one of a fixed number of slots (256). A slot no
  acceptor has recorded is served by the cluster's genesis set, which
  every node is configured with (`NodeConfig::agreement_genesis`), the
  same everywhere and fixed for the cluster's life. `found()` records
  every slot on the genesis set once.
- The hosted system keeps each acceptor's state (`AcceptorStore`), in
  memory or durably. A store that restarted without its state must not
  vote until it has caught up (`ready()`, `catch_up()`).
- A minority side is refused, saying why. `Outcome::changed_at` is a
  fencing token per name.

## The design

**1. An agreed tree is one register.** A node setting names the agreed
trees, as topic filters ending in `/#` that do not overlap, for example
`agreed_trees: ["stack/pis/#"]`. Each tree is one CASPaxos register,
named by its filter, in the space `lir`, slot `slot_of(name)`. Its value
is the whole tree:
- `revision`, a counter, raised by one at every change;
- each retained topic under the tree: its message and the revision that
  last wrote it;
- each cleared topic: a tombstone with the revision that cleared it, kept
  for a window (10 minutes, as the QoS 2 window) and then dropped at the
  next change.

One register per tree, not one per topic, because a guard covers a filter
and a change can name only one register. It also makes the revision a
plain counter that is the same on every node. The cost is that every
write to the tree contends for one register, and that the whole tree
travels in each accept. The board writes a few times a second at most,
and `stack/pis/#` holds about 6 to 50 small messages, a few KiB. A tree
over `agreed_tree_bytes` (default 256 KiB) refuses a write that would grow
it, with 0x97 or 413.

**2. A write is a change.** Every publish to an agreed tree, from MQTT,
MQTT-SN, HTTP, a will or a rule, retained or not, guarded or not, is one
`change()` on the tree's register:
- The change function checks the guard, if there is one: is any topic
  under F, or any tombstone, at a revision above R? If so, it leaves the
  value as it was and answers "conflict" with the current revision.
- Otherwise it raises the revision. A retained publish sets the topic, an
  empty retained publish leaves a tombstone, and a publish that is not
  retained changes nothing but the revision. A guarded publish that is not
  retained still needs the guard to hold, so it goes through too.
- Once committed, the message is delivered, and retained everywhere, as
  today. The committed tree goes on `lir.retained` with its revision, and
  each node's retained map for the tree takes the higher revision. So
  every node's copy converges to the register, and Lamport ordering does
  not apply inside an agreed tree.

**3. Acknowledgement.** QoS 1 and 2 are acknowledged once the change
commits (the `decided` level). A conflict is PUBACK 0x83 (Implementation
specific error) with a reason string naming the revision for 5.0, or
closing a 3.x client. A refusal on a minority side is 0x97. QoS 0 to an
agreed tree is refused, since nothing can tell its sender (a 5.0 client
is sent DISCONNECT 0x83; a 3.x client is closed). Over HTTP: 200 with the
revision, 409 with the current revision, 503 on a minority side or past
the deadline.

**4. Reads stay local.** `GET /retained` answers from the node's own
copy, as today, adding `revision` to each message under an agreed tree
and `as_of` per tree. A filter that spans two trees, or a tree and
anything else, gets an `as_of` for each tree it touches. A guard's filter
must lie inside one tree, or the publish is refused 400. A stale copy
costs only a 409 and a fresh read, never a wrong write: the guard is
checked against the register at commit. A minority side still serves
reads. A strict read (`GET /retained?...&strict=1`, one round on a
majority) is offered for a reader that must not see an old copy.

**5. Where the register lives.** Its replica set is the genesis set, the
nodes running Lir when agreed trees are first configured. Lir moves it
only by an operator command, `lir-node --move-agreed <tree>` with the
new set (Dannan's `transfer()`), never by placement on its own. Each
membership change otherwise moves no registers, and the board has no
automatic moves to get wrong. `lir-node --retire` refuses to retire a
node in a register's set until the register has moved.

**6. The acceptor store.** Lir's own, in memory: the interim keeps
retained messages in memory too, so the board is as durable as it is
today. A node that restarts says `ready()` false until it has caught up
from a majority of the other replicas (`catch_up()`), so a rolling
restart keeps everything. If a majority of the set restarts at once,
every agreed tree is lost, as the retained values of a restart of every
node are today. Step 5 moves the store onto Dagda (a node-local log, as
Dagda proposed for agreement), and then it survives.

**7. Fencing.** A write's answer carries its revision. `changed_at` is not
needed: the revision is the token.

## What it costs

- One round trip on the fast path, from a node that wrote last; two when
  another node wrote meanwhile; more under contention. On the build nodes' LAN,
  about 1 to 5 ms a round.
- Each node in the set holds one register per tree.
- No Lir change outside the agreed trees: everything else stays
  multimaster and Lamport-ordered.

## Asked of the maintainer

1. One register per agreed tree, as above.
2. The acceptor store in memory until step 5. The alternative is Lir's
   own durable file, against the stack's rule that what a hosted system
   persists lives in Dagda.
3. The register's set fixed at the genesis set, moved only by an
   operator. The alternative is moves by Dannan's placement as nodes come
   and go, which is more code and more ways to fail for a four-node
   cluster.
4. Introducing it on the standing cluster. The genesis set is fixed for
   the cluster's life, and a node whose slot table differs is refused when
   it joins. A rolling upgrade may therefore split the cluster between
   nodes with a genesis set and nodes without one. This is to be
   confirmed with Dannan before anything is built. If so, the four nodes
   are restarted together on the new configuration, a short outage of
   the board, with the maintainer's approval.

## Asked of Dannan, after the maintainer's go

- Does an empty genesis set and a configured one count as different
  tables, refusing the join? Can a running cluster take a genesis set
  without every node restarting at once?
- Is a change's value of a few hundred KiB, carried whole in each accept,
  within what one message carries?

## Tests, first

- `AgreedTree.AGuardedPublishFromAStaleRevisionIsRefusedWithTheCurrentOne`
- `AgreedTree.TwoGuardedPublishesFromOneRevisionCommitOnlyOne`
- `AgreedTree.AClearIsAWriteTheGuardSees`
- `AgreedTree.ATombstoneLeavesAfterItsWindow`
- `AgreedTree.EveryNodeServesTheSameRevisionAfterACommit`
- `AgreedTree.TheMinoritySideRefusesWritesAndServesReads`
- `AgreedTree.QoS0ToAnAgreedTreeIsRefused`
- `AgreedTree.AGuardWhoseFilterLeavesTheTreeIsRefused`
- `AgreedTree.ATreeOverItsBoundRefusesAWriteThatGrowsIt`
- `AgreedTree.ARestartedNodeVotesOnlyAfterCatchingUp`
- `AgreedTree.ARollingRestartLosesNoAgreedTopic`
- `AgreedTree.TopicsOutsideAnAgreedTreeStayMultimaster`
- `HttpAgreed.RetainedCarriesRevisionsAndAsOfPerTree`
- `HttpAgreed.APublishWithAStaleGuardIs409WithTheRevision`
- `ProcessRetire.ANodeInAnAgreedSetIsNotRetiredUntilMoved`
