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

The source override applies only to that CLI process and its build children; it does not replace the configured `HUXERUI_HOME` in the parent shell.

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
Static SVG resources may use paths and basic shapes, groups, file-local `defs`/`use` references, one-path `clipPath` geometry, solid or linear/radial gradient fills, solid stroke styles, transforms, and root `preserveAspectRatio` mapping.
Gradients support local inheritance, object-bounds or user-space coordinates, stop opacity, and pad extension.
The compiler rejects browser-dependent SVG features such as text, scripts, external styles, gradient strokes or transforms, masks, filters, animation, and external references instead of approximating them differently on each renderer.

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
huxerui create library <name> [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>]
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
Android builds accept `--java-home <path>` to use that JDK for the current CLI invocation without changing the shell or generated Gradle project.

Continue with [Core Concepts](core-concepts.md) and [Components and Input](components.md).
