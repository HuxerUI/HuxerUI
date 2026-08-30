# Project Workflow

Use the actual installed CLI and generated project as the contract. CLI arguments can change between SDK versions.

## Locate the tools and SDK

For an existing configured project, read `HuxerUI_DIR` in the active `CMakeCache.txt` and prefer that package over `HUXERUI_HOME` or another CLI installation.

When the task creates, configures, builds, runs, or diagnoses a project:

1. Identify the `huxerui` executable selected by the shell.
2. Run `huxerui --version` and inspect `huxerui --help` before relying on its commands.
3. Run `huxerui doctor` for setup or environment diagnosis, scoped to the affected platform when possible.
4. Try the relevant subcommand help before use. If that CLI does not implement subcommand `--help`, rely on its top-level usage and an isolated generated project rather than guessing flags.

The packaged CLI exposes these top-level forms at the time this Skill is distributed:

```text
huxerui create app <name> [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>]
huxerui create library <name> [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>]
huxerui platform add <platform-list>
huxerui doctor [platform-list]
huxerui setup <platform-list> [--yes]
huxerui devices [platform]
huxerui build [platform-list] [--device <id>] [--profile debug|release] [--generator <name>] [--source <path>]
huxerui run <platform> [--device <id>] [--profile debug|release] [--generator <name>] [--source <path>]
huxerui package <platform-list> [--device <id>] [--profile debug|release] [--generator <name>] [--source <path>]
huxerui open ios [--source <path>]
```

Re-read current help before copying these commands.

The CLI copies this Skill into new projects by default.
`--agent` accepts `codex`, `claude`, `antigravity`, `opencode`, `command-code`, `omp`, `dsh`, and `zcode`; `all` selects every distinct supported Skill directory, while `none` omits the Skill.
An explicit list replaces the default `codex` selection.

## Create a minimal application

Choose the name, project identifier, and platforms from the user's request or current CLI defaults. Do not add unrequested platforms.

```powershell
huxerui doctor windows
huxerui create app SampleApp --id org.example.sampleapp --platform windows
Set-Location SampleApp
huxerui build windows --profile debug
huxerui run windows --profile debug
```

Use `--source <path>` only when the user explicitly wants the application to compile HuxerUI from a local source checkout instead of the installed SDK binaries.
The path must name the HuxerUI repository root, and the override applies only to that `build`, `run`, `package`, or `open ios` invocation; do not rewrite the user's persistent `HUXERUI_HOME` for this workflow.

On another host, replace `windows` with an available requested platform. Do not claim a platform ran unless it did.

The generated application has this common shape; each requested platform contributes its own subtree:

```text
SampleApp/
  CMakeLists.txt
  src/
    app.cpp
  resources/
    images/
    raw/
    strings/
      default.properties
  platform/
    <selected-platform>/
  <selected-skill-root>/
    huxerui-app-development/
```

The selected Skill root is `.agents/skills` by default and may instead or additionally be `.claude/skills` or `.zcode/skills` according to `--agent`. The root CMake file recursively discovers C++ sources under `src`, calls `huxerui_add_app`, registers `resources` under namespace `app`, and enables composable code generation through that helper.

A generated library has its public header and implementation at the root, plus a normal consumer application under `examples/preview`:

```text
LibraryName/
  CMakeLists.txt
  include/<target>/<target>.h
  src/<target>.cpp
  resources/
    images/
    raw/
    strings/default.properties
  platform/<selected-platform>/
  examples/preview/
    CMakeLists.txt
    src/app.cpp
    resources/
      images/
      raw/
      strings/default.properties
    platform/<selected-platform>/
  <selected-skill-root>/huxerui-app-development/
```

The library root uses `huxerui_add_library`; its preview consumes that target through `huxerui_use_library`. Some platform package subtrees are present only when the selected platform needs them.

Inspect the current generated result because this layout can evolve. Do not hand-create a stale replacement when the CLI is available.

## Understand an existing application

Before the smallest safe edit:

1. Read root and nested `CMakeLists.txt` files, relevant CLI configuration, `src`, `resources`, and only the platform configuration involved in the task.
2. Identify the application target and whether it uses `huxerui_add_app`, `HuxerUI::huxerui`, or `HuxerUI::huxerui_static`.
3. Locate the active build directory and its `HuxerUI_DIR`, generator, compiler, configuration, and platform options.
4. Identify the application root, `Application` declaration, theme boundary, state ownership, codegen-enabled sources, resource namespaces, and app-side libraries.
5. Preserve the existing generated structure. Do not regenerate or scaffold over the project to make a small edit.

## Public CMake contract

Installed SDK consumption uses:

```cmake
find_package(HuxerUI CONFIG REQUIRED)
```

The package can expose `HuxerUI::huxerui` and `HuxerUI::huxerui_static`. Android selects shared, Web and iOS select static, and desktop packages can expose both. Prefer `huxerui_add_app` for generated applications because it selects the platform-appropriate framework target, enables C++20/codegen, and owns app resource integration.

The macOS SDK carries the iOS device and Simulator static slices in `share/huxerui/platform/ios/HuxerUI.xcframework`.
Installed iOS projects consume the selected slice through the canonical HuxerUI CMake target; do not link an XCFramework archive path directly.

For manually defined consumer targets, call `huxerui_enable_codegen(target)` after all marked source files are added. Do not include SDK-private paths or link binary files by hand when an exported target owns them.

## Build and run without changing toolchains

- Reuse the project's compatible build directory and generator.
- On Windows, keep the existing MSVC generator; do not switch to MinGW or pin a Visual Studio release without a project requirement.
- Build only the current host and platforms affected by the task by default.
- Preserve existing Android ABI, NDK, Gradle, iOS Xcode, Web Emscripten, and desktop settings.
- Use `huxerui devices` when a platform needs a device selection.
- On Termux, Android and Web use the current device directly: do not pass `--device` or add ADB. Web uses the CLI-planned Python loopback server and opens its URL through `termux-open`.
- A Termux Android run opens the APK in the system installer; complete installation and choose Open there.

## Diagnose by phase

Classify a failure before changing code:

- SDK discovery: `HUXERUI_HOME`, `HuxerUI_DIR`, version, package architecture.
- CLI setup: doctor result and platform tools.
- Project planning or platform shell generation.
- CMake configure and target selection.
- composable code generation.
- resource compilation or staging.
- C++ compilation.
- linking and runtime binary deployment.
- platform launch, device, browser, or host integration.

Report the exact failing phase and command. Never use a source checkout as an implicit dependency unless the existing project explicitly does so.
