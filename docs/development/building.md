# Building HuxerUI

This guide covers framework development from a source checkout.
Application developers using a released SDK should follow [Getting Started](../guide/getting-started.md).

## Requirements

- CMake 3.20 or later
- Ninja, Visual Studio, Xcode, or another supported host generator
- A C++20 compiler
- The required platform SDK and dependencies

Apple builds require Xcode 26 or later for the standard library's `std::stop_token` and `std::stop_source` support without experimental-library flags. Release CI selects Xcode 26.2 explicitly through `DEVELOPER_DIR` for iOS artifacts and both macOS architectures, including their host-tool builds. For a local build, select a supported Xcode before configuring a fresh build directory. The build-machine requirement is separate from the deployment targets, which remain iOS 15.0 and macOS 12.0.

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
| `HUXERUI_ENABLE_PROFILING` | `ON` | Compile private Runtime diagnostics for source builds; excluded from SDKs |
| `HUXERUI_WINDOWS_7_COMPAT` | `OFF` | Build the documented Windows 7 compatibility variant |

## Test

```bash
ctest --test-dir build --output-on-failure
```

Run focused test executables directly when changing a narrow subsystem.
The desktop runtime-deployment tests build and run relocated dependency fixtures; Linux requires `patchelf` and binutils, and Windows requires the toolchain's Release VC++ redistributables.
Code-generation changes also require the codegen tests and updated required host tools.
CI maintains all checked-in host packages through [Host-tool updates](sdk-packaging.md#host-tool-updates).
When changing that workflow or its support script, run `python -B tests/scripts/host_tools_test.py`; it uses temporary Git repositories and requires only Python 3.12 or later and Git.

Android Runtime tests run on a device or emulator with `./gradlew :HuxerUI:connectedDebugAndroidTest` from `platform/android` (`gradlew.bat` on Windows).
The library's `androidTest` source set uses `tests/platform/HuxerUIRuntimeTest.java` as its platform Instrumentation runner and needs no AndroidX/JUnit dependency or native HuxerUI library.
It covers paragraph geometry and local-notification Intent identity and activation normalization, and installs only the separate test package rather than replacing an example application.

## Runtime profiling

Source builds default to `HUXERUI_ENABLE_PROFILING=ON`, making private Runtime diagnostics available to source applications and repository examples.
The option is independent of Debug and Release; use an optimized build for performance measurements.
Existing build directories retain their cached value; pass `-DHUXERUI_ENABLE_PROFILING=ON` explicitly to enable diagnostics in a directory previously configured with the option off.
Defining a macro only on an application cannot add instrumentation to a precompiled SDK.
With the option off, the recorder source is excluded and instrumentation macros do not evaluate their arguments; there is no added capture buffer, timer, environment lookup, or per-node profiling branch.
Existing DebugOverlay metrics are separate and retain their current behavior.

```bash
cmake -S . -B build/profile -G Ninja -DCMAKE_BUILD_TYPE=Release -DHUXERUI_ENABLE_PROFILING=ON
cmake --build build/profile --target example_ui_gallery huxerui_tests --parallel
ctest --test-dir build/profile -R HuxerUIProfilingBuildTests --output-on-failure
```

A profiling-enabled application records in `detailed` mode by default, including nested scope, factory, reconciliation, compilation, layout, paint-recording, and resource-resolution spans.
The optional `HUXERUI_PROFILE` environment variable selects `overview` for frame stages and aggregate counters, `detailed` for the default detail level, or `off` to disable recording for that process.
Leaving `HUXERUI_PROFILE` unset or empty uses `detailed`; with `off`, the compiled instrumentation still performs recorder checks but allocates no capture buffer and creates no output directory.
Captures default to `traces/` under the current working directory, not the executable's directory.
Set `HUXERUI_PROFILE_DIRECTORY` only to override that location; it does not change the recording mode.
The output path is resolved when each Runtime is created, so a later working-directory change does not redirect its export.
If the output directory cannot be resolved or created, HuxerUI reports a diagnostic and disables capture for that Runtime while allowing the application to continue.

PowerShell, using the build above:

```powershell
& ./build/profile/bin/example_ui_gallery.exe
```

To disable capture for a subsequent launch, set `$env:HUXERUI_PROFILE = "off"`; remove the variable to return to the default detailed capture.

Recording begins when each Runtime is created and exports a separate `runtime-<timestamp>-<identity>.json` file when that Runtime is destroyed during normal shutdown.
The event buffer is limited to 8 MiB per recording Runtime; exhaustion stops recording and discards the incomplete final frame while preserving earlier complete frames and their counters.
Frames aborted by exceptions are also discarded, and a stopped private recorder retains its buffer until restart or destruction.
The capture contains framework event names, Runtime-local node identities, frame numbers, and counters; it does not collect application text, resource contents, file paths, or user-defined names.
Scope flags identify initial composition, invalidation, and parent-driven recomposition; measure flags identify cache hits.

Open the JSON with [Perfetto](https://perfetto.dev/docs/getting-started/other-formats#chrome-json-format).
Durations describe shared Runtime CPU-side work, including scene generation; they do not include GPU execution or native presentation.
The `huxerui` JSON field holds complete-frame counters and truncation information.
Recording and export can affect performance, so use a profiling-disabled optimized build for the application timing baseline.
The collector and test controls are private development tools, not an installed public API or a production capture facility.
Capturing on platforms without a writable working directory requires an output-directory override or a source-level test harness; no platform launch integration is supplied here.

SDK packaging scripts explicitly set `HUXERUI_ENABLE_PROFILING=OFF` for their target libraries, including both Windows configurations.
Installing libraries or producing an SDK from a profiling-enabled host build is rejected; reconfigure with the option off and rebuild before packaging.

## Examples

Examples live under `examples/<name>` and produce targets named `example_<name>`.

```bash
cmake --build build --target example_ui_gallery
```

Desktop binaries or application bundles are emitted under the configured build output.
Android examples use `platform/android/example_runner`, and iOS examples use the repository platform runner.
The Android runner resolves the selected example's final CMake shared-library name into `BuildConfig.HUXERUI_APP_LIBRARY`; MainActivity loads it before creating the hosted Runtime, without a fixed application `.so` name.

[`example_buffer_reference`](../../examples/buffer_reference/main.cpp) pulls successive grayscale frames from one fixed native allocation, analyzes row-strided bytes in C++, and displays the mean, histogram, and backing-reference reuse check without converting frames into images.
Select `-PhuxeruiExample=buffer_reference` on Android or `HUXERUI_APP_TARGET=example_buffer_reference` in the iOS runner; Android uses DirectByteBuffer and Apple uses NSMutableData through PlatformPayload, while Windows and Linux use the C++ reference directly.
The next button is enabled only after analysis finishes; there is no background producer or implicit read lock. Web displays an unsupported message.

`example_application` demonstrates lifecycle, general activation, activated-file reading, and runtime permissions. The separate [`example_local_notification`](../../examples/local_notification/main.cpp) demonstrates notification authorization, default and template presentation, one-shot scheduling, cancellation, and activation data.

On Windows, `example_application` calls the public URL scheme registration API at entry to register `huxerui-example` for the current executable and user. Open `huxerui-example://documents/42` to exercise cold-start or running-window activation. Registration conflicts are reported at entry; close the old copy and run its `--unregister-url-scheme` cleanup argument before moving to a different build path. See [Windows URL scheme registration](../guide/packaging.md#windows-url-scheme-registration).

On Windows 10 or later, build and run `example_local_notification` directly. Its Windows shell registers a separate development notification identity for the current user before the host starts, without installation or a registration script. Registration persists for notification clicks after exit; close the example and use the [development cleanup command](../guide/packaging.md#windows-notification-registration) before moving or deleting its executable. Registration success and an accepted submission do not prove user-visible delivery.

For Android, select it with `-PhuxeruiExample=local_notification` when building `platform/android/example_runner`. The runner loads the selected example's Java sources and resources from `examples/<name>/android/src/main/{java,res}` and merges its optional `AndroidManifest.xml` over the shared manifest for debug and release. Notification channel setup, permission, receiver, and RemoteViews belong to this example; other examples do not package that configuration. For example, from `platform/android`:

```bash
./gradlew :example_runner:assembleDebug -PhuxeruiExample=local_notification
```

The notification example supplies a Windows ToastGeneric XML provider, an Android template provider, and an iOS Content Extension. Windows progress bars require Windows 10 version 1703 or later; progress submissions replace the notification and may show another popup rather than updating in place. In the iOS runner, select the dedicated `HuxerUINotifications` scheme; changing the generic runner's C++ example target alone does not embed an extension. The [iOS runner instructions](../../platform/ios/example_runner/README.md#download-notification-example) cover signing and device validation. macOS can exercise default notifications, and unsupported hosts display capability results instead of simulating notifications.

The shared download example retrieves the fixed HuxerUI v0.2.0 Windows SDK ZIP (54,836,850 bytes) only after the user presses Download. It streams into an attempt-specific cache directory, updates the same notification at most about once per second during transfer, and publishes a terminal complete, failed, or canceled snapshot. Only successfully closed downloads are renamed from `.part` to `.zip`; files are not extracted or installed. Failed or canceled partial files and completed downloads remain in app cache, and retries use separate directories. Downloading depends on the running application's Task lifetime, not a platform background download service or restartable download manager; process termination can leave the last progress notification visible.

The Android example's channel uses low importance so progress updates do not request sound or heads-up interruptions, subject to user settings. Compact and optional expanded RemoteViews read `file_name`, `downloaded_bytes`, nullable `total_bytes`, `status`, and `expanded` from `data`. Clicking returns the submitted snapshot through startup or subsequent activation and displays its fields without treating them as file-access authority. Unknown totals use indeterminate progress; the release's advertised size is not substituted for a transport-provided total. Default reminders retain their own identifier and remain available alongside the download.

`example_streaming_text` is a local Agent repair simulation. Edit the message at the bottom and press Enter or Send: the first response follows a compact inspection, failure, patch, and verification sequence. Its 15 blocks retain a scripted analysis summary, numbered steps, nested bullets, four tool calls, attributed text, code, file actions, a result table, three concept images, and a controlled question form. Every later message, including a form submission or revision request, uses a four-block interaction-review continuation instead of replaying the investigation. It carries the user's follow-up into a fixed scripted review, not a model-generated answer; earlier messages and proposals remain unchanged. Analysis summaries are fictional, not private model reasoning; no model is contacted, no command is executed, and no project file is read or changed.

Tool cards show a command and expandable output, with Running, Completed, Failed, or Stopped status. Completed calls include their scripted exit code; stopping an active call preserves its received log prefix without claiming an exit result. Logs arrive in batches of up to two lines every 140 ms. Prose arrives in varying batches of 12–24 Unicode scalars every 36 ms, and analysis summaries in batches of six every 50 ms; prose publication combines two transport chunks. Transport batching keeps updates visible without one-character-per-frame delivery. Code coloring remains source-versioned so late worker results cannot overwrite a newer tail.

Completed blocks and the selection index remain shared while only the active text part changes. A tool's command stays immutable while its output streams through the same independently observed tail used by ordinary paragraphs. Generation, answers, tool outcomes, and expansion choices live outside virtual rows. Incoming updates follow the bottom only when the reader is already there; Latest returns to the end, and Stop preserves received content. Table cells copy in row order with tab separators. List prefixes, including bullet symbols and nested indentation, belong to the copied text; wrapped item bodies align under their own text. Collapsed analysis and collapsed tool output are excluded from selection, while the tool command remains available. Code, terminal output, and tables scroll horizontally on narrow screens. Web retains the documented basic-shaping limits.

File names, addition/deletion counts, source snippets, and previews derive from the same immutable mock diff data. Click a file or Review proposal to open the scrollable Dialog with old/new line numbers and marked additions/deletions; copied diff text excludes the line-number gutters. Previewing changes does not interrupt generation or send a new message. Escape or the close icon dismisses the Dialog. Submitting the controlled question form or choosing Request revision appends another user turn; submitted forms retain an Answered status. The example uses public APIs and structured fixtures without a Markdown parser, an Agent framework, or embedded Views inside text runs.

The three concept images stay in one horizontally scrollable row, with selectable captions in document order. Click a thumbnail to open a larger, aspect-preserving image in a viewport-constrained Dialog; Escape, platform Back, an outside press, or the close icon dismisses it. The preview retains the image and caption independently of virtual-row lifetime and does not interrupt streaming.

The example supplies its own light ThemeSpec to MaterialTheme: neutral surfaces, graphite text, and restrained teal accents. Tool cards use white surfaces, code and questions use a pale neutral fill, and the composer has a distinct bordered surface. Commands and code use neutral foregrounds; completed and failed tool states use green and red labels without relying on color alone. Concept images share the example palette, including the diff preview's addition/deletion colors. Hover, ripple, focus, form controls, and dialogs follow this local theme; framework defaults are unchanged.

## Source and installed consumption

The same application CMake contract supports a source checkout and an installed SDK.
Consumers use `huxerui_add_app`, `huxerui_add_library`, `huxerui_add_resources`, and the exported `HuxerUI::huxerui` or `HuxerUI::huxerui_static` targets.
Consumer projects never include private `src` headers or checkout-specific platform paths.
