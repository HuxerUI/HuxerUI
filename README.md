# HuxerUI

HuxerUI is a cross-platform declarative UI framework powered by C++20. Android, macOS, and Windows share the same state, recomposition, layout, input, scrolling, text editing, and display-list runtime while retaining native platform hosts and renderers.

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

## Highlights

- Local state and dependency-tracked scope recomposition
- Stable integer, string, and enum node keys
- Typed component events and built-in interaction events
- Row, Column, Flow, Stack, scrolling, virtual lists, and virtual grids
- Public custom layout and virtual-layout protocols
- Controlled single-line and multiline TextField with native IME integration
- Selection, clipboard, validation, secure input, undo, and redo
- Flat and Material light and dark themes
- Retained interaction indications and presentation animation
- Per-window Toast, Dialog, root services, and layers
- Android View, AppKit, and Win32 hosts
- StaticLayout, CoreText, and DirectWrite text layout
- Canvas, CoreGraphics, and Direct2D rendering

## Supported platforms

| Platform | Status | Native integration |
|---|---|---|
| Android | Supported | View, Canvas, StaticLayout, InputConnection |
| macOS | Supported | AppKit, CoreGraphics, CoreText, NSTextInputClient |
| Windows | Supported | Win32, Direct2D, DirectWrite |
| iOS, OHOS, Linux, Web | Planned | Shared Runtime with platform-specific hosts |

See [Platform Support](docs/platform-support.md) for backend responsibilities and integration details.

## Build

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

Android:

```bash
cd platform/android
./gradlew :demo:assembleDebug
```

For application setup, CMake integration, build options, and example launch commands, see [Getting Started](docs/getting-started.md).

## Documentation

### User guide

| Document | Contents |
|---|---|
| [Getting Started](docs/getting-started.md) | First app, CMake setup, builds, and examples |
| [Core Concepts](docs/core-concepts.md) | Views, scopes, state, keys, events, modifiers, and Environment |
| [Layout and Scrolling](docs/layout-and-scrolling.md) | Layout constraints, ScrollView, controllers, virtualization, and custom layout |
| [Components and Input](docs/components-and-input.md) | Controls, focus, selection, TextField, validation, and IME behavior |
| [Theme, Animation, and Presentation](docs/theme-animation-and-presentation.md) | Themes, styles, indications, animation, Toast, and Dialog |
| [Extending HuxerUI](docs/extending-huxerui.md) | Custom layouts, modifiers, NodeExtension, root services, and platform hosts |
| [Platform Support](docs/platform-support.md) | Native backends and Runtime boundaries |
| [Roadmap](docs/roadmap.md) | Framework, platform, SDK, and distribution work |

### Design documents

| Document | Contents |
|---|---|
| [Architecture Design](docs/design/architecture.md) | Runtime, MountedNode, modifiers, animation, Theme, and layers |
| [Text Input and TextField Design](docs/design/text-input.md) | Shared editing protocol and native adapter contracts |
| [Scope Code Generation Design](docs/design/scope-codegen.md) | Scope attribute transformation and build integration |
| [SDK, CLI, and Module Design](docs/design/sdk-cli.md) | Project tooling, distribution, modules, and NativeView |

## Examples

| Target | Demonstrates |
|---|---|
| `example_counter` | Component scopes and local state |
| `example_ui_gallery` | Built-in controls, layout, input, and motion |
| `example_dynamic_list` | `ForEach`, stable keys, and per-item state |
| `example_scroll_view` | Nested scrolling, metrics, controllers, and retained state |
| `example_virtual_list` | Variable-height virtualization and item positioning |
| `example_horizontal_virtual_list` | Horizontal fixed-extent virtualization |
| `example_virtual_grid` | Adaptive columns, spans, and large data sets |
| `example_custom_event` | Typed custom component events |
| `example_toast` | Per-window Toast presentation |
| `example_dialog` | Declarative modal presentation |
| `example_theme` | Material, Flat, nested themes, and style precedence |
| `example_environment` | Typed defaults, inheritance, and nested overrides |
| `platform/android/demo` | Android native host and application packaging |

## Architecture

```text
component functions and State
  -> ViewSpec
  -> reconciliation
  -> MountedNode
  -> measure, layout, and input
  -> DisplayList
  -> Android, macOS, or Windows renderer
```

The native layer owns the window or host view, frame scheduling, platform input, text services, and drawing surface. Shared application code does not depend on native UI objects.

## License

HuxerUI is available under the terms in [LICENSE](LICENSE).
