# Getting Started

This guide uses an installed HuxerUI SDK.
Repository contributors should use [Building HuxerUI](../development/building.md).

## Create a project

```bash
huxerui create app hello_huxer --id dev.example.hello --platform windows,macos,linux,web,android,ios
cd hello_huxer
```

The command creates shared sources under `src`, resources under `resources`, the requested platform shells, and the HuxerUI application-development Skill under `.agents/skills`.
Only build platforms supported by the current development host.

Use `--agent` to select a different Agent Skill directory:

```bash
huxerui create app hello_huxer --platform windows --agent claude,zcode
```

`codex`, `antigravity`, `opencode`, `command-code`, `omp`, and `dsh` share `.agents/skills`; `claude` uses `.claude/skills`; and `zcode` uses `.zcode/skills`.
The default is `codex`, `all` writes all three directories, and `none` omits the Skill.

Add another shell later with:

```bash
huxerui platform add web
```

A Library created without `--platform` remains a common C++ Library but includes an all-platform `examples/preview` application, so `huxerui run <platform>` from the Library tree runs that Preview on a compatible host.

## Diagnose the environment

```bash
huxerui doctor
huxerui doctor web,android
```

`doctor` reports the selected SDK, project configuration, compiler, platform tools, and actionable missing prerequisites without changing the machine.

For supported downloadable prerequisites:

```bash
huxerui setup web,android
```

## Build and run

```bash
huxerui build windows
huxerui run windows
huxerui run web
```

Use `--profile release` for a release configuration.
Use `--generator <name>` only when the host has multiple compatible CMake generators and an explicit selection is required.
Use `--source <path>` to compile HuxerUI from one explicit source checkout for that build instead of consuming the installed SDK binaries:

```bash
huxerui run windows --source ../HuxerUI
```

The source override applies only to that CLI process and its build children; it does not replace the configured `HUXERUI_HOME` in the parent shell. It works without an installed SDK. Without `--source`, the CLI requires an installed SDK; setting `HUXERUI_HOME` to a source checkout does not implicitly select source mode.

Direct CMake builds of generated projects use `-DHUXERUI_HOME=<path>`. CMake loads the source checkout or installed package at that location. Use a separate build directory for each framework location; the CLI does this automatically.

The CLI passes the selected framework home through its process environment and build arguments without writing it into platform configuration files. Direct Android Studio builds require `HUXERUI_HOME` in the IDE's environment; direct Xcode builds require it in the IDE's environment or explicit build settings, such as `Config/Local.xcconfig`. An already running IDE does not inherit the CLI's temporary environment or remember its last `--source` selection.

Android and iOS accept a device selected from:

```bash
huxerui devices android
huxerui devices ios
huxerui run android --device <id>
huxerui run ios --device <id>
```

Open the generated iOS project with:

```bash
huxerui open ios
```

Android and iOS commands first resolve the CMake library declarations so Gradle and Xcode can attach their platform packages before compiling. This graph-only configure does not require a host C++ compiler or host HuxerUI binaries. Keep library declarations and aliases available under `HUXERUI_LIBRARY_GRAPH_ONLY`; guard native target configuration and platform dependency discovery with `if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)`. The generated library template already separates these operations. After changing library dependencies, run the CLI build command (or `huxerui open ios`) to refresh platform integration before building directly in the IDE.

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

Place application resources in the generated resource tree:

```text
resources/
  images/
    logo.png
    logo@2x.png
    mark.svg
  raw/
    config.json
  strings/
    default.properties
    zh.properties
```

The build generates typed resource identifiers in `app_resources.h` and packages framework and application resources together.
Raster density variants share one logical size; SVG resources are compiled to platform-neutral vector data.
Static SVG resources may use paths and basic shapes, groups, file-local `defs`/`use` references, one-path `clipPath` geometry, solid or linear/radial gradient fills and strokes, transforms, and root `preserveAspectRatio` mapping.
Gradients support local inheritance, object-bounds or user-space coordinates, `gradientTransform`, stop and paint opacity, and pad extension.
The compiler rejects browser-dependent SVG features such as text, scripts, external styles, invalid gradient transforms, masks, filters, animation, and external references instead of approximating them differently on each renderer.

```cpp
#include <app_resources.h>

return Column {
  Image(app::images::logo),
  Text(app::strings::welcome),
};
```

## Project commands

```text
huxerui create app <name> [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>]
huxerui create library <name> [--namespace <cpp-namespace>] [--target <public-cmake-target>] [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>]
huxerui platform add <platform-list>
huxerui doctor [platform-list]
huxerui setup <platform-list> [--yes]
huxerui devices [platform]
huxerui build [platform-list] [--device <id>] [--profile debug|release] [--generator <name>] [--source <path>] [--java-home <path>]
huxerui run <platform> [--device <id>] [--profile debug|release] [--generator <name>] [--source <path>] [--java-home <path>]
huxerui package <platform-list> [--device <id>] [--profile debug|release] [--generator <name>] [--source <path>] [--java-home <path>]
huxerui open ios [--source <path>]
```

Build outputs stay outside the source tree under the project-owned `.huxerui` directory.
Packaged application artifacts are collected under `dist/<platform>`.
Desktop packages are a Windows setup executable, macOS DMG, or Linux AppImage; see [Packaging Applications](packaging.md) for runtime payload and custom Windows installer guidance.
Android builds accept `--java-home <path>` to use that JDK for the current CLI invocation without changing the shell or generated Gradle project.
The generated `CMakeLists.txt` keeps source discovery, target creation, resources, and library dependencies explicit.
Its sibling `HuxerUIProject.cmake` contains the generated project plan, SDK discovery, and platform-shell connection details.

On Android, the application library defaults to `lib<cmake-target>.so` and links the shared `libhuxerui.so` framework.
The generated Gradle shell resolves the final CMake artifact name into `BuildConfig.HUXERUI_APP_LIBRARY`, which MainActivity passes to `System.loadLibrary`; changing `OUTPUT_NAME` or a configuration postfix does not require a second library-name setting in Gradle.
All packaged ABIs must use the same application library name, and that name must not be `huxerui`.
Custom Android hosts must load their application library before creating a HuxerUI Runtime; HuxerUIActivity does not load application code on their behalf.
Existing source-controlled shells are not rewritten by SDK updates: migrate their Gradle integration and MainActivity loading code together when adopting this contract.

Library names remain repository and display identities.
`--namespace` selects the exact generated C++ namespace, while `--target` selects an unqualified or single-package-qualified public CMake target.
Package prefixes such as `HuxerUI::` are allowed; the generated targets must not collide with existing CMake targets.
For example, `--namespace scave::camera --target Scave::Camera` generates `<scave/camera.h>`, `scave::camera::Install`, and the consumer target `Scave::Camera`.
When omitted, both values retain the name-derived defaults.

Continue with [Core Concepts](core-concepts.md) and [Components and Input](components.md).
