# Platform Views

`PlatformView` embeds a platform-owned interactive leaf control in HuxerUI layout.
It is a View, not a modifier, layout, Canvas command, or non-visual Module.

## Typed declaration

Wrap the generic declaration in a concrete library component:

```cpp
struct WebViewProperties {
  std::string url;

  bool operator==(const WebViewProperties&) const = default;
};

struct WebViewEvents {
  struct NavigationChanged : Event<const NavigationState&> {
    static constexpr std::string_view Name = "navigationChanged";
  };
};

View WebView(WebViewProperties properties) {
  return PlatformView("web/WebView", std::move(properties));
}
```

Properties are one complete immutable strongly typed snapshot.
The no-properties form is `PlatformView(name)`.
Register the exact Properties and optional Controller types with `RootContext::RegisterPlatformView()` from one RootHook.
Registration names are nonempty case-sensitive UTF-8 identities and do not require `/`.

Events need no separate declaration list.
The component attaches ordinary typed handlers with `.On<Key>(...)`, and the platform factory receives one `PlatformEventEmitter` that calls `Emit<Key>(value)`.
When an implementation crosses a platform-language boundary, the event value type owns `Decode(const PlatformPayload&)`; direct C++ emission remains strongly typed.

An optional Controller is a library-defined typed command facade.
Attach it with `.Controller(controller)` and register that exact Controller type.
The factory connects the retained platform instance on mount or Controller replacement and disconnects before disposal.
HuxerUI does not require Controller inheritance, State, pimpl, Access, Backend, or Connection types.

## Factory and platform boundary

Use the active platform's public `platform_registry.h` factory contract:

- Windows returns a same-process, same-thread child `HWND` of the supplied parent.
- Android returns a Java `View` through either a direct JNI factory or `android::JavaPlatformViewFactory`.
- iOS returns a detached stable `UIView*` from Objective-C++ or an actual Objective-C/Swift factory object.
- macOS returns a detached stable `NSView*` from Objective-C++ or an actual Objective-C/Swift factory object.
- Web returns a detached DOM element through a direct Emscripten C++ factory or `web::JavaScriptPlatformViewFactory`.

`PlatformValue` is the public low-level in-process carrier used by RenderScene and platform factory adaptation to retain exact C++ Properties, Controller, and event value types.
It never crosses a platform-language boundary, and ordinary components and direct factories use their concrete types rather than constructing it themselves.

Registry installation supplies the owning `PlatformAdapter&` only to the factory's internal binding operation.
Direct create, update, Controller, and disposal callbacks receive their exact platform handles and typed values rather than the adapter.

Android currently provides the common Java/Kotlin class adapter.
Web currently provides the common JavaScript structural adapter: the RootHook supplies the actual factory object, Properties use `Module.HuxerUI.PlatformPayload`, and events use one framework-owned emitter.
iOS and macOS expose `UIKitPlatformViewFactory` or `AppKitPlatformViewFactory`, their View protocols, payload endpoints, and cancellation endpoints through the `HuxerUIPlatform` Clang module.
The Objective-C++ RootHook supplies the actual Objective-C or Swift factory object to `ios::ObjectiveCPlatformViewFactory<Properties, Controller>` or `macos::ObjectiveCPlatformViewFactory<Properties, Controller>`.
Its `connect` callback attaches the returned `PlatformChannel` to the exact library Controller, and `disconnect` detaches that Controller before View disposal.
Factories receive the owning `UIViewController` or `NSWindow`; they return the stable detached View and never attach it themselves.
Direct Objective-C++ factories remain available through `ios::PlatformViewFactory` and `macos::PlatformViewFactory` without a payload round trip.
Every path still registers once through the library RootHook; application hosts, delegates, and Web mount calls do not form a second registry.

Factories own create, update, optional Controller connect/disconnect, and dispose symmetry.
Failed creation publishes no event and releases any instance or platform object already returned by the factory.
Events are accepted only after a candidate commits, and disposal invalidates delivery before releasing platform state.

## Geometry and behavior

Provide bounded geometry because a PlatformView has no portable intrinsic size.
The shared contract covers layout placement, rectangular visibility and clipping, composition order, focus/input integration, update, typed events, and lifecycle according to the backend.

Do not promise arbitrary rotation, path clipping, group opacity, backdrop filters, or transparent mixing unless the active platform contract explicitly supports them.
Choose `ExternalTexture` when the platform only produces frames and HuxerUI should own effects, interaction, and surrounding semantics.

## Review points

- concrete typed component instead of raw names in page code;
- complete Properties snapshot and controlled owner updates;
- inferred typed events with no parallel event registry;
- optional Controller with deterministic connect/disconnect lifetime;
- bounded geometry and documented backend limits;
- create/update/dispose failure symmetry, focus, IME, accessibility, and unmount behavior;
- payload conversion only where a platform-language boundary exists.
