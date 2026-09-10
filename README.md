<p align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/assets/HuxerUI-logo-dark.png"><source media="(prefers-color-scheme: light)" srcset="docs/assets/HuxerUI-logo-light.png"><img src="docs/assets/HuxerUI-logo-light.png" width="220" alt="HuxerUI logo"></picture></p>

<h1 align="center">HuxerUI</h1>

<p align="center"><strong>Declarative, cross-platform UI in modern C++.</strong></p>

<p align="center">One shared runtime for Windows, macOS, Linux, Web, Android, and iOS.</p>

HuxerUI provides C++20 components, typed state, responsive layout, input, text editing, navigation, animation, accessibility semantics, resources, files, HTTP, and first-class platform integration.
Application code stays platform-neutral while each backend uses the windowing, text, input, accessibility, and rendering services it supports.

## Install the SDK

Windows PowerShell:

```powershell
irm https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.ps1 | iex
```

macOS or Linux:

```bash
curl -fsSL https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.sh | sh
```

The installer selects the release archive for the current host, configures `HUXERUI_HOME`, and adds the SDK CLI to the user `PATH`.
Open a new terminal, then verify the installation:

```bash
huxerui doctor
```

Or install through [xlings](https://github.com/d2learn/xlings), on any of the three desktop hosts:

```bash
xlings install huxerui -y
```

> Installing this way also brings the tools the SDK builds with — CMake, mcpp and the rest of the toolchain — so a fresh machine needs nothing else.

See [Installation](docs/guide/installation.md) for supported host architectures, explicit versions, custom prefixes, upgrades, and uninstall commands.

## Create an application

HuxerUI offers two ways to build one. They share the same public API and the
same `[[huxerui::composable]]` model; they differ in how the project is
described and what it can target.

### With headers and CMake

The full-platform path, and the only one that reaches Android, iOS and Web.

```bash
huxerui create app hello_huxer --platform windows,macos,linux,web,android,ios
cd hello_huxer
huxerui doctor
huxerui run windows
```

Replace `windows` with a platform enabled for the project and available on the current host.
The generated project contains shared C++ sources, packaged resources, source-controlled platform shells, and the
HuxerUI application-development Skill under `.agents/skills` by default.

```cpp
#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("Increment").OnClick([count] {
      count += 1;
    }),
  }.With(
      Padding(24.0F),
      Spacing(12.0F)
  );
}

View App() {
  return MaterialTheme {
    Counter(),
  };
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

### With C++20/23 modules and mcpp

Modules all the way down. `import huxerui;` replaces the umbrella header,
`import std;` replaces the standard headers, and an application ends up with
**no `#include` in it at all** — a page is a module unit, and the units reach
each other by `import` rather than through a header and a list of sources.

[mcpp](https://github.com/mcpp-community/mcpp) builds it, from one manifest for
Linux, Windows and macOS, with the toolchain and the GTK stack provisioned
rather than assumed.

```bash
huxerui create app hello_huxer --build mcpp
cd hello_huxer
mcpp build
mcpp run
```

The generated project has no `CMakeLists.txt`, no platform shells, and **no
`#include` anywhere**:

```cpp
export module app;

import std;
import huxerui;

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
  return MaterialTheme { Counter() };
}

const Application application{
    App,
    {.window = {.title = "Counter", .initial_size = {480.0F, 320.0F}}},
};
```

`--template navigation` and `--template library` produce a multi-page
application and a reusable component library from the same command. The whole
manifest is one dependency line, and `mcpp new hello_huxer --template
huxerui.huxerui` instantiates the same project without the HuxerUI CLI.

See [Developing with C++20/23 modules and mcpp](skills/huxerui-app-development/references/cpp-modules-and-mcpp.md)
and the three worked applications in [`mcpp/examples`](mcpp/examples/).

Explore complete application demos in [HuxerUI-Demos](https://github.com/HuxerUI/HuxerUI-Demos), or browse the [examples](examples/) for focused API usage.

## Platforms

| Platform | Host integration | Renderer |
|---|---|---|
| Windows | Win32 | Direct2D and DirectWrite |
| macOS | AppKit | Core Graphics and Core Text |
| Linux | GTK 4.14 | GSK, Cairo, and Pango |
| Web | Emscripten and browser APIs | Canvas 2D |
| Android | Android View and InputConnection | Android Canvas |
| iOS | UIKit and UITextInput | Core Graphics and Core Text |

See [Platform Support](docs/guide/platforms.md) for host requirements and platform-specific capabilities.

## Documentation

- [Documentation index](docs/README.md)
- [Getting Started](docs/guide/getting-started.md)
- [Core Concepts](docs/guide/core-concepts.md)
- [Components and Input](docs/guide/components.md)
- [Architecture Design](docs/design/architecture.md)
- [C++20/23 modules and mcpp](skills/huxerui-app-development/references/cpp-modules-and-mcpp.md)
- [Examples](examples/)

Repository contributors should start with [Building HuxerUI](docs/development/building.md).

## Community

Join the HuxerUI community on Discord or QQ.

| Discord | QQ group `1090609035` |
|:---:|:---:|
| <img src="docs/assets/community-discord.png" width="280" alt="Discord invitation QR code"> | <img src="docs/assets/community-qq.jpg" width="280" alt="QQ group invitation QR code"> |

## License

HuxerUI is available under the terms in [LICENSE](LICENSE).
