# Platform Support

## Supported backends

| Platform | Host | Text layout | Rendering | Text input |
|---|---|---|---|---|
| Android | Native View | StaticLayout | Canvas | InputConnection and IME |
| macOS | AppKit | CoreText | CoreGraphics | NSTextInputClient |
| Windows | Win32 | DirectWrite | Direct2D | Native keyboard and IME adapter |

State, recomposition, node reconciliation, layout, hit testing, focus, scrolling, text editing behavior, and retained-scene generation remain in the shared C++ runtime.

## Runtime and PlatformAdapter

Each native host view owns one `Runtime`. Multiple host views may share the same registered root factory without sharing state, layout, frame scheduling, focus, or input sessions.

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
The host presents the committed frame before scheduling that deadline, which prevents continuous animation from starving the native paint phase.
macOS and Windows translate `DamageRegion` into native invalidation bounds.
Android receives the same committed damage but invalidates the complete native View because current Android View APIs ignore dirty rectangles.
All three backends replay only the committed scene during native paint callbacks.
Exact `DrawTextRunsCommand` geometry is supplied by TextMeasurer and is not replaced by renderer-side layout decisions.
Native font, layout, and decoded-image caches are host-owned and bounded; see [Text and Font Design](design/text.md) and [App Resources, Images, and Localization Design](design/resources.md).

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

Coordinates remain density independent. The host maps multi-touch, mouse hover, wheel, keyboard, viewport, and frame-clock events to the shared model. Frame callbacks commit Runtime work before full View invalidation, while `onDraw()` only presents the committed scene. The minimum supported Android API level is 23.
Rounded-rectangle shadows use hardware shadow layers on API 28 and later, with density-aware cached alpha masks on older supported versions.
Arbitrary Paths use the same native Canvas, and Path shadows use hardware layers on API 28 and later with a bounded software mask fallback on older supported versions.
Neither path disables hardware acceleration for the complete host View.
Packaged resources are read from Android assets, system changes proactively update the Runtime resource configuration, and encoded images are transferred to Java only on a Bitmap cache miss.

## macOS

The macOS backend creates an AppKit host, renders through CoreGraphics, measures text with CoreText, and exposes a dedicated `NSTextInputClient` adapter for native selection, composition, and geometry queries. Scheduled callbacks commit Runtime work before AppKit invalidation, while `drawRect:` only presents the committed scene.
Core Graphics resolves retained shadow commands with native blurred path shadows.
Canvas Paths map directly to Core Graphics fill, stroke, clip, and shadow operations.
Packaged resources are read from the application bundle, locale and backing-scale changes proactively update the Runtime resource configuration, and ImageIO-backed decoded images remain renderer-owned.

Example targets build as application bundles and can be launched from `build/bin`.

## Windows

The Windows backend targets Windows 10 and later by default.
It owns the Win32 window, uses DirectWrite for text layout, and renders shared PaintCommands through a Direct2D device context backed by D3D11 and a DXGI swap chain.
Partial Runtime damage updates a retained scene bitmap before the affected pixels are presented.
Direct2D Shadow effects consume cached rounded-rectangle masks while color, opacity, and offset remain draw-time properties.
Canvas Paths map to Direct2D path geometry for fill, stroke, geometric clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory, locale and DPI changes proactively update the Runtime resource configuration, and WIC decoding produces device-dependent Direct2D bitmap cache entries.

`HUXERUI_WINDOWS_7_COMPAT=ON` builds an opt-in binary for Windows 7 SP1 with Platform Update or later.
That build resolves modern per-monitor DPI APIs at runtime, uses system-DPI fallbacks on Windows 7, and falls back from flip presentation to a sequential bitblt swap chain when necessary.
Windows 7 without Platform Update is not supported.

## Planned platforms

iOS, OHOS, Linux, and Web should reuse the same Runtime and add one platform-specific `PlatformAdapter` integration. Platform availability and cross-build support must be reported explicitly by future SDK and CLI tooling.

See the [SDK, CLI, and Module Design](design/sdk-cli.md) for the planned distribution and native-module model.

