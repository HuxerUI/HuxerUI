# Getting Started

HuxerUI applications use C++20 and share the same declarative UI code across Windows, macOS, Linux, Web, Android, and iOS. The platform-independent runtime owns state, recomposition, layout, input routing, and retained-scene generation; each platform backend owns its window or host view, text services, and rendering surface.

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

const Application application{
    App,
    {
        .window = {
            .title = "Counter",
            .initial_size = {480.0F, 320.0F},
        },
    }
};
```

The platform shell owns the process entry point.
For a desktop shell, its `main.cpp` can be:

```cpp
#include <huxerui/app.h>

int main() {
  return huxerui::RunApplication();
}
```

The static `Application` declares the process-level root and `AppOptions`; its constructor makes the declaration available to the platform shell without requiring a fixed C++ variable or factory name. Desktop platform projects call `RunApplication()` from their native entry point, while Android and Web create Runtime sessions from the same registered application after loading its native library. The root already owns an implicit scope. `[[huxerui::scope]]` is needed only when a component requires its own local state and recomposition boundary.

## CMake target

```cmake
huxerui_add_app(my_app
        SOURCES
            src/app.cpp
            platform/main.cpp
)
```

`huxerui_add_app()` creates the platform-appropriate application target, links HuxerUI, and enables scope code generation after all declared sources are known.
CLI-generated projects select `platform/windows/main.cpp`, `platform/macos/main.cpp`, or `platform/linux/main.cpp` for the current desktop host; Web and Android are hosted, while iOS owns the corresponding Objective-C++ entry.
Advanced embedded targets may still create their target directly and call `huxerui_enable_codegen()` after adding all sources.
The code generator detects `[[huxerui::scope]]` in `.cpp`, `.cc`, and `.cxx` definitions and generates the scope boundary before compilation.

## App resources

Place packaged resources under one target-owned root:

```text
resources/
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

Register additional roots after creating the application with `huxerui_add_app()`:

```cmake
huxerui_add_resources(my_app
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        NAMESPACE "app"
)
```

The generated `app_resources.h` contains typed ImageResource, RawResource, and StringResource constants.
`huxerui_add_app()` also registers the installed `huxerui` resource package before application resources so framework defaults participate in the same final package and may be overridden by a later root in the `huxerui` namespace.
Manually created targets do not receive that framework package implicitly.
Windows and Linux stage the generated package beside the executable, while macOS places it inside the application bundle.
Android CMake builds generate a resource package inside each ABI build directory.
The Gradle integration waits for native builds, selects one package, and synchronizes it into a generated assets
source so concurrent ABI builds never mutate the same directory.

```cpp
#include <app_resources.h>

const ImageAsset logo = UseImage(app::images::logo);
const VectorAsset mark = UseVectorImage(app::images::mark);

return Column {
  Text::Format(app::strings::welcome, "Ada"),
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

Linux:

Linux system and build dependencies must be installed before configuring the project; CMake does not download them.
The following commands install the required compiler, build tools, X11/EGL libraries, and libsoup 3 development files on common distributions.

Debian or Ubuntu:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake git pkg-config meson ninja-build gperf nasm \
    libx11-dev libxext-dev libxkbcommon-dev libxrandr-dev \
    libegl1-mesa-dev libgles2-mesa-dev libsoup-3.0-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake git pkgconf-pkg-config meson ninja-build gperf nasm \
    libX11-devel libXext-devel libxkbcommon-devel libXrandr-devel \
    libglvnd-devel libsoup3-devel
```

Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake git pkgconf meson ninja gperf nasm \
    libx11 libxext libxkbcommon libxrandr libglvnd libsoup3
```

`libsoup-3.0` is a required system dynamic library and is not part of HuxerUI's fetched static dependency stack.
The selected package must provide `libsoup-3.0.pc`, the headers, and the shared library; GLib/GIO and TLS support are installed through its distribution dependencies.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The Linux backend resolves the manually installed X11, Xext, XKB common, XRandR, EGL, OpenGL ES 2, and libsoup 3 packages through pkg-config.
Source-checkout builds fetch the pinned Cairo, FreeType, HarfBuzz, fontconfig, pixman, libpng, libjpeg, zlib, and expat stack and require the upstream build tools reported by CMake.
Host tools are distributed as prebuilt executables under `tools/prebuilt/linux/<architecture>/` and CMake stops configuration when a matching host package is unavailable.

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
./gradlew :example_runner:assembleDebug
./gradlew :example_runner:assembleDebug -PhuxeruiExample=image
```

The Android project contains the reusable `HuxerUI` library module and an `example_runner` application.
The runner uses `ui_gallery` by default and accepts any example directory through the `huxeruiExample` Gradle property.
It adds that example with CMake `add_subdirectory()`, includes its optional Android Java source set, emits `libhuxerui_app.so`, and registers the example's generated resources as variant assets.
Cross-compilation resolves the matching host code generators from `tools/prebuilt/<system>/<architecture>`.

iOS applications use the source-controlled Xcode project created by the CLI:

```bash
huxerui create app hello_huxer --platform ios
cd hello_huxer
huxerui open ios
huxerui build ios
huxerui run ios --device <id>
```

The Xcode project owns its App target, Info.plist, launch screen, asset catalog, build configurations, signing, and shared scheme.
Its build phase asks CMake for an architecture-correct application core containing the C++ application, generated scope code, generated resources, and HuxerUI static library.
The resulting App Bundle remains a normal Xcode product and can be built, debugged, archived, and extended with native files in Xcode.

`huxerui open ios` records the detected SDK location in the ignored `platform/ios/Config/Local.xcconfig` before opening Xcode. Only optional local signing settings need manual configuration:

```xcconfig
DEVELOPMENT_TEAM = YOUR_TEAM_ID
```

`huxerui build` and `huxerui run` pass the detected SDK location to Xcode automatically. Generated projects never require a source-controlled machine-specific SDK path.
The iOS backend targets iOS 13 or later. Development builds support Xcode automatic signing; archive export automation and a public embeddable UIView remain outside the current preview.

Framework contributors can debug repository examples with the shared Xcode project at `platform/ios/example_runner/HuxerUIExamples.xcodeproj`. It runs `example_ui_gallery` by default. Copy its `Config/Local.xcconfig.example` to the ignored `Config/Local.xcconfig` and change `HUXERUI_APP_TARGET` to select another `example_*` target without creating another Xcode project.

## Project CLI

Top-level repository builds enable the `huxerui` CLI by default:

```bash
cmake --build build --target huxerui_cli --parallel
```

Create a project with source-controlled platform shells:

```bash
huxerui create app hello_huxer --platform windows,web,ios
cd hello_huxer
huxerui platform add android
huxerui doctor
huxerui devices ios
huxerui run ios --device <id>
huxerui open ios
huxerui build windows
huxerui run windows
huxerui run linux
huxerui run web
```

`create app` writes the common CMake application, `.gitignore`, `resources/images`, `resources/raw`, the default string catalog, and the selected platform shells.
Use `--id <reverse-domain-id>` to set one exact cross-platform application identifier instead of the editable `com.example.<normalized-name>` default.
`create module` writes a common CMake module and an ordinary application under `examples/preview`.
Module names do not require a `huxerui-` prefix and may contain uppercase letters; the supplied directory name is preserved while common identifiers are normalized to lowercase snake case.
Selecting Linux creates the module's CMake `platform/linux/src` root, selecting Android also creates an independent Gradle library, and selecting iOS creates a Swift Package. Android and iOS builds attach consumed platform module packages to the preview application automatically.
Running `doctor`, `build`, `run`, or `open ios` from a module root resolves to this ordinary Preview application; the same commands also work directly inside `examples/preview`.
The generated CMake project recursively collects `.cpp`, `.cc`, and `.cxx` files under `src`, plus Linux sources under `platform/linux/src` when configuring for Linux.
`doctor` discovers the nearest project from a nested directory, validates each platform shell, and checks host tools without changing the project.
`devices` lists runnable Android devices, paired physical iOS devices, and booted iOS Simulators without requiring a project.
`build` preserves platform build output under `.huxerui/build`, while `run` builds and launches exactly one target platform. iOS Simulator and device builds use separate `ios-simulator` and `ios-device` directories. `xcodebuild` builds the source-controlled Xcode project, `simctl` installs Simulator builds, and `devicectl` installs automatically signed physical-device builds.
`huxerui open ios` records the ignored local SDK setting and opens `platform/ios/<target>.xcodeproj` without regenerating the Xcode project.
`run android` selects the only ready device automatically or requires `--device <id>` when several are available.
For a fresh desktop build the CLI selects Ninja when available; `--generator <name>`, `CMAKE_GENERATOR`, and an existing CMake cache take precedence. `--generator` is rejected for Android and iOS because their platform generators belong to Gradle and Xcode.
The CLI selects an explicit source checkout or installed prefix through `HUXERUI_HOME`, otherwise it locates a compatible SDK relative to its executable, and propagates the resolved home to native build tools.
Linux, Web, and Android CLI projects currently require a source SDK. iOS accepts a source checkout or an installed SDK built for the selected Apple SDK and architectures.
Android includes that checkout's Java Gradle library and configures the application root `CMakeLists.txt` directly for its native libraries and final HuxerUI resources, while Web compiles the framework and application together through Emscripten.
Android SDK levels, the NDK version, ABIs, application identity, dependencies, and packaging policy remain entirely in the generated Gradle shell.
The generated Android shell uses a local Gradle wrapper when the project supplies one and otherwise requires `gradle` on `PATH`.

These source-SDK restrictions are transitional rather than the public distribution contract.
The approved model provides one relocatable SDK selected through `HUXERUI_HOME` or CLI self-location, configures every application through its root `CMakeLists.txt`, and stages one final resource package containing framework, module, and application resources.
Formal Windows, macOS, Linux, Web, Android, and iOS artifacts, Windows, macOS, and Linux installers, and package commands are implemented in later phases.
Their platform integration contracts are defined in [SDK, CLI, Platform Shell, and Module Design](design/sdk-cli.md).

## Run examples

On macOS:

```bash
open build/bin/example_counter.app
open build/bin/example_ui_gallery.app
```

On Linux:

```bash
./build/bin/example_counter
./build/bin/example_ui_gallery
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
