# Guarded publish: agreed topics on Dannan's agreement (2026-11-19)

The first draft of this file is withdrawn (Tom, 2026-11-19). It is in the
history at `9712275`. This file records why, and the plan that replaces it.
Nothing is built.

## What was wrong with the first draft

It was four workarounds, and each put to Tom as a choice something his rules
had already settled.

| The draft | Tom's answer | The rule it broke |
|---|---|---|
| A whole tree in one register, because a change names only one name | Wait for multi-name commits | No workaround of Lir's own for a primitive Dannan has not shipped (Decisions, 2026-11-15) |
| Acceptor state in Lir's memory, or a file of Lir's own | Wait for Dagda | What Lir persists lives in Dagda (rule 2) |
| The register on a fixed genesis set, moved by an operator | "Leaderless does not mean 'group of immovable leaders'." | No node is special: every node is a broker, and placement is Dannan's |
| Perhaps restart all four nodes together to introduce it | "You need to replan." | The cluster is never stopped to change it; a node at a time, always |

The draft also bent the broker to fit: it refused QoS 0, and made the
operator the keeper of where state lives. Lir is a leaderless, multimaster
broker in which any node does anything, built only from what Dannan and
Dagda provide. A feature that cannot be built that way yet is not built yet.

## What the board asked for (unchanged)

- `GET /retained` gives each topic the revision of its last write, the same
  on every node, and an `as_of` for the answer.
- `POST /publish` takes `{"unchanged": {"filter": F, "since": R}}`, applied
  only if nothing under F was written or cleared after R, else 409.
- A clear is a write, with a revision.

## The plan

Guarded publish comes after step 5, on three things Lir does not have yet.
Lir builds none of them itself and nothing in their place.

1. **A register per topic**, in Dannan's agreement. A guard over a filter is
   then a change that names several registers, so it needs Dannan's atomic
   commit across names (Dannan's plan, item 7). Until that ships there is no
   guarded publish.
2. **Acceptor state in Dagda**, on its node-local log, as Dagda proposed for
   agreement (`../dannan/TODO/2026-11-15-agreement-design.md`, "Where a
   persisting store lives"). That is Dagda's slice 4, which step 5 waits on
   too.
3. **Replica sets from Dannan's placement**, moving as nodes join and leave,
   as every other placement in Lir does. An agreed topic's set is its topic
   shard's replicas, the same nodes that hold its retained space in step 5,
   moved by Dannan's `transfer()` when the shard's replicas move. No node
   list is configured in Lir and no operator moves anything.

What agreement itself costs is not a workaround and stays: a write to an
agreed topic needs a majority of its replicas, so a minority side cannot
commit one. That is the `decided` level. It applies only to topics a node
is configured to agree on; every other topic stays multimaster.

## Asked of Dannan

Told as needs, not built around:

- Atomic commit across names (item 7), for a guard over a filter. Lir is a
  user of it, beside Mimir and Kvasir.
- A running cluster must be able to start using agreement one node at a
  time. Today the slot table's genesis set is in each node's configuration,
  and a node whose table differs is refused when it joins. How the standing
  cluster adopts a table without stopping is Dannan's to say.
- Replica sets that follow placement: whether Dannan's runtime proposes the
  transfers as the roster changes, or the hosted system does from Dannan's
  placement. Either way the rule is Dannan's, once, for every user.

## The details that carry over

To be redrawn on per-topic registers when the three things above exist:
reads stay local, with `revision` and `as_of` added; a guard is checked at
the commit, so a stale read costs a 409 and never a wrong write; QoS 1 and 2
are acknowledged once agreed; the tests are written first.
