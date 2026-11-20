# mcpp Build System Design

HuxerUI has two build systems. This document specifies the second one: how the
framework is an [mcpp](https://github.com/mcpp-community/mcpp) package, how an
application consumes it, and what the C++20 module surface guarantees.

Both build the framework and applications for Linux, Windows, macOS, the Web,
Android and iOS; on Android and iOS the standard Gradle and Xcode shells consume
the core either one builds, as [SDK, CLI, Platform Shell, and Library](sdk-cli.md)
describes. Nothing in the CMake build reads any file described here, and deleting
`mcpp.toml`, `build.mcpp` and `buildtools/mcpp/` returns the repository to a CMake-only
project.

## 1. Division of ownership

| | CMake | mcpp |
|---|---|---|
| Platforms | Linux, Windows, macOS, Android, iOS, Web | Linux, Windows, macOS, Android, iOS, Web |
| Consumption | `#include <huxerui/huxerui.h>` | `import huxerui;` *and* the headers |
| Linux dependencies | the distribution's packages | xlings payloads |
| Toolchain | the host's | mcpp's own, per target |
| SDK packaging | `sdk-release.yml` | `mcpp pack` |

The two answer *where does GTK come from* differently on purpose; §5 explains
why they must.

## 2. Package layout

```
mcpp.toml                     the framework as a package
build.mcpp                    its build program
modules/huxerui.cppm          the C++20 module interface (generated, committed)
buildtools/mcpp/
  huxerui-build-rules/        `import huxerui.rules;` -- the application-facing API
  huxerui-source-select/      the scanning the rules and the tools share
  huxerui-tools/              huxerui-build-check, huxerui-module-gen
  huxerui-tests/              the unit suite, compiled against the mcpp build
  examples/                   four applications
templates/                    mcpp package templates: app, navigation, library
tools/codegen/mcpp.toml       hcg as a package
tools/resource_compiler/…     hrc as a package
```

`huxerui-build-rules` is deliberately not a workspace member: it imports mcpp,
so it compiles only inside a build program's compile.

## 3. The application surface

An application's manifest names one dependency and its build program is one
call:

```toml
[dependencies]
huxerui = "0.5.0"

[build]
sources = []
```

```cpp
import mcpp;
import huxerui.rules;

int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```

Everything else arrives through that edge. The framework declares its tools and
its rule module as **build**-dependencies with `reexport = true`:

```toml
[build-dependencies]
huxerui-codegen           = { path = "tools/codegen",            tools = ["hcg"], reexport = true }
huxerui-resource-compiler = { path = "tools/resource_compiler",  tools = ["hrc"], reexport = true }
huxerui-build-rules       = { path = "buildtools/mcpp/huxerui-build-rules", host-module = true, reexport = true }
```

`reexport` is what carries them one dependency edge further, so the application
reaches `huxerui.rules`, `hcg` and `hrc` without naming any of them. None of
the three reaches the artifact: two run on the build machine and produce
sources, and the third is compiled into `build.mcpp` itself.

### `sources = []` is not an oversight

`huxerui.rules` owns the source selection. A build program can **add** a source
but cannot replace one, so a package that also globbed `src/**/*.cpp` would
link both the original and the transformed copy of every composable —
`multiple definition of Counter()`. The rule selects, transforms and submits;
the manifest stays out of it.

The target's entry is the one file the rule never transforms, for the same
reason: a transformed copy of `main.cpp` would link beside the original.

On the Web and Android the platform layer starts the application: the page mounts the module through its exports, and the Java host loads the shared library.
No `main` runs application code there, and the CMake declaration compiles no entry for them.
An application's Web and Android rows therefore state `main = false`: the image has no program start, and mcpp links the Web module with `--no-entry` itself.
The platform's entry points, the JNI exports and the Web module's factory, stay in the framework, retained by `build.mcpp` as CMake's link interface retains them.

### Resources and libraries

`configure()` produces the one final resource package the [Build System Integration Contract](../../buildtools/specs/build-system-integration.md) requires and deploys it to `<program>.resources/` beside the program, where the Runtime looks for it.
An application's own root uses the namespace `app` unless the build program states another.

A HuxerUI library states its public identity and resource root about itself:

```toml
[package.metadata.huxerui]
library   = "CameraKit::CameraKit"
resources = "resources"
```

The application's build program reads the resolved graph through `mcpp::graph_file()`, which carries that table for every package in the closure, including libraries the application does not name.
It compiles each library's root under the namespace its identity projects to, merges builtin, library, and application packages in contract order, and writes the ordered library-graph record to `.huxerui/generated/libraries.json` with the schema the CMake integration uses.
A library's own build program reads the same table from its own manifest to generate its resource header.
A package that has module interface units also gets a generated `<namespace>.resources` module, so module code writes `import app.resources;` instead of including the header.

A library with public headers under `include/` that declares mcpp, names the C++ `namespace` of `[package.metadata.huxerui]`, and states a `[lib]` path is also a module.
Its interface is the `[lib]` unit, wherever the author puts it, and its module is named after the package identity: `<package namespace>.<package name>`, or the name alone, with `-` written as `_`.
The package identity rather than the C++ namespace names the module because a module name is global to a program and a package identity is unique where packages are published, while one C++ namespace may be shared by several libraries; the C++ namespace still decides which names the interface exports.
The interface has the shape of `modules/huxerui.cppm`: the headers in the global module fragment, `export import huxerui;`, and a `using` declaration for each name the headers declare directly in the namespace, so `import <module>;` and `#include` reach the same entities and the library needs no second binary.

| `cpp_modules_gen` | The interface |
| --- | --- |
| absent or `true` | `configure()` generates it from the headers with the scanner `huxerui-module-gen` uses, writing it only when its content changes and reporting a note when it does |
| `false` | the author writes it; `configure()` reports the names the headers declare and the interface does not export |

Either way the rules add the interface to the package's sources, rerun when a header, the interface, or the manifest changes, and warn about a macro a header defines, which no module exports. A name declared `static` has internal linkage and is not exported.
They watch the manifest, the set of headers, and the set of units below `modules/` before they decide whether the package is a module, so a headers library made a module later, by adding the `namespace` or the interface file after a build, runs them again.
A module carries no macro, and the rules map none: which macros importers need is the author's choice, and whether a macro is a constant, a type, or syntax is not decidable from the header's text.
The author maps the chosen ones to constants in a unit named `<module>.macros`, under their own names in `<namespace>::macros`.
The rules compile every unit below `modules/` and the generated interface re-exports those named under the module, so the unit is found by its name, not its file; the documentation recommends `modules/macros.cppm` beside a `modules/<package name>.cppm` interface, and no layout is checked.
A macro such a unit names, and one its header undefines again, is not reported; with `cpp_modules_gen = false` the rules report an interface that does not re-export such a unit.
`buildtools/mcpp/examples/04-headers-library` builds this shape.
The key matters only to such a library: a CMake-only library has no mcpp build, and a C++ modules library's interface is its source.

## 4. The module surface

`modules/huxerui.cppm` includes the public headers in its global module
fragment and re-exports their names, so `import huxerui;` and
`#include <huxerui/huxerui.h>` name the **same entities** with the same
linkage. It is generated by `huxerui-module-gen` and committed; CI asserts it
is current, which is what catches a public name added to a header and not to
the shell.

### No headers in an application

An application built by mcpp contains no `#include` at all:

```cpp
export module app;

import std;
import huxerui;

[[huxerui::composable]]
View Counter() { auto count = UseState(0); … }
```

`import std;` is load-bearing. `UseState()`, `View` and `Layout` instantiate
`typeid` in their **caller**, GCC checks that per translation unit, and a
global module fragment's includes do not reach whoever imports it — so
`import huxerui;` cannot supply `<typeinfo>` the way the umbrella header does.
`huxerui.rules` cannot force it either: `-include` prepends before `module;`,
which is ill-formed. Importing `std` answers it without a header.

The std module is a C++23 *library* feature that every implementation HuxerUI
builds with offers in C++20 mode as well — the MSVC STL's named modules are
documented as needing `/std:c++20` or later.

**Every mcpp package of HuxerUI states `standard = "c++23"`, mcpp's default.** A build program is compiled at its package's standard, and `huxerui.rules` imports the official `mcpp:plugins` distribution members, which need C++23; a module graph has one standard, so the framework, its tools and its applications agree on it. mcpp's clang-on-Windows path also refuses `import std;` below c++23 ([mcpp#603](https://github.com/mcpp-community/mcpp/issues/603)). The CMake integration keeps its C++20 floor.

`[toolchain] windows = "msvc@system"` would sidestep it and put both build
systems on one compiler, and is blocked separately: a host module's BMI reaches
`cl.exe` in clang's `name=path` spelling and the compile dies with `C1083`
([mcpp#604](https://github.com/mcpp-community/mcpp/issues/604)).

### Macros are the exception

`HUXERUI_SCOPE` and friends are macros, and macros do not cross a module
boundary. Two different holes, two different fixes:

- **Generated code.** `hcg` injects the macro's *expansion*, not its name, so a
  transformed composable compiles under `import` and under `#include` alike.
  Preprocessing the two forms yields identical tokens, so the CMake build sees
  no change at all. See [Composable Code Generation](composable-codegen.md).
- **Hand-written code.** A scope written by hand is spelled the way the macro
  expands — `return Scope([=]() -> View { … });` — and `Scope` and `View` are
  exported, so a module unit needs neither the macro nor a header.
  `buildtools/mcpp/examples/02-module-units/src/banner.cppm` is the worked case.
  `<huxerui_scope_prelude.h>` remains for a **non-module** translation unit in
  an mcpp project, which the rule force-includes; that is the only case left
  where the macro is reachable and needed.

The prelude is generated from `view.h` by the same `huxerui-module-gen`, so the
two definitions cannot drift into an illegal redefinition.

## 5. Linux dependencies come from xlings, not from the machine

mcpp is payload-first: it compiles with its own toolchain and its own glibc.
Reaching for the host's GTK mixes two C libraries in one binary, and it does
not fail politely. `pkg-config --cflags gtk4` on Debian emits the system
multiarch include directory, the payload glibc's `<time.h>` then finds the
**system's** `<bits/time.h>`, and every core translation unit fails:

```
error: 'time' has not been declared in '::'
error: 'struct timespec' has no member named 'tv_sec'
```

Not only the platform units — a build program's include directories colour the
whole package. `-idirafter` makes it compile again and is still wrong: the link
would then join libraries built against one glibc to objects compiled against
another.

So the transitive `.pc` closure of `gtk4 + epoxy + libsoup` is declared as
xlings payloads and `build.mcpp` points `PKG_CONFIG_LIBDIR` at those only,
which keeps the host out entirely. They are declared on the **target** axis
(`[target.'cfg(all(os = "linux", not(env = "android")))'.xlings.workspace]`) because they are what the produced
code is compiled and linked against.

### Dialect flags belong to the whole graph

`pkg-config --cflags gtk4` also emits `-pthread -msse -msse2 -mfpmath=sse`. A
`cxxflag` emitted by a build program colours that package's own translation
units and nothing else, so the framework compiled with them and every consumer
compiled without. clang records the thread model and target-feature set in the
BMI it writes and refuses one that disagrees, which surfaces as every name in
`import huxerui;` being undefined — nineteen errors that never mention a
module. GCC's check is laxer, which is the only reason this held together until
the build ran on clang.

`-pthread` is therefore `[build] dialect_cxxflags`, which mcpp applies to every
translation unit in the graph. The `-m` flags are dropped: SSE2 is baseline on
x86-64, so naming them explicitly only put a feature list in the BMI.

## 6. Platform interfaces

**Windows.** mcpp builds with clang targeting `x86_64-windows-msvc` — the MSVC
ABI and the Windows SDK, not MinGW — so MSVC-dialect flags (`/W4`,
`/permissive-`, `/utf-8`) are rejected by its GNU driver and are absent.
`[target.windows.runtime] libraries` names five libraries CMake never states
either: `CMAKE_CXX_STANDARD_LIBRARIES` adds kernel32, gdi32, winspool, uuid and
comdlg32 implicitly, and mcpp links exactly what the manifest names. Omitting
them surfaces at the link as `undefined symbol: CreateDIBSection`.

Windows application templates and examples select the GUI subsystem and `mainCRTStartup` with MSVC-compatible linker directives in their `main.cpp` entry, preserving CRT initialization and the portable `main()` signature without creating a console. This selection belongs to the entry object: mcpp currently has no per-target link flags, and package-level `ldflags` or `link-flag` directives also reach tests and consumers. `huxerui.rules::configure()` therefore does not emit these flags, since component libraries use it too. CMake makes the equivalent selection on each application target in `huxerui_add_app`.

**macOS.** `.mm` is a first-class source kind but is not in the default glob,
so `platform/macos/*.mm` is listed explicitly. `frameworks` is a **top-level**
`[runtime]` key — the per-target vocabulary is `libraries` only, and a
`[target.macos.runtime] frameworks` entry is ignored, failing at link on macOS
alone. Unconditional is correct: the engine renders it only for Mach-O.

## 7. Packaging an application

`mcpp pack --pkg-format <format>` packages an application through the official `mcpp:plugins` distribution members, and `huxerui package <platform>` runs it for an mcpp project.
`huxerui-build-rules` depends on `mcpp:plugins` with the `dist-appimage`, `dist-deb`, `dist-rpm`, `dist-wix`, `dist-apple`, `dist-web`, and `dist-apk` features, and `configure()` configures each member from the application's identity and its platform shell; a member declares its format in every pass and acts only in the pass that asked for it, and its tool is installed only when a pack asks for it.

| Platform | Formats | What the rule supplies |
| --- | --- | --- |
| Linux | `appimage` (the CLI's default), `deb`, `rpm` | name, categories, `platform/linux/package/<target>.svg`; for deb and rpm the maintainer, by default the first `[package] authors` entry |
| Windows | `setup` (the CLI's default, as CMake's), `msi` | product name, manufacturer, and the upgrade code derived from the identifier as `string(UUID ... TYPE SHA1)` derives it in CMake |
| macOS | `dmg`, `app` | name, identifier, `platform/macos/AppIcon.icns` |
| iOS | `app` | name and identifier |
| Web | `web` | `platform/web/index.html.in` rendered with the CMake integration's values, and its icons |
| Android | `apk`, `aab` | identifier, label, and a generated application shell: an Application that loads the program's library and an Activity on `org.huxerui.HuxerUIActivity` |

The identity is `[package.metadata.huxerui] name` and `id` in the application's manifest, or `bundle_name` and `bundle_identifier` in `configure()`.
Packaging options beyond it are `configure({.package = {...}})`: categories, maintainer, depends, manufacturer, and upgrade code.
An Android package needs the identifier; without one the rule warns and configures no package, and the build goes on.

The final resource package is deployed where each format's Runtime reads it: `<program>.resources/` beside the program on the desktop, `HuxerUI/` for the Apple bundles (`Contents/Resources/HuxerUI` on macOS, the bundle root on iOS), and the asset root of an APK.
The framework states what an Android package carries from it, its Java host and its manifest entries, in `[package.metadata.dist-apk]` of its own manifest; `dist-apk` reads that table from every package in the application's resolved graph and substitutes `${applicationId}`.
A library contributes its Android platform package without stating anything: for every library in the graph that has a Gradle library module at `platform/android` and no `[package.metadata.dist-apk]`, the rule passes `dist-apk` the module's `src/main/java`, `src/main/kotlin`, `src/main/AndroidManifest.xml`, `src/main/res`, and `src/main/assets`, under the package its `[package.metadata.huxerui] id` names, which is the namespace the CLI gives that module.
A library that states `[package.metadata.dist-apk]` is not inferred; its table replaces the inference as a whole, and carries what the layout cannot, such as Maven coordinates, archives, or further source roots.
A library's iOS Swift package reaches the application through the Xcode shell; `rules-swift` compiles only the Swift sources a package names in its own manifest, so packaging for iOS without the shell warns that the package is not included.
Android applications state `cxx_runtime = "self-contained"` for their Android triples, so the application's shared library links the C++ runtime statically, as the CMake integration's `c++_static`.

A Web build also lays the rendered page beside the module as `<program>.html`, the name the CMake build gives it, so the CLI serves and watches either integration's build the same way.
`mcpp pack` reports the Web page as the distributable; the CLI publishes the page's directory, which holds the module, its WebAssembly, the preloaded data, and that page beside dist-web's `index.html`.

## 8. Verification

| Check | What it catches |
|---|---|
| `huxerui-build-check` | a source, define or platform library in one build system and not the other |
| `huxerui-module-gen --check` | a public name added to a header and not to the module shell |
| `mcpp test` in `huxerui-source-select` | the scanning the rules and the tools share |
| `mcpp test` in `huxerui-tests` | 209 cases and 11,521 assertions against the mcpp-built framework |
| `mcpp pack` shape | headers, module interface and library all present |
| Build System Conformance | each integration's build, resources, launch, and distributable against the projects' expected facts (`buildtools/cts/README.md`) |

Both tools are C++ programs built by mcpp: a repository that offers mcpp as a
first-class build system should not need a second language to check its own
build.

CI covers Linux with GCC, Linux with clang, macOS and Windows. The clang leg is
not redundant with the other two — Windows and macOS compile a BMI and its
consumer in the same job with the same flags, so a dialect divergence like §5's
shows only where GCC is the default and clang is the exception.

## 9. Known gaps

**A published package carries no consumer dependencies.** `mcpp emit xpkg`
derives a descriptor's `xpm.<platform>.deps` from the top-level
`[xlings.workspace]` only, and the GTK payloads are declared on the target axis
because that is what they are, and mcpp's own guidance says so: *for anything
the produced code is compiled or linked against, the target axis is the
recommended form*. Declaring them on both axes satisfies the descriptor, but
the host-axis copy is right only by accident — it states a target fact about
the build machine, and stops being right the first time the package is
cross-compiled. Until mcpp derives consumer dependencies from the target axis,
a published `huxerui.huxerui` would resolve and then fail to link on a machine
without the payloads. Path and git dependencies are unaffected — they provision the target
axis, which is what `buildtools/mcpp/examples/` relies on.

**`tests/runtime` and `tests/platform` are not in the mcpp suite.** They reach
fixtures a CMake step generates, and each needs an mcpp equivalent before it
compiles there. `tests/cli` links the CLI, a CMake target.

**`mcpp test` cannot run from the repository root.** mcpp defines a test as
every `tests/**/*.cpp` with no manifest key to point that elsewhere, and this
repository's `tests/` is CMake's. `buildtools/mcpp/huxerui-tests` selects instead of
discovering.
