# Extending HuxerUI

Prefer the narrowest extension point that owns the required behavior.
HuxerUI already separates composition, layout, retained node behavior, application and window services, platform services, and drawing.

## Custom components

A custom component is an ordinary function returning `View`.
Mark it `[[huxerui::composable]]` only when it directly calls a composition-bound `UseXxx()` facility or needs its own local recomposition lifetime.

```cpp
[[huxerui::composable]]
View FavoriteButton() {
  auto selected = UseState(false);
  return IconButton(star_icon, "Favorite").OnClick([selected] {
    selected = !selected;
  });
}
```

Custom typed events derive from `Event<Result(Arguments...)>` and use the event key type as identity.
Use `void` for notifications and a value result only for synchronous decisions owned by the event key.
`UseEvents().Emit<Key>(...)` returns `void` for a void event and `std::optional<Result>` for a value-returning event.

## Custom layouts

Derive from `Layout<Derived>` when behavior is a measurement and placement policy over ordinary children.
The `ViewNode` interface provides geometry, interaction state, and layout metadata for a UiWindow-owned mounted node; application code does not create or own these nodes.

```cpp
class SimpleRow final : public Layout<SimpleRow> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    LayoutResult result;
    float x = 0.0F;
    float height = 0.0F;

    for (ViewNode& child : node.Children()) {
      const Size size = context.Measure(child, constraints.Loose());
      result.Place(child, {x, 0.0F});
      x += size.width;
      height = std::max(height, size.height);
    }

    result.SetSize(constraints.Constrain({x, height}));
    return result;
  }
};
```

Measure children only through the context and do not retain child references across reconciliation.

`Children()` visits the current direct children; both const and mutable `ChildAt(index)` access throw `std::out_of_range` when `index >= ChildCount()`.
Use typed `LayoutValue<Key>` metadata for semantics owned by the parent layout.

## Custom virtual layouts

Derive from `VirtualLayout<Derived>` for a demand-driven logical item source.
The layout chooses items to realize and returns their geometry.
In both ordinary and virtual layouts, `Measure()` updates the node's `LayoutSize()` even when the layout ignores the returned size.

UiWindow owns declaration reconciliation, keys, retained item state, clipping, hit testing, scrolling, semantics, and cleanup.
The layout must not maintain a second mounted item tree.

A derived layout can call `EnableBothAxes()` during construction to scroll within a bounded width and height. Return the viewport size with `SetSize()`, the full logical extent with `SetContentSize()`, and item placements in content coordinates. `Viewport()` supplies the candidate offset and size for the current measure pass.

Use `SetScrollOffset(Axis, float)` to correct an axis after an extent change. The same measure session can retry with the corrected viewport before committing placements. Use committed geometry, such as the bound controllers' published metrics, as the anchor reference so the correction survives repeated measurements with different constraints.

To support `ScrollToItem()` on two-axis layouts, define `static std::optional<Rect> ItemBounds(ViewNode&, std::size_t)`. Return the item's bounds in content coordinates without realizing it, using an estimate when exact geometry is unavailable. The controller applies `ScrollAlignment` along its bound axis. Single-axis layouts continue to report their axis and scalar correction through `SetAxis()` and `SetScrollOffset(float)` and can implement `ScrollOffsetForItem()`.

## Modifiers and NodeExtension

Use a property modifier for reusable declaration data that applies directly to `ViewSpec`.
Use a retained modifier with `NodeExtension` when one mounted node needs lifecycle, gesture, animation, semantics, or foreground paint state.

`NodeExtension` is not a general plugin registry.
Update compatible declarative configuration without discarding retained state, request timing through `FrameResult`, and invalidate paint when paint-visible retained state changes.

Use the protected `EmitEvent<Key>(...)` operation to report semantic output to `.On<Key>()` on the View carrying the extension.
It uses the node's latest handlers after recomposition and does not bubble to ancestors or emit through an enclosing component's scope.
Notifications return `void`; value-returning events return `std::optional<Result>`, with an empty optional when no handler is bound.
Handler exceptions propagate to the caller.

```cpp
struct Submitted : Event<void()> {};

struct SubmitOnEnter {
  class Extension;
  bool operator==(const SubmitOnEnter&) const = default;
};

class SubmitOnEnter::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const SubmitOnEnter&) {}
  void Update(ViewNode&, const SubmitOnEnter&) {}

  bool OnKey(ViewNode&, const KeyEvent& event) override {
    if (event.type != KeyEventType::Down || event.key != Key::Enter) {
      return false;
    }
    EmitEvent<Submitted>();
    return true;
  }
};

View App() {
  auto submitted = UseState(false);
  return Text(submitted ? "Submitted" : "Press Enter")
      .With(Focusable{}, SubmitOnEnter{})
      .On<Submitted>([submitted] { submitted = true; });
}
```

This modifier needs neither `UseEvents()` nor its own composition scope.
Runtime connects the event binding after construction; call `EmitEvent()` only on the owning UI thread from mounted behavior such as input, semantic actions, or frame updates.
Do not emit while constructing, updating, destroying, hit testing, preparing geometry, or recording paint.
Emission is synchronous and does not extend the extension's lifetime; do not retain the extension in a deferred callback.
When an internal child must emit an outer component's event, continue to pass the `EventEmitter` returned by that component's `UseEvents()` explicitly.
Presentation handles such as `PopupHandle` and `DialogHandle` also remain explicit dependencies rather than additional NodeExtension methods.

`PrepareGeometry(ViewNode&, TextMeasurer&)` runs after final presentation geometry is resolved and before paint recording.
Use its borrowed measurer for synchronous text-dependent geometry and retain the resulting metrics or immutable `TextLayout` on the UI thread.
Return the exact `PaintInvalidation` phases whose recorded inputs changed; text that affects measurement belongs in `Layout` because this callback does not provide another layout pass.
This is also the phase for `ViewNode` local-to-window conversion when retained behavior must publish geometry to a window-owned service such as Popup placement.
Paint callbacks continue to record node-local commands and do not use window coordinates to draw.
Use `ViewNode::Bounds()` for the complete node rectangle and `ContentBounds()` when custom content should respect the node's resolved Padding.

For self-drawn controls, `BuildSemantics(SemanticBuilder&)` can publish virtual children with `AddChild(local_id, bounds, semantics, enabled)` and declare their actions through `AddAction` or `AddCustomAction`.
Keep local IDs stable and use the same availability predicate as actual input.
Runtime combines each child's availability with the mounted owner's `Enabled` state: disabled children remain discoverable but have no executable standard or custom actions.
Call `InvalidateSemantics()` when retained availability changes; this does not replace the control's pointer or keyboard checks.

Custom editors expose their `TextInputClient` through `GetTextInputClient()` on a focusable node. Return `SoftwareKeyboardPolicy::Manual` in the client's `TextInputConfiguration::keyboard_policy` to control when the software keyboard opens. From mounted input or focus callbacks on the UI thread, use the protected `ShowKeyboard()` and `HideKeyboard()` methods to address that node's editor. A request in the same event that focuses the editor is resolved after the editing session is ready.

## Paint commands

Use `Canvas` and existing `PaintCommand` variants for custom drawing.
A new platform-neutral command is a framework change: it must update the command variant, type-safe paint API, tests, public rendering documentation, and every renderer.

## Root services

Use a window service for a per-window capability shared by unrelated components, such as presentation or a window-bound platform module. Clipboard, files, and HTTP belong to application services and handles.
Install shared services and platform factories through `AppOptions::application_hooks`; use `window_hooks` for window services and layers.

## PlatformModule

`PlatformModule` exposes a typed nonvisual platform service to shared application code.
The application-facing service owns typed requests, results, and events; the platform implementation owns operating-system objects and UI-thread delivery.
`PlatformPayload` uses the top-level `Bytes` type for owned binary payloads, while `AsBytes()` returns a borrowed `std::span<const std::byte>`.
Keep strings for valid UTF-8 text and bytes for encoding-independent data; `PlatformPayload` does not convert between them.
When a cross-language module must give a native media, document, or provider API an external file, carry the existing `FileReference` as a payload capability instead of importing it, serializing its bytes, or reconstructing a path.
Android receives a retained `HuxerUIFileReference` with `uri()`, Apple receives a retained `FileReference` with `fileURL`, and Web receives a retained `HuxerUI.FileReference` with asynchronous `getFile()`.
The platform wrappers preserve the C++ access lifetime but do not expose a second `CanWrite()` contract; shared code continues to own capability metadata and file operations.

iOS and macOS libraries may implement the platform side in Objective-C or Swift through the `HuxerUIPlatform` Clang module.
Their Objective-C++ `ApplicationHook` registers the actual factory object and wraps the resulting `PlatformChannel` in the library's typed service.
iOS uses `ios::ObjectiveCPlatformModuleFactory` with `HUXPlatformModuleFactory` for application services, and `ios::ObjectiveCWindowPlatformModuleFactory` with `HUXUIKitWindowPlatformModuleFactory` for window services.
The application factory receives options and events without requiring UI; the window factory also receives its original `UIViewController`.
Each opening creates an instance; application binding does not make it a singleton.
macOS continues to use `macos::ObjectiveCPlatformModuleFactory` with its owning `NSWindow`.
The adapter's `create` callback performs that strongly typed wrapping; PlatformView reserves `connect` and `disconnect` for Controller attachment.
Direct iOS Objective-C++ window factories use `ios::WindowPlatformModuleFactory`; application factories use an ordinary callable accepting `Runtime&`.
The same platform headers retain the direct Objective-C++ factory path; factory class-name lookup and application-host registration are not used.

### iOS native presentation

A window module obtains `HUXGetPresentationCoordinator(viewController)` (`presentationCoordinator(for:)` in Swift) once from the exact controller supplied by its factory and retains it for delayed work.
`enqueueWithStart:failure:` (`enqueue(start:failure:)`) returns an `HUXPlatformCancellation` before starting native work asynchronously on the main thread.
FilePicker and extension libraries share this FIFO queue per original window; another window has an independent queue.
Inactive windows pause admission, a closed/unavailable window fails, and an unmanaged modal controller fails as busy instead of being covered.
FilePicker save preparation runs before entering this native queue; requests still waiting in FilePicker's own C++ queue have not entered it.

The starter receives the presenter and a `finished` closure, installs native UI synchronously, and returns its native cancellation object.
It may return nil only after calling `finished` synchronously because no presentation remains.
The library owns the picker, delegate, dismissal, and data processing.
Call `finished` after programmatic dismiss completion or `presentationControllerDidDismiss` for interactive dismissal; UIKit does not call the latter for programmatic dismissal.
Failed startup without installed UI also calls `finished`.
This releases presentation ownership, independently of subsequent provider loading or file copying.

Canceling a queued request prevents its starter from running; canceling an active one invokes native cancellation once and holds the slot until `finished`.
The library's cancellation must wait through presentation/dismissal transitions and call `finished` only when UI is actually absent.
Duplicate and late completions cannot release another request.
Window teardown permanently closes the coordinator, fails uncanceled requests, and requests active cleanup; it never rebinds to a replacement window.
All coordinator entry points and callbacks, including cancellation and `finished`, require the main thread.

Use a UI Task scope for a window picker and hand acquired FileReferences to application work afterward.
Coordinator cancellation covers presentation only; a media operation also owns cancellation of its provider loading/copying work.
The existing PlatformChannel close contract still discards pending callbacks; an application Task that outlives a window-bound channel needs its own facade closure/failure path when using `AwaitCallback`.

Use PlatformView only when a real platform visual control must participate in HuxerUI layout and ordering.
Use ExternalTexture when a producer supplies image frames rather than an interactive platform view.

## Platform backends

Adding or changing a backend is framework work, not an application extension.
The platform-derived Runtime owns application services, event dispatch, and ordinary timers. Its platform-derived UiWindow owns host events, text services, accessibility mapping, frame scheduling, and rendering of the shared committed scene.
Shared UiWindow code owns composition, layout, and component state machines; application code owns navigation policy.

See [Architecture Design](../design/architecture.md) for subsystem ownership.
