# Contributing to Lir

## Read first

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Lir is
the event system, a hosted system on a stack with one set of conventions:
Dannan the network layer (`../dannan`), Dagda the store (`../dagda`) and
Lugh the directory (`../lugh`). `../dagda/TODO/2026-11-03-stack.md` says
what each owns. Hosted systems use Dannan as their network layer and Dagda
for what they persist, and no code is duplicated between the projects (Tom,
2026-11-03). Lir links Dannan, and Dagda from step 5; only the optional Lugh
plugin (`-DLIR_BUILD_LUGH=ON`) links Lugh's client library, and Lir builds
without it (Tom, 2026-11-14). Lir is an MQTT broker, clustered and
distributed on Dannan, written new from the design, "Lir: Distributed Event
System on Dannan" (`TODO/2026-11-03-lir-plan.md`, with
`TODO/2026-11-04-lir-sections.md`).

`../dannan/CONTRIBUTING.md` is the reference text for the conventions the
projects share. This file follows it in everything but the project's name,
its "Read first", how it is built and tested, and what Dannan alone has
(its packages and its ABI check).

Every change is checked against Lir's premise
(`TODO/2026-11-19-premise-and-audit.md`): no node is special; Lir replicates
and persists nothing itself, which is Dagda's; membership and placement are
Dannan's; nothing stands in for a primitive that has not shipped; nothing
acknowledges a guarantee it does not have. A change that cannot be made that
way yet is not made yet.

The four laws and five disciplines (`docs/principles-and-laws.md`, a verbatim copy
of Macha's) govern every change here, cited by number. A change that touches
scheduling, admission, priority, retry or recovery says which law it serves and
which discipline it follows.

## Branches

- `main` is the last stable state. Nothing is committed to it directly; stable
  states reach it by merging `develop`.
- `develop` is where work happens.
- Work that needs isolation branches off `develop` as `feature/<short-name>` and
  merges back into `develop`.

## Tags and releases

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix, cut from
  `main`.
- The tag, the CMake `project(... VERSION ...)` and `--version` are the same
  number.
- No release is cut yet. The first, `0.1.0`, follows step 5 (persistence on
  Dagda).

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a move, a rename, a fix and a feature
  are four commits. A commit that moves files does not change their content.
- A commit message ends with its last line of prose. No tool attribution trailers.

## Building and testing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIR_BUILD_TESTING=ON \
  -DLIR_WARNINGS_AS_ERRORS=ON
cmake --build build -j8
./build/lir_tests
```

Dannan is fetched into the build tree at the tag pinned in `CMakeLists.txt`
(`LIR_DANNAN_TAG`), from the checkout at `../dannan` or from the repository
`-DLIR_DANNAN_REPOSITORY` names, a path or a URL. Building Lir never touches
`../dannan`. `-DLIR_BUILD_LUGH=ON` fetches Lugh's `lugh_client` the same way,
at a pinned commit. `-DLIR_BUILD_SERVER=OFF` builds only the codec
(`lir::mqtt`) and the client (`lir::client`), as a project that fetches Lir
builds them.

Run the test binary rather than `ctest`. `tools/ci.sh <step>` is one CI step
on Debian 13, as the nodes have it: `check` (the header block and the
format), `release`, `clang`, `asan`, `tsan`, `coverage`, `client` (the codec
and the client alone) and `lugh` (the plugin on). Hosted CI
(`.github/workflows/ci.yml`) runs each on every push to `main` and `develop`,
with release and ASan on x86-64 and on arm64, and a build and test on macOS.
CodeQL analyses every push. The `lugh` job needs the repository secret
`LUGH_READ_TOKEN`.

The project's own sessions build and test on the shared Pi build servers
only, never on the laptop (Tom, 2026-11-14): claim cores on the board
(`deploy/pi-cluster/README.md`), run `tools/ci.sh <step>` with `LIR_CI_JOBS`
no more than the cores claimed, under `nice 19`, and release the claim when
the job ends. Sanitizers run at the release gate, or to hunt a bug that
cannot be pinned any other way. A test is repeated only for a stated reason:
a flaky test, odd behaviour, or a fix.

`-DLIR_BUILD_FUZZING=ON` (clang only) builds the libFuzzer targets in
`lir/fuzz/`, one for each parser of untrusted bytes, with the whole build
instrumented. They run at the release gate, each for a bounded time.

Lir has no compose bed. Its proof on real machines is the standing cluster
(`deploy/pi-cluster/`), upgraded by hand and only with Tom's approval.

Write tests from the design and the laws, not from the current behaviour. A
test is never deleted.

## Dependencies

Dannan, Dagda (from step 5), Boost (Asio, and Beast and JSON, which are part
of it) and OpenSSL 3, plus GoogleTest for the test build. yaml-cpp comes with
Dannan, whose settings reader `lir-node` uses. The optional Lugh plugin adds
Lugh's `lugh_client`. The process tests use `mosquitto_pub` and
`mosquitto_sub` when they are installed. Prefer writing something by hand
over adding a library, and say so if a dependency really is the right call.

## Layout

One layout for all the projects, as `../dannan/CONTRIBUTING.md` "Layout"
gives it, with `<p>` = `lir`: the library target `lir` in `lir/`, public
headers in `lir/include/lir/`, subsystems in `lir/<subsystem>/`,
interfaces in `lir/contract/`, tests in `lir/test/` (one binary
`lir_tests`, fixtures in `lir/test/fixtures/`), fuzz targets in `lir/fuzz/`,
tools in `tools/`. No presets. Lir adds `lir/client/` (the client library,
its own target), `deploy/pi-cluster/` (the standing cluster's unit, config
and install script), and has no `test/compose/`.

## Shared code

What more than one project needs lives once, in the lowest project that
needs it, and the others link it. Lir links Dannan's crypto primitives,
durable files, byte codec, thread-safety contract, time sources, logging (a
logger from `Context::logger`), settings reader, listeners and connectors,
Status surface and test fixtures, and from step 5 Dagda's logs, spaces,
objects, claims and leases. It writes none of its own. When Lir finds itself
writing something another project has, or needs, it asks that project's
session instead.

## Errors

An operation that could not happen throws (`std::invalid_argument` for a
caller's mistake, `std::runtime_error` or a named subclass for bytes or a
system that refused). An operation that happened returns its result, and one
that happened on more than one node returns its evidence (who answered, what
failed and why), never a bare boolean. A read that finds nothing succeeds
with an empty value.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same
  line, headers `.h`.
- `snake_case` for functions, methods, variables and files; `PascalCase` for
  types; private and protected members prefixed with `_`. Namespaces are
  `<project>::<subsystem>`.
- Every source file starts with the Lir header block (`CLAUDE.md` has it;
  `tools/ci.sh check` checks it).
- No emoji anywhere.
- Dependency injection over globals; the only process global is the io context.
- Each component logs through its own scoped logger.
- Thread-safety annotations on every guarded member; contracts declare their waits
  and thread safety beside each operation.
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items
across as they land, with the gate counts.
