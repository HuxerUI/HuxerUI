<p align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/HuxerUI-logo-dark.png"><source media="(prefers-color-scheme: light)" srcset="docs/HuxerUI-logo-light.png"><img src="docs/HuxerUI-logo-light.png" width="220" alt="HuxerUI logo"></picture></p>

<h1 align="center">HuxerUI</h1>

<p align="center"><strong>Declarative, native, cross-platform UI in modern C++.</strong></p>

<p align="center">One runtime. Native integration. Shared application code.</p>

<p align="center"><a href="docs/getting-started.md">Getting Started</a> · <a href="docs/core-concepts.md">Core Concepts</a> · <a href="docs/design/architecture.md">Architecture</a> · <a href="docs/roadmap.md">Roadmap</a></p>

HuxerUI brings a functional, declarative UI model to C++20. Android, Linux, macOS, Windows, and the iOS and Web technical previews share the same state, recomposition, layout, input, scrolling, text editing, animation, and retained-scene runtime while retaining platform-specific integration, text systems, and renderers.

## Why HuxerUI

| Declarative C++ | Shared Runtime | Native Integration |
|---|---|---|
| Compose interfaces with ordinary C++ functions, typed state, events, themes, and modifiers. | Reuse one implementation of reconciliation, layout, interaction, virtualization, animation, and text editing. | Integrate through Android View, UIKit, AppKit, Win32, X11, or an Emscripten Canvas while preserving platform services. |

HuxerUI includes Row, Column, Flow, Stack, ScrollView, virtual lists and grids, responsive viewport classes, Tabs, NavigationBar, NavigationPane, DrawerLayout, NavigationStack, controlled text editing, selection, validation, Flat and Material themes, retained animation, shadows, Canvas and Path drawing, typed app resources, Image, Toast, Dialog, BottomSheet, Popup, Menu, custom layouts, and typed extension points.

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

HUXERUI_APP(
    App,
    {
        .title = "Counter",
        .width = 480.0F,
        .height = 320.0F,
    }
)
```

Add the application target. The helper links HuxerUI and enables scope generation:

```cmake
huxerui_add_app(my_app
        SOURCES
            main.cpp
)
```

Build the repository on macOS or Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Top-level builds also produce the `huxerui` CLI:

```bash
huxerui create hello_huxer --platform android,ios,windows,macos,web
huxerui doctor
huxerui devices ios
huxerui open ios
huxerui build windows
huxerui run windows
```

The CLI creates and validates source-controlled platform shells, including a native Xcode project for iOS, discovers Android and iOS devices, builds enabled Android, iOS Simulator or physical-device, Windows, macOS, and Web targets from compatible hosts, and launches development artifacts.
The CMake install exports a platform-specific SDK package, the CLI, and host code generators.
Android and Web CLI projects currently build against a source SDK checkout. iOS accepts either a source checkout or an installed SDK built for the selected Apple SDK and architectures; versioned mobile distribution artifacts, package commands, and module integration remain staged work.

See [Getting Started](docs/getting-started.md) for application setup, platform builds, CMake options, code generation, and example launch commands.

## Platform Support

| Platform | Status | Native integration |
|---|---|---|
| Android | Supported | View, Canvas, StaticLayout, InputConnection |
| Linux | Supported | X11, Cairo, EGL/OpenGL ES, FreeType, HarfBuzz, XIM |
| iOS | Technical preview | UIKit, CoreGraphics, CoreText, UITextInput |
| macOS | Supported | AppKit, CoreGraphics, CoreText, NSTextInputClient |
| Windows | Supported | Win32, D3D11, Direct2D, DirectWrite |
| Web | Technical preview | Emscripten, WebAssembly, Canvas 2D, browser text input |
| OHOS | Planned | Shared Runtime with platform-specific adapters |

See [Platform Support](docs/platform-support.md) for backend responsibilities and integration details.

## Documentation

### User guide

| Document | Contents |
|---|---|
| [Getting Started](docs/getting-started.md) | First app, CMake setup, builds, and examples |
| [Core Concepts](docs/core-concepts.md) | Views, scopes, state, keys, events, modifiers, and Environment |
| [Layout and Scrolling](docs/layout-and-scrolling.md) | Constraints, ScrollView, controllers, virtualization, and custom layout |
| [Components and Input](docs/components-and-input.md) | Controls, focus, selection, TextField, validation, and IME behavior |
| [Theme, Animation, and Presentation](docs/theme-animation-and-presentation.md) | Themes, styles, animation, layers, and typed presentation services |
| [Extending HuxerUI](docs/extending-huxerui.md) | Custom layouts, modifiers, NodeExtension, root services, and platform adapters |
| [Platform Support](docs/platform-support.md) | Native backends and Runtime boundaries |
| [Roadmap](docs/roadmap.md) | Framework, platform, SDK, and distribution work |

### Design documents

| Document | Contents |
|---|---|
| [Architecture Design](docs/design/architecture.md) | Runtime, MountedNode, modifiers, animation, Theme, and layers |
| [Incremental Layout and Rendering Design](docs/design/incremental-rendering.md) | Local geometry, invalidation, retained rendering, and damage |
| [Canvas and Path Design](docs/design/canvas.md) | Vector paths, custom drawing, native replay, and invalidation |
| [Text and Font Design](docs/design/text.md) | Fonts, styles, measurement, paragraph drawing, and exact text runs |
| [App Resources, Images, and Localization Design](docs/design/resources.md) | Typed resources, Image, raw assets, packaging, locale, and formatted strings |
| [Text Input and TextField Design](docs/design/text-input.md) | Shared editing protocol and native adapter contracts |
| [Semantics and Accessibility Design](docs/design/semantics.md) | Semantic declarations, committed frames, actions, component defaults, and native accessibility mapping |
| [Navigation Design](docs/design/navigation.md) | Page stacks, scoped controllers, transitions, Back routing, and future URL paths |
| [Window Insets and System Bars Design](docs/design/window-insets.md) | Safe-area layout, edge-to-edge content, system-bar theming, and mobile platform mapping |
| [Scope Code Generation Design](docs/design/scope-codegen.md) | Scope attribute transformation and build integration |
| [SDK, CLI, and Module Design](docs/design/sdk-cli.md) | Project tooling, distribution, modules, and NativeView |
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
| `example_navigation` | Page stacks, selection navigation, start and end drawers, Back routing, and transitions |
| `example_environment` | Typed defaults, inheritance, and nested overrides |
| `example_canvas` | Tabbed Canvas effects, retained transforms, paths, clipping, and shadows |
| `example_image` | Raster variants, compiled SVG resources, VectorAsset tint, localized strings, and Image fitting |
| `platform/android/demo` | Android platform integration and application packaging |

## Architecture

```text
declarative components and State
  -> ViewSpec
  -> reconciliation
  -> MountedNode
  -> measure, layout, input, and animation
  -> RenderScene
  -> native renderer
```

The platform layer owns the native window or View, frame scheduling, input services, text services, and drawing surface. Shared application code does not depend on native UI objects.

Explore the complete runtime and extension model in [Architecture Design](docs/design/architecture.md).

## License

HuxerUI is available under the terms in [LICENSE](LICENSE).
