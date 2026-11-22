# The system topics on Dannan (2026-11-22)

A design for the maintainer, before any code. The contract keeps the topics
(`docs/contract.md`, section 6) and asks for "a shape with no special node":
the withdrawn code published `$lir/cluster` and the roster events from the
Lir node with the lowest id, which the premise forbids. The rewrite's design
already says how (`2026-11-20-rewrite-design.md`, "System topics"): each node
answers its own local subscribers from its own view. This is that, worked
out.

## The rule

Nothing about the cluster is published by one node for the others. Each
topic is one of two kinds:

- **A view.** Derived on every node from what Dannan already gossips to
  every node: the roster, each node's adverts, its telemetry. A node renders
  it for its own subscribers only. Nothing goes on Lir's channel and nothing
  is stored, so nothing waits on Dagda, and there is no publisher to lose.
- **An event of one node's own.** Something only one node knows, such as a
  client connecting to it. That node publishes it on Lir's channel like any
  publish (Delivery), from the Link that saw it.

| Topic | Kind | Built on | When a subscriber gets it |
|---|---|---|---|
| `$lir/nodes/<id>` | view, current state | Each Lir node's `lir.figures` advert (connections, sessions, message counters, its limits), read from the roster on every node with `Nodes::watch_advert` | At SUBSCRIBE, one message per node in the roster, sent with RETAIN 1; then on each change, at most once per advert interval; an empty message when the node leaves the roster |
| `$dannan/nodes/<id>` | view, current state | Dannan's own node record and telemetry (its peers and their latency), as every node sees them | As above |
| `$lir/cluster` | view, current state | Computed on each node from the same adverts: nodes, connections, sessions, messages a second, whether every node runs the same limits | At SUBSCRIBE, RETAIN 1; then on each change |
| `$dannan/roster/joined`, `/left`, `/changed` | view, events | Each node's own `Nodes::watch` (RosterChange) | From SUBSCRIBE on, once per change as that node saw it; never stored |
| `$lir/clients/connected`, `/disconnected` | event of one node | The Link that saw it, through Delivery on Lir's channel | From SUBSCRIBE on, wherever the subscriber is; QoS 0, at most once |

Every payload is JSON, and carries `"from"`, the node id that rendered it. A
view is that node's view: during a split, or while gossip converges, two
nodes may say different things, and the payload says whose answer it is
rather than claiming one truth (law: nothing acknowledges a guarantee it
does not have).

## How it sits beside Delivery and Link

- A `lir::system::Views` component per node holds the current rendering of
  each view topic, kept from Dannan's watches. It holds no state of its own
  beyond what it renders from them.
- A Link subscription whose filter matches `$lir/...` or `$dannan/...`:
  - is answered at once with the current view topics it matches, with
    RETAIN 1 (section 3.3.1.3: a message sent because of a new subscription
    carries RETAIN 1), as its Retain Handling says: sent for 0, only for a
    subscription that did not exist for 1, never for 2 (MQTT-3.3.1-9 to
    -11). These are rendered state, not stored retained messages, so Retain
    Available 0 in CONNACK stays true of what clients publish;
  - then gets each view change from `Views`, posted to the Link's strand;
  - is also a Delivery subscription as usual, for the connection events,
    which travel on the channel.
- A client never writes a system topic: a PUBLISH to `$lir/...` or
  `$dannan/...` goes nowhere (QoS 0 has nothing to answer), as a grant's
  refusal does. MQTT 4.7.2 lets a server treat `$` topics this way.
- `#` and `+` at the first level do not match `$` topics (4.7.2), which
  `lir::mqtt::matches` already holds.

## What it waits on

- **JSON**, from Dannan's second minor (item 24, Boost.JSON, carried once in
  Dannan): every payload. Lir writes no JSON of its own.
- Nothing from Dagda: the views are rendered, not stored.
- The `lir.figures` advert's contents grow as Lir does (sessions, retained,
  memory figures once Dagda's nouns ship); the advert itself is Dannan's,
  bounded at 64 adverts a node.

## Tests

On the simulated cluster, named by what they prove:
- a subscriber on any node gets every node's `$lir/nodes/<id>` at SUBSCRIBE,
  each saying which node rendered it;
- a node leaving the roster empties its topics on every node, and joining
  fills them;
- `$lir/cluster` agrees across nodes once gossip has converged, and says
  whose view it is before then;
- roster events arrive once per change for each subscriber, never once per
  node in the cluster;
- a client connecting on one node is seen by a subscriber on another;
- a client's PUBLISH to a system topic reaches no one.
