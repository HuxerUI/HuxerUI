# Installation

HuxerUI releases provide portable SDK archives and installers for desktop development hosts and Android arm64-v8a devices running Termux.
The SDK contains public headers, CMake package files, host tools, the `huxerui` CLI, framework resources, the HuxerUI application-development Skill, and target artifacts required by supported project platforms.

## Windows

Run in PowerShell:

```powershell
irm https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.ps1 | iex
```

The default installation directory is `%LOCALAPPDATA%\HuxerUI`.
The installer sets the user `HUXERUI_HOME` variable and adds `%HUXERUI_HOME%\bin` to the user `PATH`.

Windows release archives currently target x86_64 hosts.

## macOS, Linux, and Android

Run in an interactive shell:

```bash
curl -fsSL https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.sh | sh
```

The default installation directory is `~/Library/Developer/HuxerUI` on macOS and `~/.local/share/HuxerUI` on Linux and Android.
The installer writes `HUXERUI_HOME` and the SDK `bin` directory to the current shell profile.

Release archives are selected for macOS arm64 or x86_64, Linux aarch64 or x86_64, and Android arm64-v8a hosts.
Android host installation currently supports Termux and uses the same `install.sh` entry point.

Open a new terminal after installation, then verify the SDK:

```bash
huxerui doctor
```

## Explicit version or prefix

Download an installer before passing options.

Windows:

```powershell
Invoke-WebRequest https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.ps1 -OutFile install.ps1
.\install.ps1 -Version 0.2.0 -Prefix D:\Environment\HuxerUI -Yes
```

macOS, Linux, or Android:

```bash
curl -fsSLO https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.sh
sh install.sh --version 0.2.0 --prefix "$HOME/Environment/HuxerUI" --yes
```

Omitting the version installs the latest GitHub release.
Running the installer again upgrades or replaces an existing HuxerUI SDK at the same prefix after validating that the directory contains an SDK.

## Local archive

Install an already downloaded archive without querying a release:

```powershell
.\install.ps1 -Archive .\huxerui-sdk-0.2.0-windows-x86_64.zip -Yes
```

```bash
sh install.sh --archive ./huxerui-sdk-0.2.0-linux-x86_64.tar.gz --yes
```

For Android arm64-v8a:

```bash
sh install.sh --archive ./huxerui-sdk-0.2.0-android-arm64-v8a.tar.gz --yes
```

Place the matching `.sha256` file beside the archive.
The archive and checksum must match the current host platform and architecture.

## Uninstall

Use the same prefix used for installation.

```powershell
.\install.ps1 -Uninstall -Yes
```

```bash
sh install.sh --uninstall --yes
```

The installer refuses to remove a directory that does not contain a recognizable HuxerUI SDK.

## Toolchains

Installing HuxerUI does not install platform SDKs, compilers, Android tooling, Emscripten, Xcode, or signing identities.
Use `huxerui doctor [platform-list]` for read-only diagnostics and `huxerui setup <platform-list>` for prerequisites the CLI can configure.
See [Platform Support](platforms.md) for host-specific requirements.
