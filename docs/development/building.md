# Building HuxerUI

This guide covers framework development from a source checkout.
Application developers using a released SDK should follow [Getting Started](../guide/getting-started.md).

## Requirements

- CMake 3.20 or later
- Ninja, Visual Studio, Xcode, or another supported host generator
- A C++20 compiler
- The required platform SDK and dependencies

Linux additionally requires SDL3 3.4, SDL3_image 3.4, SDL3_ttf 3.2, GIO, and libsoup 3 development packages.
The SDL libraries must provide CMake package configurations; GIO and libsoup are discovered through pkg-config.

Debian or Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build pkg-config libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev libglib2.0-dev libsoup-3.0-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config SDL3-devel SDL3_image-devel SDL3_ttf-devel glib2-devel libsoup3-devel
```

## Configure and build

Use the host platform toolchain.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

On Windows, select an installed Visual Studio generator or use a developer shell configured for MSVC.
Do not use MinGW for the Windows backend.

Useful project options:

| Option | Desktop top-level default | Purpose |
|---|---:|---|
| `HUXERUI_BUILD_SHARED` | `ON` | Build `HuxerUI::huxerui` |
| `HUXERUI_BUILD_STATIC` | `ON` | Build `HuxerUI::huxerui_static` |
| `HUXERUI_BUILD_TESTS` | `ON` | Build repository tests |
| `HUXERUI_BUILD_EXAMPLES` | `ON` | Build examples |
| `HUXERUI_BUILD_CLI` | `ON` | Build the `huxerui` CLI |
| `HUXERUI_WINDOWS_7_COMPAT` | `OFF` | Build the documented Windows 7 compatibility variant |

## Test

```bash
ctest --test-dir build --output-on-failure
```

Run focused test executables directly when changing a narrow subsystem.
Code-generation changes also require the codegen tests and updated required host tools.

## Examples

Examples live under `examples/<name>` and produce targets named `example_<name>`.

```bash
cmake --build build --target example_ui_gallery
```

Desktop binaries or application bundles are emitted under the configured build output.
Android examples use `platform/android/example_runner`, and iOS examples use the repository platform runner.

## Source and installed consumption

The same application CMake contract supports a source checkout and an installed SDK.
Consumers use `huxerui_add_app`, `huxerui_add_library`, `huxerui_add_resources`, and the exported `HuxerUI::huxerui` or `HuxerUI::huxerui_static` targets.
Consumer projects never include private `src` headers or checkout-specific platform paths.
