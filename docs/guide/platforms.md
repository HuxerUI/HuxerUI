# Platform Support

HuxerUI application code and the shared Runtime support Windows, macOS, Linux, Web, Android, and iOS.
Each backend uses platform lifecycle, input, text, accessibility, file, network, and rendering services where available.

## Capability overview

| Platform | Rendering and text | HTTP and files | Accessibility | PlatformView | ExternalTexture | System tray |
|---|---|---:|---:|---:|---:|---:|
| Windows | Direct2D and DirectWrite | Yes | UI Automation | Yes | Yes | Yes |
| macOS | Core Graphics and Core Text | Yes | AppKit accessibility | Yes | Yes | Yes |
| Linux | Cairo and Pango | Yes | Not implemented | No | Yes | StatusNotifierItem host |
| Web | Canvas 2D and browser text metrics | Yes | Not implemented | Yes | Yes | No |
| Android | Android Canvas and StaticLayout | Yes | AccessibilityNodeInfo | Yes | Yes | No |
| iOS | Core Graphics and Core Text | Yes | UIKit accessibility | Yes | Yes | No |

Capabilities not listed as implemented are not implied by the shared API.
OHOS does not currently have a repository-owned backend.

## Windows

The default backend targets Windows 10 or later and uses Win32, D3D11, Direct2D, DirectWrite, DXGI, and IMM32.
Build with MSVC and a supported Visual Studio installation.

The optional `HUXERUI_WINDOWS_7_COMPAT=ON` configuration targets Windows 7 SP1 with Platform Update by using capability-based fallbacks.
PlatformView composition requires DirectComposition and is unavailable when that capability is missing.
Windows 7 without Platform Update is unsupported.

Custom chrome keeps Win32 window behavior while HuxerUI draws the title-bar content and caption controls.
Windows 11 Snap Layout is available through the shared maximize-button geometry on the default backend.
System tray presentation uses the Windows notification area and restores its item after Explorer restarts.

## macOS

The macOS backend uses AppKit, Core Graphics, Core Text, and `NSTextInputClient`.
Build with Xcode and the macOS SDK.

Custom chrome extends application content into the title bar while preserving AppKit traffic lights and window behavior.
External file references preserve security-scoped access when required.
System tray presentation uses an AppKit status item and platform menu.
The installed SDK exposes AppKit PlatformModule, PlatformView, PlatformPayload, and ExternalTexture protocols to Objective-C and Swift through `HuxerUIPlatform`.

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

Typed routed navigation can bind the authoritative `NavigationPath` to browser URL and history state.
Browser restrictions still govern clipboard, file pickers, autoplay, cross-origin requests, and storage persistence.

## Android

The Android backend requires API 23 or later and uses an Android View host, Canvas, StaticLayout, InputConnection, JNI, and platform accessibility APIs.
The generated Gradle project links the SDK-provided Android shared library and application C++ library for each configured ABI.

Build and run require an Android SDK, NDK, Java, Gradle wrapper dependencies, and a compatible emulator or device.
Insets, system-bar appearance, lifecycle, activation, file pickers, HTTP, PlatformView, and ExternalTexture are translated at the Android host boundary.

An Android arm64-v8a host SDK provides the `huxerui` CLI, `hcg`, and `hrc` as native Bionic executables for Termux.
The SDK installer does not install Java, Gradle dependencies, the Android SDK, or the Android NDK; use `huxerui doctor android` to inspect those application-build prerequisites.

## iOS

The iOS backend requires iOS 13 or later and uses UIKit, Core Graphics, Core Text, `UITextInput`, and UIKit accessibility.
Build on macOS with Xcode and an installed simulator runtime or paired device.

```bash
huxerui devices ios
huxerui open ios
huxerui run ios --device <id>
```

Physical-device builds use Xcode signing settings owned by the generated project and local developer configuration.
External files preserve security-scoped access when required.
The iOS XCFramework exposes UIKit PlatformModule, PlatformView, PlatformPayload, and ExternalTexture protocols to Objective-C and Swift through `HuxerUIPlatform`.

## Shared behavior

Composition, state, reconciliation, layout, navigation, scrolling, gestures, text editing behavior, animation, resources, semantics generation, and retained scene construction live in shared C++.
Platform differences belong at explicit platform capability boundaries rather than in application components.

For internal mapping details, use the relevant document in the [Design Index](../design/README.md).
