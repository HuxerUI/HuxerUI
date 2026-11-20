# Getting Started

Start with an [installed HuxerUI SDK](installation.md) and check the [platform requirements](platforms.md) for the targets you plan to build. For framework development, see [Building HuxerUI](../development/building.md).

## Create a project

```bash
huxerui create app hello_huxer --id dev.example.hello --platform windows,macos,linux,web,android,ios
cd hello_huxer
```

The command creates `src/app.cpp`, the resource directories, the selected shells under `platform`, and the application-development Skill. The CMake scaffold enables all six shells by default. Use a subset with `--platform` and add another shell later with `huxerui platform add <platform-list>`.

Run in a terminal, `huxerui create` asks for whatever the command line leaves out: the kind and name, the build system, the template, the platforms, and the agents whose skill files to add. It confirms before writing anything, then names the command to run next.
`--yes`, or running without a terminal, applies the defaults instead, and the command prints the options that recreate the same project.

A project's source style follows its build systems:

| `--build-system` | Source style | Sources write |
| --- | --- | --- |
| `cmake` (an application's default) | headers | `#include <huxerui/huxerui.h>` |
| `mcpp` | C++ modules | `import huxerui;` in module units |
| `cmake,mcpp`, application | headers, with both declarations | `#include <huxerui/huxerui.h>` |
| `cmake,mcpp`, library (a library's default, recommended) | headers and C++ modules | headers; consumers include them or import the library's module |

Keep both for a library: other libraries and applications consume it, and with both declarations each of them can, whether it includes headers or imports C++ modules, while you maintain only the headers.
An existing headers library gains the module interface without changing its sources, as [Add C++ modules to an existing headers library](../../skills/huxerui-app-development/references/cpp-modules-and-mcpp.md#add-c-modules-to-an-existing-headers-library) describes.
Such a library is a module named after its package as well: the mcpp build generates its interface, `modules/<package name>.cppm`, from its public headers, so a consumer built with mcpp writes `import <module>;` and one built with CMake includes the headers. Set `cpp_modules_gen = false` under `[package.metadata.huxerui]` to write that file yourself; the build then reports the names it leaves out.

```bash
huxerui create app hello_modules --build-system mcpp
huxerui create app hello_both --build-system cmake,mcpp
```

`--template` selects a C++ modules application's content, `app` (the default) or `navigation`.
`build`, `run`, and `package` are the same commands for every style and build system.
A project with both declarations builds with CMake; add `--build-system mcpp` to a command to build it with mcpp instead.

Use `--agent` to select where the SDK copies the Skill. To enable the Claude and ZCode directories, pass `--agent claude,zcode` when creating the project.

`codex`, `antigravity`, `opencode`, `command-code`, `omp`, and `dsh` share `.agents/skills`. `claude` uses `.claude/skills` and `zcode` uses `.zcode/skills`. The default is `codex`. Use `all` for all three directories or `none` to omit the Skill.

See [C++ Modules and mcpp](../../skills/huxerui-app-development/references/cpp-modules-and-mcpp.md) for C++ modules projects.

## Diagnose the environment

```bash
huxerui doctor
huxerui doctor web,android
```

`doctor` reports SDK, project, and tool readiness without changing the machine. With no platform list it checks the common environment and, inside a project, the enabled targets available on the host. `--operation` defaults to `build`; select `run`, `devices`, or `package` to check a different workflow.
For a library it also states whether importers can `import` it as a C++ module.

For supported downloadable prerequisites:

```bash
huxerui setup web,android
```

Use `huxerui doctor android --json` to get the read-only installation plan as JSON. Both commands accept `--sdk <path>` and the applicable `--generator`, `--abi`, `--java-home`, `--device`, or `--format` selectors. For example, `huxerui doctor linux --operation package --format deb` checks tools for DEB packaging.

`setup` rechecks the environment after each installation and reuses compatible tools on subsequent runs. `--yes` confirms the HuxerUI plan. Vendor tools still handle license and privilege prompts. Both commands return `0` when ready, `1` for an incomplete check or failure, and `2` for invalid arguments. Setup also returns `3` for cancellation and `4` when a restart is required.

## Build and run

Choose a target available on your host. For example, on Windows you can build or launch the native application or run the Web target.

```bash
huxerui build windows
huxerui run windows
huxerui run web
```

Use `huxerui run <platform> --watch` to rebuild after edits and relaunch the application or refresh the Web page. A build failure leaves the current instance running. Each successful reload starts a new application session. Press Ctrl+C to end the watch session.

`build` and `run` default to `debug`. Use `--config release` for a release build or [package the application](packaging.md) to produce distributable output. `--generator <name>` selects a compatible CMake generator for macOS, Linux, or Web. Desktop builds honor `CMAKE_BUILD_PARALLEL_LEVEL`.

### Select the build system

A project's build system is identified by its native declaration: `CMakeLists.txt`, or `mcpp.toml` with `build.mcpp`.
`build`, `run`, and `package` dispatch to that integration, so the same commands drive a CMake project and an mcpp project.
A project that declares both builds with CMake and says so before it starts; `--build-system mcpp` selects mcpp for one command.
For an mcpp project, desktop platforms and the Web build with `mcpp build`, run with `mcpp run`, and package with `mcpp pack`; Android and iOS build, run, and package through the same Gradle and Xcode shells as a CMake project, and `--no-shell` packages them with `mcpp pack` instead. Packages are published below `dist/<platform>` either way.
An mcpp project takes HuxerUI from the `huxerui` dependency in `mcpp.toml`, so `--sdk` is a usage error there. `--optimization` and `--lto` select the release profiles `huxerui create` declares, `--profile` opens the framework's profiling feature, and `--abi`, `--device`, and `--java-home` apply to the shells.
`--generator` steers only the CMake driver; for an mcpp project the CLI reports it with a warning and does not apply it.
mcpp provisions its own compilers and libraries, so it needs nothing from the host but itself: install it with `xlings install mcpp -y`, as its [installation guide](https://github.com/mcpp-community/mcpp#install) describes. `huxerui doctor` checks the build system a project builds with and lists what is missing.
Capabilities that only mcpp offers are used through `mcpp` directly.

### Select an SDK for one invocation

Use `--sdk <path>` to try another installed SDK or build against a source checkout. The selection applies to that invocation and its child processes. CMake caches are separate for each framework location.

```bash
huxerui run windows --sdk ../HuxerUI
```

Without `--sdk`, the CLI uses the installed SDK selected by `HUXERUI_HOME` or located next to the running CLI. Direct CMake builds of generated projects can select either layout with `-DHUXERUI_HOME=<path>`. Use a separate build directory for each location. To collect [Runtime profiling](../development/building.md#runtime-profiling), pass `--profile` with an explicit source checkout.

### Select an Android or iOS device

List destinations before selecting a device for `run`.

```bash
huxerui devices android
huxerui devices ios
huxerui run android --device <id>
huxerui run ios --device <id>
```

Desktop hosts use a ready Android device or emulator. Supply `--device` when several devices are ready. Pass `--java-home <path>` to select the JDK for the Gradle build. On Termux, `run android` opens the APK in the system installer on the local device. iOS can target a booted Simulator or a paired physical device. A plain `build ios` uses the generic Simulator destination.

### Open a platform project in an IDE

Use the CLI to open the generated iOS project.

```bash
huxerui open ios
```

For direct Android Studio builds, set `HUXERUI_HOME` in the IDE's environment. For direct Xcode builds, set it in the environment or an explicit build setting such as `Config/Local.xcconfig`. After changing library dependencies, run `huxerui build android`, `huxerui build ios`, or `huxerui open ios` to refresh platform integration before building in the IDE.

## Application source

The application root is an ordinary function returning `View`.
The root already owns a composition scope.

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("Increment").OnClick([count] {
      count += 1;
    }),
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
}

View App() {
  return MaterialTheme {
    Counter(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "Counter",
            .initial_size = {480.0F, 320.0F},
            .minimum_size = Size{320.0F, 240.0F},
        },
    }
};
```

In a C++ modules project the same application is a module unit, `src/app.cppm`, and each desktop platform's `main.cpp` holds the process entry:

```cpp
export module app;

import std;
import huxerui;

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("Increment").OnClick([count] {
      count += 1;
    }),
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
}

View App() {
  return MaterialTheme {
    Counter(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "Counter",
            .initial_size = {480.0F, 320.0F},
            .minimum_size = Size{320.0F, 240.0F},
        },
    }
};
```

`import std;` makes the standard-library declarations that HuxerUI's templates instantiate visible to the module unit.

Mark a reusable function `[[huxerui::composable]]` when it directly calls a composition-bound `UseXxx()` function.
Composable code generation is enabled by the generated project, under either build system.

## Resources

The scaffold loads typed keys from its `resources` directory. See [Resources and Images](resources.md) for package structure, localization, raw assets, and image editing.

## Libraries

Use `huxerui create library <name>` to scaffold a common C++ library. Its `examples/preview` application receives all platform shells by default. Pass `--platform` to scaffold selected platform integration and the corresponding preview shells. Commands such as `huxerui run <platform>` resolve to the preview when run from the library root.

Use `--namespace` and `--target` to override the name-derived C++ namespace and public CMake target. For example, `--namespace scave::camera --target Scave::Camera` generates `<scave/camera.h>`, `scave::camera::Install`, and the public target `Scave::Camera`.

Pass the dependency's public CMake target to `huxerui_use_library`. The consumer can be an application or another library.

```cmake
huxerui_use_library(my_app
        TARGET CameraKit::CameraKit
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/libraries/CameraKit"
)
```

For a Git dependency, use an HTTPS `URL` and either `COMMIT` with a full SHA or `TAG` with a Git tag. For an already declared CMake target, use `TARGET` alone. Linkage defaults to `PRIVATE`. Use `PUBLIC` when the consumer's public headers expose the dependency.

Keep library targets, aliases, and `huxerui_use_library()` declarations available during `HUXERUI_LIBRARY_GRAPH_ONLY`. Put compilation and platform dependency discovery after that boundary. The CLI uses the declaration-only graph to attach Android Gradle projects and iOS Swift products. An iOS package lives at `platform/ios/<Swift product>/`, such as `platform/ios/Camera/` for `HuxerUI::Camera`.

Use the [library integration design](../design/sdk-cli.md#libraries-and-platform-integration) for target naming and platform package rules, and [Extending HuxerUI](extending.md) for the runtime extension APIs.

## Project commands

Run `huxerui --help` for the command list and supported options. `build`, `run`, `package`, and `open` also accept `--help` immediately after the command. `b`, `r`, and `pkg` are aliases for `build`, `run`, and `package`.

| Task | Command |
| --- | --- |
| Create a project or add a platform shell | `huxerui create app <name>`, `huxerui create library <name>`, `huxerui platform add <platform-list>` |
| Check or prepare a toolchain | `huxerui doctor [platform-list]`, `huxerui setup <platform-list>` |
| Discover destinations | `huxerui devices [platform]` |
| Build, launch, or distribute an application | `huxerui build [platform-list]`, `huxerui run <platform>`, `huxerui package <platform-list>` |
| Open an iOS project | `huxerui open ios` |

Use a comma-separated platform list or `all`. For `build`, `all` selects the enabled shells and an omitted list selects the host platform if enabled. Build intermediates live under `.huxerui`. For ABI selection, signing, and final artifacts, see [Packaging Applications](packaging.md).

`build`, `run`, and `package` reach the project's build system; `--build-system` selects one when the project declares several.

Continue with [Core Concepts](core-concepts.md) and [Components and Input](components.md).
