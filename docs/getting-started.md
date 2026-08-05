# Getting Started

HuxerUI applications use C++20 and share the same declarative UI code across Android, iOS, macOS, Windows, and Web. The platform-independent runtime owns state, recomposition, layout, input routing, and retained-scene generation; each native backend owns its window or host view, text services, and rendering surface.

## Requirements

- CMake 3.20 or later
- A C++20 compiler
- The native toolchain for the target platform
- Android SDK and Gradle for Android builds
- Xcode and an installed iOS Simulator runtime or paired iOS device for iOS builds

The repository vendors the Catch2 sources used by its tests, so a normal configure does not download test dependencies.

## First application

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::scope]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("+1").OnClick([count] {
      count += 1;
    }),
  }.With(Spacing(12.0F));
}

View App() {
  return MaterialTheme(Counter);
}

HUXERUI_APP(
    App,
    {
        .title = "Counter",
        .width = 480.0F,
        .height = 320.0F,
    }
)
```

`HUXERUI_APP` generates the desktop entry point or registers the application definition for a mobile host. The root already owns an implicit scope. `[[huxerui::scope]]` is needed only when a component requires its own local state and recomposition boundary.

## CMake target

```cmake
huxerui_add_app(my_app
        SOURCES
            main.cpp
)
```

`huxerui_add_app()` creates the platform-appropriate application target, links HuxerUI, and enables scope code generation after all declared sources are known.
Advanced embedded targets may still create their target directly and call `huxerui_enable_codegen()` after adding all sources.
The code generator detects `[[huxerui::scope]]` in `.cpp`, `.cc`, and `.cxx` definitions and generates the scope boundary before compilation.

## App resources

Place packaged resources under one target-owned root:

```text
assets/
  images/logo.png
  images/logo@2x.png
  images/logo@3x.png
  images/mark.svg
  raw/config.json
  strings/default.properties
  strings/zh.properties
```

String catalogs are UTF-8 `.properties` files with `key = value` entries and indexed placeholders such as `{0}`.
Raster image scale suffixes must preserve the same intrinsic logical size; for example, 418-pixel, 836-pixel, and 1254-pixel square images form matching 1x, 2x, and 3x variants.
SVG files are compiled into platform-neutral vector payloads and do not use density suffixes.

Register the root after creating the target:

```cmake
huxerui_add_resources(my_app
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        NAMESPACE "app"
)
```

The generated `app_resources.h` contains typed ImageResource, RawResource, and StringResource constants.
Desktop targets stage the generated package beside the executable or inside the application bundle.
Android CMake builds generate a resource package inside each ABI build directory.
The Gradle integration waits for native builds, selects one package, and synchronizes it into a generated assets
source so concurrent ABI builds never mutate the same directory.

```cpp
#include <app_resources.h>

const ImageAsset logo = UseImage(app_resources::images::logo);
const VectorAsset mark = UseVectorImage(app_resources::images::mark);

return Column {
  Text::Format(app_resources::strings::welcome, "Ada"),
  Image(logo).Fit(ImageFit::Contain),
  Image(mark).Tint(Color::Rgb(132, 78, 255)),
};
```

See [App Resources, Images, and Localization Design](design/resources.md) and `example_image` for the complete contract.

## Build the repository

The following commands use `build` as the build directory.

macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

CMake may use any generator supported by the current machine. Pass the desired generator, toolset, and architecture explicitly when the target requires them.

Android:

```bash
cd platform/android
./gradlew :demo:assembleDebug
./gradlew :demo:assembleDebug -PhuxeruiDemoExample=image
```

The Android project contains the reusable `HuxerUI` library module and a `demo` application.
The demo uses `ui_gallery` by default and accepts any example directory through the `huxeruiDemoExample` Gradle property.
It adds that example with CMake `add_subdirectory()`, emits `libhuxerui_app.so`, and registers the example's generated resources as variant assets.
Cross-compilation resolves the matching host code generators from `tools/prebuilt/<system>/<architecture>`.

iOS applications use the source-controlled Xcode project created by the CLI:

```bash
huxerui create hello_huxer --platform ios
cd hello_huxer
huxerui open ios
huxerui build ios
huxerui run ios --device <id>
```

The native project owns its App target, Info.plist, launch screen, asset catalog, build configurations, signing, and shared scheme.
Its build phase asks CMake for an architecture-correct application core containing the C++ application, generated scope code, generated resources, and HuxerUI static library.
The resulting App Bundle remains a normal Xcode product and can be built, debugged, archived, and extended with native files in Xcode.

`huxerui open ios` records the detected SDK location in the ignored `platform/ios/Config/Local.xcconfig` before opening Xcode. Only optional local signing settings need manual configuration:

```xcconfig
DEVELOPMENT_TEAM = YOUR_TEAM_ID
```

`huxerui build` and `huxerui run` pass the detected SDK location to Xcode automatically. Generated projects never require a source-controlled machine-specific SDK path.
The iOS backend targets iOS 13 or later. Development builds support Xcode automatic signing; archive export automation and a public embeddable UIView remain outside the current preview.

Framework contributors can debug repository examples with the shared native project at `platform/ios/example_runner/HuxerUIExamples.xcodeproj`. It runs `example_ui_gallery` by default. Copy its `Config/Local.xcconfig.example` to the ignored `Config/Local.xcconfig` and change `HUXERUI_APP_TARGET` to select another `example_*` target without creating another Xcode project.

## Project CLI

Top-level repository builds enable the `huxerui` CLI by default:

```bash
cmake --build build --target huxerui_cli --parallel
```

Create a project with source-controlled platform shells:

```bash
huxerui create hello_huxer --platform ios,windows,web
cd hello_huxer
huxerui platform add android
huxerui doctor
huxerui devices ios
huxerui run ios --device <id>
huxerui open ios
huxerui build windows
huxerui run windows
huxerui run web
```

`create` writes the common CMake application, `.gitignore`, `assets/images`, `assets/raw`, the default string catalog, and the selected platform shells.
The generated CMake project recursively collects `.cpp`, `.cc`, and `.cxx` files under `src`.
`doctor` discovers the nearest project from a nested directory, validates each platform shell, and checks host tools without changing the project.
`devices` lists runnable Android devices, paired physical iOS devices, and booted iOS Simulators without requiring a project.
`build` preserves native incremental output under `.huxerui/build`, while `run` builds and launches exactly one target platform. iOS Simulator and device builds use separate `ios-simulator` and `ios-device` directories. `xcodebuild` builds the source-controlled native project, `simctl` installs Simulator builds, and `devicectl` installs automatically signed physical-device builds.
`huxerui open ios` records the ignored local SDK setting and opens `platform/ios/<target>.xcodeproj` without regenerating the native project.
`run android` selects the only ready device automatically or requires `--device <id>` when several are available.
For a fresh desktop build the CLI selects Ninja when available; `--generator <name>`, `CMAKE_GENERATOR`, and an existing CMake cache take precedence.
The CLI locates a compatible installed SDK beside its executable or uses `HUXERUI_SDK_ROOT` to select a source checkout or installed prefix explicitly.
Android and Web CLI projects currently require a source SDK. iOS accepts a source checkout or an installed SDK built for the selected Apple SDK and architectures.
Android includes that checkout's `HuxerUI` Gradle module directly, while Web compiles the framework and application together through Emscripten.
The generated Android shell uses a local Gradle wrapper when the project supplies one and otherwise requires `gradle` on `PATH`.

Versioned mobile distribution artifacts, installed Android artifacts, package commands, and module resolution remain staged work.
Their native integration contracts are defined in [SDK, CLI, Native Shell, and Module Design](design/sdk-cli.md).

## Run examples

On macOS:

```bash
open build/bin/example_counter.app
open build/bin/example_ui_gallery.app
```

On Windows:

```powershell
.\build\bin\Debug\example_counter.exe
.\build\bin\Debug\example_ui_gallery.exe
```

See the [README](../README.md#examples) for the complete example index.

## CMake options

| Option | Default | Description |
|---|---:|---|
| `HUXERUI_BUILD_SHARED` | `ON` | Build the shared library |
| `HUXERUI_BUILD_STATIC` | `ON` | Build the static library |
| `HUXERUI_BUILD_TESTS` | `ON` for the top-level project | Build tests |
| `HUXERUI_BUILD_EXAMPLES` | `ON` for the top-level project | Build examples |
| `HUXERUI_BUILD_CLI` | `ON` for the top-level project | Build the `huxerui` project CLI |
| `HUXERUI_WINDOWS_7_COMPAT` | `OFF` | Build the Windows backend for Windows 7 SP1 with Platform Update |
