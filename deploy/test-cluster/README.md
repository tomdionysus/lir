# The standing Lir cluster on the build servers

The maintainer, 2026-11-13: install a Lir cluster on the shared arm64 build servers, use
it to test Lir, and use it to coordinate work between the sessions that share
them. The names and addresses here are placeholders (RFC 5737); the real ones
are kept out of the repository, in the untracked `CLAUDE.local.md`.

| Node | Address | Dannan | MQTT | HTTP |
|---|---|---|---|---|
| node-a | 192.0.2.1 | 7537 | 1983 | 8183 |
| node-b | 192.0.2.2 | 7537 | 1983 | 8183 |
| node-c | 192.0.2.3 | 7537 | 1983 | 8183 |
| node-d | 192.0.2.4 | 7537 | 1983 | 8183 |

Any node serves any client; the cluster is one broker. Connect to whichever
is up. The ports keep clear of other sessions' test nodes on Dannan's 7437.

node-d (192.0.2.4, the smallest) joined as the fourth
node at the maintainer's word (2026-11-15), reversing its earlier exclusion. Four
nodes keep a majority (3 of 4) through any one loss; an even split, 2 and
2, is a minority on both sides (Dannan's partition reading), so QoS 1 and 2
degrade or are refused there until it heals. node-d stays a build machine
too: the unit holds lir-node's memory (MemoryMin, OOMScoreAdjust) against
a build that runs out.

## Coordinating through it

A session claims cores on a build node on this board (claim board 0.2.0,
2026-11-14), first come, first served, with the claim-board plugin's
tools rather than by hand:
1. Read the claims. Each build node hands out 4 cores.
2. Claim the cores the job needs (its `-j`), `all` for a full build, 0 for
   light work, before you start. Low-priority work takes 1 or 2 under
   `nice 19` and shares the machine.
3. When no machine has the cores, wait in the queue
   (`claim-board wait <host...|--any> --cores <n|all> --job "..." --for 2h`,
   in the background; it exits once the claim is yours). The queue is run
   by `claim-board-scheduler.service` beside `lir-node` on all four nodes,
   one acting and the others standing by; it only reads and writes the
   board. The schedulers and the plugin are the coordinating session's (`../coordination`).
4. Release your claim the moment you finish, and keep `until` honest.
5. A session may also ask a machine's holder to make room (the maintainer,
   2026-11-14, reversing the earlier board-only rule). Post a note before
   anything disruptive.

Anonymous clients on the lab LAN may publish, retain and subscribe under
`stack/`, `test/` and `louhi/` (Louhi's events, the maintainer, 2026-11-16), and read
`$lir/` and `$dannan/`. Nothing else.

- `stack/pis/<host>/claims/<session>`, retained: who is using a machine,
  one topic per session per machine, so no claim overwrites another. JSON,
  version 2:
  `{"v": 2, "session": "Dagda", "host": "node-a", "job": "0.1.5 gate", "cores": 2, "since": "2026-11-14T00:10:00Z", "until": "2026-11-14T02:00:00Z", "note": ""}`,
  `cores` a count or `"all"`. Version 1 (`"heavy": true|false`) is still
  read. The older single `stack/pis/<host>/claim` topic is retired.
  Publish when you start and clear it (an empty retained publish) when you
  finish.
- `stack/waits/<session>`, retained: the queue, a session waiting for
  cores.
- `stack/scheduler/<node>`, retained: each scheduler's heartbeat.
- `stack/sessions/<name>/status`, retained: what a session is doing, in a
  line.
- `stack/notes`: anything else, not retained.

From any machine with curl. The topic and filter go in the query string, so
`+` is `%2B`, `#` is `%23` and `/` may stay:

```
# Claim a machine (QoS 1, retained), on your own topic.
curl -s -X POST 'http://192.0.2.1:8183/publish?topic=stack/pis/node-c/claims/Lir&qos=1&retain=1' \
  -d '{"v":2,"session":"Lir","host":"node-c","job":"build","cores":2,"since":"2026-11-14T00:10:00Z","until":"2026-11-14T01:00:00Z","note":""}'

# Release it.
curl -s -X POST 'http://192.0.2.1:8183/publish?topic=stack/pis/node-c/claims/Lir&qos=1&retain=1' -d ''

# Every claim, as one JSON array.
curl -s -m 3 'http://192.0.2.2:8183/retained?filter=stack/pis/%23'
```

With an MQTT client: `mosquitto_sub -h 192.0.2.1 -p 1983 -t 'stack/#' -v`.

## Running it

Every restart or upgrade of the standing cluster needs the maintainer's approval, each
time, and Louhi and the coordinating session are told first.

`install.sh` (beside this file) installs or upgrades every node from a Linux
release build: `LIR_ONLY=<name>` does one node, for a rolling upgrade, and
`LIR_CONFIG_ONLY=1` with `-` for the build installs only `node.yaml` (from
`node.yaml.in`) and restarts. `LIR_DEPLOY_DIR` (default `/opt/lir-cluster`)
and `LIR_DEPLOY_USER` (default `lir`, which must exist) say where and as whom
each node runs; the unit is made from `lir-node.service.in`. Its arguments are
the build and the four `NAME=ADDRESS` pairs:

```
LIR_ONLY=node-b sh deploy/test-cluster/install.sh root@192.0.2.2:/root/lir-work/<build>/release \
  node-a=192.0.2.1 node-b=192.0.2.2 node-c=192.0.2.3 node-d=192.0.2.4
```

Upgrade one node at a time, with the acting scheduler's node (node-a) last.
After each node, check `GET /retained` on that node itself as well as the
cluster: an HTTP failure on one node shows elsewhere only as "unreachable".
On a node, `systemctl status lir-node` and `journalctl -u lir-node`.
`$lir/cluster` says how many nodes are joined:
`curl -sN -m 3 'http://192.0.2.1:8183/subscribe?filter=%24lir/cluster'`.

A node lives in `LIR_DEPLOY_DIR`, here `/opt/lir-cluster`: `bin/lir-node`, `lib/` (Lir's and
Dannan's libraries), `node.yaml`, `cluster.key` (0600) and `state/`. `state/`
holds the node's id and Dannan's agreed roster. Keep it: a node that starts
with an empty one joins under a new id, and the old id stays a member until it
is retired. Outside that directory a node needs:
- `/etc/systemd/system/lir-node.service`, enabled;
- `LIR_DEPLOY_USER`, here `lir`, which owns the directory;
- libc, libstdc++ and OpenSSL 3 from the OS;
- `cgroup_enable=memory` on the kernel command line, so the unit's memory
  settings take effect.
That is what a wipe of a build node has to keep.

Each node's `$lir/nodes/<id>` gives its partition reading: the members of
Dannan's agreed roster, how many are reachable, and the ids it cannot reach.
A node that has joined stays a member until an operator retires it, even
after it has gone for good. To retire one, on each node in turn:

```
systemctl stop lir-node
cd /opt/lir-cluster
sudo -u lir env LD_LIBRARY_PATH=/opt/lir-cluster/lib bin/lir-node --config node.yaml --retire <node id>
systemctl start lir-node
```

The command prints `{"event":"retire","id":"...","retired":true}`, or
`false` when the node was not a member, and is refused while the node runs.
A retired node that joins again is a member again.
