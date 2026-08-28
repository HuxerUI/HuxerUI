<p align="center"><picture><source media="(prefers-color-scheme: dark)" srcset="docs/assets/HuxerUI-logo-dark.png"><source media="(prefers-color-scheme: light)" srcset="docs/assets/HuxerUI-logo-light.png"><img src="docs/assets/HuxerUI-logo-light.png" width="220" alt="HuxerUI logo"></picture></p>

<h1 align="center">HuxerUI</h1>

<p align="center"><strong>Declarative, cross-platform UI in modern C++.</strong></p>

<p align="center">One shared runtime for Windows, macOS, Linux, Web, Android, and iOS.</p>

HuxerUI provides C++20 components, typed state, responsive layout, input, text editing, navigation, animation, accessibility semantics, resources, files, HTTP, and first-class platform integration.
Application code stays platform-neutral while each backend uses its platform windowing, text, input, accessibility, and rendering services.

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

See [Installation](docs/guide/installation.md) for supported host architectures, explicit versions, custom prefixes, upgrades, and uninstall commands.

## Create an application

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

## Platforms

| Platform | Host integration | Renderer |
|---|---|---|
| Windows | Win32 | Direct2D and DirectWrite |
| macOS | AppKit | Core Graphics and Core Text |
| Linux | SDL3 | CPU rasterizer and SDL_ttf |
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
- [Examples](examples/)

Repository contributors should start with [Building HuxerUI](docs/development/building.md).

## License

HuxerUI is available under the terms in [LICENSE](LICENSE).
