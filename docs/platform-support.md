# Platform Support

## Supported backends

| Platform | Application surface | Text layout | Rendering | Text input |
|---|---|---|---|---|
| Android | Native View | StaticLayout | Canvas | InputConnection and IME |
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
When the debug performance panel is open, `PlatformAdapter::QueryProcessMetrics()` optionally reports cumulative process CPU time, a current process-memory footprint, and logical processor count. Android reports proportional set size (PSS); Windows and macOS report their current working-set or resident-set values. Runtime derives interval CPU utilization and keeps the sampling lifecycle in the platform layer.

## Android

The Android integration provides:

- `HuxerUIActivity` for full-screen applications
- `HuxerUIView` for embedding HuxerUI in an existing Android interface
- The `huxerui` Gradle library module
- The `demo` application module

A full-screen launcher derives from `HuxerUIActivity`:

```java
public final class MainActivity extends HuxerUIActivity {}
```

The application native library is named `huxerui_app`. Loading it registers the immutable `HUXERUI_APP` definition before the activity creates its `HuxerUIView`.

`HuxerUIActivity` owns a lifecycle-bound Android 13 Back callback and forwards Back to the shared Runtime. Applications using this full-screen Activity set `android:enableOnBackInvokedCallback="true"` on their manifest `application` element, as the demo module does. When Runtime returns `false`, the Activity calls its overridable `onUnhandledBack()` fallback, which finishes the Activity with transition by default. On older Android versions, `onBackPressed()` forwards to the same Runtime path before calling the native Activity fallback. An embedded integration owns registration itself, may call `HuxerUIView.handleBack()`, and continues its native fallback only when that method returns `false`.

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

The configured Emscripten compiler must provide the C++20 language and library support required by HuxerUI; obsolete toolchains are not supported through compatibility headers.
The backend remains a technical preview until platform-neutral semantics and browser accessibility mapping, broader browser integration tests, production packaging, and real mobile-browser IME validation are complete.
See [Web Platform Design](design/web.md) for the implemented boundary and deferred work.

## Planned platforms

iOS, OHOS, and Linux should reuse the same Runtime and add one platform-specific `PlatformAdapter` integration. Platform availability and cross-build support must be reported explicitly by future SDK and CLI tooling.

See the [SDK, CLI, and Module Design](design/sdk-cli.md) for the planned distribution and native-module model.
