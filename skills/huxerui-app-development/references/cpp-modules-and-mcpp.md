# C++ Modules and mcpp

Use this reference for a C++ modules project: its sources are module units that write `import huxerui;`, its `mcpp.toml` depends on `huxerui`, and its `build.mcpp` imports `huxerui.rules`. `import huxerui;` exposes the same public entities as the umbrella header. Preserve the dependency version and build setup selected by the project.

A headers project that also carries an mcpp declaration (`--build-system cmake,mcpp`) builds with the same rules; its sources stay as described in the other references. Such a library, which is a library's default and the recommended form, is also a module named after its package: the mcpp build generates the interface its `[lib]` path names from its public headers, so do not edit that file; to keep a hand-written one, set `cpp_modules_gen = false` under `[package.metadata.huxerui]` and export every public name it reports. An existing headers library gains that interface as [Add C++ modules to an existing headers library](#add-c-modules-to-an-existing-headers-library) describes.

Drive the project with the HuxerUI CLI, which dispatches to mcpp:

```bash
huxerui build linux
huxerui run linux
huxerui package linux
```

The mcpp integration builds Linux, Windows, macOS, the Web, Android, and iOS. `huxerui package <platform>` writes the platform's package through the official `mcpp:plugins` members, and Android, iOS, and macOS packages need the `id` in `[package.metadata.huxerui]`. `mcpp build`, `mcpp run`, `mcpp test`, and `mcpp pack --pkg-format <format>` remain available underneath.

For an existing project that manually links prebuilt SDK binaries instead of using the package rules, read [mcpp-build.md](mcpp-build.md).

## Project structure

```text
mcpp.toml               package, dependency, targets, and language standard
build.mcpp              HuxerUI build-rule configuration
src/app.cppm            application module: the page, its composables, and the Application object
platform/<id>/main.cpp  each desktop platform's process entry; the Android, iOS, and Web shells as in any project
resources/              optional resource input
```

Create a C++ modules application with `huxerui create app <name> --build-system mcpp`; `huxerui create library <name> --build-system mcpp` creates a C++ modules library. mcpp alone selects C++ modules, and a build-system list that names `cmake` selects headers. `--template` selects the application content (`app` or `navigation`), and `--platform` selects the platform shells as for any other project. Inspect the generated manifest rather than copying a fixed dependency version into an existing project.

A C++ modules library exports a module named after its package, records its identity in `[package.metadata.huxerui]`, and carries an `examples/preview` application that imports it; `huxerui run <platform>` from the library tree runs that Preview. Its Android platform package under `platform/android` reaches an application's package without further declaration; state `[package.metadata.dist-apk]` only to replace that inference, for example to add Maven coordinates.

Name a library's module after its package identity, `<package namespace>.<package name>` or the name alone, with `-` written as `_`: a module name is global to a program, and a package identity is unique where packages are published, while one C++ namespace may be shared by several libraries.
Give a library meant for publication its package namespace from the start, because adding it later renames the module for every importer.
The root interface of a C++ modules library is `src/<package name>.cppm`, its `[lib]` path. As the library grows, its other units follow the module name with one directory per segment below the root:

```text
src/widgets.cppm        export module acme.widgets;
src/widgets/meter.cppm  export module acme.widgets.meter;
src/widgets/dial.cppm   export module acme.widgets.dial;
```

The build program imports the HuxerUI rules:

```cpp
import mcpp;
import huxerui.rules;

int main() {
  return huxerui::rules::configure({.resources = "resources"}) ? 0 : 1;
}
```

Keep `[build] sources = []` when the rules own source discovery. They transform composables and add the resulting sources; also globbing the originals can link duplicate definitions. Preserve the generated application target and each platform's `main` path.

The rules compile and stage resource packages through `hrc`. An application's resource namespace is `app`, as in a CMake application, unless `resource_namespace` is supplied; a library's follows its public identity. A package written as module units reaches its resources with `import <namespace>.resources;`, such as `import app.resources;`, and needs no header.

## Module code

```cpp
export module app;

import std;
import huxerui;

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);
  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count),
  };
}
```

Use module imports rather than `#include` in the generated module application. Import `std` in units that instantiate HuxerUI templates or use standard-library entities; importing HuxerUI does not make all standard-library declarations reachable to the caller.

mcpp projects use C++23, mcpp's default: `huxerui.rules` runs in the build program and imports `mcpp:plugins` members that need it. The CMake path keeps its C++20 floor. A newer language mode alone does not solve binary compatibility with a prebuilt SDK.

Define annotated composables in source or module units processed by `huxerui.rules`. The application root already owns a scope. Keep each `platform/<id>/main.cpp` focused on required entry setup and `RunApplication()`; platform registration required before startup can still live there.

Macros do not cross module boundaries. Prefer `[[huxerui::composable]]`; when explicitly writing a scope, use the exported `Scope` and `View` APIs rather than assuming `HUXERUI_SCOPE` is imported.

## Add C++ modules to an existing headers library

A headers library becomes importable as a C++ module without changing its sources or its CMake declaration: the mcpp build generates the module interface from its public headers.
It needs its public headers below `include/` and its public names in one C++ namespace, which `namespace` names; the module is named after the package.
A headers library has one interface, and at most a macros unit beside it, both directly below `modules/`:

```text
modules/widgets.cppm   export module acme.widgets;         generated from the headers
modules/macros.cppm    export module acme.widgets.macros;  optional, written by hand
```

1. Without an mcpp declaration, take `build.mcpp` and `mcpp.toml` from a library `huxerui create library <name> --build-system cmake,mcpp --target <public-cmake-target> --namespace <cpp-namespace>` writes with the same SDK, so the dependency on HuxerUI matches it, and add each library dependency to `[dependencies]` as a path or package entry.
2. In `mcpp.toml`, name the C++ namespace whose names the module exports, and the interface file:

   ```toml
   [package]
   namespace = "acme"
   name      = "widgets"

   [package.metadata.huxerui]
   namespace = "acme::widgets"

   [lib]
   path = "modules/widgets.cppm"
   ```

3. Create `modules/widgets.cppm` as an empty file; mcpp requires the `[lib]` path to exist before the build writes it.
4. Build in the library root with `mcpp build`. The first build reports `generated modules/widgets.cppm from the public headers (<n> names)`. Commit the file and do not edit it: later builds rewrite it whenever the headers change.
5. Resolve the build's macro warnings as [Macros in the public headers](#macros-in-the-public-headers) describes.

The interface exports the names declared directly in the namespace: functions, classes, templates, enumerations, aliases, and variables.
Names in a nested namespace, such as `detail`, and names declared `static` are not exported; move a name a consumer needs into the namespace itself.
A consumer built with mcpp depends on the library and writes `import acme.widgets;`, and a consumer built with CMake keeps including the headers; both reach the same entities, so the library needs no second binary.
To write the interface by hand instead, set `cpp_modules_gen = false` under `[package.metadata.huxerui]`; the build then reports each public name the file does not export.
`huxerui doctor` in the library states the module an importer names.

### Macros in the public headers

A module carries no macro, so a macro reaches only the translation units that include the header defining it.
These rules cover the macros of the public headers, which are part of the library's interface; macros in the library's own sources, private headers, and module units are unrestricted.

- Prefer a language entity to a macro in the interface: a constant as `inline constexpr`, a type as `using`, a computation as a `constexpr` function, and a scope or similar syntax through `[[huxerui::composable]]` or a lambda. Declared in the namespace, it is exported like any other name.
- Keep a macro only where a macro is required, such as a version or feature test that consumers evaluate with `#if`.
- To offer such a macro to importers, map it by hand in `modules/macros.cppm`, the unit `<module>.macros`: a constant with the macro's own name in `<namespace>::macros`. Map only the macros importers need; there is no generated mapping. The build compiles the units below `modules/`, and the generated interface re-exports those named under the module, so `import <module>;` reaches the constants.
- `#undef` a helper macro at the end of the header that defines it; the build does not report it.
- In a translation unit that both includes the header and imports the module, write the macro, not `<namespace>::macros::<name>`: the defined macro expands inside the qualified name.

The macros unit reads the values while the macros are defined, then undefines them, because a macro defined in the global module fragment stays defined to the end of the unit:

```cpp
module;

#include <acme/widgets/version.h>

namespace acme_widgets_macros_capture {
inline constexpr auto value_ACME_WIDGETS_VERSION = ACME_WIDGETS_VERSION;
} // namespace acme_widgets_macros_capture

#undef ACME_WIDGETS_VERSION

export module acme.widgets.macros;

export namespace acme::widgets::macros {
inline constexpr auto ACME_WIDGETS_VERSION = acme_widgets_macros_capture::value_ACME_WIDGETS_VERSION;
} // namespace acme::widgets::macros
```

A macro the unit names is no longer reported. With `cpp_modules_gen = false`, the hand-written interface writes `export import <module>.macros;` itself, and the build reports its absence.

## Build and verification

```bash
huxerui build <platform>
huxerui run <platform>
mcpp test
```

Reuse the project's selected toolchain. Do not install a fixed compiler version or change the user's default toolchain merely because this reference includes mcpp. Inspect the installed command help and package configuration when selecting targets, workspace builds, tests, or packaging.

Use the resolved package's public interface and user documentation for HuxerUI APIs. A source checkout is not required beside an application, and relative paths into the framework repository are not portable skill references.

For manifest or build-program syntax, consult the documentation matching the installed mcpp version:

- [mcpp documentation](https://github.com/mcpp-community/mcpp/tree/main/docs)
- [Manifest](https://github.com/mcpp-community/mcpp/blob/main/docs/04-mcpp-toml.md)
- [Dependencies](https://github.com/mcpp-community/mcpp/blob/main/docs/05-dependencies.md)
- [Testing](https://github.com/mcpp-community/mcpp/blob/main/docs/08-testing.md)
- [Build programs](https://github.com/mcpp-community/mcpp/blob/main/docs/30-build-mcpp.md)

The default-branch links are discovery entry points, not proof that an older installed version accepts the same syntax.
