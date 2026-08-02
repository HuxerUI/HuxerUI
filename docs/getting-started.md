# Getting Started

HuxerUI applications use C++20 and share the same declarative UI code across Android, Linux, macOS, and Windows. The platform-independent runtime owns state, recomposition, layout, input routing, and retained-scene generation; each native backend owns its window or host view, text services, and rendering surface.

## Requirements

- CMake 3.20 or later
- A C++20 compiler
- The native toolchain for the target platform
- Android SDK and Gradle for Android builds

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
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE HuxerUI::huxerui)
huxerui_enable_codegen(my_app)
```

Call `huxerui_enable_codegen()` after adding all sources to the target. The code generator detects `[[huxerui::scope]]` in `.cpp`, `.cc`, and `.cxx` definitions and generates the scope boundary before compilation.

## App resources

Place packaged resources under one target-owned root:

```text
assets/
  images/logo.png
  images/logo@2x.png
  images/logo@3x.png
  raw/config.json
  strings/default.properties
  strings/zh.properties
```

String catalogs are UTF-8 `.properties` files with `key = value` entries and indexed placeholders such as `{0}`.
Image scale suffixes must preserve the same intrinsic logical size; for example, 418-pixel, 836-pixel, and 1254-pixel square images form matching 1x, 2x, and 3x variants.

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

return Column {
  Text::Format(app_resources::strings::welcome, "Ada"),
  Image(logo).Fit(ImageFit::Contain),
};
```

See [App Resources, Images, and Localization Design](design/resources.md) and `example_image` for the complete contract.

## Build the repository

The following commands use `build` as the build directory.

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The Linux backend requires system packages for X11, XKB common, Vulkan, Cairo, FreeType, HarfBuzz, fontconfig, libpng, and libjpeg, resolved through pkg-config.
When `tools/prebuilt/linux/<architecture>/` host tools are absent, CMake builds the code generators from their `tools/` sources on the Linux host.

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

The Android project contains the reusable `huxerui` library and a `demo` application.
The demo uses `ui_gallery` by default and accepts any example directory through the `huxeruiDemoExample` Gradle property.
It adds that example with CMake `add_subdirectory()`, emits `libhuxerui_app.so`, and registers the example's generated resources as variant assets.
Cross-compilation resolves the matching host code generators from `tools/prebuilt/<system>/<architecture>`.

## Run examples

On Linux:

```bash
./build/bin/example_counter
./build/bin/example_ui_gallery
```

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
| `HUXERUI_WINDOWS_7_COMPAT` | `OFF` | Build the Windows backend for Windows 7 SP1 with Platform Update |

