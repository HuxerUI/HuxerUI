# Platform Support

## Supported backends

| Platform | Application surface | Text layout | Rendering | Text input | HTTP |
|---|---|---|---|---|---|
| Windows | Win32 | DirectWrite | Direct2D | Keyboard and IMM32 adapter | Planned |
| macOS | AppKit | CoreText | CoreGraphics | NSTextInputClient | NSURLSession |
| Linux | X11 Window | FreeType and HarfBuzz | Cairo and EGL/OpenGL ES | XIM | libsoup 3 |
| Web | Browser Canvas | Canvas TextMetrics | Canvas 2D | Hidden input, textarea, and composition events | Fetch |
| Android | Android View | StaticLayout | Canvas | InputConnection and IME | HttpURLConnection |
| iOS | UIKit View | CoreText | CoreGraphics | UITextInput | NSURLSession |

State, recomposition, node reconciliation, layout, hit testing, focus, scrolling, text editing behavior, and retained-scene generation remain in the shared C++ runtime.
The shared Runtime publishes an immutable `SemanticFrame` with built-in semantics, secure TextField redaction, action routing, and NodeExtension virtual children.
Windows exposes the shared frame as a UI Automation fragment tree with role-specific control patterns and events, macOS maps it through AppKit accessibility, Android exposes it through virtual `AccessibilityNodeInfo` descendants, and iOS maps it through a retained UIKit accessibility hierarchy.
Linux and Web platform mappings remain planned as defined in [Semantics and Accessibility Design](design/semantics.md).

## Runtime and PlatformAdapter

Each application surface owns one `Runtime`. Multiple surfaces may use the same `Application` without sharing state, layout, frame scheduling, focus, or input sessions.

```cpp
class ExamplePlatformAdapter final : public PlatformAdapter {
  // Implement the platform frame, text, input, and rendering boundary.
};

ExamplePlatformAdapter platform;
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

Platform adapters translate density, platform coordinate systems, key events, pointer events, IME commands, clipboard operations, packaged resource reads, and renderer conventions.
Full-window mobile adapters submit viewport and safe-area geometry atomically through `Runtime::SetWindowMetrics()` and apply the light or dark system-bar foreground resolved by Runtime.
Desktop client areas submit zero insets.
PlatformAdapter also implements the shared `TextMeasurer` service, resolving platform-neutral Font and TextStyle values through the platform text stack.
Its optional private HttpTransport capability backs the built-in HttpClient Root Service without exposing native networking types or using PlatformModule payloads.
They traverse the committed `RenderScene` in `commit.render_frame` and do not duplicate component state machines or layout behavior.
`SemanticFrame` is a second committed Runtime output beside RenderFrame, not data reconstructed by a renderer or inferred from concrete components in a platform adapter.
`PlatformAdapter::RequestFrameAt()` accepts an absolute monotonic deadline.
Runtime uses it for invalidations outside frame construction; work discovered while building is returned through `FrameCommit::next_frame_deadline`.
The platform adapter presents the committed frame before scheduling that deadline, which prevents continuous animation from starving the platform paint phase.
Windows and macOS translate `DamageRegion` into platform invalidation bounds.
Android receives the same committed damage but invalidates the complete `HuxerUIView` because current Android View APIs ignore dirty rectangles.
All three backends replay only the committed scene during platform paint callbacks.
Exact `DrawTextRunsCommand` geometry is supplied by TextMeasurer and is not replaced by renderer-side layout decisions.
Font, layout, and decoded-image caches are platform-owned and bounded; see [Text and Font Design](design/text.md) and [App Resources, Images, and Localization Design](design/resources.md).
When the debug performance panel is open, `PlatformAdapter::QueryProcessMetrics()` optionally reports cumulative process CPU time, a current process-memory footprint, and logical processor count. Android reports proportional set size (PSS); Windows and macOS report their current working-set or resident-set values. Runtime owns the sampling lifecycle and derives interval CPU utilization while platform-specific metric collection remains behind the adapter boundary.

## Windows

The Windows backend targets Windows 10 and later by default.
It owns the Win32 window, uses DirectWrite for text layout, and renders shared PaintCommands through a Direct2D device context backed by D3D11 and a DXGI swap chain.
Custom window chrome keeps system window styles and commands while making the complete restored window a normal HuxerUI client surface.
HuxerUI draws standard caption controls, and the adapter maps resize, drag, and maximize-button geometry back to Win32 non-client hit testing, including Windows 11 Snap Layout.
Partial Runtime damage updates a retained scene bitmap before the affected pixels are presented.
Direct2D Shadow effects consume cached rounded-rectangle masks while color, opacity, and offset remain draw-time properties.
Canvas Paths map to Direct2D path geometry for fill, stroke, geometric clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory, locale and DPI changes proactively update the Runtime resource configuration, and WIC decoding produces device-dependent Direct2D bitmap cache entries.
Debug process metrics use process times, working-set counters, and the system logical processor count.
FileSystem derives its application identity from the executable filename and exposes protected `data`, `cache`, and `temporary` children under the current user's Local App Data directory.
FilePicker uses the system open and save dialogs, retains selected paths behind `FileReference`, and executes external reads and copies through the shared bounded file executor while dialog presentation and cancellation remain on the UI thread.
The application shell maps one startup URL or a command line containing only existing regular files into the shared application activation model before the first composition; unrecognized or mixed arguments remain an ordinary launch.
Later external URL or file launches forward their validated UTF-16 arguments to a window created by the same executable, restore that window, and enter the existing Runtime activation queue; ordinary launches remain independent and are never forced into single-instance behavior.
Applications and packaging own URL protocol registration. `example_application` registers `huxerui-example` for the current Windows user so a browser can exercise cold and subsequent URL activation.
Window activation updates the shared application lifecycle to `Active` or `Inactive`, while minimization changes it to `Background`.
Nonvisual Windows PlatformModules register the platform-neutral `PlatformModuleFactory` from CMake sources under `platform/windows/src`.
Platform results and events enter a FIFO owned by the adapter and wake its UI thread through a coalesced private window message; work emitted before HWND creation waits until attachment, and shutdown discards late callbacks.
The Windows `example_platform_module` implementation uses a thread-pool timer behind the same typed Timer Root Service used by Apple platforms, Linux, and Android.
`windows::ExternalTextureSource` accepts borrowed, untagged sRGB RGBA8888 or BGRA8888 pixel spans with explicit dimensions and row stride from any producer thread.
Publish copies straight-alpha pixels into a bounded latest-wins premultiplied BGRA mailbox, and the Direct2D renderer retains and updates one bitmap per active source while preserving its last CPU frame across device recreation.
This bounded path does not claim zero-copy or direct shared-D3D texture import; the Windows PlatformModule example uses it for the same generated color stream demonstrated by the other backends.
RootHook-installed `windows::PlatformViewFactory` registrations create same-process, same-thread child HWNDs inside framework clipping containers.
When a committed scene contains PlatformViews, one premultiplied DirectComposition surface replays every retained HuxerUI slice and clears ordered rectangular apertures that expose those child windows; no surface is allocated per slice.
The input overlay arbitrates each hit through Runtime, focus and Tab traversal cross the PlatformView boundary through the shared identity, and UI Automation attaches the HWND provider beneath its semantic anchor.
The Windows `example_platform_view` target hosts a controlled `PlatformTextField` through this path.

`HUXERUI_WINDOWS_7_COMPAT=ON` builds an opt-in binary for Windows 7 SP1 with Platform Update or later.
That build resolves modern per-monitor DPI APIs at runtime, uses system-DPI fallbacks on Windows 7, and falls back from flip presentation to a sequential bitblt swap chain when necessary.
Custom window chrome uses the same HuxerUI controls with the compatibility renderer; Windows 11 Snap Layout is naturally unavailable there.
PlatformView composition requires DirectComposition and fails explicitly when that capability is unavailable.
Windows 7 without Platform Update is not supported.

## macOS

The macOS backend creates an AppKit window and View, renders through CoreGraphics, measures text with CoreText, and exposes a dedicated `NSTextInputClient` adapter for AppKit selection, composition, and geometry queries. Scheduled callbacks and AppKit view-size changes commit Runtime work before invalidation, while `drawRect:` only presents the committed scene.
AppKit application activation updates the shared lifecycle to `Active` or `Inactive`, and hiding the application changes it to `Background`.
It implements HttpClient through an ephemeral NSURLSession, returns complete in-memory responses, and cancels native data tasks when their owning HuxerUI Task is canceled.
It implements FilePicker through NSOpenPanel and NSSavePanel, retains security-scoped file references, and coordinates external reads, imports, and replacements away from the Runtime thread.
The adapter dispatches nonvisual `PlatformInstance` results and events asynchronously through the AppKit main queue, preserving the owning Runtime thread and preventing synchronous platform completion from reentering application callbacks.
RootHook-installed macOS PlatformView factories create controlled NSViews under the per-surface `PlatformModules` registry. The adapter retains compatible PlatformView instances, applies property revisions, clips and positions them in logical coordinates, and alternates transparent HuxerUI slice views with platform containers in final `RenderComposition` order. Shared hit testing routes a point either to the frontmost HuxerUI interaction or to the PlatformView subtree without placing PlatformViews in a global foreground plane. AppKit first-responder changes synchronize the shared PlatformView focus leaf, and the accessibility bridge substitutes the unignored NSView accessibility root at its `SemanticFrame` anchor.
Library platform source includes `<huxerui/macos/platform_view.h>` and calls `root.Modules().Register(type, macos::PlatformViewFactory{...})` from its explicit RootHook. The factory creates an NSView from the complete initial `PlatformPayload`, optionally applies later complete-property updates, emits declared events through its `PlatformEventSink`, and may release library-owned platform state from its dispose callback.
The Apple `example_platform_module` implementation registers a nonvisual Foundation timer factory and exposes it as the same typed Root Service used across the supported platforms. It demonstrates asynchronous Start and Stop calls, ordered Tick events, request cancellation, and platform disposal without exposing payloads or wire names to application code. On macOS and iOS it also registers a color-stream service that returns one `ExternalTexture` capability, publishes latest-wins `CVPixelBuffer` frames, and lets Image render them without per-frame PlatformModule calls.
The Apple `example_platform_view` target embeds a controlled `PlatformTextField`, routes platform edits through a typed HuxerUI event, updates properties from application State, and exercises PlatformView unmount and recreation.
Custom window chrome extends the AppKit content view through a transparent title bar while preserving AppKit traffic lights, accessibility, resizing, and window metadata. The adapter centers the AppKit traffic lights within the resolved title-bar height, converts their actual bounds into shared metrics, and delegates marked drag regions and window commands back to AppKit.
Core Graphics resolves retained shadow commands with blurred path shadows.
Canvas Paths map directly to Core Graphics fill, stroke, clip, and shadow operations.
Packaged resources are read from the application bundle, locale and backing-scale changes proactively update the Runtime resource configuration, and ImageIO-backed decoded images remain renderer-owned.
`macos::ExternalTextureSource` accepts platform-owned `CVPixelBuffer` frames from any producer thread. The AppKit renderer acquires only the newest pending frame, converts it through Core Image into its retained Core Graphics cache, and releases inactive cache entries during drawing. This path is bounded and does not claim zero-copy.
Debug process metrics use `getrusage`, Mach task information, and `NSProcessInfo`.

Example targets build as application bundles and can be launched from `build/bin`.

## Linux

The Linux backend creates an X11 window, measures text with FreeType and HarfBuzz, rasterizes shared PaintCommands with Cairo into a retained device-pixel bitmap, and presents it through an EGL/OpenGL ES 2 swap.
Top-level X11 focus updates the shared application lifecycle to `Active` or `Inactive`, while unmapping the window changes it to `Background`.
Partial Runtime damage limits Cairo redraw to the affected pixel bounds; the retained bitmap is then presented whole or as damaged rows, matching the Windows cost model of a retained scene bitmap plus swap-chain presentation.
Canvas Paths map to Cairo path geometry for fill, stroke, clipping, and blurred shadow masks.
Packaged resources are read from the executable-specific `<name>.resources` directory (overridable with `HUXERUI_RESOURCES_DIR`), locale and `Xft.dpi` changes update the Runtime resource configuration, and libpng/libjpeg decoding produces Cairo bitmap cache entries with a bounded byte budget.
FileSystem uses the executable filename as its application identity, maps durable and cache files through `XDG_DATA_HOME` and `XDG_CACHE_HOME` with standard home-directory fallbacks, and obtains the executable directory from `/proc/self/exe` rather than the working directory.
Temporary files use an owner-only application child under a valid `XDG_RUNTIME_DIR`, or an owner-only `huxerui-<uid>` subtree of the system temporary directory when the runtime directory is unavailable or unsafe.
FilePicker uses `org.freedesktop.portal.FileChooser` on the session D-Bus and passes the current X11 window as an `x11:<hex-xid>` parent when available.
Portal `file://` results remain private inside FileReference, while reads, imports, replacements, and completed save copies reuse the shared bounded file executor.
The picker capabilities are unavailable when the session bus or xdg-desktop-portal service cannot be reached; the backend does not fall back to GTK or Qt dialogs.

Text input uses the X Input Method protocol with full preedit callbacks, mirroring the Windows IMM32 adapter; when no input method is available the backend degrades gracefully to direct key text.
Clipboard reads and writes use the X11 `CLIPBOARD` selection with UTF-8 string transfers.
Nonvisual PlatformModule results and events enter a thread-safe FIFO and wake the X11 event loop through `eventfd`, so application callbacks run asynchronously on the Runtime thread without producer-thread reentry.
`linux::ExternalTextureSource` accepts borrowed, untagged sRGB RGBA8888 or BGRA8888 pixel spans with explicit dimensions and row stride from any producer thread.
Publish validates and copies each frame into a bounded latest-wins mailbox, converting straight alpha to Cairo's premultiplied ARGB32 representation so the producer may immediately reuse its buffer.
The Cairo renderer acquires one coherent frame per physical draw, retains the last acquired frame, applies Image cropping, sampling, transforms, clipping, and opacity, and releases inactive cache entries.
This path is bounded and does not claim zero-copy or direct DMA-BUF import.
The Linux `example_platform_module` registers a platform-neutral timer factory backed by `timerfd` and a background RGBA color stream, exposing the same typed Root Services and disposal behavior as the other supported platform implementations.
HttpClient uses one libsoup 3 Session on a dedicated GLib network thread, preserving connection reuse, redirects, system proxy configuration, and the system TLS trust store without adding network descriptors to the X11 event loop.
Requests and responses remain complete in-memory values, GCancellable backs Task cancellation, and a GLib timeout source enforces the complete request deadline.
X11, Xext, XKB common, XRandR, EGL, OpenGL ES 2, GIO, and libsoup 3 are manually installed system dependencies resolved through pkg-config; source-checkout builds do not download them.
Source-checkout builds fetch only the pinned graphics, text, image, and compression libraries used by the renderer.
Following the Windows and macOS distribution model, Linux host tools are distributed as prebuilt executables under `tools/prebuilt/linux/<architecture>/`.
The CLI records Linux enablement under `platform/linux`, builds the root CMake application in `.huxerui/build/linux/<profile>`, and launches the exact executable recorded by generated application integration metadata.
The relocatable Linux SDK supports installed CLI projects and exports both canonical CMake targets without retaining build-tree paths.
Shared consumers load `HuxerUI::huxerui` without requiring pkg-config development metadata; static consumers request `COMPONENTS static` and resolve the packaged archive closure plus the system development packages through pkg-config.
Linux release binaries target glibc 2.31 and GLIBCXX 3.4.28 or older symbol versions.

## Web

The Web backend compiles the same static `Application` declaration through Emscripten, mounts one `Runtime` and `WebPlatformAdapter` pair per browser-owned host element, and emits an ES module with WebAssembly output.
Document visibility and window focus map to the shared `Background`, `Inactive`, and `Active` lifecycle states without treating browser History as application activation.
Canvas 2D replays the shared `RenderScene`, while browser Pointer Events, wheel events, keyboard events, hidden DOM text controls, resource preloading, asynchronous `ImageBitmap` decoding, and browser-event-loop PlatformModule dispatch remain platform-owned services.
Web PlatformModule sources use the existing platform-neutral `PlatformModuleFactory` from C++ and Emscripten glue rather than a second JavaScript registry; the Web `example_platform_module` registers an interval-backed Timer through the same typed Root Service used by the other platforms.
`web::ExternalTextureSource` accepts open WebCodecs `VideoFrame` values on the browser main thread, synchronously clones each publication into a latest-wins mailbox, and leaves ownership of the original frame with the caller. Canvas acquires one coherent frame per browser animation frame even when PlatformViews divide rendering into several Canvas slices, retains the last acquired frame, and closes replaced or inactive frames. This path does not add a texture registry or claim zero-copy. The same PlatformModule example returns that texture capability once and then publishes generated frames without per-frame PlatformModule calls.
RootHook-installed `web::PlatformViewFactory` registrations return detached `HTMLElement` values through Emscripten. The adapter retains compatible elements, applies controlled property revisions, clips and positions them in logical coordinates, and alternates DOM containers with base and transparent Canvas slices in final `RenderComposition` order. Root-capture hit arbitration and focus synchronization preserve the PlatformView boundary between browser interaction and shared Runtime input. The Web `example_platform_view` hosts a controlled `PlatformTextField` through this path. Browser accessibility substitution remains deferred with the wider Web semantics bridge.
HttpClient uses browser Fetch, buffers complete responses, and aborts canceled or timed-out operations through AbortController. Browser CORS, forbidden-header, credential, and response-header visibility policies remain authoritative; HuxerUI does not add a proxy or a permissive request mode.
FileSystem restores one application-specific IDBFS mount before Runtime creation, publishes persistent data and cache directories with a volatile MEMFS temporary directory, and omits an executable directory. Generated shells supply a stable project identifier as `huxeruiStorageKey`. Persistent mutations are serialized through the browser event loop and complete only after explicit IndexedDB synchronization; synchronous mutation remains available only outside the persistent subtree.
FilePicker prefers browser file handles for opening and falls back to a transient file input, producing writable or read-only FileReference values according to the capability actually granted. Saving is available only through `showSaveFilePicker()` and completes after its writable stream closes; an anchor download is not reported as equivalent success. Picker presentation requires a direct transient user activation, granted handles remain session scoped, and imports into persistent application storage join the existing serialized IndexedDB synchronization domain.

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

For example, open `http://127.0.0.1:8000/example_ui_gallery.html`, `http://127.0.0.1:8000/example_http.html`, or `http://127.0.0.1:8000/example_files.html`.
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
On Windows hosts, `emrun` opens the local URL through `explorer.exe` and therefore follows the system default browser association.
Source checkouts build the Web framework with the application, while installed SDKs import the Emscripten 4.0.19 static framework artifact through the same root-project configuration.

The configured Emscripten compiler must provide the C++20 language and library support required by HuxerUI; obsolete toolchains are not supported through compatibility headers.
Platform-neutral semantics and browser accessibility mapping, broader browser integration tests, production packaging, and real mobile-browser IME validation remain follow-up work.
See [Web Platform Design](design/web.md) for the implemented boundary and deferred work.

## Android

The Android integration provides:

- `HuxerUIActivity` for full-screen applications
- `HuxerUIView` for embedding HuxerUI in an existing Android interface
- The `HuxerUI` Gradle library module
- The `example_runner` application module

Generated source-SDK applications use the Gradle library for Java integration while their app module configures the application root `CMakeLists.txt` directly for the shared C++ library and final resources.
The generated Gradle shell owns Android SDK and NDK versions, ABIs, identifiers, C++ and Java dependencies, manifests, signing, and packaging policy without a CMake configuration projection.

A full-screen launcher derives from `HuxerUIActivity`:

```java
public final class MainActivity extends HuxerUIActivity {}
```

The application JNI library is named `huxerui_app`. Loading it constructs the application's static `Application` before the activity creates its `HuxerUIView`; the adapter requires that exactly one application declaration is alive when it creates a Runtime session.

`HuxerUIActivity` owns a lifecycle-bound Back callback and forwards Back to the shared Runtime. Applications using this full-screen Activity set `android:enableOnBackInvokedCallback="true"` on their manifest `application` element, as the example runner does. Android 14 and later forward predictive Back start, progress, cancel, and commit phases; Android 13 forwards Commit; older versions use `onBackPressed()` for the same Commit path. When Runtime does not consume Commit, the Activity calls its overridable `onUnhandledBack()` fallback, which finishes the Activity with transition by default. An embedded integration owns registration itself, may call `HuxerUIView.handleBack()`, and continues its platform fallback only when that method returns `false`.

`HuxerUIActivity` also maps startup and `onNewIntent()` `ACTION_VIEW` or `ACTION_EDIT` data into the shared application activation model. Non-file schemes produce `UrlActivation`, while `content://` and `file://` values produce `FileActivation` capabilities backed by `ContentResolver`. The Activity launch mode and Intent filters remain application policy. Embedded integrations set the startup Intent before attaching `HuxerUIView` and explicitly forward later Intents; unsupported or share Intents are not reinterpreted.
The full-screen Activity maps resume, pause, foreground entry, and stop into the shared application lifecycle. Embedded owners call `HuxerUIView.setApplicationLifecycleState()` from their own lifecycle authority.

Coordinates remain density independent. The Android integration maps multi-touch, mouse hover, wheel, keyboard, viewport, and frame-clock events to the shared model. Frame callbacks commit Runtime work before full View invalidation, while `onDraw()` only presents the committed scene. The minimum supported Android API level is 23.
It implements HttpClient through HttpURLConnection on a bounded Java worker executor, buffers complete responses, enforces the shared request deadline, and disconnects native requests when their owning HuxerUI Task is canceled. Applications retain authority over declaring the `INTERNET` permission; the framework library does not inject it.
It installs FileSystem with app-owned files, cache, and temporary directories while exposing the native library directory as a read-only executable location.
It implements FilePicker with the Storage Access Framework and keeps `content://` grants inside FileReference rather than projecting them into local paths. `HuxerUIActivity` installs the picker launcher and forwards results automatically. An embedded host installs `HuxerUIView.FilePickerLauncher`, launches the supplied Intent with its request code, forwards the matching result to `dispatchFilePickerResult()`, and may finish that child Activity when cancellation is requested. The capability predicates remain false until a launcher is installed, so an embedded View never casts its arbitrary Context to Activity. Provider I/O runs on a bounded worker executor and the framework does not request broad storage permission or persist URI grants across launches. Application file activations reuse the same FileReference implementation and retain only the temporary grant delivered with their Activity Intent.

RootHook-installed Android PlatformView factories create controlled Views under the per-surface `PlatformModules` registry. `HuxerUIView` is a ViewGroup that retains compatible PlatformView instances, applies complete property revisions, clips and positions them in logical coordinates, and alternates HuxerUI Canvas slices with ordinary child drawing in final `RenderComposition` order. Library platform source includes `<huxerui/android/platform_view.h>` and registers an `android::PlatformViewFactory`; `<huxerui/android/jni.h>` supplies move-only local references and checked UTF-8, Java String, and byte-array conversion without adding a Java View or PlatformModule base class. The Android `example_platform_view` target uses this path to host a controlled EditText. SurfaceView subtrees are rejected because they cannot preserve this Canvas and child-drawing order. Shared hit testing routes touch, hover, and wheel input to the frontmost HuxerUI or PlatformView target. Android View focus changes, Runtime-driven focus, Tab traversal, IME dismissal, and accessibility traversal remain synchronized across that boundary. The accessibility provider replaces each PlatformView semantic anchor with the real Android View subtree at the same sibling position.

Java-backed nonvisual PlatformModules register `android::PlatformModuleFactory` from `<huxerui/android/platform_module.h>`. The Android adapter owns the current surface Context and injects it with the calling-thread `JNIEnv` when creating the PlatformModule instance; neither value enters `PlatformPayload`, `RootContext`, or application services. Platform result and event sinks may run on background threads and return through the existing `HuxerUIView` UI-thread dispatcher. The Android `example_platform_module` uses this path for a Java scheduled timer while sharing its typed C++ service across the supported platforms.
`android::ExternalTextureSource` accepts retained `android.graphics.Bitmap` frames from any Java-attached producer thread. Its bounded latest-frame mailbox schedules through the owning `HuxerUIView`, while the Canvas renderer acquires one coherent frame per physical draw, preserves the last acquired frame, and releases inactive cache entries. Publish retains the Bitmap independently, so the producer may release its own Java reference but must not recycle or mutate that frame afterward; recycled or generation-changed frames fail explicitly during drawing. The example PlatformModule publishes generated Bitmaps without per-frame PlatformModule calls. This API 23 path supports software and hardware-backed Bitmaps but does not claim zero-copy or direct `AHardwareBuffer` import.

`android::LocalRef<T>` adopts one JNI local object reference and releases it through the creating `JNIEnv`; it remains on that JNI thread and must not outlive the native call that owns the local-reference frame. String conversion preserves embedded nulls and supplementary Unicode without using JNI modified UTF-8, rejects null or malformed input, and reports values beyond JNI signed array limits. A JVM allocation failure returns an empty local reference with its Java exception still pending.

`HuxerUIView` also exposes the committed shared semantics as an Android virtual accessibility hierarchy. TalkBack queries stay in Java, accessibility focus and touch exploration remain provider-owned, and supported actions return to the shared Runtime on the UI thread.
Rounded-rectangle shadows use hardware shadow layers on API 28 and later, with density-aware cached alpha masks on older supported versions.
Arbitrary Paths use the same Android Canvas, and Path shadows use hardware layers on API 28 and later with a bounded software mask fallback on older supported versions.
Neither path disables hardware acceleration for the complete HuxerUIView.
Packaged resources are read from Android assets, system changes proactively update the Runtime resource configuration, and encoded images are transferred to Java only on a Bitmap cache miss.
Debug process metrics use `getrusage`, `Debug.getPss()`, and the online processor count.

## iOS

The iOS backend creates a UIKit window and safe-area-constrained HuxerUI View, measures text with CoreText, and replays the shared RenderScene through an independent CoreGraphics renderer. CADisplayLink schedules Runtime commits before UIKit invalidation, while `drawRect:` presents only the committed frame.
UIKit application callbacks map directly to the shared `Active`, `Inactive`, and `Background` lifecycle states.
It implements HttpClient through an iOS-owned ephemeral NSURLSession, returns complete in-memory responses, and cancels native data tasks when their owning HuxerUI Task is canceled.
It installs FileSystem with sandbox-owned Application Support, Caches, and temporary directories while exposing the application executable directory as a read-only location.
It implements FilePicker through UIDocumentPickerViewController, retains security-scoped file references, coordinates external file access away from the Runtime thread, and exports local files without moving their source.

Multi-touch, Apple Pencil, indirect pointer, hardware keyboard, clipboard, locale, display scale, keyboard viewport, and packaged-resource events cross one UIKit adapter boundary. A dedicated UITextInput implementation maps UIKit UTF-16 positions, selection, marked text, actions, and caret geometry to the shared text-input session and revision protocol. UIKit does not own a second editing value.

RootHook-installed iOS PlatformView factories create controlled UIViews under the per-surface `PlatformModules` registry. The adapter retains compatible PlatformView instances, applies complete property revisions, clips and positions them in logical coordinates, and alternates transparent HuxerUI slice views with platform containers in final `RenderComposition` order. Shared hit testing preserves that order, while UIKit focus and Runtime-driven focus changes synchronize the shared PlatformView focus leaf. Library platform source includes `<huxerui/ios/platform_view.h>` and registers an `ios::PlatformViewFactory`; the Apple `example_platform_view` target uses this path to host a controlled UITextField. The UIKit accessibility hierarchy substitutes the UIView subtree at its PlatformView semantic anchor. Hardware-keyboard traversal across the PlatformView boundary remains follow-up work.

The iOS adapter dispatches nonvisual `PlatformInstance` results and events asynchronously through the main queue. The Apple `example_platform_module` uses this path for a Foundation timer while sharing its typed C++ service and factory implementation across Apple platforms.
`ios::ExternalTextureSource` accepts platform-owned `CVPixelBuffer` frames from any producer thread. The UIKit renderer acquires only the newest pending frame from its iOS-owned mailbox, converts it through Core Image into its retained Core Graphics cache, and releases inactive cache entries during drawing. This path is bounded and does not claim zero-copy.

The minimum deployment target is iOS 13. CLI projects contain a source-controlled Xcode project that owns application packaging, signing, Apple assets, launch metadata, and debugging. Its build phase links an architecture-correct CMake application core containing the common C++ application and HuxerUI. The CLI discovers paired devices through `devicectl`, discovers booted Simulators through `simctl`, and keeps their DerivedData directories separate:

```bash
huxerui create app hello_huxer --platform ios
cd hello_huxer
huxerui doctor ios
huxerui devices ios
huxerui open ios
huxerui run ios --device <id>
```

Physical-device development builds use Xcode automatic signing and the `DEVELOPMENT_TEAM` value in `platform/ios/Config/Local.xcconfig` or the Xcode project settings. `huxerui open ios` records the detected HuxerUI SDK in that ignored local file before opening the project. Distribution archive automation, export signing, a public embeddable UIView, and UIKit selection rectangles remain outside the preview.

The repository-owned `platform/ios/example_runner/HuxerUIExamples.xcodeproj` provides one Xcode debugging host for every CMake `example_*` application target. Its ignored local configuration selects the example and optional signing team, while the shared project continues to build the selected application core through CMake.

## OHOS

OHOS should reuse the same Runtime and add one platform-specific `PlatformAdapter` integration. Platform availability and cross-build support must be reported explicitly by future SDK and CLI tooling.

See the [SDK, CLI, Platform Shell, and Library Design](design/sdk-cli.md) for the distribution, library, and PlatformModule model.
