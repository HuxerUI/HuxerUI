<p align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/HuxerUI-logo-dark.png"><source media="(prefers-color-scheme: light)" srcset="docs/HuxerUI-logo-light.png"><img src="docs/HuxerUI-logo-light.png" width="220" alt="HuxerUI logo"></picture></p>

<h1 align="center">HuxerUI</h1>

<p align="center"><strong>Declarative, native, cross-platform UI in modern C++.</strong></p>

<p align="center">One runtime. Platform integration. Shared application code.</p>

<p align="center"><a href="docs/getting-started.md">Getting Started</a> · <a href="docs/core-concepts.md">Core Concepts</a> · <a href="docs/design/architecture.md">Architecture</a> · <a href="docs/roadmap.md">Roadmap</a></p>

HuxerUI brings a functional, declarative UI model to C++20. Windows, macOS, Linux, Web, Android, and iOS share the same state, recomposition, layout, input, scrolling, text editing, animation, and retained-scene runtime while retaining platform-specific integration, text systems, and renderers.

## Why HuxerUI

| Declarative C++ | Shared Runtime | Platform Integration |
|---|---|---|
| Compose interfaces with ordinary C++ functions, typed state, events, themes, and modifiers. | Reuse one implementation of reconciliation, layout, interaction, virtualization, animation, and text editing. | Integrate through Win32, AppKit, X11, an Emscripten Canvas, Android View, or UIKit while preserving platform services. |

HuxerUI includes Row, Column, Flow, Stack, ScrollView, virtual lists and grids, responsive viewport classes, Tabs, NavigationBar, NavigationPane, DrawerLayout, factory and typed routed NavigationStack flows, controlled text editing, selection, validation, Flat and Material themes, retained animation, shadows, Canvas and Path drawing, typed app resources, Image, Toast, Dialog, BottomSheet, Popup, Menu, custom layouts, and typed extension points.

## Quick Start

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
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
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
A desktop shell can use:

```cpp
#include <huxerui/app.h>

int main() {
  return huxerui::RunApplication();
}
```

Add the common declaration and the selected platform entry to the application target.
The helper links HuxerUI and enables scope generation:

```cmake
huxerui_add_app(my_app
        SOURCES
            src/app.cpp
            platform/main.cpp
)
```

Build the repository on macOS or Linux:

Linux requires system development packages, including libsoup 3, to be installed before CMake configuration; see [Getting Started](docs/getting-started.md#build-the-repository) for distribution-specific commands.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Top-level builds also produce the `huxerui` CLI:

```bash
huxerui create app hello_huxer --platform windows,macos,linux,web,android,ios
huxerui doctor
huxerui setup android,web
huxerui devices ios
huxerui open ios
huxerui build windows
huxerui run windows
huxerui run linux
huxerui package windows,web
```

The CLI creates and validates source-controlled platform shells, diagnoses and sets up external toolchains, discovers Android and iOS devices, builds and launches enabled targets, and collects release artifacts under `dist/<platform>`.
The current CMake install exports a platform-specific SDK package, the CLI, host code generators, and built-in resources.
Tagged releases produce verified macOS arm64, macOS x86_64, Windows x86_64, and Linux x86_64 portable SDK archives with shell and PowerShell installers.
Each SDK includes Android Java and ABI artifacts plus the pinned Emscripten Web library, so Android and Web projects can consume an installed SDK without framework sources.
Android Gradle shells own their platform configuration and invoke the application root `CMakeLists.txt` directly.
The distribution model uses one relocatable SDK selected through `HUXERUI_HOME` or CLI self-location, preserves one root-CMake contract for source and installed use, and merges framework, library, and application resources into one final package.

Build a complete SDK archive for the current host, including Android and Web target artifacts, with the repository packaging script:

```powershell
.\scripts\package_sdk.ps1
```

```bash
sh scripts/package_sdk.sh
```

The scripts require the current platform toolchain, Android SDK and NDK, Java, and the pinned Emscripten version.

See [Getting Started](docs/getting-started.md) for application setup, platform builds, CMake options, code generation, and example launch commands.

## Platform Support

| Platform | Status | Platform integration |
|---|---|---|
| Windows | Supported | Win32, D3D11, Direct2D, DirectWrite |
| macOS | Supported | AppKit, CoreGraphics, CoreText, NSTextInputClient |
| Linux | Supported | X11, Cairo, EGL/OpenGL ES, FreeType, HarfBuzz, XIM, optional Fcitx5 DBus preedit |
| Web | Supported | Emscripten, WebAssembly, Canvas 2D, browser text input |
| Android | Supported | View, Canvas, StaticLayout, InputConnection |
| iOS | Supported | UIKit, CoreGraphics, CoreText, UITextInput |
| OHOS | Planned | Shared Runtime with platform-specific adapters |

See [Platform Support](docs/platform-support.md) for backend responsibilities and integration details.

## Documentation

### User guide

| Document | Contents |
|---|---|
| [Getting Started](docs/getting-started.md) | First app, CMake setup, builds, and examples |
| [Core Concepts](docs/core-concepts.md) | Views, scopes, state, Lifecycle, Tasks, keys, events, modifiers, and Environment |
| [Layout and Scrolling](docs/layout-and-scrolling.md) | Constraints, ScrollView, controllers, virtualization, and custom layout |
| [Components and Input](docs/components-and-input.md) | Controls, focus, selection, TextField, validation, and IME behavior |
| [Theme, Animation, and Presentation](docs/theme-animation-and-presentation.md) | Themes, styles, animation, layers, and typed presentation services |
| [Files and Application Storage](docs/files.md) | Local files, application directories, errors, and asynchronous I/O |
| [Extending HuxerUI](docs/extending-huxerui.md) | Custom layouts, modifiers, NodeExtension, root services, and platform adapters |
| [Platform Support](docs/platform-support.md) | Platform backends and Runtime boundaries |
| [Roadmap](docs/roadmap.md) | Framework, platform, SDK, and distribution work |

### Design documents

| Document | Contents |
|---|---|
| [Architecture Design](docs/design/architecture.md) | Runtime, MountedNode, modifiers, animation, Theme, and layers |
| [Application Activation and Lifecycle Design](docs/design/application.md) | Startup input, subsequent activation, Runtime delivery, and future lifecycle state |
| [Animation and Scene Transition Design](docs/design/animation.md) | Timing, retained motion, synchronized properties, and frozen-scene transitions |
| [Task and Structured Concurrency Design](docs/design/tasks.md) | C++20 Task values, scope ownership, cancellation, and UI-thread resumption |
| [HTTP Client Design](docs/design/http.md) | Typed requests, Task cancellation, UI-thread resumption, and platform transports |
| [File and Application Storage Design](docs/design/files.md) | Local files, application directories, external references, file pickers, and Task-based I/O |
| [Incremental Layout and Rendering Design](docs/design/incremental-rendering.md) | Local geometry, invalidation, retained rendering, and damage |
| [Canvas and Path Design](docs/design/canvas.md) | Vector paths, custom drawing, platform replay, and invalidation |
| [Text and Font Design](docs/design/text.md) | Fonts, styles, measurement, paragraph drawing, and exact text runs |
| [App Resources, Images, and Localization Design](docs/design/resources.md) | Typed resources, Image, raw assets, packaging, locale, and formatted strings |
| [Text Input and TextField Design](docs/design/text-input.md) | Shared editing protocol and platform adapter contracts |
| [Semantics and Accessibility Design](docs/design/semantics.md) | Semantic declarations, committed frames, actions, component defaults, and platform accessibility mapping |
| [Navigation Design](docs/design/navigation.md) | Page stacks, typed routes, scoped controllers, transitions, Back routing, and Web URL history |
| [Window Insets and System Bars Design](docs/design/window-insets.md) | Safe-area layout, edge-to-edge content, system-bar theming, and mobile platform mapping |
| [Window Chrome Design](docs/design/window-chrome.md) | Desktop title-bar ownership, application content, standard controls, and platform fallbacks |
| [Scope Code Generation Design](docs/design/scope-codegen.md) | Scope attribute transformation and build integration |
| [SDK, CLI, Platform Shell, and Library Design](docs/design/sdk-cli.md) | Project tooling, distribution, libraries, and PlatformView |
| [Web Platform Design](docs/design/web.md) | Emscripten, Canvas rendering, browser input, resources, and accessibility |

## Examples

| Target | Demonstrates |
|---|---|
| `example_counter` | Component scopes and local state |
| `example_ui_gallery` | Responsive drawer shell, controls, layout, motion, and theme tools |
| `example_dynamic_list` | `ForEach`, stable keys, and per-item state |
| `example_scroll_view` | Nested scrolling, metrics, controllers, and retained state |
| `example_virtual_list` | Variable-height virtualization and item positioning |
| `example_horizontal_virtual_list` | Horizontal fixed-extent virtualization |
| `example_virtual_grid` | Adaptive columns, spans, and large data sets |
| `example_custom_event` | Typed custom component events |
| `example_presentation` | Toast, Dialog, BottomSheet, Popup, and Menu presentation |
| `example_theme` | Material, Flat, nested themes, and style precedence |
| `example_tabs` | Controlled Tabs, disabled items, theme policies, and viewport classes |
| `example_navigation` | Factory and typed page stacks, Back routing, transitions, and Web URL history |
| `example_environment` | Typed defaults, inheritance, and nested overrides |
| `example_application` | Cold and subsequent URL or file activation on Windows and Android |
| `example_lifecycle` | Component setup, dependency restarts, and unmount cleanup |
| `example_task` | Coroutine tasks, structured cancellation, Lifecycle launch, and direct State updates |
| `example_http` | Windows, macOS, iOS, Linux, Android, and Web platform HTTP requests, Task resumption, response status, and transport errors |
| `example_files` | macOS, Linux, iOS, Android, and Web application directories, UTF-8 local files, external pickers, asynchronous operations, persistence, and Task resumption |
| `example_canvas` | Tabbed Canvas effects, retained transforms, paths, clipping, and shadows |
| `example_image` | Raster variants, compiled SVG resources, VectorAsset tint, localized strings, and Image fitting |
| `example_window_chrome` | Application-defined desktop title-bar content with platform-appropriate window controls |
| `example_platform_module` | Windows, macOS, Linux, Web, Android, and iOS typed platform services, plus Windows/Linux RGBA/BGRA, WebCodecs `VideoFrame`, Android `Bitmap`, and Apple `CVPixelBuffer` ExternalTexture streams |
| `example_platform_view` | Windows, macOS, Web, Android, and iOS `PlatformTextField` integration with HuxerUI layout, state, events, and rendering order |
| `platform/android/example_runner` | Android example selection, platform integration, and application packaging |

## Architecture

```text
declarative components and State
  -> ViewSpec
  -> reconciliation
  -> MountedNode
  -> measure, layout, input, and animation
  -> RenderScene
  -> platform renderer
```

The platform layer owns the system window or host View, frame scheduling, input services, text services, and drawing surface. Shared application code does not depend on platform UI objects.

Explore the complete runtime and extension model in [Architecture Design](docs/design/architecture.md).

## License

HuxerUI is available under the terms in [LICENSE](LICENSE).
