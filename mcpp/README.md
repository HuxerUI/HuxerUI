# Developing HuxerUI and its applications with mcpp

HuxerUI has two build systems. **CMake is the full-platform one and the only
path for Android, iOS and Web**; mcpp builds the framework and applications on
Linux, Windows and macOS. Neither reads the other's files, and deleting this
directory plus `mcpp.toml` and `build.mcpp` returns the repository to a
CMake-only project.

The design and the open gaps are in
[`docs/design/mcpp-build-system.md`](../docs/design/mcpp-build-system.md);
three worked applications are in [`examples/`](examples/).

## Building

```bash
mcpp build                          # the framework
cd mcpp/examples/01-import && mcpp build  # an application on top of it
```

**Nothing needs to be installed first.** The GTK 4 stack comes from the xlings
payloads declared in `../mcpp.toml`, which mcpp provisions on the first build.

## What lives here

| Path | Contents |
|---|---|
| `huxerui-build-rules/` | `huxerui.rules`, the module a consumer's `build.mcpp` imports |
| `huxerui-source-select/` | the pure half of the rules -- globbing, the composable pre-filter, header scanning -- split out so `mcpp test` can reach it |
| `huxerui-tools/` | `huxerui-build-check` (parity) and `huxerui-module-gen` (the module shell and scope prelude) |

Elsewhere: `../modules/huxerui.cppm` is the C++20 module front door (generated
by `huxerui-module-gen`), and `../tools/{codegen,resource_compiler}/mcpp.toml`
make `hcg` and `hrc` buildable packages.

Both tools were Python once. They are C++ now for the reason this directory
exists: a repository that offers mcpp as a first-class build system should not
need a second language to check its own build. The rewrite was not free of
consequence -- the C++ header scanner finds five public names the Python one
missed, all of which compile, and correctly drops one it exported that was a
template parameter rather than a type.

`../mcpp.toml` is the framework package and the workspace root; `../build.mcpp`
resolves the Linux platform dependencies and produces the builtin resource
header.

## Writing an application

The manifest names one dependency and the build program makes one call:

```toml
[dependencies]
huxerui = { path = "../.." }        # or a version, once published
```

```cpp
import mcpp;
import huxerui.rules;
int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```

`configure()` schedules the composable transform (`hcg`) and the resource
compiler (`hrc`) as build-graph edges. GTK, the HuxerUI library and the include
path arrive through the dependency edge: a dependency's `build.mcpp` emits
`link-lib` / `link-search` that reach the **final** link, so an application
never restates them. `mcpp/examples/` holds three worked examples.

## Three things worth knowing before editing

**The GTK stack is NOT the machine's.** mcpp compiles with its own toolchain and
its own glibc, so reaching for the host's GTK mixes two C libraries in one
binary. Measured: `pkg-config --cflags gtk4` emits
`-I/usr/include/x86_64-linux-gnu`, the payload glibc's `<time.h>` then reaches
the system's `<bits/time.h>`, and every core translation unit fails on
`'time' has not been declared in '::'`. The payload list in `../mcpp.toml` is
the transitive `.pc` closure, pinned. CMake keeps using the distribution's
packages, as `cmake/platform/Linux.cmake` and AGENTS.md require.

**The host tools are built from source, not taken from `tools/prebuilt/`.**
Those binaries are produced for the host's C library by
`.github/workflows/update-host-tools.yml`, and after a change to
`tools/codegen/transform.cpp` they are stale until that workflow runs on main.
CMake keeps using them; the mcpp leg builds its own with mcpp's toolchain.

**The builtin resources are compiled twice.** The framework compiles them for
the header its own 11 translation units `#include`; an application compiles
them again for the package it merges with its own resources. mcpp has no
channel from a dependency's build output to a consumer's build input -- a
consumer is given `dep_dir()`, the dependency's *source* root -- and inventing
one would mean writing into a package root that may be read-only. 44 files /
196 KB, incrementally cached; the alternatives are enumerated in the plan.

## Packaging

```bash
mcpp pack
```

writes `target/dist/huxerui-<version>-<compatibility tag>.tar.gz` containing
`include/huxerui/**`, `interface/huxerui.cppm` and `lib/<triple>/libhuxerui.a`
-- both consumption paths and the binary, with no packaging script of its own.
It is per-leg by construction: a package for another platform comes from that
platform's runner, and `mcpp pack` says so when `[package] platforms` claims one
that was not packed.

**This is not the release SDK.** That one covers six platforms and ships a CMake
package config for CMake consumers; `mcpp pack` produces the three legs mcpp
builds, for mcpp consumers. `cmake/HuxerUISdk.cmake` and `scripts/package_sdk.sh`
keep owning the release -- the script also *builds* the Android, Web and iOS
artifacts, which mcpp cannot.

## Parity

Two build systems maintaining one set of facts drift, and the drift is quiet:
add `platform/windows/win32_foo.cpp`, forget `mcpp.toml`, and the Windows mcpp
build fails at link with `undefined reference`, far from the cause.

```bash
cd mcpp/huxerui-tools && mcpp run huxerui-build-check
```

is the first job in `.github/workflows/mcpp-build.yml`. It already caught one
real defect: a bare
`platform/windows/*.cpp` glob swept in `windows_installer.cpp`, which is the
WiX custom-action DLL built separately by `cmake/HuxerUISdk.cmake`, not part of
the framework.
