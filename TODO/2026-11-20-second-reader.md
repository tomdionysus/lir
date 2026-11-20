# What full MQTT needs that Dannan does not provide: a second reading

Written 2026-11-20 as an independent answer to Tom's question: "What is required to
implement full MQTT that Dannan does not already provide, AND how would you do that
building on top of Dannan?" Read from the design (`2026-11-03-lir-plan.md`, the
sections file), Dannan's contract at tag 0.9.2 (cited `tag:file:line`), and Dagda's
shipped contract (`dagda/context.h`). Dagda's designed-but-unshipped nouns are taken as
briefed (no file to cite) and marked "designed".

Status words: **shipped** (in Dannan 0.9.2 or Dagda's shipped context), **designed**
(Dagda has designed it, not shipped), **missing** (no noun in either).

Two facts about the shipped contracts shape everything below:

- Dagda's shipped Leases are held "in this node's memory alone ... another node does
  not see them" (`dagda/context.h:307-312`), and shipped head spaces are every-node only,
  "`replicas: N` is refused" (`dagda/context.h:334-338`). So nothing per-key and
  cluster-wide is persisted in Dagda today except objects, claims and every-node heads.
- Dannan 0.9.2 does ship leaderless per-key agreement (CASPaxos registers changed from
  any node, committed by a majority of a slot's replica set,
  `0.9.2:dannan/agree/agreement.h:187-205`, `0.9.2:dannan/context.h:460-464`). It is the
  only linearizable noun in the stack, and it refuses a minority side
  (`agreement.h:219-221`). It stores nothing: the acceptors' state is kept by "a store of
  [the hosted system's] own" (`agreement.h:27-29, 38-41`).

## Per connection, no cluster

Lir's own, on one node, touching no other node. None of it is a distributed mechanism.

- The codec for 3.1, 3.1.1 and 5.0, every packet, every property, every malformed-packet
  and protocol-error rule, and the reason codes.
- CONNECT and CONNACK negotiation: protocol level, clean start, keep alive and Server
  Keep Alive, Receive Maximum, Maximum Packet Size, Topic Alias Maximum, the limits a
  node advertises and the clamping of a session moved to a node with smaller limits.
- Authentication and enhanced authentication (AUTH, re-authentication), the Lugh plugin
  as one provider, and the topic-scope check on each PUBLISH and SUBSCRIBE.
- Packet identifiers, the QoS 1 and QoS 2 handshakes on the live connection, flow
  control by Receive Maximum, topic aliases both ways, retransmission on resume in
  original order.
- Topic name and filter validation, UTF-8 and payload-format checks, and matching a
  topic against this node's sessions' filters (the trie). Dannan carries topics as
  opaque strings and leaves wildcards to the hosted system (`0.9.2:dannan/net/fanout.h:31-35`).
- Subscription options applied at delivery: No Local (the envelope carries the
  publishing client id), Retain As Published, Retain Handling, Subscription Identifiers,
  the granted QoS cap.
- Message Expiry at delivery: decrementing the interval by the time held, and not
  sending an expired message.
- Request/response, correlation data, user properties, content type: passed through.
- Server-assigned client identifiers: unique from this node's id and its HLC stamp,
  which with the node id is a total order (`0.9.2:dannan/contract/hlc.h:23-26`).
- Will publication when a connection on a live node ends without a normal DISCONNECT,
  and the Will Delay timer while this node lives (`0.9.2:dannan/context.h:417`,
  `Time::after`). Whether the will still applies when the timer fires is not local: see
  "Across nodes".
- Server Reference / redirect at CONNECT, read from the connection counts nodes
  advertise (`Nodes::advertise`, `context.h:154-159`).
- WebSockets, the MQTT-SN gateway, evaluating rules on the publishing node, and memory
  admission through Dannan's Budget (`context.h:365-392`).

## Across nodes

| MQTT need | Built on (exact noun) | Status |
|---|---|---|
| Reaching every node that has a matching subscriber | `Channels::publish` on a channel, with each node subscribing the literal prefix of its filters before the first wildcard (`context.h:242-266`); Dannan sends only to nodes whose gossiped prefixes match (`fanout.h:27-35`). An interest advert over 1 KiB coarsens to everything (`fanout.h:73-77`): correct, costlier. | shipped |
| SUBACK only once a later publish will reach the subscriber | `Channels::subscribe` with a deadline, answering `membership::Pushed` (`context.h:256-261`, `node_record.h:88-94`) | shipped |
| QoS 0 delivery | `Delivery::notify`, at most once, dropped under pressure (`fanout.h:38`) | shipped |
| QoS 1/2 transport between nodes, in order per origin | `Delivery::stream`, a sequence per origin and destination, gaps refilled from a bounded replay buffer, loss reported (`fanout.h:39-41, 56-61`); deliveries in order per origin on a strand (`context.h:253-254`). Not durable: the replay buffer is the sender's memory, so it cannot back a PUBACK alone. | shipped |
| PUBACK / PUBREC only when the message is durable (QoS 1/2 accepted) | Dagda replicated log: append from the publishing node, group commit, the entry's dot owned by that node, acknowledged once k replicas hold it | designed |
| Payloads over the inline limit | `Objects::write` and `Claims::retain` (`dagda/context.h:193-305`), released by `Objects::remove` | shipped |
| Retained messages: one per topic, replace, empty publish deletes, expiry | Dagda last-writer-wins register map with per-key expiry, keyed by topic, the writer's stamp an HLC stamp (`hlc.h:29-36`) with node id as tiebreak, which is a total order (`hlc.h:23-26`); a delete is a tombstone in the same order | map designed; HLC shipped |
| Retained delivered on SUBSCRIBE | A read of the local replica of that register map, by filter | designed |
| Session state (subscriptions, will, expiry interval, QoS 2 inbound packet ids, outbound inflight) surviving its node and following the client | Dagda head space held on N nodes chosen by Dannan's placement, commit acknowledged at k replicas; keyed by client id, placed by `Nodes::placement` (`context.h:148-153`) | designed (shipped heads refuse `replicas: N`, `dagda/context.h:336-337`) |
| One connection per client id (MQTT-3.1.4-3: the old connection is closed, 0x8E) | One agreement register per client id, value (node, connection, takeover count), changed by `Agreement::change` from the node taking the CONNECT (`agreement.h:187-205`); the losing node learns by the register (`Outcome::changed_at` is a fencing token, `agreement.h:101-104`) | shipped, but see "Not known" (acceptor store, split) |
| Messages for an offline persistent session | Dagda replicated log entries (per-origin order reads, a per-entry expiry, bounds by count and age) and the session's per-origin read position held in its session record | designed; see "Not known" |
| Message Expiry while queued | The log's per-entry expiry, checked again at delivery | designed |
| Session Expiry while no node holds the client | The register map's per-key expiry for discarding the record | designed; who acts on it is "Not known" |
| Learning that a node restarted (its connections are gone) | `RosterChange::changed` with a new incarnation, "a new incarnation is a restart" (`context.h:71-79`, `node_record.h:62-65`) | shipped |
| Which side of a split this node is on | `Nodes::partition` / `watch_partition` (`context.h:169-180`, `agreed_roster.h:22-45`) | shipped |
| Retained ordering and identity of a write | `HlcClock::now` and `receive` (`context.h:465-471`, `hlc.h:76-89`) | shipped |
| Per-node system topics (`$lir/nodes/<id>`, `$dannan/nodes/<id>`, `$dannan/roster/...`) | Derived on each node at read time from the roster watch, adverts and gossiped Telemetry (`context.h:98-117, 283-293`), published by the node they describe; nothing stored | shipped |
| Cluster aggregates (`$lir/cluster`) | Computed by each node from the same gossip and served as a locally derived retained value, so no node is the one that publishes it | shipped |
| `$lir/clients/...` connect and disconnect events | Published by the node where the event happens, as any publish | shipped |
| Placement of anything keyed (session records, log streams) | `Nodes::placement` with Lir's `placement::Scheme` salts (`context.h:148-153`, `placement.h:37-46, 56-65`) | shipped |
| Message history (opt-in by filter) | The same Dagda log, bounded by count and age | designed |
| Rules (`$lir/rules/<name>`) | Retained messages, so the register map | designed |

## Not known

Each item is the need in MQTT's terms and why each obvious noun does not fit. I do not
offer a design for any of them.

1. **One connection per client id, across the cluster and in a split.** MQTT-3.1.4-3
   requires that a second CONNECT with the same id close the first. Agreement gives this
   on a majority side, but a minority side is refused (`agreement.h:219-221`), so a
   minority-side node cannot know whether the id is connected elsewhere. Accepting the
   CONNECT there means two live connections for one id until the heal; refusing it means
   that side does not serve the client. Neither the partition reading
   (`context.h:169-175`) nor any Dagda noun changes this: it is a choice between the
   requirement and availability, and it is Tom's.

2. **Where agreement's acceptor state lives, and who moves slots.** Agreement stores
   nothing; the acceptors are served "from a store of its own" (`context.h:460-463`,
   `agreement.h:38-41`). Lir persists nothing, and Dagda's shipped context has no
   acceptor store. An in-memory store is allowed by the contract, but if every replica
   of a slot restarts the ownership registers are lost while connections on other nodes
   live on. And slot replica sets are moved by `Agreement::transfer`, "the hosted
   system's choice" (`agreement.h:224-230`): who calls it as the roster changes is
   placement, which is Dannan's. Both are needs for Dannan or Dagda.

3. **Matching and queuing for an offline session.** Whose interest is it? Not the node
   it was last on: that node's channel interest goes when it leaves the roster
   (`context.h:82-87`). Two shapes, both on designed nouns:
   (a) the publishing node appends once per matching offline session, into that
   session's stream, the entry's dot being the publishing node's so a retry is the same
   entry; this needs every publishing node to know every offline session's filters, and
   the only shipped noun that every node holds is an every-node head space, which does
   not scale with sessions and is not keyed;
   (b) the publishing node appends the message once to a stream for its topic, and the
   session keeps a read position per origin; duplicates are avoided by the dots, and
   nothing is fanned out at publish time. But retiring an entry then needs to know which
   offline sessions still want it (the log's acks by stage might carry it, I cannot tell
   from the brief), a session subscribed to `#` reads every stream on resume, and a
   bound by count or age drops messages a PUBACK accepted for that session.
   I cannot choose between them, or say either is complete, from the nouns as briefed.

4. **Anything that must happen when no node holds the client.** Session expiry, the end
   of a Will Delay, and the will published at session end all fall due at a time, on no
   particular node. Dannan's only timer is local (`Time::after`, `context.h:417`) and
   dies with the node. The designed register map expires a key, but I do not know
   whether expiry produces the designed "notice of which keys changed" on any node, and
   a notice on every node would publish the will once per node. Settling who publishes
   by an agreement change ("will W published") is shipped but majority-only. A noun for
   "this action, once, at time T, by whichever node is alive" is missing.

5. **Wills when a node dies.** A new incarnation proves the old connections are gone
   (`context.h:71-73`). A node that has left the roster (`dead_after`) does not: on a
   split, the other side sees it leave while its clients are still connected. Publishing
   their wills then announces a disconnection that did not happen. I know of no noun
   that tells dead from unreachable, and I believe none can.

6. **Shared subscriptions.** Each message goes to one session of the group. Dannan's
   channels deliver to every matching node; there is no one-of-group delivery
   (`fanout.h:27-35, 38-44`). `Nodes::placement` with `only_advertising` a per-group
   advert would pick one node per message deterministically (`context.h:148-153`), but a
   node carries at most 64 adverts of 1 KiB (`node_record.h:46-50`), so groups are
   bounded by that; offline members of a group have no advertising node; and MQTT's
   redelivery to another member when the chosen session ends needs the inflight record
   of the chosen session. Missing as a general noun; possibly a need for Dannan.

7. **Ordering.** Per publishing connection and topic, the stream channel's per-origin
   order serves (`fanout.h:39-44`). MQTT-4.6.0-6 asks for the order "received from any
   Client" on an Ordered Topic, and the cluster has no single receipt order across
   nodes; Dannan states "no ordering across origins" (`fanout.h:44`). A per-topic
   agreement register as a sequencer would give one, at a majority round per publish and
   none on a minority side. Also unknown: order for one client that reconnects to
   another node mid-stream, since the origin changes with the node.

8. **Retained ordering after a split heals.** Last-writer-wins by HLC stamp then node id
   is a total order (`hlc.h:23-26`) and every node converges on one value per topic.
   Whether that is MQTT's "replace the retained message" across a split, where no "last"
   exists, is Tom's call; and a stamp further ahead than `max_ahead` is not merged into
   the clock (`hlc.h:42-52`), so a node with a fast wall clock can win every retained
   write it makes until wall time catches up.

9. **QoS 2 exactly-once across a node move.** The packet ids the client has sent
   PUBLISH for but not yet PUBREL must be in the session record before PUBREC, and the
   record must be readable on the node the client reconnects to. That is the designed
   head space with k-replica acknowledgement; I do not know whether its commit latency
   fits a PUBREC per message, or how a read on a minority side behaves.

## Where Tom's design and the rules disagree

- **Shard primaries.** The design's primary per shard by `acquire("lir/shard/<n>")`
  (plan:40, 55, 63), and the later "per-shard lease says only who resumes a departed
  node's owed entries" (plan:188-192), make a holder per shard a special node. Rule 1
  forbids it. Dagda's shipped lease is one node's memory anyway (`dagda/context.h:307-312`).
- **Lir's own replicated state.** The wildcard index "soft state in memory, rebuilt from
  announcements" (plan:143, 198-199) and the inflight log "with the set of edges still
  owed" (plan:108) are Lir replicating and tracking delivery itself. The first is
  Dannan's channel interest (`fanout.h:27-35`); the second is the designed log's acks.
- **Interim measures.** "Sessions before step 5 are mirrored in memory to standby nodes"
  (plan:200-201) is Lir replicating; "Until step 5 [PUBACK] is sent on acceptance"
  (plan:202-204) acknowledges a durability Lir does not have. Both are stopgaps the
  rules forbid.
- **Degrade on a minority side.** "write Persisted, PUBACK, ... re-replicate on heal"
  (sections:27-30): a PUBACK at fewer than k replicas is honest only if MQTT's PUBACK is
  defined to mean "persisted here"; and "re-replicate on heal" is Lir's mechanism unless
  the designed log does it.
- **Takeover counter merge at heal** (sections:32-34, plan:110): Lir's own merge rule for
  connection ownership, where agreement now exists (but see Not known 1).
- **Shared subscription round-robin at the primary** with a timeout to the next edge
  (plan:154): a primary, and Lir's own redelivery.
- **Retained ordering by "the Lamport counter, then the node id"** (plan:196-197): a
  counter of Lir's own, where Dannan ships an HLC whose stamp with the node id is the
  same kind of total order (`hlc.h:23-26`).
- **Exactly-once bounded by `dedup_window`** (plan:132-134, sections:35): the inbound
  QoS 2 rule is per packet id until PUBREL, with no window; a window bounds a guarantee
  PUBREC states without bound.
- **Epoch fencing of head writes** (plan:114): Lir's own fencing over a lease epoch, a
  mechanism the rules give to Dagda.
