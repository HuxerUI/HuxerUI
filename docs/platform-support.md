# Platform Support

## Supported backends

| Platform | Host | Text layout | Rendering | Text input |
|---|---|---|---|---|
| Android | Native View | StaticLayout | Canvas | InputConnection and IME |
| macOS | AppKit | CoreText | CoreGraphics | NSTextInputClient |
| Windows | Win32 | DirectWrite | Direct2D | Native keyboard and IME adapter |

State, recomposition, node reconciliation, layout, hit testing, focus, scrolling, text editing behavior, and retained-scene generation remain in the shared C++ runtime.

## Runtime and PlatformHost

Each native host view owns one `Runtime`. Multiple host views may share the same registered root factory without sharing state, layout, frame scheduling, focus, or input sessions.

```cpp
class NativeHost final : public PlatformHost {
  // Implement the native frame, text, input, and rendering boundary.
};

NativeHost host;
Runtime runtime{
    {
        .root_factory = App,
        .options = {.title = "HuxerUI"},
    },
    host,
};

runtime.SetViewport({width, height});
const FrameCommit& commit = runtime.BuildFrame();
renderer.Render(commit.render_frame);
if (commit.next_frame_deadline.has_value()) {
  host.RequestFrameAt(*commit.next_frame_deadline);
}
```

Platform adapters translate density, native coordinate systems, key events, pointer events, IME commands, clipboard operations, and renderer conventions.
They traverse the committed `RenderScene` in `commit.render_frame` and do not duplicate component state machines or layout behavior.
`PlatformHost::RequestFrameAt()` accepts an absolute monotonic deadline.
Runtime uses it for invalidations outside frame construction; work discovered while building is returned through `FrameCommit::next_frame_deadline`.
The host presents the committed frame before scheduling that deadline, which prevents continuous animation from starving the native paint phase.
macOS and Windows translate `DamageRegion` into native invalidation bounds.
Android receives the same committed damage but invalidates the complete native View because current Android View APIs ignore dirty rectangles.
All three backends replay only the committed scene during native paint callbacks.

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

## macOS

The macOS backend creates an AppKit host, renders through CoreGraphics, measures text with CoreText, and exposes a dedicated `NSTextInputClient` adapter for native selection, composition, and geometry queries. Scheduled callbacks commit Runtime work before AppKit invalidation, while `drawRect:` only presents the committed scene.

Example targets build as application bundles and can be launched from `build/bin`.

## Windows

The Windows backend targets Windows 10 and later. It owns the Win32 window, uses DirectWrite for text layout, and renders shared PaintCommands through Direct2D.

## Planned platforms

iOS, OHOS, Linux, and Web should reuse the same Runtime and add one platform-specific `PlatformHost` integration. Platform availability and cross-build support must be reported explicitly by future SDK and CLI tooling.

See the [SDK, CLI, and Module Design](design/sdk-cli.md) for the planned distribution and native-module model.

