# Getting Started

Start with an [installed HuxerUI SDK](installation.md) and check the [platform requirements](platforms.md) for the targets you plan to build. For framework development, see [Building HuxerUI](../development/building.md).

## Create a project

```bash
huxerui create app hello_huxer --id dev.example.hello --platform windows,macos,linux,web,android,ios
cd hello_huxer
```

The command creates `src/app.cpp`, the resource directories, the selected shells under `platform`, and the application-development Skill. The CMake scaffold enables all six shells by default. Use a subset with `--platform` and add another shell later with `huxerui platform add <platform-list>`.

Use `--agent` to select where the SDK copies the Skill. To enable the Claude and ZCode directories, pass `--agent claude,zcode` when creating the project.

`codex`, `antigravity`, `opencode`, `command-code`, `omp`, and `dsh` share `.agents/skills`. `claude` uses `.claude/skills` and `zcode` uses `.zcode/skills`. The default is `codex`. Use `all` for all three directories or `none` to omit the Skill.

For a desktop C++ modules project, use `--build mcpp` and omit `--platform`. See [C++ Modules and mcpp](../../skills/huxerui-app-development/references/cpp-modules-and-mcpp.md).

## Diagnose the environment

```bash
huxerui doctor
huxerui doctor web,android
```

`doctor` reports SDK, project, and tool readiness without changing the machine. With no platform list it checks the common environment and, inside a project, the enabled targets available on the host. `--operation` defaults to `build`; select `run`, `devices`, or `package` to check a different workflow.

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

Mark a reusable function `[[huxerui::composable]]` when it directly calls a composition-bound `UseXxx()` function.
Composable code generation is enabled by the generated CMake project.

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
| Build an mcpp project through the bundled CLI | `huxerui mcpp build [--project <dir>]` |

Use a comma-separated platform list or `all`. For `build`, `all` selects the enabled shells and an omitted list selects the host platform if enabled. Build intermediates live under `.huxerui`. For ABI selection, signing, and final artifacts, see [Packaging Applications](packaging.md).

The `huxerui mcpp build` command delegates to the installed `mcpp` executable. `--project` selects the directory containing `mcpp.toml` and defaults to the current directory.

Continue with [Core Concepts](core-concepts.md) and [Components and Input](components.md).
