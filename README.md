# Lir - The Message Broker

[![Release](https://img.shields.io/github/v/release/tomdionysus/lir?sort=semver)](https://github.com/tomdionysus/lir/releases)
[![CI](https://github.com/tomdionysus/lir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/lir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/lir/graph/badge.svg)](https://codecov.io/gh/tomdionysus/lir)
[![CodeQL](https://github.com/tomdionysus/lir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/lir/actions/workflows/codeql.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![MQTT](https://img.shields.io/badge/MQTT-5.0%20%7C%203.1.1%20%7C%203.1-660066.svg)](docs/conformance.md)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Lir is an MQTT broker (MQTT 5.0, 3.1.1 and 3.1) made of a cluster of
trusted machines: connect to any node, publish or subscribe on any node, and
the cluster behaves as one broker, with no leader and no node whose loss
stops it. It is built on [Dannan](https://github.com/tomdionysus/dannan), the
distributed system and network layer, and
[Dagda](https://github.com/tomdionysus/dagda), the store.

## Status

**The broker is not yet functional.** Lir is being rewritten from scratch,
and no release yet contains a broker that serves clients. What a release
holds today is `lir::mqtt`, the MQTT codec the stack's projects link:

- every control packet of MQTT 5.0, 3.1.1 and 3.1, encoded and decoded, with
  every MUST the specification states about a packet's own bytes checked;
- MQTT 5.0 properties, checked against the packet that carries them;
- topic names, topic filters, shared subscriptions and matching.

The broker's progress is tracked in [`TODO/ACTIVE.md`](TODO/ACTIVE.md);
what "supports MQTT" will mean is [`docs/conformance.md`](docs/conformance.md),
and the contract other systems build against is
[`docs/contract.md`](docs/contract.md). Changes per release are in
[`CHANGELOG.md`](CHANGELOG.md).

## Installing

Debian packages for trixie, on x86-64 and arm64, are attached to each
[release](https://github.com/tomdionysus/lir/releases):

- `liblir-mqtt0.1`: the shared library `lir::mqtt`;
- `liblir-dev`: its headers and CMake package.

They depend on Dannan's packages of the matching minor (`libdannan0.9`,
`libdannan-dev`). Install both projects' packages together:

```sh
sudo apt install ./libdannan*.deb ./liblir*.deb
```

How the packages are built and versioned is in
[`docs/packaging.md`](docs/packaging.md).

## Using lir::mqtt

From CMake, with `liblir-dev` installed:

```cmake
find_package(lir 0.1 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE lir::mqtt)
```

```cpp
#include "lir/mqtt/packet.h"
#include "lir/mqtt/topic.h"

lir::mqtt::Publish publish;
publish.topic = "sensors/kitchen";
publish.payload = {'2', '1'};
const auto bytes = lir::mqtt::encode(publish, lir::mqtt::Version::v5);
const auto packet = lir::mqtt::decode(bytes, lir::mqtt::Version::v5);
const bool hit = lir::mqtt::matches("sensors/+", publish.topic);  // true
```

A stream's packets are framed with `read_frame`, which says how long the
packet in front is once its fixed header has arrived. A malformed packet
throws `MalformedPacket` carrying the reason code a server sends for it.

## Building from source

On Linux (Debian trixie), with g++ or clang++, CMake, Ninja, Boost, OpenSSL,
yaml-cpp and GoogleTest:

```sh
cmake -S . -B build -G Ninja -DLIR_BUILD_TESTING=ON
cmake --build build
./build/lir_tests
```

Dannan is fetched at its pinned tag (`-DLIR_DANNAN_REPOSITORY` names where
from); `-DLIR_SYSTEM_DANNAN=ON` uses the installed `libdannan-dev` instead
and adds the install rules. The tests need the mosquitto command-line
clients. `tools/ci.sh <step>` runs each CI step, sanitizers and fuzzing
included.

## License

GPLv3; see [LICENSE](LICENSE).
