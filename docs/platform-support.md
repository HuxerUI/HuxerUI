# Platform Support

## Supported backends

| Platform | Application surface | Text layout | Rendering | Text input |
|---|---|---|---|---|
| Android | Native View | StaticLayout | Canvas | InputConnection and IME |
| Linux | X11 Window | FreeType and HarfBuzz | Cairo and EGL/OpenGL ES | XIM |
| iOS preview | UIKit View | CoreText | CoreGraphics | UITextInput |
| macOS | AppKit | CoreText | CoreGraphics | NSTextInputClient |
| Windows | Win32 | DirectWrite | Direct2D | Native keyboard and IME adapter |
| Web preview | Browser Canvas | Canvas TextMetrics | Canvas 2D | Hidden input, textarea, and composition events |

State, recomposition, node reconciliation, layout, hit testing, focus, scrolling, text editing behavior, and retained-scene generation remain in the shared C++ runtime.

## Runtime and PlatformAdapter

Each application surface owns one `Runtime`. Multiple surfaces may share the same registered root factory without sharing state, layout, frame scheduling, focus, or input sessions.

```cpp
class NativeAdapter final : public PlatformAdapter {
  // Implement the native frame, text, input, and rendering boundary.
};

NativeAdapter platform;
Runtime runtime{
    {
        .root_factory = App,
        .options = {.title = "HuxerUI"},
    },
    platform,
};

runtime.SetViewport({width, height});
const FrameCommit& commit = runtime.BuildFrame();
renderer.Render(commit.render_frame);
if (commit.next_frame_deadline.has_value()) {
  platform.RequestFrameAt(*commit.next_frame_deadline);
}
```

Platform adapters translate density, native coordinate systems, key events, pointer events, IME commands, clipboard operations, packaged resource reads, and renderer conventions.
PlatformAdapter also implements the shared `TextMeasurer` service, resolving platform-neutral Font and TextStyle values through the native text stack.
They traverse the committed `RenderScene` in `commit.render_frame` and do not duplicate component state machines or layout behavior.
`PlatformAdapter::RequestFrameAt()` accepts an absolute monotonic deadline.
Runtime uses it for invalidations outside frame construction; work discovered while building is returned through `FrameCommit::next_frame_deadline`.
The platform adapter presents the committed frame before scheduling that deadline, which prevents continuous animation from starving the native paint phase.
macOS and Windows translate `DamageRegion` into native invalidation bounds.
Android receives the same committed damage but invalidates the complete native View because current Android View APIs ignore dirty rectangles.
All three backends replay only the committed scene during native paint callbacks.
Exact `DrawTextRunsCommand` geometry is supplied by TextMeasurer and is not replaced by renderer-side layout decisions.
Native font, layout, and decoded-image caches are platform-owned and bounded; see [Text and Font Design](design/text.md) and [App Resources, Images, and Localization Design](design/resources.md).
When the debug performance panel is open, `PlatformAdapter::QueryProcessMetrics()` optionally reports cumulative process CPU time, a current process-memory footprint, and logical processor count. Android reports proportional set size (PSS); Windows and macOS report their current working-set or resident-set values. Runtime owns the sampling lifecycle and derives interval CPU utilization while platform-specific metric collection remains behind the adapter boundary.

## Android

The Android integration provides:

- `HuxerUIActivity` for full-screen applications
- `HuxerUIView` for embedding HuxerUI in an existing Android interface
- The `HuxerUI` Gradle library module
- Prefab metadata in the source Gradle module for source-SDK application builds
- The `demo` application module

A full-screen launcher derives from `HuxerUIActivity`:

```java
public final class MainActivity extends HuxerUIActivity {}
```

The application native library is named `huxerui_app`. Loading it registers the immutable `HUXERUI_APP` definition before the activity creates its `HuxerUIView`.

`HuxerUIActivity` owns a lifecycle-bound Back callback and forwards Back to the shared Runtime. Applications using this full-screen Activity set `android:enableOnBackInvokedCallback="true"` on their manifest `application` element, as the demo module does. Android 14 and later forward predictive Back start, progress, cancel, and commit phases; Android 13 forwards Commit; older versions use `onBackPressed()` for the same Commit path. When Runtime does not consume Commit, the Activity calls its overridable `onUnhandledBack()` fallback, which finishes the Activity with transition by default. An embedded integration owns registration itself, may call `HuxerUIView.handleBack()`, and continues its native fallback only when that method returns `false`.

Coordinates remain density independent. The Android integration maps multi-touch, mouse hover, wheel, keyboard, viewport, and frame-clock events to the shared model. Frame callbacks commit Runtime work before full View invalidation, while `onDraw()` only presents the committed scene. The minimum supported Android API level is 23.
Rounded-rectangle shadows use hardware shadow layers on API 28 and later, with density-aware cached alpha masks on older supported versions.
Arbitrary Paths use the same native Canvas, and Path shadows use hardware layers on API 28 and later with a bounded software mask fallback on older supported versions.
Neither path disables hardware acceleration for the complete HuxerUIView.
Packaged resources are read from Android assets, system changes proactively update the Runtime resource configuration, and encoded images are transferred to Java only on a Bitmap cache miss.
Debug process metrics use `getrusage`, `Debug.getPss()`, and the online processor count.

## macOS

The macOS backend creates an AppKit window and View, renders through CoreGraphics, measures text with CoreText, and exposes a dedicated `NSTextInputClient` adapter for native selection, composition, and geometry queries. Scheduled callbacks commit Runtime work before AppKit invalidation, while `drawRect:` only presents the committed scene.
Core Graphics resolves retained shadow commands with native blurred path shadows.
Canvas Paths map directly to Core Graphics fill, stroke, clip, and shadow operations.
Packaged resources are read from the application bundle, locale and backing-scale changes proactively update the Runtime resource configuration, and ImageIO-backed decoded images remain renderer-owned.
Debug process metrics use `getrusage`, Mach task information, and `NSProcessInfo`.

Example targets build as application bundles and can be launched from `build/bin`.

## iOS technical preview

The iOS backend creates a UIKit window and safe-area-constrained HuxerUI View, measures text with CoreText, and replays the shared RenderScene through an independent CoreGraphics renderer. CADisplayLink schedules Runtime commits before UIKit invalidation, while `drawRect:` presents only the committed frame.

Multi-touch, Apple Pencil, indirect pointer, hardware keyboard, clipboard, locale, display scale, keyboard viewport, and packaged-resource events cross one UIKit adapter boundary. A dedicated UITextInput implementation maps native UTF-16 positions, selection, marked text, actions, and caret geometry to the shared text-input session and revision protocol. UIKit does not own a second editing value.

The minimum deployment target is iOS 13. CLI projects contain a source-controlled native Xcode project that owns application packaging, signing, native assets, launch metadata, and debugging. Its build phase links an architecture-correct CMake application core containing the common C++ application and HuxerUI. The CLI discovers paired devices through `devicectl`, discovers booted Simulators through `simctl`, and keeps their DerivedData directories separate:

```bash
huxerui create hello_huxer --platform ios
cd hello_huxer
huxerui doctor ios
huxerui devices ios
huxerui open ios
huxerui run ios --device <id>
```

Physical-device development builds use Xcode automatic signing and the `DEVELOPMENT_TEAM` value in `platform/ios/Config/Local.xcconfig` or the native project settings. `huxerui open ios` records the detected HuxerUI SDK in that ignored local file before opening the project. Distribution archive automation, export signing, a public embeddable UIView, native selection rectangles, and accessibility semantics remain outside the preview.

The repository-owned `platform/ios/example_runner/HuxerUIExamples.xcodeproj` provides one native debugging host for every CMake `example_*` application target. Its ignored local configuration selects the example and optional signing team, while the shared project continues to build the selected application core through CMake.

## Windows

The Windows backend targets Windows 10 and later by default.
It owns the Win32 window, uses DirectWrite for text layout, and renders shared PaintCommands through a Direct2D device context backed by D3D11 and a DXGI swap chain.
Partial Runtime damage updates a retained scene bitmap before the affected pixels are presented.
Direct2D Shadow effects consume cached rounded-rectangle masks while color, opacity, and offset remain draw-time properties.
Canvas Paths map to Direct2D path geometry for fill, stroke, geometric clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory, locale and DPI changes proactively update the Runtime resource configuration, and WIC decoding produces device-dependent Direct2D bitmap cache entries.
Debug process metrics use process times, working-set counters, and the native logical processor count.

`HUXERUI_WINDOWS_7_COMPAT=ON` builds an opt-in binary for Windows 7 SP1 with Platform Update or later.
That build resolves modern per-monitor DPI APIs at runtime, uses system-DPI fallbacks on Windows 7, and falls back from flip presentation to a sequential bitblt swap chain when necessary.
Windows 7 without Platform Update is not supported.

## Linux

The Linux backend creates an X11 window, measures text with FreeType and HarfBuzz, rasterizes shared PaintCommands with Cairo into a retained device-pixel bitmap, and presents it through an EGL/OpenGL ES 2 swap.
Partial Runtime damage limits Cairo redraw to the affected pixel bounds; the retained bitmap is then presented whole or as damaged rows, matching the Windows cost model of a retained scene bitmap plus swap-chain presentation.
Canvas Paths map to Cairo path geometry for fill, stroke, clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory (overridable with `HUXERUI_RESOURCES_DIR`), locale and `Xft.dpi` changes update the Runtime resource configuration, and libpng/libjpeg decoding produces Cairo bitmap cache entries with a bounded byte budget.

Text input uses the X Input Method protocol with full preedit callbacks, mirroring the Windows IMM32 adapter; when no input method is available the backend degrades gracefully to direct key text.
Clipboard reads and writes use the X11 `CLIPBOARD` selection with UTF-8 string transfers.
System dependencies are resolved through pkg-config: X11, XKB common, EGL, OpenGL ES 2, Cairo, FreeType, HarfBuzz, fontconfig, libpng, and libjpeg.
Host tools are distributed as prebuilt executables under `tools/prebuilt/linux/<architecture>/`, matching the macOS and Windows distribution model.

## Web technical preview

The Web backend compiles the same `HUXERUI_APP` application through Emscripten, mounts one shared `Runtime` and `WebPlatformAdapter` per browser Canvas, and emits an ES module with WebAssembly output.
Canvas 2D replays the shared `RenderScene`, while browser Pointer Events, wheel events, keyboard events, hidden native text controls, resource preloading, and asynchronous `ImageBitmap` decoding remain platform-owned services.

Configure and build all examples with a modern Emscripten toolchain:

```bash
emcmake cmake -S . -B cmake-build-web \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_EXAMPLES=ON
cmake --build cmake-build-web --parallel
```

Serve the generated files rather than opening the HTML directly:

```bash
python3 -m http.server 8000 --directory cmake-build-web/bin
```

For example, open `http://127.0.0.1:8000/example_ui_gallery.html`.
Each example produces an HTML entry point, an ES module, a WebAssembly module, and resource data when the target packages resources.

CLI applications can generate and run a source-controlled Web shell directly:

```bash
huxerui create hello_huxer --platform web
cd hello_huxer
huxerui doctor web
huxerui build web
huxerui run web
```

The CLI uses `emcmake` for configuration and `emrun` to serve and open the generated application entry point.
Web CLI projects currently require `HUXERUI_SDK_ROOT` to identify a source SDK checkout.

The configured Emscripten compiler must provide the C++20 language and library support required by HuxerUI; obsolete toolchains are not supported through compatibility headers.
The backend remains a technical preview until platform-neutral semantics and browser accessibility mapping, broader browser integration tests, production packaging, and real mobile-browser IME validation are complete.
See [Web Platform Design](design/web.md) for the implemented boundary and deferred work.

## Planned platforms

OHOS should reuse the same Runtime and add one platform-specific `PlatformAdapter` integration. Platform availability and cross-build support must be reported explicitly by future SDK and CLI tooling.

See the [SDK, CLI, and Module Design](design/sdk-cli.md) for the planned distribution and native-module model.
