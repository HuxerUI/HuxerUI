# SDK, CLI, Platform Shell, and Module Design

This document defines the HuxerUI SDK and CLI ownership model, the implemented project workflow, and the extension boundary for platform modules and supported or future platforms.

## Status

The current implementation provides:

- An installable platform-specific CMake package with canonical `HuxerUI::huxerui` and `HuxerUI::huxerui_static` targets.
- `huxerui_add_app`, installed host code generators, and generated application integration metadata.
- A `huxerui` CLI with explicit application and module creation, `platform add`, `doctor`, `devices`, `build`, `run`, and `open ios`.
- Source-controlled Windows, macOS, Linux, Web, Android, and iOS platform enablement, with the Linux runtime backend built directly through CMake.
- Installed-SDK Windows and macOS builds, source-SDK Linux, Web, and Android integration, and source- or installed-SDK iOS integration.
- Android and iOS device discovery with deterministic device selection.
- Compile-time module targets, local and pinned HTTPS Git acquisition, predeclared-target consumption, ordered resource packages, common module scaffolding, an application-based module preview, Android Gradle library attachment, and iOS Swift Package aggregation and attachment.
- Direct Android root-CMake builds with Gradle-owned SDK, NDK, ABI, identifier, dependency, and packaging configuration.
- `HUXERUI_HOME` selection, CLI executable-relative self-discovery, child-process propagation, and relocatable Windows and macOS installed-SDK validation.

Versioned SDK distribution and installers, `package` and `clean` commands, production nonvisual modules, PlatformView hosting on Linux, ExternalTexture production and renderer frame import on Windows, iOS device distribution, and OHOS remain proposed. The shared `PlatformPayload`, its closed `ExternalTexture` capability kind, the platform-neutral `ExternalTexture` value, Image and paint integration, retained frame scheduling and damage, Apple `CVPixelBuffer`, Linux RGBA/BGRA, WebCodecs `VideoFrame`, and Android `Bitmap` sources and renderer frame import, nonvisual `PlatformInstance` protocol, low-level PlatformView leaf, placement command, unified registry and event routes, `RenderComposition` derivation, platform UI-thread dispatch, Windows child-HWND hosting with single-surface DirectComposition, macOS NSView hosting, Web HTMLElement hosting with retained Canvas slices, Android View hosting with slice composition, iOS UIView hosting, shared hit testing, focus traversal, IME coordination, platform accessibility attachment, Android and iOS module package attachment, and Windows, macOS, Linux, Web, Android, and iOS nonvisual timer reference integrations are implemented.
The current Linux, Web, and Android CLI paths require a source SDK checkout. iOS can consume a locally installed compatible SDK, but versioned distribution archives, relocatable Linux dependencies, and export automation are not implemented.
Generated projects use the shared `resources/images`, `resources/strings`, and `resources/raw` layout, and CMake preserves ordered resource roots for the application target.
The approved distribution architecture below is the contract for the next implementation phases.
Where the current source-oriented CLI differs, this document identifies the transitional behavior explicitly instead of preserving it as a second architecture.

## Decisions

- Applications and modules do not use a HuxerUI-specific manifest.
- CMake owns common C++ sources, resources, targets, and the module dependency graph.
- Source-controlled `platform/<platform-id>` shells own platform lifecycle, packaging, signing, and platform-only configuration.
- Every platform builds the application through the repository root `CMakeLists.txt`; platform-specific wrapper CMake projects are not part of the target architecture.
- `.huxerui` contains only reproducible generated metadata and platform incremental build output.
- The CLI orchestrates CMake and platform tools; it does not replace Gradle, Xcode, Emscripten, or another platform build system.
- Platform drivers are private CLI implementation, not a public plugin ABI.
- A formal release provides one versioned HuxerUI SDK whose layout keeps host-executed tools distinct from target libraries without exposing separate SDK products.
- Source checkout use is an override of the same SDK contract, not a separate integration model.
- HuxerUI built-in resources, module resources, and application resources produce one ordered final `resources.bin` per application.
- Generated module topology contains only ordered module targets and resolved source roots; it never mirrors platform configuration.
- Runtime ownership remains one shared `Runtime` and one `PlatformAdapter` per application surface.

These decisions keep direct CMake use viable and prevent a second project model from drifting away from platform tools.

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
  CLI and CMake helpers
  host tools selected by host and architecture
  built-in resource package
  public headers and libraries
  platform package-manager artifacts where required
  platform integration sources or libraries

platform toolchains
  compiler and CMake
  Gradle, Android SDK, NDK, and ADB
  Xcode and Apple tooling
  Linux desktop tools
  future platform tools
```

The CLI does not introduce another Runtime, platform-specific application definition, or platform host hierarchy.

## Application project

A generated project has this shape:

```text
hello_huxer/
  .gitignore
  CMakeLists.txt
  src/app.cpp
  resources/
    images/
    strings/default.properties
    raw/
  platform/
    android/
      .gitignore
      settings.gradle
      build.gradle
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
      main.cpp
      app.manifest
    macos/
      main.cpp
      Info.plist.in
    linux/
      main.cpp
    web/
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

Each platform shell owns its platform-specific ignore rules.
Android ignores Gradle and CMake intermediates inside `platform/android`, while Apple and future platforms keep their own IDE and package-manager state local to their shell.
The current generator still writes transitional platform CMake configuration files for several shells.
They are migration inputs, not part of the target project shape, and are removed as each platform starts configuring the root CMake project directly.

The application declaration remains shared:

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

View App() {
  return MaterialTheme([] {
    return Text("Hello, HuxerUI");
  });
}

const Application application{App};
```

Platform shells own the process entry point and call `RunApplication()` after static initialization.
Hosted platforms such as Android and Web create sessions from the same automatically registered `Application`.

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
            resources
        RESOURCE_NAMESPACE
            app
)
```

The SDK owns the public `HuxerUIConfig.cmake`, application helpers, host tools, built-in resource location, public headers, and the target-platform libraries present in that SDK form.
`HuxerUIConfig.cmake` is the only public CMake package and exposes only the canonical `HuxerUI::huxerui` and `HuxerUI::huxerui_static` targets.
Platform package managers may carry platform libraries and integration code, but they do not introduce a second public HuxerUI package or a forwarding target hierarchy.
The CLI or source-controlled platform shell supplies the resolved SDK location to CMake; application source does not encode an SDK archive layout.
An in-tree source override creates the same canonical targets and loads the same helpers without changing the application declarations below the SDK bootstrap.

`huxerui_add_app`:

- Creates an executable, application bundle, or Android application library for the active platform.
- Links a canonical HuxerUI target.
- Enables scope code generation after all declared sources are known.
- Registers the optional resource root.
- Applies bundle metadata supplied by the application.
- Emits minimal application artifact metadata consumed by CLI launch commands.

Advanced consumers may still create targets directly and call `huxerui_enable_codegen` and `huxerui_add_resources`, but manually created targets do not implicitly register the framework resource package.
Application executables use `huxerui_add_app` so built-in resources are always the first merge input.
`huxerui_add_resources` may be called repeatedly for one target.
CMake retains call order, and the final resource build lets later matching variants override earlier variants while keeping nonmatching variants.
For application targets, the precompiled framework package and all registered roots are merged through outputs attached directly to the target, without auxiliary resource targets.
Each call supplies a root and a `NAMESPACE` value that is both the resource domain and exact generated C++ namespace; only its generated header adds `_resources`.
`huxerui_add_app` registers the framework package first and then the compact `RESOURCES resources` application root.
An application that needs to replace selected framework defaults adds a later root with `NAMESPACE huxerui`; no bundle metadata is required.

The current install already contains public headers, platform libraries, the precompiled framework resource package, CMake helpers, the CLI, and host code generators.
Formal releases preserve that single-SDK contract while platform package managers carry the artifacts they own.
Host tools are selected from `share/huxerui/tools/<host>/<architecture>` and always run on the development host, independently of the target architecture.

## Generated integration

Generated files are projections, not another source of truth.
Desktop and Web builds use application artifact metadata emitted by the shared application helper.
Android reads Gradle's APK `output-metadata.json`, while iOS uses build-result metadata emitted by its Xcode build phase, so launch uses the final variant application ID and artifact name.
Launch metadata remains build output rather than a duplicate project identity and is distinct from dependency discovery.

When a platform package manager must attach source-backed module packages, CMake emits `.huxerui/generated/modules.json` from the resolved common module graph.
Each source-backed entry contains only:

- The requested CMake target identity.
- The resolved module source root.

Entry order is `huxerui_use_module` declaration order.
Predeclared binary targets without a source root do not appear; their platform artifacts are declared through the owning platform package manager.
Android settings use the graph to include `platform/android` library projects, and the Apple module aggregator may use the same graph to resolve `platform/ios` Swift packages.
Windows and Linux require no platform projection when their module integration remains entirely inside CMake.

The module graph never contains an application identifier, SDK or NDK versions, ABIs, product names, permissions, platform dependencies, hooks, resource namespaces, or SDK selection.
Those values remain in their owning Gradle, Xcode, platform package, or CMake files.
Deleting `.huxerui/generated` and configuring again must reproduce the module graph and derived platform-package integration.
The CLI never parses `CMakeLists.txt` as source text.

## CLI surface

The implemented command surface is:

```text
huxerui create app <name> [--id <reverse-domain-id>] [-p|--platform <platform-list>]
huxerui create module <name> [--id <reverse-domain-id>] [-p|--platform <platform-list>]
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
huxerui create app hello_huxer --id dev.example.hello --platform windows,android
cd hello_huxer
huxerui platform add macos
```

Application creation writes the common CMake project and complete minimal shells for the selected platforms.
Module creation writes a common CMake library and a normal application under `examples/preview` that consumes it through a local path.
The generated project recursively collects C++ sources under `src`, so adding a source file does not require a platform-specific CMake edit.
The template creates `resources/images`, `resources/raw`, and `resources/strings` directly, without an additional domain directory.
It uses a temporary tree and publishes the project only after every file succeeds.
`platform add` similarly refuses to overwrite an existing platform directory and rolls back directories created by a failed multi-platform operation.

### Doctor

`doctor` is read-only.
Outside a project it reports the SDK, common tools, available drivers, and any explicitly requested platform tools.
Inside a project it also validates required common files, unknown platform directories, shell contents, current-host support, and required platform tools.

Checks are scoped to requested platforms.
A missing Android toolchain does not make a Windows-only diagnostic fail.

### Devices

Device discovery does not require a project.
The Android driver parses `adb devices -l` and preserves ready, offline, unauthorized, and unavailable states. The iOS driver combines paired physical devices from `devicectl` with booted Simulators from `simctl` and retains the selected device kind through build and launch.
Desktop drivers do not expose synthetic devices.

### Build

Builds retain platform output below `.huxerui/build/<platform>/<profile>`. iOS uses explicit `ios-simulator` and `ios-device` DerivedData roots so Simulator and device products, intermediates, architectures, and signing state never share one directory.
The CLI validates the complete requested set before executing commands and prints each platform command.

Desktop builds configure the root CMake project and then build it.
Fresh desktop builds use Ninja when it is available unless an explicit generator, `CMAKE_GENERATOR`, or an existing CMake cache takes precedence.
`--generator` applies only to CMake-owned desktop and Web builds. Android and iOS reject it because Gradle and Xcode own their platform build generators.

iOS builds invoke the source-controlled Xcode project. A build without `--device` uses the generic Simulator destination; selecting a Simulator or physical device uses its Xcode destination identifier, and a physical-device build allows automatic provisioning updates. The App target invokes CMake to produce a destination-specific application core and links it into the bundle.

Android builds invoke the source-controlled Gradle shell, whose `externalNativeBuild` configures the repository root `CMakeLists.txt` directly.
Gradle owns the application namespace, application identifier, compile and target SDK versions, minimum SDK, NDK version, ABI filters, dependencies, manifest merging, signing, and APK output.
The application attaches each consumed module's `platform/android` Gradle library in module-graph order.
Before Gradle starts, the CLI incrementally configures the same root project with the host toolchain to refresh only the platform-neutral module graph needed during Gradle settings evaluation. This dedicated module-graph build directory retains its own generator and is independent of Gradle, Xcode, and platform build directories.
The ABI-specific C++ build remains exclusively owned by Gradle's root-project `externalNativeBuild` invocation.
The shell uses its local `gradlew` or `gradlew.bat` when present and otherwise requires `gradle` on `PATH`.
The current CLI does not generate or download Gradle wrapper binaries.

Web builds use `emcmake` to configure the same root CMake project and produce the ES module, WebAssembly module, and project-owned HTML entry point.
The Web shell owns the HTML document and host-element mount code rather than hiding them in the SDK.

### Run

`run` accepts exactly one enabled platform and performs a build before launch.
Windows starts the executable, macOS opens the application bundle, Web delegates the generated HTML entry point to `emrun`, Android installs and launches the generated APK, and iOS uses `simctl` for a selected booted Simulator and `devicectl` for a paired physical device.

For Android, one ready device is selected automatically.
Multiple ready devices require `--device <id>`, and an explicit device must exist and be ready before building.

## SDK selection and distribution

An official HuxerUI version may publish several host installers and platform artifacts, but they are release forms of one SDK rather than independently selected SDK layers.
The project version, CMake package version, platform package version, CLI version, and resource binary compatibility are produced from the same release.

### SDK home and discovery

`HUXERUI_HOME` is the public environment variable for selecting an installed SDK or an explicit source checkout.
For an installed SDK it names the self-contained installation prefix:

```text
HUXERUI_HOME/
  bin/
    huxerui
  include/
  lib/
    cmake/HuxerUI/
  share/huxerui/
    resources/
    tools/<host>/<architecture>/
```

`HUXERUI_HOME/bin` belongs on `PATH` for a portable or installer-managed SDK.
The CLI resolves its home in this order:

- A valid explicit `HUXERUI_HOME`.
- A valid installation or source root derived from the running `huxerui` executable.

The CLI validates the resolved root, exports the resolved `HUXERUI_HOME` to CMake, Gradle, Xcode, and other child processes, and reports the source of the selection through `doctor`.
The CLI must remain usable when the environment variable is absent, because a platform installer can place `huxerui` on `PATH` more reliably than every operating system can persist an arbitrary environment variable for all shells and GUI processes.
Direct CMake consumers may set `HUXERUI_HOME` or use the standard `CMAKE_PREFIX_PATH` package lookup.
The former `HUXERUI_SDK_ROOT` input has been removed rather than retained as an alias.

A source root exposes the same canonical targets, helpers, tools, and resource contract as a local development override.
The application root CMake project therefore does not branch into a second source-SDK application model.
No `sdk.json` is required: standard CMake and platform-package metadata describe the installed SDK and platform artifacts, while platform-specific facts remain in the platform integration that owns them.

### Platform release forms

Platform build systems consume platform artifacts through their normal mechanisms:

- Android uses a Maven AAR containing Java integration, JNI libraries, and Prefab metadata.
- macOS and iOS may use signed XCFramework and Swift Package artifacts where platform-package integration is required.
- Windows and Linux use architecture- and toolchain-compatible CMake SDK archives.
- Web uses an Emscripten-version-compatible SDK archive while configuring the application root through `emcmake`.

Platform packages do not duplicate common application policy or introduce another runtime resource store.
Android may substitute the repository Gradle library explicitly for source development, while installed and source CMake paths preserve the same canonical HuxerUI targets.
The current implementation predates the complete release layout: Windows and macOS consume installed packages, Linux, Web, and Android require a source checkout, and iOS accepts source or a compatible installed prefix.
The implementation phases replace these restrictions rather than preserving them as supported distribution modes.

### Installers

The canonical install rules produce one relocatable SDK tree, and thin platform installers place that tree, expose its `bin` directory, and remove only state they own during uninstall.
The planned release forms are:

- A signed Windows MSI, installed under `C:\Program Files\HuxerUI` by default, which sets `HUXERUI_HOME`, adds `%HUXERUI_HOME%\bin` to `Path`, and broadcasts the environment change.
- A signed and notarized macOS package, installed under `/Library/Developer/HuxerUI` by default, which exposes its `bin` directory through `/etc/paths.d` and relies on CLI self-location when `HUXERUI_HOME` is not present in a process environment.
- Linux DEB and RPM packages installed under `/opt/huxerui`, plus a portable archive, with package-owned environment setup exposing `HUXERUI_HOME` and its `bin` directory.

CMake install rules are the single file-layout source of truth.
CPack may wrap those rules with WiX, productbuild, DEB, RPM, and portable archive generators, while signing, notarization, and release publication remain CI responsibilities.
Installers do not edit application projects, download platform toolchains, or silently select an Android SDK, NDK, Xcode installation, compiler, or signing identity.

### Built-in resources

The precompiled HuxerUI built-in resource package is a first-class SDK artifact at the same version as the CLI, CMake package, and platform artifacts.
It is not hidden inside an AAR, XCFramework, Swift package, or platform library as another runtime resource store.

An application produces exactly one final resource package with this order:

```text
HuxerUI built-in package
  -> module packages in huxerui_use_module order
  -> application resource roots in declaration order
  -> final resources.bin
```

Later matching variants replace earlier variants, so an application may deliberately override the `huxerui` namespace.
`hapt` validates its binary format while merging and rejects incompatible packages rather than requiring separate bundle metadata.
The final package is staged into Android assets, an Apple application bundle, the Windows or Linux application resource directory, or the Web preload set by the owning platform build.
Android resources and manifests, Apple asset catalogs and property lists, Windows resources, Linux desktop metadata, and Web shell assets remain platform resources and do not enter `resources.bin` unless the application explicitly declares them as HuxerUI resources.

The SDK, platform artifacts, and built-in resource versions must agree.
Compatibility is expressed through standard CMake and platform-package versions plus the resource binary format, not through an additional HuxerUI manifest.

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

The current registry contains Windows, macOS, Linux, Web, Android, and iOS.
The registry is compiled into the CLI; it is not a dynamic extension mechanism.
Adding a platform may split template storage or driver implementations when their size justifies it, but does not change project discovery or command parsing.

Editable project, module, Preview, platform-shell, and generated-integration templates live as ordinary files under `tools/huxerui_cli/templates`.
CMakeRC compiles that tree into the CLI, and the internal template loader renders both relative output paths and file contents from the same project identity and feature-specific replacements.
The installed CLI therefore remains a single executable and never searches the current directory, source checkout, or SDK for template files at runtime.
These CLI templates are build-time tool resources and are independent of the application-facing `resources.bin` package.
Empty scaffold files and directories remain explicit generator structure because an embedded filesystem cannot represent an empty directory.

### Windows

The shell supplies an application manifest and optional platform CMake inputs.
The root CMake project creates the executable and links the installed or source HuxerUI target.
The driver runs only on Windows.

### macOS

The shell supplies bundle metadata through `Info.plist.in` and optional platform CMake inputs.
The root CMake project creates the application bundle.
The driver runs only on macOS.

### Linux

The Linux backend is supported and the root CMake project owns its executable, common application target, generated resources, and HuxerUI linkage.
The source-controlled `platform/linux` directory marks Linux as enabled without adding a second CMake project or placeholder platform configuration.
The CLI driver runs on Linux hosts, configures and builds the root project, discovers the executable through generated application integration metadata, and launches it with its containing directory as the working directory.
Linux-specific module C++ sources under `platform/linux/src` join the ordinary module target and therefore require no platform-package projection.
Missing PlatformView, accessibility, or module capabilities are backend limitations to implement explicitly; they do not make Linux a future platform.

### Web

The shell supplies the browser-owned HTML document and empty host element used by the adapter-owned composition root.
The driver wraps the existing Emscripten CMake backend with `emcmake`, retains incremental output under `.huxerui/build/web`, and uses `emrun` for local development.
It does not define a parallel JavaScript component system or expose browsers as synthetic devices.
Formal distribution uses an Emscripten-compatible SDK archive, and a source checkout remains an explicit override of the same root-project configuration.

### Android

The shell is a Gradle application with an `app` module.
Gradle owns Android packaging, manifest merging, SDK selection, ABI variants, and APK output.
CMake owns the common application target, code generation, common modules, and final HuxerUI resource generation.

Published builds consume the HuxerUI AAR and its Prefab targets through normal Gradle dependency resolution.
Source development substitutes the repository Android library explicitly while preserving the same Gradle and CMake target contract.
The app module's `externalNativeBuild` points directly at the repository root `CMakeLists.txt`.
There is no project-level `huxerui.cmake`, generated Android SDK configuration projection, or CMake-owned copy of Gradle configuration in the target architecture.

### iOS

The shell is a source-controlled Xcode application project. It owns the Info.plist, launch screen, asset catalog, build configurations, shared scheme, product identifier, signing, Capabilities, platform sources, archive behavior, and final App Bundle.

On iOS, the application-core archive contains the static `Application` declaration, while the shell's minimal Objective-C++ `main.mm` calls `RunApplication()`. `huxerui_add_app()` produces an application-core archive instead of another executable or App Bundle. CMake places the archive, linker response file, and merged HuxerUI resource package under `huxerui-ios/<target>` so every Xcode shell consumes the same stable application-core contract. CMake remains responsible for the common C++ sources, scope code generation, resource generation, and linking the selected installed or source HuxerUI static target. Xcode remains responsible for process entry, platform resources, destination selection, signing, packaging, installation metadata, and debugging, and fails its staging phase when the merged HuxerUI resource package is absent.

iOS has one application build path. Source-checkout development and a packaged SDK use the same application-core contract; only `HUXERUI_HOME` resolution changes. The driver discovers paired devices and booted Simulators, invokes `xcodebuild`, installs through `devicectl` or `simctl`, and opens the checked-in project directly. Distribution export automation and public UIView embedding remain outside the current preview.

`huxerui open ios` writes the resolved SDK location only to the ignored local Xcode configuration. Repository examples use one source-controlled platform runner whose `HUXERUI_APP_TARGET` build setting selects an `example_*` application core; adding an example does not add another Xcode project or platform application target.

## Modules and platform integration

The CMake target, acquisition, and resource contracts in this section are implemented.
Android and iOS platform-package scaffolding is implemented.
Android platform-package discovery and application attachment are implemented.
iOS builds project the same module graph into one generated local Swift package aggregator and attach its stable product to the source-controlled Xcode application.

A HuxerUI module is a compile-time CMake target that may also contain platform packages implementing typed services, PlatformView factories, or ExternalTexture producers.
It is not a runtime plugin and does not require a universal public `Module` base class.

```text
CameraKit/
  CMakeLists.txt
  README.md
  LICENSE
  include/camera_kit/
  resources/
    images/
    strings/
    raw/
  src/
  platform/
    android/
    ios/
    macos/
    windows/
    linux/
    web/
  examples/
    preview/
      CMakeLists.txt
      src/app.cpp
      resources/
      platform/
        android/
        ios/
  tests/
```

### Module scaffolding and preview

Project scaffolding distinguishes the application and module shapes explicitly:

```text
huxerui create app <name> [--id <reverse-domain-id>] [-p|--platform <platform-list>]
huxerui create module <name> [--id <reverse-domain-id>] [-p|--platform <platform-list>]
```

Module names do not require a `huxerui-` prefix and may use uppercase ASCII letters.
The scaffold preserves the supplied name for the repository directory and project display name, while deriving one lowercase snake-case identifier for CMake, C++, headers, resources, and Preview targets.
For example, `CameraKit`, `camera-kit`, and `camera_kit` derive `camera_kit`; leading, trailing, or repeated separators are rejected.
The public product removes separators and capitalizes lowercase segment starts while preserving intentional capitalization, so `CameraKit` generates `CameraKit::CameraKit` and `HuxerUI-CameraKit` generates `HuxerUICameraKit::HuxerUICameraKit`.
The generated target therefore retains acronyms without placing third-party targets under the framework-owned `HuxerUI::` namespace.

`--id` is the complete stable reverse-domain project identifier rather than an organization prefix.
For an application it initializes the Android application ID and namespace, Apple bundle identifier, and equivalent platform product identifiers.
For a module it initializes the Android library namespace and the CLI's stable platform-package identity.
Maven coordinates, Swift package and product names, the C++ namespace, the CMake target, and resource namespaces remain owned by their respective platform or common project files and are not inferred by splitting `--id`.
When omitted, the scaffold uses `com.example.<normalized-name>` as an editable development default.

Project kind selects the platform artifact shape, so there are no separate Android application-versus-library or Apple application-versus-package options.
An Android application receives a Gradle application shell, while an Android module receives an independent Gradle library build.
An iOS application receives an Xcode application project, while an iOS module receives a Swift package with a library product.
Platform-specific SDK levels, dependencies, permissions, capabilities, publishing coordinates, and product policy are edited in those generated platform projects instead of being additional cross-platform CLI arguments.

Application creation retains the current all-platform default when `--platform` is omitted.
Module creation without `--platform` creates only the common C++ module and common preview sources; it does not create empty platform packages.
Each platform selected for a module creates the matching application shell below `examples/preview`.
Windows and Linux create CMake source roots under `platform/<platform>/src`, Android additionally creates an independent Gradle library under `platform/android`, and iOS creates a Swift Package under `platform/ios`.
macOS and Web currently add only the Preview shell because no separate platform-package shape has been defined for them.
`platform add` applies the same behavior after creation and refuses to overwrite either an existing platform package or Preview shell.
Later commands obtain launch artifacts from the owning platform or CMake build output, while platform-package attachment uses the platform-neutral generated module graph.
Deleting `.huxerui` and regenerating it does not require parsing `CMakeLists.txt` or maintaining a second editable manifest.

A module is a library and never gains an application entry solely for previewing.
Every generated module instead contains `examples/preview`, an ordinary standalone HuxerUI application that consumes the module through a local path:

```cmake
huxerui_add_app(example_camera_kit
        SOURCES
            src/app.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
)

huxerui_use_module(example_camera_kit
        TARGET CameraKit::CameraKit
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
```

The preview installs the same public RootHook that a consuming application uses:

```cpp
#include <huxerui/huxerui.h>
#include <camera_kit/camera_kit.h>

using namespace huxerui;

View App() {
  return MaterialTheme([] {
    return camera_kit::CameraPreview();
  });
}

const Application application{
    App,
    {
        .root_hooks = {
            camera_kit::Install,
        },
    }
};
```

Developers use the ordinary application commands from either the module root or the preview directory. Commands issued at the module root resolve to `examples/preview` without introducing a separate module build path:

```bash
huxerui run android
huxerui run ios
huxerui open ios
```

There is no separate module preview Runtime or `module run` build path.
The current preview is a real HuxerUI application consuming the common module through CMake.
Its Android Gradle application consumes the module's Gradle library, and its iOS Xcode application consumes the module's Swift package through the generated aggregator.
These paths validate platform dependency resolution, mergeable platform declarations where supported, explicit RootHook installation, PlatformView behavior, and nonvisual service lifecycle through the same path used by an external application.
Common C++ tests, platform-package tests, and the preview application remain complementary rather than replacing one another.

The module repository declares its common C++ target and resources in CMake rather than adding a HuxerUI-specific JSON or YAML manifest:

```cmake
huxerui_add_module(camera_kit
        SOURCES
            src/camera_kit.cpp
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            camera_kit
)

add_library(CameraKit::CameraKit ALIAS camera_kit)
```

Applications acquire and link one module through one repeated helper.
A local path supports application and module development in one checkout:

```cmake
huxerui_use_module(my_app
        TARGET CameraKit::CameraKit
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/modules/CameraKit"
)
```

A GitHub or other HTTPS Git repository uses a pinned revision:

```cmake
huxerui_use_module(my_app
        TARGET CameraKit::CameraKit
        URL "https://github.com/example/CameraKit.git"
        REVISION "0123456789abcdef0123456789abcdef01234567"
)
```

A module target declared by the application or another CMake package is consumed without a source location:

```cmake
find_package(CameraKit CONFIG REQUIRED)

huxerui_use_module(my_app
        TARGET CameraKit::CameraKit
)
```

PATH and URL are mutually exclusive.
PATH resolves relative to the caller and uses the local source directly.
URL accepts HTTPS Git repositories only and requires REVISION.
Remote source uses FetchContent's normal build-directory cache, and repeated use by several application targets acquires and configures the repository only once.
If the requested target already exists, PATH and URL must be omitted; the helper never assigns a requested origin to an unrelated target.
The helper verifies that acquisition creates the requested CMake target, links it to the application, and appends its compiled resource package in declaration order without a separate finalize call.
Calling `target_link_libraries` alone links ordinary code but intentionally does not merge module resources or request platform-package discovery.

The application CMakeLists is both the dependency declaration and revision lock.
A full commit SHA is the reproducible remote form; a release tag may identify a human-facing version on GitHub but is not treated as immutable dependency identity.
There is no second dependency list or lock manifest in the initial design.
Remote CMake source executes with the same authority as any other build dependency, so HTTPS transport does not replace commit review and pinning.

Module resource directories are ordinary ordered target resource roots.
Their CMake `NAMESPACE` selects the domain, and applications may add a later root with the same namespace to replace selected variants.
The final package is one merged binary index and payload set rather than a runtime collection of module bundles.
PATH and URL modules compile these packages from source; the binary installation contract for resource-bearing predeclared modules remains future packaging work.

The implemented common module integration pipeline is:

```text
huxerui_use_module declaration order
  -> acquire or reuse module target
  -> link common C++ target and append its resource package
```

Android platform integration continues from the resolved module source roots:

```text
module target closure
  -> retain each resolved module source root
  -> discover the current platform package by convention
  -> attach that package to the platform shell
  -> platform build system
```

CMake does not parse platform directories or model platform dependencies, permissions, products, manifests, or package-manager options.
Its generated module graph exposes only the module target identity, declaration order, and resolved source root already established while acquiring the common target.
The Android CLI driver uses that topology to locate `platform/android`, attaches each discovered Gradle library to the application shell, and delegates its contents to Gradle.
The application platform CMake build includes the same root CMake project, so common module targets and Android C++ sources participate in one graph rather than a second library graph.
The iOS CLI driver projects the same graph into one generated local Swift package aggregator below `.huxerui/generated/ios/modules` before building or opening Xcode.
The source-controlled Xcode application references one stable aggregator product, so adding or removing modules does not rewrite the project file.
The aggregator only composes module packages; application privacy text, entitlements, capabilities, signing, and final product policy stay in the Xcode shell.
Windows, Linux, and Web module sources join the common target directly from their platform source roots, so those platforms do not need platform module projection.
Web modules select C++ and Emscripten glue from their own CMake target and declare any JavaScript link inputs there; the CLI does not translate JavaScript package metadata into the common module graph.

Runtime installation remains explicit C++ application policy.
An application includes the module's public header and places its typed installer directly in `AppOptions::root_hooks`:

```cpp
#include <camera_kit/camera_kit.h>

const Application application{
    App,
    {
        .root_hooks = {
            camera_kit::Install,
        },
    }
};
```

There is no generated installer header, hidden application-entry rewriting, or generic runtime module registry. The process-level `Application` registers only its root and `AppOptions`; module installation remains explicit through `root_hooks`.
Generated platform attachment files remain build output rather than another editable project or runtime plugin list.

Platform dependencies remain expressed in their owning ecosystem.
An Android module's `platform/android` directory is an Android library whose Gradle build declares Java or Kotlin sources, Android resources, C++ dependencies, and third-party libraries, while its library manifest declares mergeable Android components and permissions.
An Apple module's `platform/ios` directory is a Swift package whose `Package.swift` declares targets, resources, linked system libraries, and package dependencies.
Its library product matches the suffix of the requested public CMake target, such as `CameraKit` for `CameraKit::CameraKit`, so the generated aggregator can attach it without parsing `Package.swift` or duplicating product metadata in the module graph.
Equivalent future platforms use their own package and build systems.

The application shell owns final application policy even when a platform package contributes mergeable declarations.
Android applications may override or remove declarations merged from a library manifest.
Apple usage descriptions, entitlements, capabilities, signing, and other application-target policy remain in the source-controlled Xcode shell; a Swift package never invents application-facing privacy text.
The CLI does not translate these declarations into CMake or synthesize policy on the application's behalf.

Modules use three runtime integration forms:

- Permission, Audio, Camera control, and similar nonvisual features install typed Root Services backed by registered platform module instances.
- WebView, map, document preview, and platform SDK controls register PlatformView factories by stable string type.
- Camera preview, video decode, and high-frequency visual streams create platform-owned ExternalTexture sources and return platform-neutral consumer values to shared code.

One module may combine the forms.
Camera normally provides a Camera service plus ExternalTexture preview, while Audio provides only a service and WebView provides a PlatformView factory.
The application retrieves services through their typed `UseXxx()` helpers; there is no generic module-service lookup.

PlatformView and nonvisual module instances share the PlatformPayload protocol defined in [Architecture Design](architecture.md#platform-payload-and-instance-protocol).
PlatformPayload is the only dynamic in-process cross-language representation and is restricted to null, scalar, bytes, list, string-keyed object data, and the closed framework capability ExternalTexture.
Concrete module headers keep application properties, calls, results, and events strongly typed and own all PlatformPayload encoding and decoding.
Callbacks, arbitrary C++ objects, system handles, and media frames never enter the payload; an ExternalTexture value only retains the opaque platform-owned source state.

Platform sources explicitly register each visual or nonvisual factory under a nonempty case-sensitive UTF-8 type such as `web/WebView` or `audio/Player`.
The two factory kinds share one type namespace, so duplicate or kind-conflicting registration fails during module installation.
Module registration does not use a generated header, hidden application-entry rewriting, editable metadata bundle, or process-global static initializer.
The module's documented Install function remains an ordinary RootHook selected explicitly by the application.

`RootContext::Modules()` exposes the per-surface registry to explicit platform module installers.
A nonvisual installer opens a registered instance and provides its public typed service through `root.Provide()`.
The service translates typed methods and events to Create, Call, Result, Event, and Dispose messages, owns pending requests and subscriptions, and closes its platform instance during reverse Root Service teardown.
Applications never call `Modules()` or use string method names directly.

The Runtime-side PlatformView lifecycle, exact RenderComposition ordering, typed events, nonvisual instance protocol, and ExternalTexture ownership are defined in [Architecture Design](architecture.md#platform-content-integration) rather than duplicated here.
Platform-package attachment only makes a module's platform implementation available to the platform application target; the module's explicit RootHook still installs its factories and services without another runtime API or composition mode.
A PlatformView factory must preserve the shared ordering, clipping, input, focus, and accessibility contract; a platform implementation that cannot do so fails explicitly instead of moving the platform object to a global foreground or background plane.
Module-owned typed Root Services keep PlatformPayload codecs and string method names behind those services.
Windows posts a coalesced private message to its application HWND, the macOS and iOS adapters supply a `UIThreadDispatcher` backed by the platform main queue, Linux uses an `eventfd`-backed X11 event-loop queue, Web queues work through the browser event loop, and Android dispatches through its owning `HuxerUIView`. `example_platform_module` provides source-level Windows thread-pool timer, Foundation, Linux `timerfd`, Emscripten interval, and Java integrations behind one typed service. On Apple platforms, Linux, Web, and Android it additionally returns an `ExternalTexture` from a typed service and publishes `CVPixelBuffer`, copied RGBA, cloned WebCodecs `VideoFrame`, or `Bitmap` frames without per-frame PlatformModule callbacks.

Camera or video may still use PlatformView when a platform interactive hierarchy is required and the platform implementation satisfies that contract.
Pure high-frequency visual output normally uses ExternalTexture because it remains an ordinary renderer command and supports unrestricted HuxerUI transforms, clipping, opacity, and paint interleaving without a platform input subtree.

ExternalTexture requires neither a factory registry nor a texture registry.
A module's platform implementation creates a move-only platform source, obtains its copyable ExternalTexture consumer value, and returns that value through its typed service and PlatformPayload codec.
The source binds once when the value first crosses a surface-owned adapter boundary, while the matching renderer keeps only a private cache for bound source states.
Image accepts ExternalTexture and records DrawExternalTextureCommand, so Camera overlays, transforms, clipping, and damage remain ordinary RenderScene behavior.
Frame publication replaces a latest-wins platform mailbox and requests presentation without writing application State, exposing a numeric texture identity, or executing a per-frame language bridge callback.
The complete binding, payload, lifetime, scheduling, and staged platform contract is defined in [Architecture Design](architecture.md#externaltexture).

Modules and platform shells provide these factories, registrations, payload codecs, and typed services without introducing Runtime subclasses or platform types into shared public headers.
Future platform integration must report a missing current-platform package, ambiguous platform product, duplicate registered type, factory-kind conflict, malformed subscribed payload, unsupported exact-composition capability, or incompatible HuxerUI version with the owning module and application target in the diagnostic.
The implemented CMake integration already rejects invalid URL schemes, absent revisions, ambiguous origins, duplicate module use, and missing requested targets.

## Implementation phases

The architecture is implemented through reviewable phases that keep generated projects usable at each boundary:

- Documentation has replaced the source-SDK-oriented architecture with the single-SDK home, installer, resource, root-CMake, and module-graph contracts.
- Platform build ownership has removed the Android configuration projection and platform wrapper CMake project, lets Gradle configure the application root directly, and reduces generated platform module data to `modules.json`.
- Formal SDK home selection now provides `HUXERUI_HOME`, relocatable Windows and macOS installation validation, CLI self-discovery and child-process propagation, resource validation, and the same canonical public targets for installed and source use.
- Future installer packaging will wrap the canonical install tree for Windows, macOS, and Linux, while platform releases will publish desktop, Web, Android, and Apple artifacts without adding another common package hierarchy.
- Platform completion lets Linux CLI applications and modules use the root CMake graph directly and projects the module graph into the iOS Swift package aggregator without broadening the common metadata contract.

Each phase ends with its focused tests, a current-host build, packaging validation where applicable, `git diff --check`, and an owner review before the next phase begins.

## Future commands and platforms

`package`, `clean`, versioned SDK installers, and artifact collection remain future work.
They should extend the existing ownership model:

- `package` invokes the source-controlled platform shell's release path and collects user-facing output under `dist`.
- `clean` removes only driver-owned generated and build output.
- SDK selection uses standard CMake and platform-package compatibility plus user-level tool selection, not project-local hidden state.
- iOS device distribution and a future OHOS backend extend platform integration without changing the common application model.

No future command may silently skip an explicitly requested platform or claim an artifact that was not produced.

## Errors, security, and reproducibility

- Unknown platform identifiers are usage errors.
- Unknown or incomplete project shells identify the exact path and expected file.
- Unsupported host-target combinations fail before starting any build.
- Platform process failures retain their command and exit code without hiding platform logs.
- Process invocation passes argument arrays and does not route ordinary commands through a shell.
- Windows batch tools are handled explicitly and reject unsafe expansion characters.
- Generated metadata contains no credentials or signing keys.
- Git modules must use full commit SHA revisions for reproducible builds.
- A clean checkout plus the selected SDK and platform toolchains reproduces generated integration metadata.

## Validation

The current workflow is covered by:

- CLI project, shell, diagnostics, device parsing, process execution, and command-construction tests.
- CLI Android shell tests for root-CMake configuration, Gradle-owned platform values, source SDK selection, module attachment, and application launch identity.
- CMake module validation for URL policy, full commit revisions, unambiguous origins, predeclared targets, ordered module graph roots and resources, and explicit runtime Root Service installation.
- Installed Windows and macOS consumer tests that install the SDK, run the installed CLI, create a project, and build it without source-tree lookup.
- Existing common, header, code-generation, platform, and example builds.

Platform work must additionally validate every affected platform toolchain available on the development host and report unavailable platforms explicitly.
The Android migration must verify that Gradle configures the root CMake project directly and that no SDK, NDK, ABI, identifier, or dependency policy is copied through generated CMake metadata.
Formal SDK validation must install a relocatable SDK, resolve it through both executable location and `HUXERUI_HOME`, build a clean generated consumer, merge the matching built-in resource package, exercise the source override through the same public targets, and inspect each available installer without mutating unrelated environment state.

## Invariants

- One static `Application` declaration per final application binary.
- One shared Runtime implementation.
- One `PlatformAdapter` boundary per application surface.
- Public identity remains `huxerui`, `<huxerui/huxerui.h>`, and `HuxerUI::huxerui`.
- CMake owns common C++ targets and resource generation.
- Every platform configures the application repository root `CMakeLists.txt`.
- CMake does not model platform package dependencies, permissions, or application policy.
- Platform shells own platform lifecycle, platform-only configuration, signing, and packaging.
- Platform module packages own their platform sources, resources, dependencies, and mergeable declarations.
- Application shells own final permissions, privacy text, capabilities, signing, and platform product policy.
- Platform shells are source-controlled and built directly.
- Generated integration files are reproducible projections, not generated projects.
- Generated module topology contains only ordered targets and resolved source roots.
- One final application `resources.bin` merges framework, module, and application resources in declaration order.
- SDK tools, platform artifacts, and built-in resources share one HuxerUI version.
- Modules are compile-time units, not dynamically loaded plugins.
- Platform services and commands use typed interfaces rather than a generic string channel.
