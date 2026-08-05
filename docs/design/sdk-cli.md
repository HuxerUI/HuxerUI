# SDK, CLI, Native Shell, and Module Design

This document defines the HuxerUI SDK and CLI ownership model, the implemented project workflow, and the extension boundary for native modules and future platforms.

## Status

The current implementation provides:

- An installable platform-specific CMake package with canonical `HuxerUI::huxerui` and `HuxerUI::huxerui_static` targets.
- `huxerui_add_app`, installed host code generators, and generated application integration metadata.
- A `huxerui` CLI with `create`, `platform add`, `doctor`, `devices`, `build`, `run`, and `open ios`.
- Source-controlled Android, iOS, Windows, macOS, and Web shell templates.
- Source-SDK Android and Web integration, source- or installed-SDK iOS integration, and installed-SDK Windows and macOS builds.
- Android and iOS device discovery with deterministic device selection.

Android binary distribution, `package` and `clean` commands, module integration, NativeView, iOS device distribution, OHOS, and Linux remain proposed.
The current Android and Web CLI paths require a source SDK checkout. iOS can consume a locally installed compatible SDK, but versioned distribution archives and export automation are not implemented.

## Decisions

- Applications and modules do not use a HuxerUI-specific manifest.
- CMake owns common C++ sources, resources, targets, and the future module dependency graph.
- Source-controlled `platform/<platform-id>` shells own native lifecycle, packaging, signing, and platform-only configuration.
- `.huxerui` contains only reproducible generated metadata and native incremental build output.
- The CLI orchestrates CMake and native tools; it does not replace Gradle, Xcode, Emscripten, or another platform build system.
- Platform drivers are private CLI implementation, not a public plugin ABI.
- Runtime ownership remains one shared `Runtime` and one `PlatformAdapter` per application surface.

These decisions keep direct CMake use viable and prevent a second project model from drifting away from native tools.

## Ownership

```text
application repository
  common C++ and resources
  root CMake project
  source-controlled platform shells

HuxerUI CLI
  project and shell creation
  SDK and tool discovery
  platform validation
  platform build and launch orchestration

HuxerUI SDK
  public headers and libraries
  CMake package and application helpers
  host code generators
  platform integrations and shell templates

native toolchains
  compiler and CMake
  Gradle, Android SDK, NDK, and ADB
  Xcode and Apple tooling
  future platform-native tools
```

The CLI does not introduce another Runtime, platform-specific application definition, or native host hierarchy.

## Application project

A generated project has this shape:

```text
hello_huxer/
  .gitignore
  CMakeLists.txt
  src/main.cpp
  assets/
    images/
    raw/
    strings/default.properties
  platform/
    android/
      .gitignore
      settings.gradle
      build.gradle
      huxerui.cmake
      app/
    ios/
      .gitignore
      App/
        main.mm
        Info.plist
        LaunchScreen.storyboard
        Assets.xcassets/
      Config/
        Base.xcconfig
        Debug.xcconfig
        Release.xcconfig
        Local.xcconfig.example
      hello_huxer.xcodeproj/
    windows/
      huxerui.cmake
      app.manifest
    macos/
      huxerui.cmake
      Info.plist.in
    web/
      huxerui.cmake
      index.html.in
  .huxerui/
```

The common files and platform shells are application source and belong in version control.
The CLI never overwrites an existing shell during `platform add` or an ordinary build.

The root `.gitignore` owns only repository-wide generated state:

```gitignore
/.huxerui/
/dist/
/build/
/cmake-build-*/
/.cache/
```

Each platform shell owns its native ignore rules.
Android ignores Gradle and native intermediates inside `platform/android`, while Apple and future platforms keep their own IDE and package-manager state local to their shell.

The application entry remains shared:

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

View App() {
  return MaterialTheme([] {
    return Text("Hello, HuxerUI");
  });
}

HUXERUI_APP(App, {})
```

Native executable targets generate the process entry point.
Hosted platforms such as Android and Web register the immutable application definition consumed by their platform shell.

## CMake SDK

Installed applications use the standard package entry point:

```cmake
find_package(HuxerUI CONFIG REQUIRED)

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

huxerui_add_app(hello_huxer
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            assets
        RESOURCE_NAMESPACE
            app
)
```

`huxerui_add_app`:

- Creates an executable, application bundle, or Android application library for the active platform.
- Links a canonical HuxerUI target.
- Enables scope code generation after all declared sources are known.
- Registers the optional resource root.
- Applies bundle metadata supplied by the application.
- Emits the minimal application artifact plan consumed by CLI launch commands.

Advanced consumers may still create targets directly and call `huxerui_enable_codegen` and `huxerui_add_resources`.

The installed package contains public headers, platform libraries, CMake helpers, the CLI, and host code generators.
Host tools are selected from `share/huxerui/tools/<host>/<architecture>` and always run on the development host, independently of the target architecture.

## Generated integration

Generated files are projections, not another source of truth.
The shared application helper emits an application plan containing the target, platform identifier, artifact path, bundle path, and bundle identifier needed by native launch commands.

Android additionally generates `.huxerui/generated/android/app.json` from the application-owned `platform/android/huxerui.cmake` and canonical SDK Android properties.
The Gradle shell reads that file to obtain:

- Application identifier and SDK levels.
- NDK, STL, and ABI constraints.
- The source SDK Gradle module.
- CMake helper and host-tool roots.

Deleting `.huxerui/generated` and configuring again must reproduce the same values.
The CLI never parses `CMakeLists.txt` as source text.

## CLI surface

The implemented command surface is:

```text
huxerui create <name> [-p|--platform <platform-list>]
huxerui platform add <platform-list>
huxerui doctor [platform-list]
huxerui devices [platform]
huxerui build [platform-list] [--device <id>] [--profile debug|release] [--generator <name>]
huxerui run <platform> [--device <id>] [--profile debug|release] [--generator <name>]
huxerui open ios
```

A platform list is comma-separated or `all`.
`all` means every platform driver known to the current CLI for `create` and every enabled application platform for project commands.

### Create and platform add

```bash
huxerui create hello_huxer --platform android,windows
cd hello_huxer
huxerui platform add macos
```

Creation writes the common CMake project and complete minimal shells for the selected platforms.
The generated project recursively collects C++ sources under `src`, so adding a source file does not require a platform-specific CMake edit.
It also creates the `assets/images`, `assets/raw`, and `assets/strings` resource categories.
It uses a temporary tree and publishes the project only after every file succeeds.
`platform add` similarly refuses to overwrite an existing platform directory and rolls back directories created by a failed multi-platform operation.

### Doctor

`doctor` is read-only.
Outside a project it reports the SDK, common tools, available drivers, and any explicitly requested platform tools.
Inside a project it also validates required common files, unknown platform directories, shell contents, current-host support, and required native tools.

Checks are scoped to requested platforms.
A missing Android toolchain does not make a Windows-only diagnostic fail.

### Devices

Device discovery does not require a project.
The Android driver parses `adb devices -l` and preserves ready, offline, unauthorized, and unavailable states. The iOS driver combines paired physical devices from `devicectl` with booted Simulators from `simctl` and retains the selected device kind through build and launch.
Desktop drivers do not expose synthetic devices.

### Build

Builds retain native output below `.huxerui/build/<platform>/<profile>`. iOS uses explicit `ios-simulator` and `ios-device` DerivedData roots so Simulator and device products, intermediates, architectures, and signing state never share one directory.
The CLI validates the complete requested set before executing commands and prints each native command.

Desktop builds configure the root CMake project and then build it.
Fresh desktop builds use Ninja when it is available unless an explicit generator, `CMAKE_GENERATOR`, or an existing CMake cache takes precedence.

iOS builds invoke the source-controlled native Xcode project. A build without `--device` uses the generic Simulator destination; selecting a Simulator or physical device uses its Xcode destination identifier, and a physical-device build allows automatic provisioning updates. The native App target invokes CMake to produce a destination-specific application core and links it into the bundle.

Android builds first execute `HuxerUIAndroidPlan.cmake`, then invoke the source-controlled Gradle shell.
The shell uses its local `gradlew` or `gradlew.bat` when present and otherwise requires `gradle` on `PATH`.
The current CLI does not generate or download Gradle wrapper binaries.

Web builds use `emcmake` to configure the same root CMake project and produce the ES module, WebAssembly module, and project-owned HTML entry point.
The Web shell owns the HTML document and Canvas mount code rather than hiding them in the SDK.

### Run

`run` accepts exactly one enabled platform and performs a build before launch.
Windows starts the executable, macOS opens the application bundle, Android installs and launches the generated APK, iOS uses `simctl` for a selected booted Simulator and `devicectl` for a paired physical device, and Web delegates the generated HTML entry point to `emrun`.

For Android, one ready device is selected automatically.
Multiple ready devices require `--device <id>`, and an explicit device must exist and be ready before building.

## SDK selection and distribution

The CLI resolves a source or installed SDK in this order:

- `HUXERUI_SDK_ROOT` when explicitly set.
- A valid SDK prefix relative to the running CLI executable.

A source root contains the repository CMake helpers and public headers.
An installed root contains public headers and a standard `HuxerUIConfig.cmake` under a portable CMake install location.

The current installed SDK supports desktop consumers and iOS consumers when the package matches the selected Apple SDK and architectures.
Android applications currently use a source SDK because the generated Gradle shell includes `platform/android/huxerui` directly.
Web applications use a source SDK so the framework and application are compiled together by Emscripten rather than linking a native installed library.
The iOS preview accepts a source checkout or a compatible installed prefix. Versioned Simulator and device artifacts, archive export, and distribution signing policy still require an explicit distribution design.
An installed Android artifact and its version-selection policy must be designed and validated before this restriction is removed.

No `sdk.json` is required: standard CMake package files describe desktop targets, while platform-specific facts remain in the platform integration that owns them.

## Platform drivers

The CLI core dispatches through one internal `PlatformDriver` interface.
A driver owns:

- Its stable platform identifier.
- Current-host capability.
- Required tools and shell diagnostics.
- Source-controlled shell templates.
- Build and run command construction.
- Optional device discovery.

The interface deliberately contains only capabilities implemented by the current command surface.
Package, clean, signing, and artifact collection operations should be added when those commands exist rather than anticipated as empty virtual methods.

The current registry contains Android, iOS, Windows, macOS, and Web.
The registry is compiled into the CLI; it is not a dynamic extension mechanism.
Adding a platform may split template storage or driver implementations when their size justifies it, but does not change project discovery or command parsing.

### Android

The shell is a Gradle application with an `app` module and the SDK source `HuxerUI` module.
Gradle owns Android packaging, manifest merging, SDK selection, native ABI variants, and APK output.
CMake owns the common application library and resource generation.

The project-level `huxerui.cmake` exposes only values shared with HuxerUI integration, such as the application identifier, SDK levels, NDK version, and ABIs.
It does not replace arbitrary Gradle configuration.

### Windows

The shell supplies an application manifest and optional native CMake inputs.
The root CMake project creates the executable and links the installed or source HuxerUI target.
The driver runs only on Windows.

### macOS

The shell supplies bundle metadata through `Info.plist.in` and optional native CMake inputs.
The root CMake project creates the application bundle.
The driver runs only on macOS.

### iOS

The shell is a source-controlled native Xcode application project. It owns the Info.plist, launch screen, asset catalog, build configurations, shared scheme, product identifier, signing, Capabilities, native sources, archive behavior, and final App Bundle.

On iOS, `HUXERUI_APP` exports the fixed application entry consumed by the shell's minimal Objective-C++ `main.mm`. `huxerui_add_app()` produces an application-core archive instead of another executable or App Bundle. CMake remains responsible for the common C++ sources, scope code generation, resource generation, and linking the selected installed or source HuxerUI static target. Xcode remains responsible for process entry, native resources, destination selection, signing, packaging, installation metadata, and debugging.

iOS has one application build path. Source-checkout development and a packaged SDK use the same application-core contract; only `HUXERUI_SDK_ROOT` resolution changes. The driver discovers paired devices and booted Simulators, invokes `xcodebuild`, installs through `devicectl` or `simctl`, and opens the checked-in project directly. Distribution export automation and public UIView embedding remain outside the current preview.

`huxerui open ios` writes the resolved SDK location only to the ignored local Xcode configuration. Repository examples use one source-controlled native runner whose `HUXERUI_APP_TARGET` build setting selects an `example_*` application core; adding an example does not add another Xcode project or native application target.

### Web

The shell supplies the browser-owned HTML document and Canvas mount code.
The driver wraps the existing Emscripten CMake backend with `emcmake`, retains incremental output under `.huxerui/build/web`, and uses `emrun` for local development.
It does not define a parallel JavaScript component system or expose browsers as synthetic devices.

## Modules and native integration

The remainder of this section is a design contract, not implemented CLI behavior.

A HuxerUI module is a compile-time CMake target that may also provide platform-native sources, dependencies, resources, permissions, typed services, or NativeView factories.
It is not a runtime plugin and does not require a universal public `Module` base class.

```text
huxerui-camera/
  CMakeLists.txt
  include/huxerui_camera/
  src/
  platform/
    android/
    macos/
    windows/
```

The ordinary CMake graph remains authoritative:

```cmake
add_subdirectory(modules/camera)
target_link_libraries(my_app PRIVATE HuxerUI::camera)
```

Installed modules use `find_package`, and Git modules use `FetchContent` with an immutable revision.
There is no second dependency list in a HuxerUI manifest.

A future module integration pipeline is:

```text
CMake target closure
  -> module platform directories
  -> versioned integration projection
  -> native shell integration
  -> native build system
```

Native dependencies remain expressed in their owning ecosystem.
Gradle dependencies are not flattened into generic CMake strings, and Apple or future package metadata remains native to those platforms.

Mergeable permissions may travel with a module, but application policy does not.
For example, a module may require camera capability while an application must still provide user-facing privacy text and signing-sensitive policy.

NativeView remains a real leaf View with Runtime-owned identity, reconciliation, measurement, layout, visibility, hit testing, focus, accessibility, and lifecycle.
Modules and platform shells provide typed factories and services without introducing Runtime subclasses or native types into shared public headers.

## Future commands and platforms

`package`, `clean`, SDK management, module plan generation, and artifact collection remain future work.
They should extend the existing ownership model:

- `package` invokes the source-controlled native shell's release path and collects user-facing output under `dist`.
- `clean` removes only driver-owned generated and build output.
- SDK selection uses standard CMake compatibility plus user-level tool selection, not project-local hidden state.
- iOS device distribution, OHOS, and Linux extend their native integration without changing the common application model.

No future command may silently skip an explicitly requested platform or claim an artifact that was not produced.

## Errors, security, and reproducibility

- Unknown platform identifiers are usage errors.
- Unknown or incomplete project shells identify the exact path and expected file.
- Unsupported host-target combinations fail before starting any build.
- Native process failures retain their command and exit code without hiding native logs.
- Process invocation passes argument arrays and does not route ordinary commands through a shell.
- Windows batch tools are handled explicitly and reject unsafe expansion characters.
- Generated metadata contains no credentials or signing keys.
- Git modules must use immutable release revisions for reproducible builds.
- A clean checkout plus the selected SDK and native toolchains reproduces generated integration metadata.

## Validation

The current workflow is covered by:

- CLI project, shell, diagnostics, device parsing, process execution, and command-construction tests.
- A CMake script test for Android plan defaults and compatibility rejection.
- An installed desktop consumer test that installs the SDK, runs the installed CLI, creates a project, and builds it without source-tree lookup.
- Existing common, header, code-generation, platform, and example builds.

Platform work must additionally validate every affected native toolchain available on the development host and report unavailable platforms explicitly.

## Invariants

- One common `HUXERUI_APP` definition.
- One shared Runtime implementation.
- One `PlatformAdapter` boundary per application surface.
- Public identity remains `huxerui`, `<huxerui/huxerui.h>`, and `HuxerUI::huxerui`.
- CMake owns common C++ and resource targets.
- Native shells own platform lifecycle, platform-only configuration, signing, and packaging.
- Native shells are source-controlled and built directly.
- Generated integration files are reproducible projections, not generated projects.
- Modules are compile-time units, not dynamically loaded plugins.
- Native services and commands use typed interfaces rather than a generic string channel.
