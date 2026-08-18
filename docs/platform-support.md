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
The shared Runtime publishes an immutable `SemanticFrame` with built-in semantics, secure TextField redaction, action routing, and NodeExtension virtual children.
Android exposes the shared frame through virtual `AccessibilityNodeInfo` descendants, iOS maps it through a retained UIKit accessibility hierarchy, macOS maps it through AppKit accessibility, and Windows exposes it as a UI Automation fragment tree with role-specific control patterns and events.
Linux and Web native mappings remain planned as defined in [Semantics and Accessibility Design](design/semantics.md).

## Runtime and PlatformAdapter

Each application surface owns one `Runtime`. Multiple surfaces may use the same `Application` without sharing state, layout, frame scheduling, focus, or input sessions.

```cpp
class NativeAdapter final : public PlatformAdapter {
  // Implement the native frame, text, input, and rendering boundary.
};

NativeAdapter platform;
Application application{
    App,
    {
        .window = {.title = "HuxerUI"},
    },
};
Runtime runtime{application, platform};

runtime.SetWindowMetrics({
    .viewport = {width, height},
    .safe_area = safe_area,
});
const FrameCommit& commit = runtime.BuildFrame();
renderer.Render(commit.render_frame);
if (commit.next_frame_deadline.has_value()) {
  platform.RequestFrameAt(*commit.next_frame_deadline);
}
```

Platform adapters translate density, native coordinate systems, key events, pointer events, IME commands, clipboard operations, packaged resource reads, and renderer conventions.
Full-window mobile adapters submit viewport and safe-area geometry atomically through `Runtime::SetWindowMetrics()` and apply the light or dark system-bar foreground resolved by Runtime.
Desktop client areas submit zero insets.
PlatformAdapter also implements the shared `TextMeasurer` service, resolving platform-neutral Font and TextStyle values through the native text stack.
They traverse the committed `RenderScene` in `commit.render_frame` and do not duplicate component state machines or layout behavior.
`SemanticFrame` is a second committed Runtime output beside RenderFrame, not data reconstructed by a renderer or inferred from concrete components in a platform adapter.
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
- Prefab metadata in the current source Gradle module
- The `example_runner` application module

Generated source-SDK applications use the Gradle library for Java integration while their app module configures the application root `CMakeLists.txt` directly for the shared native library and final resources.
The generated Gradle shell owns Android SDK and NDK versions, ABIs, identifiers, native and Java dependencies, manifests, signing, and packaging policy without a CMake configuration projection.

A full-screen launcher derives from `HuxerUIActivity`:

```java
public final class MainActivity extends HuxerUIActivity {}
```

The application native library is named `huxerui_app`. Loading it constructs the application's static `Application` before the activity creates its `HuxerUIView`; the adapter requires that exactly one application declaration is alive when it creates a Runtime session.

`HuxerUIActivity` owns a lifecycle-bound Back callback and forwards Back to the shared Runtime. Applications using this full-screen Activity set `android:enableOnBackInvokedCallback="true"` on their manifest `application` element, as the example runner does. Android 14 and later forward predictive Back start, progress, cancel, and commit phases; Android 13 forwards Commit; older versions use `onBackPressed()` for the same Commit path. When Runtime does not consume Commit, the Activity calls its overridable `onUnhandledBack()` fallback, which finishes the Activity with transition by default. An embedded integration owns registration itself, may call `HuxerUIView.handleBack()`, and continues its native fallback only when that method returns `false`.

Coordinates remain density independent. The Android integration maps multi-touch, mouse hover, wheel, keyboard, viewport, and frame-clock events to the shared model. Frame callbacks commit Runtime work before full View invalidation, while `onDraw()` only presents the committed scene. The minimum supported Android API level is 23.

RootHook-installed Android PlatformView factories create controlled Views under the per-surface `PlatformModules` registry. `HuxerUIView` is a ViewGroup that retains compatible native instances, applies complete property revisions, clips and positions them in logical coordinates, and alternates HuxerUI Canvas slices with ordinary child drawing in final `RenderComposition` order. Platform-specific module source includes `<huxerui/android/platform_view.h>` and registers an `android::PlatformViewFactory`; `<huxerui/android/jni.h>` supplies move-only local references and checked UTF-8, Java String, and byte-array conversion without adding a Java View or module base class. The Android `example_platform_view` target uses this path to host a controlled EditText. SurfaceView subtrees are rejected because they cannot preserve this Canvas and child-drawing order. Shared hit testing routes touch, hover, and wheel input to the frontmost HuxerUI or native target. Native focus changes, Runtime-driven focus, Tab traversal, IME dismissal, and accessibility traversal remain synchronized across that boundary. The accessibility provider replaces each PlatformView semantic anchor with the real Android View subtree at the same sibling position.

Java-backed nonvisual modules register `android::PlatformModuleFactory` from `<huxerui/android/platform_module.h>`. The Android adapter owns the current surface Context and injects it with the calling-thread `JNIEnv` when creating the native instance; neither value enters `PlatformPayload`, `RootContext`, or application services. Native result and event sinks may run on background threads and return through the existing `HuxerUIView` UI-thread dispatcher. The Android `example_platform_module` uses this path for a Java scheduled timer while sharing its typed C++ service across the supported native platforms.
`android::ExternalTextureSource` accepts retained `android.graphics.Bitmap` frames from any Java-attached producer thread. Its bounded latest-frame mailbox schedules through the owning `HuxerUIView`, while the Canvas renderer acquires one coherent frame per physical draw, preserves the last acquired frame, and releases inactive cache entries. Publish retains the Bitmap independently, so the producer may release its own Java reference but must not recycle or mutate that frame afterward; recycled or generation-changed frames fail explicitly during drawing. The example module publishes generated Bitmaps without per-frame PlatformModule calls. This API 23 path supports software and hardware-backed Bitmaps but does not claim zero-copy or direct `AHardwareBuffer` import.

`android::LocalRef<T>` adopts one JNI local object reference and releases it through the creating `JNIEnv`; it remains on that JNI thread and must not outlive the native call that owns the local-reference frame. String conversion preserves embedded nulls and supplementary Unicode without using JNI modified UTF-8, rejects null or malformed input, and reports values beyond JNI signed array limits. A JVM allocation failure returns an empty local reference with its Java exception still pending.

`HuxerUIView` also exposes the committed shared semantics as an Android virtual accessibility hierarchy. TalkBack queries stay in Java, accessibility focus and touch exploration remain provider-owned, and supported actions return to the shared Runtime on the UI thread.
Rounded-rectangle shadows use hardware shadow layers on API 28 and later, with density-aware cached alpha masks on older supported versions.
Arbitrary Paths use the same native Canvas, and Path shadows use hardware layers on API 28 and later with a bounded software mask fallback on older supported versions.
Neither path disables hardware acceleration for the complete HuxerUIView.
Packaged resources are read from Android assets, system changes proactively update the Runtime resource configuration, and encoded images are transferred to Java only on a Bitmap cache miss.
Debug process metrics use `getrusage`, `Debug.getPss()`, and the online processor count.

## macOS

The macOS backend creates an AppKit window and View, renders through CoreGraphics, measures text with CoreText, and exposes a dedicated `NSTextInputClient` adapter for native selection, composition, and geometry queries. Scheduled callbacks and native view-size changes commit Runtime work before AppKit invalidation, while `drawRect:` only presents the committed scene.
The adapter dispatches nonvisual `PlatformInstance` results and events asynchronously through the AppKit main queue, preserving the owning Runtime thread and preventing synchronous native completion from reentering application callbacks.
RootHook-installed macOS PlatformView factories create controlled NSViews under the per-surface `PlatformModules` registry. The adapter retains compatible native instances, applies property revisions, clips and positions them in logical coordinates, and alternates transparent HuxerUI slice views with native containers in final `RenderComposition` order. Shared hit testing routes a point either to the frontmost HuxerUI interaction or to the native subtree without placing native views in a global foreground plane. AppKit first-responder changes synchronize the shared PlatformView focus leaf, and the accessibility bridge substitutes the unignored native accessibility root at its `SemanticFrame` anchor.
Platform-specific module source includes `<huxerui/macos/platform_view.h>` and calls `root.Modules().Register(type, macos::PlatformViewFactory{...})` from its explicit RootHook. The factory creates an NSView from the complete initial `PlatformPayload`, optionally applies later complete-property updates, emits declared events through its `PlatformEventSink`, and may release module-owned native state from its dispose callback.
The Apple `example_platform_module` implementation registers a nonvisual Foundation timer factory and exposes it as the same typed Root Service used across the supported native platforms. It demonstrates asynchronous Start and Stop calls, ordered Tick events, request cancellation, and native disposal without exposing payloads or wire names to application code. On macOS and iOS it also registers a color-stream service that returns one `ExternalTexture` capability, publishes latest-wins `CVPixelBuffer` frames, and lets Image render them without per-frame module calls.
The Apple `example_platform_view` target embeds a controlled native text field, routes native edits through a typed HuxerUI event, updates properties from application State, and exercises native unmount and recreation.
Custom window chrome extends the AppKit content view through a transparent title bar while preserving native traffic lights, accessibility, resizing, and window metadata. The adapter centers the native traffic lights within the resolved title-bar height, converts their actual bounds into shared metrics, and delegates marked drag regions and window commands back to AppKit.
Core Graphics resolves retained shadow commands with native blurred path shadows.
Canvas Paths map directly to Core Graphics fill, stroke, clip, and shadow operations.
Packaged resources are read from the application bundle, locale and backing-scale changes proactively update the Runtime resource configuration, and ImageIO-backed decoded images remain renderer-owned.
`macos::ExternalTextureSource` accepts platform-owned `CVPixelBuffer` frames from any producer thread. The AppKit renderer acquires only the newest pending frame, converts it through Core Image into its retained Core Graphics cache, and releases inactive cache entries during drawing. This path is bounded and does not claim zero-copy.
Debug process metrics use `getrusage`, Mach task information, and `NSProcessInfo`.

Example targets build as application bundles and can be launched from `build/bin`.

## iOS technical preview

The iOS backend creates a UIKit window and safe-area-constrained HuxerUI View, measures text with CoreText, and replays the shared RenderScene through an independent CoreGraphics renderer. CADisplayLink schedules Runtime commits before UIKit invalidation, while `drawRect:` presents only the committed frame.

Multi-touch, Apple Pencil, indirect pointer, hardware keyboard, clipboard, locale, display scale, keyboard viewport, and packaged-resource events cross one UIKit adapter boundary. A dedicated UITextInput implementation maps native UTF-16 positions, selection, marked text, actions, and caret geometry to the shared text-input session and revision protocol. UIKit does not own a second editing value.

RootHook-installed iOS PlatformView factories create controlled UIViews under the per-surface `PlatformModules` registry. The adapter retains compatible native instances, applies complete property revisions, clips and positions them in logical coordinates, and alternates transparent HuxerUI slice views with native containers in final `RenderComposition` order. Shared hit testing preserves that order, while touch focus and Runtime-driven focus changes synchronize the shared PlatformView focus leaf with UIKit. Platform-specific module source includes `<huxerui/ios/platform_view.h>` and registers an `ios::PlatformViewFactory`; the Apple `example_platform_view` target uses this path to host a controlled UITextField. The UIKit accessibility hierarchy substitutes the native UIView subtree at its PlatformView semantic anchor. Hardware-keyboard traversal across the PlatformView boundary remains follow-up work.

The iOS adapter dispatches nonvisual `PlatformInstance` results and events asynchronously through the main queue. The Apple `example_platform_module` uses this path for a Foundation timer while sharing its typed C++ service and factory implementation across Apple platforms.
`ios::ExternalTextureSource` accepts platform-owned `CVPixelBuffer` frames from any producer thread. The UIKit renderer acquires only the newest pending frame from its iOS-owned mailbox, converts it through Core Image into its retained Core Graphics cache, and releases inactive cache entries during drawing. This path is bounded and does not claim zero-copy.

The minimum deployment target is iOS 13. CLI projects contain a source-controlled native Xcode project that owns application packaging, signing, native assets, launch metadata, and debugging. Its build phase links an architecture-correct CMake application core containing the common C++ application and HuxerUI. The CLI discovers paired devices through `devicectl`, discovers booted Simulators through `simctl`, and keeps their DerivedData directories separate:

```bash
huxerui create app hello_huxer --platform ios
cd hello_huxer
huxerui doctor ios
huxerui devices ios
huxerui open ios
huxerui run ios --device <id>
```

Physical-device development builds use Xcode automatic signing and the `DEVELOPMENT_TEAM` value in `platform/ios/Config/Local.xcconfig` or the native project settings. `huxerui open ios` records the detected HuxerUI SDK in that ignored local file before opening the project. Distribution archive automation, export signing, a public embeddable UIView, and native selection rectangles remain outside the preview.

The repository-owned `platform/ios/example_runner/HuxerUIExamples.xcodeproj` provides one native debugging host for every CMake `example_*` application target. Its ignored local configuration selects the example and optional signing team, while the shared project continues to build the selected application core through CMake.

## Windows

The Windows backend targets Windows 10 and later by default.
It owns the Win32 window, uses DirectWrite for text layout, and renders shared PaintCommands through a Direct2D device context backed by D3D11 and a DXGI swap chain.
Custom window chrome keeps system window styles and commands while making the complete restored window a normal HuxerUI client surface.
HuxerUI draws standard caption controls, and the adapter maps resize, drag, and maximize-button geometry back to native non-client hit testing, including Windows 11 Snap Layout.
Partial Runtime damage updates a retained scene bitmap before the affected pixels are presented.
Direct2D Shadow effects consume cached rounded-rectangle masks while color, opacity, and offset remain draw-time properties.
Canvas Paths map to Direct2D path geometry for fill, stroke, geometric clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory, locale and DPI changes proactively update the Runtime resource configuration, and WIC decoding produces device-dependent Direct2D bitmap cache entries.
Debug process metrics use process times, working-set counters, and the native logical processor count.
Nonvisual Windows modules register the platform-neutral `PlatformModuleFactory` from CMake-native sources under `platform/windows/src`.
Native results and events enter a FIFO owned by the adapter and wake its UI thread through a coalesced private window message; work emitted before HWND creation waits until attachment, and shutdown discards late callbacks.
The Windows `example_platform_module` implementation uses a thread-pool timer behind the same typed Timer Root Service used by Android, Apple platforms, and Linux.
RootHook-installed `windows::PlatformViewFactory` registrations create same-process, same-thread child HWNDs inside framework clipping containers.
When a committed scene contains PlatformViews, one premultiplied DirectComposition surface replays every retained HuxerUI slice and clears ordered rectangular apertures that expose those child windows; no surface is allocated per slice.
The input overlay arbitrates each hit through Runtime, focus and Tab traversal cross the native boundary through the shared PlatformView identity, and UI Automation attaches the native HWND provider beneath its semantic anchor.
The Windows `example_platform_view` target hosts a controlled native edit control through this path.

`HUXERUI_WINDOWS_7_COMPAT=ON` builds an opt-in binary for Windows 7 SP1 with Platform Update or later.
That build resolves modern per-monitor DPI APIs at runtime, uses system-DPI fallbacks on Windows 7, and falls back from flip presentation to a sequential bitblt swap chain when necessary.
Custom window chrome uses the same HuxerUI controls with the compatibility renderer; Windows 11 Snap Layout is naturally unavailable there.
PlatformView composition requires DirectComposition and fails explicitly when that capability is unavailable.
Windows 7 without Platform Update is not supported.

## Linux

The Linux backend creates an X11 window, measures text with FreeType and HarfBuzz, rasterizes shared PaintCommands with Cairo into a retained device-pixel bitmap, and presents it through an EGL/OpenGL ES 2 swap.
Partial Runtime damage limits Cairo redraw to the affected pixel bounds; the retained bitmap is then presented whole or as damaged rows, matching the Windows cost model of a retained scene bitmap plus swap-chain presentation.
Canvas Paths map to Cairo path geometry for fill, stroke, clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory (overridable with `HUXERUI_RESOURCES_DIR`), locale and `Xft.dpi` changes update the Runtime resource configuration, and libpng/libjpeg decoding produces Cairo bitmap cache entries with a bounded byte budget.

Text input uses the X Input Method protocol with full preedit callbacks, mirroring the Windows IMM32 adapter; when no input method is available the backend degrades gracefully to direct key text.
Clipboard reads and writes use the X11 `CLIPBOARD` selection with UTF-8 string transfers.
Nonvisual module results and events enter a thread-safe FIFO and wake the X11 event loop through `eventfd`, so application callbacks run asynchronously on the Runtime thread without native-thread reentry.
`linux::ExternalTextureSource` accepts borrowed, untagged sRGB RGBA8888 or BGRA8888 pixel spans with explicit dimensions and row stride from any producer thread.
Publish validates and copies each frame into a bounded latest-wins mailbox, converting straight alpha to Cairo's native premultiplied ARGB32 representation so the producer may immediately reuse its buffer.
The Cairo renderer acquires one coherent frame per physical draw, retains the last acquired frame, applies Image cropping, sampling, transforms, clipping, and opacity, and releases inactive cache entries.
This path is bounded and does not claim zero-copy or native DMA-BUF import.
The Linux `example_platform_module` registers a platform-neutral timer factory backed by `timerfd` and a background RGBA color stream, exposing the same typed Root Services and disposal behavior as the other supported native implementations.
System dependencies are resolved through pkg-config for X11, Xext, XKB common, XRandR, EGL, and OpenGL ES 2; source-checkout builds fetch the pinned graphics, text, image, and compression libraries used by the renderer.
Host tools are distributed as prebuilt executables under `tools/prebuilt/linux/<architecture>/`, matching the macOS and Windows distribution model.
The CLI records Linux enablement under `platform/linux`, builds the root CMake application in `.huxerui/build/linux/<profile>`, and launches the exact executable recorded by generated application integration metadata.
The current Linux CLI path uses a source SDK checkout. A relocatable installed Linux SDK remains release-packaging work because its static dependency closure must be packaged without build-tree paths.

## Web technical preview

The Web backend compiles the same static `Application` declaration through Emscripten, mounts one `Runtime` and `WebPlatformAdapter` pair per browser-owned host element, and emits an ES module with WebAssembly output.
Canvas 2D replays the shared `RenderScene`, while browser Pointer Events, wheel events, keyboard events, hidden native text controls, resource preloading, asynchronous `ImageBitmap` decoding, and browser-event-loop PlatformModule dispatch remain platform-owned services.
Web module sources use the existing platform-neutral `PlatformModuleFactory` from C++ and Emscripten glue rather than a second JavaScript registry; the Web `example_platform_module` registers an interval-backed Timer through the same typed Root Service used by native platforms.
RootHook-installed `web::PlatformViewFactory` registrations return detached `HTMLElement` values through Emscripten. The adapter retains compatible elements, applies controlled property revisions, clips and positions them in logical coordinates, and alternates DOM containers with base and transparent Canvas slices in final `RenderComposition` order. Root-capture hit arbitration and focus synchronization preserve the boundary between native browser interaction and shared Runtime input. The Web `example_platform_view` hosts a controlled native input through this path. Browser accessibility substitution remains deferred with the wider Web semantics bridge.

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
huxerui create app hello_huxer --platform web
cd hello_huxer
huxerui doctor web
huxerui build web
huxerui run web
```

The CLI uses `emcmake` for configuration and `emrun` to serve and open the generated application entry point.
Web CLI projects currently require `HUXERUI_HOME` to identify a source SDK checkout.
The formal SDK design adds an Emscripten-compatible SDK archive while preserving the same root-project configuration and SDK-home discovery contract.

The configured Emscripten compiler must provide the C++20 language and library support required by HuxerUI; obsolete toolchains are not supported through compatibility headers.
The backend remains a technical preview until platform-neutral semantics and browser accessibility mapping, broader browser integration tests, production packaging, and real mobile-browser IME validation are complete.
See [Web Platform Design](design/web.md) for the implemented boundary and deferred work.

## Planned platforms

OHOS should reuse the same Runtime and add one platform-specific `PlatformAdapter` integration. Platform availability and cross-build support must be reported explicitly by future SDK and CLI tooling.

See the [SDK, CLI, and Module Design](design/sdk-cli.md) for the planned distribution and native-module model.
