# SDK, CLI, and Module Design

Status: proposed design

This document defines the target project model, SDK distribution, command-line workflow, native module system, and platform UI interoperability model for HuxerUI. The design builds on the existing `HUXERUI_APP` entry point, `Runtime`, `PlatformAdapter`, CMake integration, Android host view, and scope code generator. It does not describe currently implemented CLI behavior unless stated explicitly.

The design has the following goals:

- Let an application keep one common C++ source tree for every supported platform.
- Create a new project without requiring users to understand generated native projects.
- Build, run, and package one or more requested platforms through one CLI.
- Keep CMake, Gradle, and native platform toolchains available as the underlying build systems instead of replacing them.
- Distribute HuxerUI headers, libraries, host tools, templates, and native integrations as a versioned SDK.
- Allow capabilities that HuxerUI cannot draw itself, such as camera previews, maps, web views, video surfaces, system pickers, and payment UI.
- Give third-party modules strongly typed C++ APIs while letting the CLI auto-link their platform implementations.
- Preserve direct use of HuxerUI from CMake and native applications without requiring the CLI at runtime.

The initial design deliberately does not include:

- Runtime loading of arbitrary binary plugins.
- A stable binary ABI between independently compiled C++ modules.
- Transparent local cross-compilation of every target from every host.
- A general-purpose dynamic method channel based on string method names.
- Automatic conversion of arbitrary platform UI into HuxerUI drawing commands.
- A second application runtime or a platform-specific application definition.

## Architecture boundaries

The product is divided into four layers:

```text
Application
├── C++ source and assets
├── huxerui.toml
└── optional platform-owned customizations

HuxerUI CLI
├── project creation
├── SDK resolution
├── module resolution and auto-linking
├── native project generation
├── build, run, and package orchestration
└── artifact collection

HuxerUI SDK
├── public headers and libraries
├── CMake package and application helpers
├── Android AAR and Prefab package
├── platform adapter integrations
├── host code generation tools
├── native project templates
└── SDK and template metadata

Native toolchains
├── CMake and a C++ compiler
├── Gradle, Android SDK, and Android NDK
├── Apple build and signing tools
└── Windows build and signing tools
```

The CLI is an orchestrator. CMake remains the C++ build system, Gradle remains the Android application build system, and native signing and packaging tools remain authoritative for their platforms.

`Runtime` and `PlatformAdapter` remain the only runtime and platform ownership boundary:

```text
Application View tree
        ↓
Runtime
        ↓
PlatformAdapter
        ↓
native window, rendering, input, and embedded native views
```

The SDK and CLI must not add another application runtime, native host hierarchy, or platform-specific `AppDefinition`.

## Application project

A newly created managed project has this shape:

```text
hello_huxer/
├── huxerui.toml
├── CMakeLists.txt
├── src/
│   └── main.cpp
├── assets/
├── platform/
├── .huxerui/
└── dist/
```

Only `huxerui.toml`, `CMakeLists.txt`, application sources, assets, and intentional platform customizations belong in source control.

`.huxerui` contains generated projects, build directories, resolved metadata, and incremental tool state. It is reproducible and ignored by source control.

`dist` contains final user-facing artifacts. It is not used as an input to incremental builds.

The optional `platform` directory contains user-owned native customization. The CLI must not place ordinary generated files there. A project that does not need native customization can leave the directory absent or empty.

The application source continues to use the existing common entry point:

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

On desktop platforms the macro defines the process entry point. On mobile platforms it registers the immutable application definition consumed by the native lifecycle host.

## Project manifest

`huxerui.toml` is the source of truth for product metadata, enabled targets, platform packaging, module dependencies, and assets. It must not duplicate ordinary C++ build logic that belongs in CMake.

A representative manifest is:

```toml
[app]
name = "hello_huxer"
display_name = "Hello Huxer"
id = "com.example.hello"
version = "0.1.0"
sdk = "0.1.0"
sources = ["src"]
assets = ["assets"]

[targets.macos]
minimum_version = "13.0"
architectures = ["arm64", "x86_64"]
package = "app"

[targets.windows]
architecture = "x86_64"
package = "zip"

[targets.android]
min_sdk = 23
target_sdk = 36
abis = ["arm64-v8a", "x86_64"]
package = "apk"

[modules]
huxerui-camera = "1.0.0"
```

`app.name` is a machine-safe project and target name. `app.display_name` is the user-facing application name. `app.id` is the default reverse-domain identity for platform packages. A platform may override the identity when required.

`app.sdk` initially resolves an exact SDK version. Version ranges and a lock file can be introduced when remote SDK and module resolution are implemented. The first implementation should prefer deterministic exact versions.

Asset roots feed the typed resource index, generated resource keys, and target staging pipeline defined in [App Resources, Images, and Localization Design](resources.md).
Managed projects do not expose Android resource identifiers, application-bundle paths, or Windows package paths to shared application code.

Platform identifiers are stable lowercase values shared by the CLI, project manifest, module manifest, generated metadata, and SDK packages:

```text
android
macos
windows
```

Future platforms add new identifiers without changing existing identifiers.

Signing configuration refers to a user-level signing profile and never stores credentials in the project:

```toml
[targets.macos]
signing = "developer-id"

[targets.android]
signing = "release"
```

Signing profiles resolve through the operating system credential store, environment variables, or user-level CLI configuration. Passwords, private keys, and tokens must not be written to generated projects or build logs.

## CMake application API

The SDK must install a complete CMake package:

```cmake
find_package(HuxerUI CONFIG REQUIRED)
```

The package exports the public targets and application helpers required by consumers. `HuxerUI::huxerui` is the canonical public library target on every platform.

The SDK adds a common application helper:

```cmake
huxerui_add_app(hello_huxer
    SOURCES ${APP_SOURCES}
)
```

The generated starter `CMakeLists.txt` remains a normal, usable CMake project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello_huxer LANGUAGES CXX)

find_package(HuxerUI CONFIG REQUIRED)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

huxerui_add_app(hello_huxer
    SOURCES ${APP_SOURCES}
)
```

`huxerui_add_app` hides platform target differences:

- On macOS it creates a bundle executable, links HuxerUI, and applies bundle metadata supplied by the generated build configuration.
- On Windows it creates the appropriate GUI executable and collects required runtime libraries.
- On Android it creates the application shared library expected by the native activity host.
- On every platform it enables scope code generation after all application sources have been added.

The lower-level `huxerui_enable_codegen` function remains available to advanced CMake consumers.

The installed CMake package supplies the absolute host code generator path. Consumer projects must not depend on a source checkout containing `tools/prebuilt`.

Android Prefab naming differences, if any, are normalized inside the SDK CMake package. Application CMake code must not switch between `HuxerUI::huxerui` and a platform-specific target name.

## SDK distribution

An SDK release contains platform-independent development files, host tools, and target packages:

```text
sdk/0.1.0/
├── metadata/
│   └── sdk.json
├── include/
│   └── huxerui/
├── cmake/
│   ├── HuxerUIConfig.cmake
│   └── HuxerUIApp.cmake
├── templates/
│   ├── project/
│   ├── android/
│   ├── macos/
│   └── windows/
├── host/
│   ├── macos-arm64/
│   ├── macos-x86_64/
│   └── windows-x86_64/
└── targets/
    ├── android/
    ├── macos/
    └── windows/
```

Host packages contain executables that run during development:

```text
huxer
huxerui-codegen
```

Target packages contain headers, libraries, native host code, packaging metadata, and platform integration artifacts used by the application target. Host architecture and application target architecture are independent. An arm64 macOS host tool can generate code for an Android x86_64 application.

Android releases use an AAR containing the Java host integration and a Prefab package containing headers and native libraries for supported ABIs. A generated application project consumes the SDK artifact rather than depending on the HuxerUI source repository's Android library module.

Desktop releases export CMake imported targets. A prebuilt Windows package must identify its supported compiler and runtime ABI. The SDK resolver must reject an incompatible toolchain instead of attempting to link it.

An initial source-based SDK may build the HuxerUI implementation as part of an application while the binary package matrix is incomplete. This is an SDK distribution choice and must not change the application-facing CMake API.

## CLI command surface

The executable is named `huxer`. Its initial command surface is:

```text
huxer create <name>
huxer doctor [target...]
huxer devices
huxer build [target...]
huxer run [target]
huxer package [target...]
huxer clean [target...]
huxer module <command>
huxer sdk <command>
```

Commands accept platform identifiers as positional targets:

```bash
huxer build macos
huxer build macos android
huxer run android
huxer package windows
huxer package macos android
huxer package --all
```

This avoids separate singular and plural forms or a comma-separated platform option.

Module dependencies are managed explicitly:

```bash
huxer module add huxerui-camera@1.0.0
huxer module remove huxerui-camera
huxer module list
```

`module add` resolves compatibility before updating `huxerui.toml`. A failed resolution leaves the manifest unchanged. `module remove` updates the manifest and generated dependency state but does not edit application source code.

SDK installations are managed through the SDK command group:

```bash
huxer sdk list
huxer sdk install 0.1.0
huxer sdk use 0.1.0
```

`sdk use` updates the current project's exact `app.sdk` version after verifying that the SDK is installed and compatible with the project manifest. SDK downloads verify release metadata and content hashes before installation.

### Project creation

The basic creation flow is:

```bash
huxer create hello_huxer --id com.example.hello
```

Creation does not require every platform toolchain to be installed. It writes the common project, starter source, stable CMake file, manifest, asset directory, and source-control ignore rules.

The generated manifest enables platforms supported by the selected project template. Platform projects themselves are generated lazily when a platform is first built or explicitly prepared.

### Build

`huxer build` produces developer build artifacts and defaults to a debug profile. It preserves the underlying CMake and Gradle incremental build directories.

```bash
huxer build macos
huxer build android --profile release
```

When no platform is specified, `build` selects the current desktop target if the project enables it. If there is no unambiguous current target, the command stops with a target selection message.

### Run

`huxer run` builds one target and launches it:

```bash
huxer run macos
huxer run android --device emulator-5554
```

Only one target can run per invocation. A desktop target starts its generated application. Android installs the debug APK and launches its activity through ADB.

If exactly one compatible Android device is available, it can be selected automatically. Multiple devices require an explicit selection or an interactive choice. The resolved device identifier is always printed.

`run` does not imply production packaging or signing.

### Package

`huxer package` produces distributable release artifacts:

```bash
huxer package macos
huxer package macos android
huxer package --all
```

Requested platforms can build in parallel. Failure of one requested platform does not discard successful artifacts from other platforms, but the command returns a failure exit status and prints a result for every requested target.

Artifacts are normalized under `dist`:

```text
dist/
├── macos/
│   └── Hello Huxer.app
├── windows/
│   └── hello_huxer.zip
└── android/
    └── hello_huxer-release.apk
```

Package formats expand over time without changing the distinction between `build` and `package`. Android can later emit AAB, macOS can add signed archives or disk images, and Windows can add MSIX.

### Doctor

`huxer doctor` performs read-only environment checks and gives actionable diagnostics:

```text
HuxerUI SDK
CMake
C++ compiler
Apple build tools
Windows build tools
Java
Gradle
Android SDK
Android NDK
ADB
signing tools
connected devices
```

Checks are scoped to requested targets when provided. A missing Android SDK does not prevent a macOS-only build.

The default output is concise. A verbose mode includes resolved executable paths, versions, generated directories, and underlying build commands.

### Clean

`huxer clean` removes only resolved build and generated output for the selected project and targets. It must not remove global SDK installations, module caches, signing profiles, user source files, or the entire project directory.

## Host and target matrix

A cross-platform source project does not imply that every package can be produced locally on every host:

| Build host | Locally supported application targets |
| --- | --- |
| macOS | macOS and Android |
| Windows | Windows and Android |
| Linux | Linux and Android |

Formal macOS packaging requires Apple tools. Formal Windows packaging requires a supported Windows toolchain. The CLI must not silently skip an explicitly requested target and must not claim that an unsupported local cross-build succeeded.

`huxer package --all` validates the complete requested matrix before building. If the local host cannot build one or more enabled targets, it reports those targets clearly.

A later remote build feature can dispatch unavailable targets to trusted build agents:

```bash
huxer package --all --remote
```

Remote build support is an orchestration feature, not a replacement for native platform toolchains.

## Generated projects and build state

Managed platform projects live under `.huxerui`:

```text
.huxerui/
├── generated/
│   ├── android/
│   ├── macos/
│   └── windows/
├── build/
│   ├── android/
│   ├── macos/
│   └── windows/
└── state/
```

Generated files record the SDK version, template version, target configuration, and generation input fingerprint. A change regenerates only the affected platform project.

The CLI uses deterministic build directories and delegates file-level incrementality to CMake, Ninja, the selected native generator, and Gradle. It must not invent a second source compilation cache.

Generated projects are implementation details, but their build commands and locations are visible in verbose output so users can diagnose native toolchain failures.

A future explicit ejection command may copy a managed platform project into the user-owned `platform` directory:

```bash
huxer eject android
```

After ejection, the CLI treats that platform project as user-owned and does not overwrite it. Ejection is not required for the initial CLI.

## HuxerUI modules

A HuxerUI Module is a compile-time distribution and integration unit. It can contain common C++ APIs, HuxerUI components, native views, platform services, platform sources, build dependencies, resources, permissions, and registration metadata.

The term `Module` is preferred over `Plugin` because modules are resolved and linked at build time. The design does not promise runtime binary discovery, dynamic loading, or a stable plugin ABI.

`Module` is primarily a package and build concept. The public runtime API should not require every module to inherit from a universal `Module` base class. A generated registrar can register only the capabilities the module actually provides.

A camera module can have this shape:

```text
huxerui-camera/
├── huxerui.module.toml
├── include/
│   └── huxerui_camera/
├── src/
│   └── common/
└── platform/
    ├── android/
    ├── macos/
    └── windows/
```

Its common API can expose:

```text
CameraPreview
CameraController
Camera events
Camera capability queries
```

Its platform implementations can use CameraX, AVFoundation, or Windows camera APIs without exposing those types to common application code.

## Module manifest

`huxerui.module.toml` declares module identity, supported platforms, platform dependencies, permissions, resources, and registration inputs.

A representative manifest is:

```toml
[module]
name = "huxerui-camera"
version = "1.0.0"
sdk = "0.1.0"
platforms = ["android", "macos"]

[platform.android]
dependencies = [
  "androidx.camera:camera-camera2:1.5.0",
  "androidx.camera:camera-lifecycle:1.5.0",
  "androidx.camera:camera-view:1.5.0",
]
permissions = ["android.permission.CAMERA"]

[platform.macos]
frameworks = ["AVFoundation"]
permissions = ["camera"]
```

`module.platforms` is an explicit list rather than a table of boolean values. It uses the same identifiers as project targets and CLI commands.

The manifest does not support a special `all` platform value. Explicit platform declarations prevent an existing module from being treated as compatible with a newly introduced target before the module author validates it.

Platform sections are only valid for platforms declared by `module.platforms`. The CLI validates this relationship before generating any native project.

Platform-independent C++ modules still declare every platform they have validated. This keeps platform support deterministic as HuxerUI adds targets.

## Module resolution and auto-linking

The CLI resolves project module dependencies before generating platform projects. Resolution produces a deterministic graph containing:

- Module version and SDK compatibility.
- Common include directories and C++ libraries.
- Platform source sets.
- Native package dependencies.
- Framework and system library dependencies.
- Resources and permission declarations.
- Native view factory registrations.
- Platform service registrations.
- Application lifecycle participation.

The generated application links a static registration unit. Users do not call module registration from `main.cpp`.

Auto-linking is target-specific. An Android generation pass does not compile macOS sources or resolve Apple frameworks.

Permission declarations merge conservatively. A module can request that a permission be included, but the application remains responsible for user-facing purpose text and release policy. Conflicting declarations produce a diagnostic instead of selecting an arbitrary value.

The initial resolver can use installed modules or local paths. A remote module registry and semantic dependency solver can be added later without changing the module manifest's platform model.

## Platform UI interoperability

HuxerUI cannot and should not reimplement every native UI capability. Camera previews, maps, web content, video surfaces, advertisements, rich text editors, and proprietary payment controls require native platform views.

The interoperability model has four directions:

| Requirement | HuxerUI mechanism |
| --- | --- |
| UI drawn by HuxerUI | `View`, `Layout`, and `Modifier` |
| Native UI embedded in HuxerUI | `NativeView` |
| Platform capability without an inline view | typed service or controller |
| HuxerUI embedded in a native application | platform-native `HuxerUIView` |

Together these mechanisms make the framework extensible across platform UI boundaries without turning every platform API into a Runtime feature.

## NativeView

`NativeView` is a real leaf View in the HuxerUI tree. It participates in identity, reconciliation, measurement, layout, visibility, hit testing, focus, accessibility, and lifecycle. It is not a Modifier and is not a `NodeExtension`.

A module normally hides the low-level primitive behind a typed component:

```cpp
auto camera = UseCamera();

return Stack{
    CameraPreview(camera).With(Fill()),
    Button("Capture").OnClick([camera] {
      camera.Capture();
    }),
};
```

The module can implement `CameraPreview` using a strongly typed native descriptor:

```cpp
return NativeView<CameraPreviewNative>({
    .session = camera.Session(),
});
```

The exact descriptor API is subject to implementation design. The public API must avoid an application-facing combination of string view types and untyped property maps.

### Native view lifecycle

The Runtime owns declarative lifecycle decisions:

```text
mount       create native instance
recompose   update native properties
measure     query optional intrinsic size
layout      apply HuxerUI-owned bounds
visibility  attach, show, hide, or detach
unmount     destroy native instance
```

When node identity and native descriptor type match, reconciliation preserves the native instance and updates its properties. A changed key, changed descriptor type, or removed node destroys the old instance.

HuxerUI owns layout. A native view must not independently change the frame assigned by its MountedNode.

The MountedNode stores a stable opaque native handle. It does not store Java, Objective-C, Swift, AppKit, UIKit, WinUI, or Win32 types in common public headers.

`Runtime` decides when an operation is required. `PlatformAdapter` performs the operation against the real native object. No additional native host abstraction is required.

### Measurement

A native view may return an intrinsic size for a HuxerUI constraint proposal. Views without meaningful intrinsic content, including camera previews, can accept their assigned size and rely on normal HuxerUI `Frame`, `Fill`, aspect-ratio, and layout modifiers.

Measurement must not create a second native instance. Expensive native measurement should be cached using the same property revision that controls updates.

### Composition

The minimum useful camera scenario is:

```cpp
return Stack{
    CameraPreview(camera),
    CameraControls(),
};
```

The native preview must appear below HuxerUI controls. A platform host therefore needs a compositing container rather than assuming that the HuxerUI drawing view is always the only native child.

The initial contract supports:

- Axis-aligned rectangular placement.
- Rectangular clipping.
- Visibility and opacity when the platform can apply them correctly.
- HuxerUI content painted over an embedded native view.
- Native input and accessibility within the native view's visible region.

The initial contract does not promise:

- Arbitrary three-dimensional transforms.
- Shader effects applied across a native view.
- Backdrop filters sampling native view content.
- Cheap embedding of many native views in a virtualized list.
- Identical native view composition performance on every platform.

These limitations must be explicit. Unsupported visual modifiers produce a diagnostic or well-defined fallback instead of silently rendering incorrect content.

A future GPU renderer may add an external surface primitive for camera and video textures. The framework should not expose a second public surface API until its ownership, synchronization, fallback, and renderer semantics are defined.

### Input and focus

The host container coordinates native and HuxerUI input:

- A HuxerUI interactive overlay receives input before the native view below it.
- Input in an uncovered native region is delivered to the native view.
- Pointer cancellation is sent when gesture ownership changes.
- Keyboard focus transfers between HuxerUI nodes and native controls.
- IME ownership follows the focused control.
- Native accessibility nodes remain reachable in the composed hierarchy.

The Runtime remains the source of HuxerUI hit-test ordering. The PlatformAdapter remains the source of native event dispatch.

### Events and commands

Native events enter the existing typed View event system:

```cpp
CameraPreview(camera)
    .On<CameraReady>([](CameraInfo info) {
    })
    .On<CameraError>([](CameraError error) {
    });
```

Imperative operations use a stable typed controller:

```cpp
camera.Start();
camera.Capture();
camera.Stop();
```

Commands are not represented as generic string calls. The controller preserves the platform session independently of a particular preview node, so recomposition or preview replacement does not unnecessarily reopen the camera.

## Platform services and native presentation

Not every platform capability is an inline view. Permissions, file access, notifications, sensors, capture operations, and full native screens belong in typed services or controllers supplied by a module.

A native image picker can expose:

```cpp
auto picker = UseImagePicker();

return Button("Choose image").OnClick([picker] {
  picker.Present();
});
```

The service presents from the current native Activity, view controller, window, or equivalent platform owner supplied by `PlatformAdapter`.

A full native picker, payment sheet, or system camera is not represented as a fake HuxerUI Dialog or an inline NativeView. Its lifecycle is owned by the native presentation system, while its result is delivered back through a typed asynchronous API.

Services must define cancellation, owner destruction, repeated presentation, and background behavior. A callback arriving after the requesting runtime or scope has been destroyed must not access invalid state.

## HuxerUIView

Native interoperability must work in both directions. Every supported platform should expose a native host view that embeds one HuxerUI Runtime:

```text
Android     HuxerUIView backed by View
macOS       HuxerUIView backed by NSView
Windows     HuxerUIView backed by the selected native window system
iOS         HuxerUIView backed by UIView
```

This supports:

- Embedding HuxerUI content in an existing native application.
- Incremental migration from native UI.
- Native screens containing reusable HuxerUI panels.
- Multiple independent HuxerUI trees in one process.

Each host view owns an independent Runtime, frame scheduler, viewport, input state, and composition tree. Host views may share an immutable root factory but must not share mounted state.

Full-screen `HUXERUI_APP` applications use the same host view internally rather than a separate rendering implementation.

## Module lifecycle

Modules can declare participation in platform application lifecycle when their services require it:

```text
application created
foreground
background
configuration changed
low-memory notification
application destroyed
```

The generated platform application forwards only the supported lifecycle events through registered module capabilities.

Lifecycle integration is not a reason to expose the entire Runtime to every module. A camera service receives the native ownership and lifecycle state it requires, while an ordinary component-only module receives no lifecycle callbacks.

## Threading and asynchronous safety

The interoperability contract requires:

- Native view creation, update, layout, and destruction on the platform UI thread.
- Runtime mutation on the Runtime's owning thread.
- Explicit marshaling of native callbacks before they affect HuxerUI state.
- Cancellation or invalidation tokens for callbacks that outlive a node, scope, host view, or application.
- No synchronous wait from the UI thread for work that requires the same UI thread.

Module APIs should use typed result and error values. Unsupported platform behavior is represented explicitly rather than as a missing registration or a null native handle.

## Capability detection

A common API can expose different runtime capabilities on different devices:

```cpp
if (Camera::IsAvailable()) {
  return CameraPreview(camera);
}

return Text("Camera is unavailable");
```

Modules can provide richer capability values:

```text
available camera positions
supported resolutions
flash availability
video support
permission status
```

Build-time platform support and runtime device capability are separate. `module.platforms` answers whether an implementation exists for the target. Runtime capability APIs answer whether the current device can perform the operation.

## Error behavior

CLI and module failures must preserve their source:

- Manifest schema errors include the file, key, and invalid value.
- Unsupported host and target combinations identify both platforms.
- Toolchain diagnostics identify the missing or incompatible tool.
- Underlying CMake and Gradle failures remain accessible in verbose logs.
- Module version conflicts include the dependency path.
- Unsupported native visual behavior identifies the component and modifier.
- Missing native registrations fail before the first frame when possible.

Multi-target commands print one result per requested target and return a non-zero exit status if any requested target fails.

## CLI implementation

The CLI should be distributed as a standalone native executable. Rust is the recommended implementation language because the tool requires reliable cross-platform process execution, TOML and JSON handling, archive management, hashing, downloads, concurrent builds, and structured diagnostics.

The CLI implementation language is not part of the application SDK contract. SDK users do not install the CLI's compiler or package manager.

The existing C++ scope code generator remains an independent host tool. It is invoked by the exported CMake integration and does not need to be merged into the CLI process.

An all-C++ CLI remains possible if repository language uniformity is more important than tooling implementation cost. This choice does not affect the project manifest, SDK layout, module model, or command surface.

## Delivery sequence

The foundation milestone contains:

- CMake target export and `HuxerUIConfig.cmake`.
- Installed host code generator resolution.
- `huxerui_add_app`.
- Stable project and module manifest schemas.
- Project creation and environment diagnostics.

The local workflow milestone contains:

- Managed macOS project generation.
- Managed Android project generation using the SDK AAR and Prefab package.
- Windows application generation.
- Incremental `build` and single-target `run`.
- Local `package` and normalized artifacts.

The module milestone contains:

- Local module resolution.
- Target-specific dependency and permission merging.
- Generated static registration.
- Typed platform services.
- Module lifecycle forwarding.

The native UI milestone contains:

- NativeView lifecycle and reconciliation.
- Host containers capable of embedded native children.
- Layout, visibility, input, focus, and accessibility integration.
- Camera preview as the reference native module.

The distribution milestone contains:

- Versioned SDK installation and selection.
- Published target packages.
- Signing profiles.
- Remote module resolution.
- Remote multi-host packaging.

Each milestone preserves direct CMake access and must leave the project usable without runtime dependence on the CLI.

## Final design constraints

The implementation should preserve these constraints:

- One common `HUXERUI_APP` definition.
- One Runtime implementation.
- One PlatformAdapter boundary per native host view.
- CMake and native toolchains remain authoritative.
- Managed generated projects are reproducible and do not overwrite user-owned platform files.
- Modules are build-time units, not dynamically loaded plugins.
- Module platform support uses an explicit `platforms` array.
- NativeView is a real node, not a Modifier or NodeExtension.
- Native UI events use typed HuxerUI events.
- Native commands use typed services and controllers.
- Common application code does not require untyped method names or property maps.
- New platform capabilities should normally be delivered by modules rather than by adding feature-specific branches to Runtime.
