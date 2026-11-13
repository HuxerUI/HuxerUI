<p align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/assets/HuxerUI-logo-dark.png"><source media="(prefers-color-scheme: light)" srcset="docs/assets/HuxerUI-logo-light.png"><img src="docs/assets/HuxerUI-logo-light.png" width="220" alt="HuxerUI logo"></picture></p>

<h1 align="center">HuxerUI</h1>

<p align="center"><strong>Build human-centered user experiences.</strong></p>

HuxerUI is a declarative, cross-platform UI framework for C++20. The name *Huxer* combines **H** for *Human*, **UX** for *User Experience*, and **-er** for the person who creates that experience.

## Quick Start

This quick start targets HuxerUI **0.4.0**. The installers select the latest published SDK.

### Install the SDK

**Windows PowerShell**

```powershell
irm https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.ps1 | iex
```

**macOS, Linux, or ARM64 Termux**

```bash
curl -fsSL https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.sh | sh
```

On desktop computers, `xlings install huxerui -y` installs the SDK through [xlings](https://github.com/d2learn/xlings).

For toolchain requirements and SDK version selection, see [Installation](docs/guide/installation.md).

### Run an application

Open a new terminal to create a CMake application. Replace `windows` with a target you can build and run from your development machine.

```bash
huxerui create app hello_huxer
cd hello_huxer
huxerui doctor
huxerui run windows
```

For a C++ modules project on Windows, macOS, or Linux, pass `--build mcpp` to `huxerui create app`. Generated modules use `import huxerui;` and run with `mcpp run`. [C++ Modules and mcpp](skills/huxerui-app-development/references/cpp-modules-and-mcpp.md)

## Example

Replace `src/app.cpp` in the generated CMake project with this counter. `UseState` retains the count and updates the view when the button changes it.

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("Increment").OnClick([count] { count += 1; }),
  }.With(Padding(24.0F), Spacing(12.0F));
}

View App() {
  return MaterialTheme {
    Counter(),
  };
}

const Application application{App, {.window = {.title = "Counter"}}};
```

Repeat the run command to try the counter.

Explore more components in the [UI Gallery](examples/ui_gallery/) and complete applications in [HuxerUI-Demos](https://github.com/HuxerUI/HuxerUI-Demos).

## Platforms

| Platform | Platform integration | Renderer |
|---|---|---|
| Windows | Win32 | Direct2D and DirectWrite |
| macOS | AppKit | Core Graphics and Core Text |
| Linux | GTK 4.14 | GSK, Cairo, and Pango |
| Web | Emscripten and browser APIs | Canvas 2D |
| Android | Android View and InputConnection | Android Canvas |
| iOS | UIKit and UITextInput | Core Graphics and Core Text |

For build requirements and backend capabilities, see [Platform Support](docs/guide/platforms.md).

## Official Libraries

| Library | Purpose |
|---|---|
| [Lib-WebView](https://github.com/HuxerUI/Lib-WebView) | Embeds web content with navigation, JavaScript evaluation, and messaging between pages and the app. |
| [Lib-RichEditor](https://github.com/HuxerUI/Lib-RichEditor) | Edits structured rich-text documents with Markdown and JSON import and export. |
| [Lib-CodeEditor](https://github.com/HuxerUI/Lib-CodeEditor) | Provides a code editor with syntax highlighting, completion, and search. |
| [Lib-Media](https://github.com/HuxerUI/Lib-Media) | Plays audio and video and provides encoding, decoding, and container I/O. |
| [Lib-SQLite](https://github.com/HuxerUI/Lib-SQLite) | Maps C++ records to SQLite with typed queries, transactions, and schema migrations. |
| [Lib-Markdown](https://github.com/HuxerUI/Lib-Markdown) | Parses CommonMark with GFM extensions and renders themed Markdown views. |
| [Lib-DataTable](https://github.com/HuxerUI/Lib-DataTable) | Displays virtualized tables with resizable columns and application-controlled sorting, selection, and editing. |
| [Lib-Charts](https://github.com/HuxerUI/Lib-Charts) | Displays interactive charts with tooltips, selection, and animated data updates. |
| [Lib-PDF](https://github.com/HuxerUI/Lib-PDF) | Displays PDF pages and supports asynchronous rendering, text extraction, and search. |
| [Lib-Camera](https://github.com/HuxerUI/Lib-Camera) | Provides camera previews, photo capture, and silent video recording. |
| [Lib-AudioCapture](https://github.com/HuxerUI/Lib-AudioCapture) | Captures timestamped microphone audio as PCM and exports recordings. |
| [Lib-QRCode](https://github.com/HuxerUI/Lib-QRCode) | Generates and decodes QR codes and barcodes with declarative views and SVG export. |
| [Lib-Lottie](https://github.com/HuxerUI/Lib-Lottie) | Plays Lottie animations with playback and seeking controls. |
| [Lib-Map](https://github.com/HuxerUI/Lib-Map) | Embeds AMap and Baidu Map views on Android and iOS. |
| [Lib-Live2D](https://github.com/HuxerUI/Lib-Live2D) | Displays interactive Live2D Cubism characters with motion and expression controls. |
| [Lib-PluginKit](https://github.com/HuxerUI/Lib-PluginKit) | Design for a C++20 plugin toolkit with typed interfaces, package management, and isolated execution. |
| [Lib-AVKit](https://github.com/HuxerUI/Lib-AVKit) | Planned toolkit for recording, real-time effects, timeline editing, and media export. |

## Documentation

- [Documentation index](docs/README.md)
- [Getting Started](docs/guide/getting-started.md)
- [Building HuxerUI](docs/development/building.md)

## Community

Join the HuxerUI community on Discord or QQ.

| Discord | QQ group `1090609035` |
|:---:|:---:|
| <img src="docs/assets/community-discord.png" width="280" alt="Discord invitation QR code"> | <img src="docs/assets/community-qq.jpg" width="280" alt="QQ group invitation QR code"> |

## License

HuxerUI is available under the terms in [LICENSE](LICENSE).
