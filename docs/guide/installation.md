# Installation

Install the SDK for the machine that runs the CLI. The SDK contains the CLI, host tools, headers, CMake helpers, framework resources, and target libraries. Check [Platform Support](platforms.md) for the toolchain and runtime requirements of your application targets.

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

The installer selects a macOS arm64 or x86_64 archive, a Linux aarch64 or x86_64 archive, or an Android arm64-v8a archive for Termux. The official Linux installer checks the [glibc requirement](platforms.md#linux) before it runs the installed CLI.

On either Windows or Unix, open a new terminal after installation and verify the SDK:

```bash
huxerui doctor
```

## Explicit version or prefix

Download an installer before passing options.

Windows:

```powershell
Invoke-WebRequest https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.ps1 -OutFile install.ps1
.\install.ps1 -Version 0.5.0 -Prefix D:\Environment\HuxerUI -Yes
```

macOS, Linux, or Android:

```bash
curl -fsSLO https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.sh
sh install.sh --version 0.5.0 --prefix "$HOME/Environment/HuxerUI" --yes
```

Omitting the version installs the latest GitHub release.
Running the installer again upgrades or replaces an existing HuxerUI SDK at the same prefix after validating that the directory contains an SDK.

## SDK versions

Manage SDK versions from any directory:

```bash
huxerui sdk list
huxerui sdk use 0.5.0
huxerui sdk update --check
huxerui sdk update
```

- `sdk list` shows the original SDK and locally installed versions, with `*` beside the default recorded by the installer.
- `sdk use` selects an installed version without a network request. It asks before downloading a missing version.
- `sdk update` compares the running SDK with the latest stable GitHub release. It asks before selecting a newer release and retains earlier versions. Add `--check` to show the comparison without downloading or switching.

Use `--yes` with `sdk use` or `sdk update` to skip confirmation. Version switching requires HuxerUI 0.4.0 or newer with the `sdk` command.

For an installation at `<prefix>`, additional versions are stored at `<prefix>-versions/<version>`. The running CLI must belong to the SDK pointed to by `HUXERUI_HOME`.
After a successful switch, `HUXERUI_HOME` and the corresponding `bin` entry on `PATH` point to the chosen SDK in new terminals. On Unix, switches reuse the shell profile already managing those SDK versions and print its path.
To select another SDK for a single build, see [SDK selection](getting-started.md#select-an-sdk-for-one-invocation).

## Local archive

Install an already downloaded archive without querying a release:

```powershell
.\install.ps1 -Archive .\huxerui-sdk-0.5.0-windows-x86_64.zip -Yes
```

```bash
sh install.sh --archive ./huxerui-sdk-0.5.0-linux-x86_64.tar.gz --yes
```

For Android arm64-v8a:

```bash
sh install.sh --archive ./huxerui-sdk-0.5.0-android-arm64-v8a.tar.gz --yes
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

After installing HuxerUI, use [environment diagnosis and setup](getting-started.md#diagnose-the-environment) to check and prepare the external toolchain for your target.
