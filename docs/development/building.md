# Building HuxerUI

This guide covers framework development from a source checkout.
Application developers using a released SDK should follow [Getting Started](../guide/getting-started.md).

## Requirements

- CMake 3.20 or later
- Ninja, Visual Studio, Xcode, or another supported host generator
- A C++20 compiler
- The required platform SDK and dependencies

Linux additionally requires GTK 4.14 or later, libepoxy, Pango, Cairo, GIO, and libsoup 3 development packages discoverable through pkg-config.

Debian or Ubuntu:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev libepoxy-dev libsoup-3.0-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config gtk4-devel libepoxy-devel libsoup3-devel
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
CI maintains all checked-in host packages through [Host-tool updates](sdk-packaging.md#host-tool-updates).
When changing that workflow or its support script, run `python -B tests/scripts/host_tools_test.py`; it uses temporary Git repositories and requires only Python 3.12 or later and Git.

Android paragraph geometry tests run on a device or emulator with `./gradlew :HuxerUI:connectedDebugAndroidTest` from `platform/android` (`gradlew.bat` on Windows).
The library's `androidTest` source set includes `tests/platform/HuxerUITextLayoutTest.java`; its platform Instrumentation runner needs no AndroidX/JUnit dependency or native HuxerUI library.
It covers soft-wrap affinity, bidirectional hit testing, explicit line breaks, and disjoint selection geometry, and installs only the separate test package rather than replacing an example application.

## Examples

Examples live under `examples/<name>` and produce targets named `example_<name>`.

```bash
cmake --build build --target example_ui_gallery
```

Desktop binaries or application bundles are emitted under the configured build output.
Android examples use `platform/android/example_runner`, and iOS examples use the repository platform runner.

`example_streaming_text` is a chat-style Agent demo. Edit the message at the bottom and press Enter or the Send icon: user messages appear in right-aligned bubbles, while Agent replies use the available conversation width. Replies progressively demonstrate a collapsible scripted reasoning summary, attributed paragraphs, a vector image, numbered items, code, simulated file changes and actions, a table, and a controlled question form. The reasoning summary uses a muted disclosure row with a chevron; file actions are lightweight text controls. Both retain keyboard activation and accessibility semantics. File names, addition/deletion counts, and previews come from the same immutable mock diff data. Click a file to preview that file, or Review proposal to preview all changes in a scrollable Dialog with old/new line numbers and marked additions/deletions. Selected diff text copies without the line-number gutters. Previewing changes does not send a message or interrupt generation; Escape or the close icon dismisses the Dialog. Submitting an answer or choosing Request revision appends another user message and starts a follow-up response; submitted forms show an Answered status instead of a disabled submission button. Earlier turns remain in the conversation. This is a local structured-event simulation: it does not call a model, parse Markdown, read or modify project files, or embed Views inside text runs.

Text arrives in small batches with versioned code coloring. The mock transport delivers three Unicode scalars every 50 ms during reasoning (roughly three to four seconds for the sample summary), and every 36 ms during the response; UI publication batches two chunks. Completed blocks and the selection index stay shared while only the active paragraph changes. The conversation virtualizes individual blocks; generation, answers, and expanded state survive row eviction. New user messages move to the latest turn, while incoming updates follow the bottom only when it is already visible. The Latest icon returns to the bottom and the Stop icon preserves received content without erasing earlier turns; composer icons include accessible names and tooltips. Table cells copy in row order with tab separators, list prefixes remain part of the copied text, and collapsed reasoning is excluded. Decorative labels and controls are not part of the logical text source; Copy and Select All still include unmounted document blocks. Code and tables scroll horizontally on narrow screens. Web retains the documented basic-shaping limits.

## Source and installed consumption

The same application CMake contract supports a source checkout and an installed SDK.
Consumers use `huxerui_add_app`, `huxerui_add_library`, `huxerui_add_resources`, and the exported `HuxerUI::huxerui` or `HuxerUI::huxerui_static` targets.
Consumer projects never include private `src` headers or checkout-specific platform paths.
