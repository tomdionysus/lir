# Packaging

Lir is packaged for Debian trixie, on x86-64 and arm64, to the stack's
convention: Dannan's and Dagda's `debian/`, the coordinating session's
record of 2026-11-20. The packages are:

- `liblir-mqtt<major.minor>` (now `liblir-mqtt0.2`): `lir::mqtt`, the shared
  library every MQTT speaker in the stack links (`docs/contract.md`,
  section 2). Its SONAME is major.minor, `liblir_mqtt.so.0.2`; a patch
  release keeps the ABI, and a minor release is a new SONAME and a new
  package, installable beside the old.
- `liblir-client<major.minor>` (now `liblir-client0.2`): `lir::client`, the
  stack's MQTT client, versioned the same way.
- `liblir-dev`: the headers, the `.so` links and the CMake package
  configuration. It depends on exactly its own release's libraries and on
  `libdannan-dev` of the minor Lir builds on.
- `lir`: the broker's node, `lir-node`, with its service
  (`lir.service`, installed disabled), the `lir` system user (sysusers),
  `/etc/lir/config.yaml` as a conffile (`config/config.yaml`), and
  `/etc/lir/secrets.yaml` made empty by postinst and never part of the
  package. The operator puts the cluster's key in `/etc/lir/cluster.key`
  (and any TLS pair in `/etc/lir/tls`), then enables the service. A purge
  removes `/etc/lir` and keeps `/var/lib/lir`, the node's identity. The
  status and admin dashboard joins it when it exists.
- Debug symbols for each, as `-dbgsym` packages.

The packages are built with debhelper from `debian/`, driving the CMake
build; the CMake install rules decide where every file goes, and
`debian/rules` splits them between the packages. Dannan is always the
installed package, never a copy: `-DLIR_SYSTEM_DANNAN=ON` finds it with
`find_package(dannan <pin> CONFIG)` and defines Lir's install rules, which
a project fetching Lir with its own Dannan never meets.

A consumer's packaging:

```
Build-Depends: liblir-dev (>= 0.2.0), liblir-dev (<< 0.3~)
Depends: ${shlibs:Depends}          # gives liblir-mqtt0.2 (>= 0.2.0)
```

and in CMake `find_package(lir 0.2.0 CONFIG REQUIRED)`, then link
`lir::mqtt` or `lir::client`, which bring Dannan.

## Building

On Debian trixie, natively for the machine's architecture, from a git
checkout, with Dannan's `libdannan-dev` installed:

```sh
sudo apt install dpkg-dev
sudo apt build-dep ./
tools/build-deb.sh
```

The packages are written to `build-deb/`. The tests run inside the build,
the real MQTT clients' among them; `DEB_BUILD_OPTIONS=nocheck` skips them.

`tools/ci.sh deb` does all of it in a clean trixie (GitHub's container, or
`docker run debian:trixie` on a build node): Dannan's packages from its pinned tag
by Dannan's own `tools/ci.sh install-debs`, then Lir's, then lintian.
CI runs it on x86-64 and arm64, fails on any lintian error or warning, and
keeps the packages as the run's artifacts. A release's packages are built
by running the workflow on its tag (`gh workflow run CI --ref develop -f
ref=<tag>`) and go to the tomcully.com apt repository, which the maintainer
publishes.
`tools/ci.sh package` installs both into a prefix instead and builds the
consumer in `test/package` from the prefix alone.

## Versions

As Dannan's: the version comes from `git describe`. The tag `0.2.0` gives
`0.2.0`; a build three commits later gives `0.2.0+git3.gabc1234`, which
sorts after `0.2.0` and before `0.2.1`. Release tags sit on `main`, so a
build on `develop`, where none is reachable, gives the tree's version and
the commit, `0.2.0+git.gabc1234`. Uncommitted changes add `.dirty`. A
build of a tag must match `project(lir VERSION ...)` in `CMakeLists.txt`,
and the library's package named in `debian/control` must be `liblir-mqtt`
and its major.minor, or the script refuses it. Only builds of a tag are
released.
