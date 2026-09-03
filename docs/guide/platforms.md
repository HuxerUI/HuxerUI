# Platform Support

HuxerUI application code and the shared Runtime support Windows, macOS, Linux, Web, Android, and iOS.
Each backend uses platform lifecycle, input, text, accessibility, file, network, and rendering services where available.

## Capability overview

| Platform | Rendering and text | HTTP and files | Accessibility | PlatformView | ExternalTexture | Permissions | System tray |
|---|---|---:|---:|---:|---:|---:|---:|
| Windows | Direct2D and DirectWrite | Yes | UI Automation | Yes | Yes | AppCapability | Yes |
| macOS | Core Graphics and Core Text | Yes | AppKit accessibility | Yes | Yes | Camera and microphone | Yes |
| Linux | Cairo and Pango | Yes | Not implemented | No | Yes | Unavailable | StatusNotifierItem host |
| Web | Canvas 2D and browser text metrics | Yes | Not implemented | Yes | Yes | Query only | No |
| Android | Android Canvas and StaticLayout | Yes | AccessibilityNodeInfo | Yes | Yes | Camera and microphone | No |
| iOS | Core Graphics and Core Text | Yes | UIKit accessibility | Yes | Yes | Camera and microphone | No |

Capabilities not listed as implemented are not implied by the shared API.
OHOS does not currently have a repository-owned backend.

## Windows

The default backend targets Windows 10 version 1607 or later and uses Win32, D3D11, Direct2D, DirectWrite, DXGI, and IMM32.
Build with MSVC and a supported Visual Studio installation.

The optional `HUXERUI_WINDOWS_7_COMPAT=ON` configuration targets Windows 7 SP1 with Platform Update by using capability-based fallbacks.
PlatformView composition requires DirectComposition and is unavailable when that capability is missing.
Windows 7 without Platform Update is unsupported.

Custom chrome keeps Win32 window behavior while HuxerUI draws the title-bar content and caption controls.
Windows 11 Snap Layout is available through the shared maximize-button geometry on the default backend.
System tray presentation uses the Windows notification area and restores its item after Explorer restarts.
Runtime camera and microphone permissions use AppCapability when present; package capability declarations remain application-owned.

Native libraries include `<huxerui/windows/external_texture.h>` for `windows::PixelTexture` and `windows::D3D11Texture`.
`PixelTexture` copies straight-alpha RGBA8888 or BGRA8888 rows into premultiplied CPU storage.
`D3D11Texture` accepts one-mip, one-slice, single-sampled `DXGI_FORMAT_B8G8R8A8_UNORM` `D3D11_USAGE_DEFAULT` textures without CPU access.
It synchronously performs one GPU copy into an immutable shared snapshot, so the producer may reuse or release its source after `Publish()` returns.

```cpp
auto texture = std::make_shared<huxerui::windows::D3D11Texture>(huxerui::Size{320.0F, 180.0F});
texture->Publish({producer_texture, huxerui::windows::D3D11Texture::Alpha::Opaque});
```

Submit producer writes before publication and externally serialize use of the source device's immediate context.
Pixel row zero is interpreted as the logical top edge.
The producer and every renderer displaying the texture must use the same graphics adapter.
Renderers open the immutable shared snapshot directly, coordinate read access with its keyed mutex, and do not make a second GPU copy or silently read pixels back to CPU memory.
Renderer-local resource recreation can reopen the snapshot while its producer device remains valid.
If the producer device is removed, publish a replacement texture from a valid device before the next render.
`D3D11Texture` is unavailable when `HUXERUI_WINDOWS_7_COMPAT=ON`.

## macOS

The macOS backend requires macOS 12 or later.
The macOS backend uses AppKit, Core Graphics, Core Text, and `NSTextInputClient`.
Build with Xcode and the macOS SDK.

Custom chrome extends application content into the title bar while preserving AppKit traffic lights and window behavior.
External file references preserve security-scoped access when required.
System tray presentation uses an AppKit status item and platform menu.
Camera and microphone permissions use AVFoundation and require the corresponding bundle usage descriptions.
The installed SDK exposes AppKit PlatformModule, PlatformView, PlatformPayload, and ExternalTexture protocols to Objective-C and Swift through `HuxerUIPlatform`.

Native libraries include `<huxerui/macos/external_texture.h>` for `macos::PixelBufferTexture` and `macos::MetalTexture`.
`PixelBufferTexture` retains an immutable `CVPixelBufferRef`; publish a different buffer before mutating later content.
`MetalTexture` synchronously copies level zero of a completed, non-framebuffer-only 2D BGRA8 or RGBA8 texture, so the producer may reuse or release its source after `Publish()` returns.

```objective-c++
auto texture = std::make_shared<huxerui::macos::MetalTexture>(huxerui::Size{320.0F, 180.0F});
texture->Publish({producer_texture, huxerui::macos::MetalTexture::Origin::TopLeft,
                  huxerui::macos::MetalTexture::Alpha::Premultiplied});
```

Finish producer command buffers before publication.
Use `Origin::BottomLeft` only when row zero represents the logical bottom edge, `Alpha::Opaque` when stored alpha must be ignored, and `Alpha::Straight` when RGB has not been multiplied by alpha.
Metal publication performs a GPU snapshot and is not a zero-copy handoff; rendering remains in the ordinary Image/Core Graphics path so transforms, clipping, opacity, overlays, scrolling, and multiple windows keep their normal semantics.

## Linux

The Linux backend uses GTK 4 for the window and event loop, Pango for text, Cairo for rendering, `GtkIMContext` for composition, GIO for platform services, and libsoup 3 for HTTP.

Install the corresponding development packages before configuring CMake.
The SDK archive does not bundle distribution-owned GTK, Pango, Cairo, GIO, or libsoup libraries.
Official Linux SDK binaries require glibc 2.35 or later.

Linux builds are provided for x86_64 and aarch64 hosts.
PlatformView and a platform accessibility bridge are not implemented.
System tray presentation requires an active StatusNotifierItem watcher and host; `IsAvailable()` tracks hosts appearing or disappearing at runtime.

## Web

The Web backend uses Emscripten, WebAssembly, Canvas 2D, browser input events, hidden text controls for IME, Fetch for HTTP, and browser-managed file storage.

Generated projects use the Emscripten version pinned by the installed HuxerUI SDK.
Run the generated output through `huxerui run web` or another HTTP server; loading the files directly with a `file:` URL is unsupported.
On Termux, `huxerui run web` starts a Python standard-library server on an available loopback port and passes the generated entry URL to `termux-open`; it does not use ADB or Emscripten's Android-device mode.
The pinned Emscripten tools, Python, and `termux-open` must be available on `PATH`.

Typed routed navigation can bind the authoritative `NavigationPath` to browser URL and history state.
Browser restrictions still govern clipboard, file pickers, autoplay, cross-origin requests, and storage persistence.
Camera and microphone permission state is queried through the Permissions API when supported; requesting access remains coupled to browser media acquisition and is not emulated by the shared permission API.

## Android

The Android backend requires API 23 or later and uses an Android View host, Canvas, StaticLayout, InputConnection, JNI, and platform accessibility APIs.
The generated Gradle project links the SDK-provided Android shared library and application C++ library for each configured ABI.

Build and run require an Android SDK, NDK, Java, Gradle wrapper dependencies, and a compatible emulator or device.
Insets, system-bar appearance, lifecycle, activation, file pickers, HTTP, PlatformView, and ExternalTexture are translated at the Android host boundary.
Camera and microphone requests use the Activity launcher and require manifest declarations owned by the application.

Android libraries include `<huxerui/android/external_texture.h>` and choose the producer that matches their source.
`BitmapTexture` retains immutable `android.graphics.Bitmap` objects and remains on the Canvas path.
`GlTexture` synchronously copies `GL_TEXTURE_2D` content from the EGL context current during `PublishCurrent()`; supply a native acquire-fence fd when producer work is asynchronous, or publication waits for current GL work.
The producer may reuse or delete the source texture after `PublishCurrent()` returns.

```cpp
auto texture = std::make_shared<android::GlTexture>(Size{320.0F, 180.0F});
texture->PublishCurrent({
    .texture_name = texture_name,
    .pixel_width = 1280,
    .pixel_height = 720,
    .acquire_fence_fd = fence_fd,
    .origin = android::GlTexture::Origin::BottomLeft,
    .alpha = android::GlTexture::Alpha::Opaque,
});
```

`SurfaceStreamTexture` owns the SurfaceTexture/OES consumer and returns a local reference to a producer-facing `android.view.Surface` suitable for Camera or MediaCodec.
Its logical intrinsic size remains fixed while `SetDefaultBufferSize()` may change the requested physical buffer size.
Surface buffers follow Android's premultiplied-alpha convention, and `Finish()` releases the producer Surface and consumer while preserving the last latched frame.

```cpp
auto texture = android::SurfaceStreamTexture::Create(environment, Size{320.0F, 180.0F}, 1280, 720);
auto producer_surface = texture->Surface(environment);
camera->SetPreviewSurface(producer_surface.Get());
```

Applications pass all three concrete types to `Image` as `std::shared_ptr<ExternalTexture>`.
GPU-backed textures require a hardware-accelerated host window; HuxerUI does not silently read them back to CPU memory.

An Android arm64-v8a host SDK provides the `huxerui` CLI, `hcg`, and `hrc` as native Bionic executables for Termux.
Termux Android builds target the local `arm64-v8a` ABI, use the Termux `aapt2` executable, and still require an Android SDK platform and NDK layout compatible with Gradle `externalNativeBuild` on Termux.
The SDK installer does not install Java, Gradle dependencies, `aapt2`, the Android SDK, or the Android NDK; use `huxerui doctor android` to inspect those application-build prerequisites.
Termux diagnosis and setup do not require `sdkmanager`, platform-tools, or ADB because they are not part of the local-device path.
`huxerui run android` opens the generated APK in the Android system installer through `termux-open`; complete the confirmation and choose Open in the installer to start the application.

## iOS

The iOS backend requires iOS 15 or later and uses UIKit, Core Graphics, Core Text, `UITextInput`, and UIKit accessibility.
Build on macOS with Xcode and an installed simulator runtime or paired device.

```bash
huxerui devices ios
huxerui open ios
huxerui run ios --device <id>
```

Physical-device builds use Xcode signing settings owned by the generated project and local developer configuration.
External files preserve security-scoped access when required.
Camera and microphone permissions use AVFoundation, require native usage descriptions, and can open the application settings page through UIKit.
The iOS XCFramework exposes UIKit PlatformModule, PlatformView, PlatformPayload, and ExternalTexture protocols to Objective-C and Swift through `HuxerUIPlatform`.

Native libraries include `<huxerui/ios/external_texture.h>` for the iOS forms of `PixelBufferTexture` and `MetalTexture`.
They have the same ownership, format, origin, alpha, synchronization, and Image rendering contract as their macOS counterparts; the C++ types remain in `huxerui::ios` rather than a shared Apple namespace.
Objective-C and Swift import the concrete objects as `HUXPixelBufferTexture`/`PixelBufferTexture` and `HUXMetalTexture`/`MetalTexture`.

## Shared behavior

Composition, state, reconciliation, layout, navigation, scrolling, gestures, text editing behavior, animation, resources, semantics generation, and retained scene construction live in shared C++.
Platform differences belong at explicit platform capability boundaries rather than in application components.

For internal mapping details, use the relevant document in the [Design Index](../design/README.md).
