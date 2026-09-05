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

`example_streaming_text` is a local Agent repair simulation. Edit the message at the bottom and press Enter or Send: a user bubble is followed by a long, coherent investigation with scripted analysis summaries, numbered steps, nested bullet lists, source inspection, a failing test, a proposed patch, a build, a successful rerun, and a final handoff. The first response contains 25 blocks, including five tool calls, attributed paragraphs, a group of three vector images, file actions, a result table, and a controlled question form. Follow-up turns replay the fixture with the new request while retaining the earlier conversation. Analysis summaries are fictional, not private model reasoning; no model is contacted, no command is executed, and no project file is read or changed.

Tool cards show a command and expandable output, with Running, Completed, Failed, or Stopped status. Completed calls include their scripted exit code; stopping an active call preserves its received log prefix without claiming an exit result. Logs arrive in batches of up to two lines every 140 ms. Prose arrives in varying batches of 12–24 Unicode scalars every 36 ms, and analysis summaries in batches of six every 50 ms; prose publication combines two transport chunks. The longer fixture remains practical to watch without slowing generation to one character per frame. Code coloring remains source-versioned so late worker results cannot overwrite a newer tail.

Completed blocks and the selection index remain shared while only the active text part changes. A tool's command stays immutable while its output streams through the same independently observed tail used by ordinary paragraphs. Generation, answers, tool outcomes, and expansion choices live outside virtual rows. Incoming updates follow the bottom only when the reader is already there; Latest returns to the end, and Stop preserves received content. Table cells copy in row order with tab separators. List prefixes, including bullet symbols and nested indentation, belong to the copied text; wrapped item bodies align under their own text. Collapsed analysis and collapsed tool output are excluded from selection, while the tool command remains available. Code, terminal output, and tables scroll horizontally on narrow screens. Web retains the documented basic-shaping limits.

File names, addition/deletion counts, source snippets, and previews derive from the same immutable mock diff data. Click a file or Review proposal to open the scrollable Dialog with old/new line numbers and marked additions/deletions; copied diff text excludes the line-number gutters. Previewing changes does not interrupt generation or send a new message. Escape or the close icon dismisses the Dialog. Submitting the controlled question form or choosing Request revision appends another user turn; submitted forms retain an Answered status. The example uses public APIs and structured fixtures without a Markdown parser, an Agent framework, or embedded Views inside text runs.

The three concept images stay in one horizontally scrollable row, with selectable captions in document order. Click a thumbnail to open a larger, aspect-preserving image in a viewport-constrained Dialog; Escape, platform Back, an outside press, or the close icon dismisses it. The preview retains the image and caption independently of virtual-row lifetime and does not interrupt streaming.

## Source and installed consumption

The same application CMake contract supports a source checkout and an installed SDK.
Consumers use `huxerui_add_app`, `huxerui_add_library`, `huxerui_add_resources`, and the exported `HuxerUI::huxerui` or `HuxerUI::huxerui_static` targets.
Consumer projects never include private `src` headers or checkout-specific platform paths.
