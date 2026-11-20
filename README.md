# Lir - The Message Broker

[![Release](https://img.shields.io/github/v/release/tomdionysus/lir?sort=semver)](https://github.com/tomdionysus/lir/releases)
[![CI](https://github.com/tomdionysus/lir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/lir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/lir/graph/badge.svg)](https://codecov.io/gh/tomdionysus/lir)
[![CodeQL](https://github.com/tomdionysus/lir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/lir/actions/workflows/codeql.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![MQTT](https://img.shields.io/badge/MQTT-5.0%20%7C%203.1.1%20%7C%203.1-660066.svg)](docs/conformance.md)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Lir is the event system for a cluster of trusted machines: one MQTT broker
(MQTT 5.0, 3.1.1 and 3.1) made of the cluster itself. Connect to any node,
publish or subscribe on any node, resume a session on another, and the
cluster behaves as a single broker. There is no broker machine, no leader and
no node whose loss stops the bus; both sides of a network split keep serving
and merge when it heals.

Lir is built on [Dannan](https://github.com/tomdionysus/dannan), the
distributed system and network layer, and
[Dagda](https://github.com/tomdionysus/dagda), the store. It is MQTT expressed
in their nouns: membership, placement, transport between nodes and what
happens in a split are Dannan's, and everything that must survive a node is
Dagda's. Lir adds what makes it MQTT and nothing below it.

## Status

**Being rewritten from scratch** (Tom, 2026-11-20). The first Lir was a
broker of its own with a cluster added beside Dannan; it is withdrawn. What
the rewrite works from:

- The premise and the plan: [`TODO/2026-11-19-premise-and-audit.md`](TODO/2026-11-19-premise-and-audit.md).
- What "supports MQTT" means: [`docs/conformance.md`](docs/conformance.md),
  every normative statement of MQTT 5.0 and 3.1.1 by its identifier.
- The contract other systems rely on, kept through the rewrite:
  [`docs/contract.md`](docs/contract.md).

The withdrawn code, its features and its README are in the history at
`92ab3a0` on `develop`. The standing Pi cluster still runs it (`e839b4a`) and
stays as it is until Tom says otherwise; `deploy/pi-cluster/README.md` is how
to look after it.

## Building

Builds and tests run on Linux (Debian 13, as the nodes have it):

```sh
cmake -S . -B build -G Ninja -DLIR_BUILD_TESTING=ON
cmake --build build
./build/lir_tests
```

Dannan is fetched at its pinned tag from the sibling checkout `../dannan`
(`-DLIR_DANNAN_REPOSITORY` names another). `tools/ci.sh` runs each CI step.

## License

GPLv3; see [LICENSE](LICENSE).
