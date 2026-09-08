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
.\install.ps1 -Version 0.3.0 -Prefix D:\Environment\HuxerUI -Yes
```

macOS, Linux, or Android:

```bash
curl -fsSLO https://github.com/HuxerUI/HuxerUI/releases/latest/download/install.sh
sh install.sh --version 0.3.0 --prefix "$HOME/Environment/HuxerUI" --yes
```

Omitting the version installs the latest GitHub release.
Running the installer again upgrades or replaces an existing HuxerUI SDK at the same prefix after validating that the directory contains an SDK.

## Update

Update the installed SDK, including its CLI, host tools, headers, libraries, and packaged target artifacts:

```bash
huxerui update --check
huxerui update
huxerui update --yes
huxerui update --version 0.3.0
```

Without `--version`, the command selects the latest stable GitHub release and never downgrades a newer installation.
An explicit `major.minor.patch` version can select an older release; an identical version is a no-op.
`--check` only reports the version comparison and does not download an SDK archive or change the installation.
Updates require confirmation unless `--yes` is present; use `--yes` for unattended invocations.

The command replaces the entire selected SDK, including local modifications inside its installation directory.
Stop builds and tools using that SDK before updating.
Custom installation directories are supported, but the running CLI must belong to the SDK selected by `HUXERUI_HOME`.
If they differ, invoke the selected SDK's `bin/huxerui` or correct the environment first.
Source checkouts must be updated through the source workflow, not this command.
Application projects, external toolchains, shell profiles, user PATH, and persistent `HUXERUI_HOME` remain unchanged.

The installer verifies the archive checksum, stages the replacement beside the existing SDK, and retains the old SDK until the replacement CLI reports the expected version.
Handled publication failures restore the old SDK; abrupt termination or power loss is not an atomic-update guarantee.
On Unix hosts, failure to remove the old backup after successful publication only produces a warning with the remaining backup path; the new SDK remains installed.
Concurrent installers targeting the same prefix are rejected.
After an interrupted run, inspect any reported backup and `<prefix>.huxerui-lock` before manually removing a stale lock.

On Windows, a temporary PowerShell worker waits for the original CLI to exit before replacing its installation.
The initial command's successful exit means the update was handed off, not that installation finished.
The command prints an `update.log` path containing the worker's final result; the temporary worker directory remains available for diagnosis and can be removed after the worker exits.
On macOS and Linux, the command waits for the installer and returns its result.

## Local archive

Install an already downloaded archive without querying a release:

```powershell
.\install.ps1 -Archive .\huxerui-sdk-0.3.0-windows-x86_64.zip -Yes
```

```bash
sh install.sh --archive ./huxerui-sdk-0.3.0-linux-x86_64.tar.gz --yes
```

For Android arm64-v8a:

```bash
sh install.sh --archive ./huxerui-sdk-0.3.0-android-arm64-v8a.tar.gz --yes
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
