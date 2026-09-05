# Extending HuxerUI

Prefer the narrowest extension point that owns the required behavior.
HuxerUI already separates composition, layout, retained node behavior, root services, platform services, and drawing.

## Custom components

A custom component is an ordinary function returning `View`.
Mark it `[[huxerui::composable]]` only when it directly calls a composition-bound `UseXxx()` facility or needs its own local recomposition lifetime.

```cpp
[[huxerui::composable]]
View FavoriteButton() {
  auto selected = UseState(false);
  return IconButton(star_icon, "Favorite").OnClick([selected] {
    selected = !selected.Get();
  });
}
```

Custom typed events derive from `Event<Result(Arguments...)>` and use the event key type as identity.
Use `void` for notifications and a value result only for synchronous decisions owned by the event key.
`UseEvents().Emit<Key>(...)` returns `void` for a void event and `std::optional<Result>` for a value-returning event.

## Custom layouts

Derive from `Layout<Derived>` when behavior is a measurement and placement policy over ordinary children.
The `ViewNode` interface provides geometry, interaction state, and layout metadata for a Runtime-owned mounted node; application code does not create or own these nodes.

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

Runtime owns declaration reconciliation, keys, retained item state, clipping, hit testing, scrolling, semantics, and cleanup.
The layout must not maintain a second mounted item tree.

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
  return Text(submitted.Get() ? "Submitted" : "Press Enter")
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
Use its borrowed measurer only for synchronous text-dependent retained geometry, and retain only the resulting value metrics.
Return the exact `PaintInvalidation` phases whose recorded inputs changed; text that affects measurement belongs in `Layout` because this callback does not provide another layout pass.
This is also the phase for `ViewNode` local-to-window conversion when retained behavior must publish geometry to a window-owned service such as Popup placement.
Paint callbacks continue to record node-local commands and do not use window coordinates to draw.
Use `ViewNode::Bounds()` for the complete node rectangle and `ContentBounds()` when custom content should respect the node's resolved Padding.

For self-drawn controls, `BuildSemantics(SemanticBuilder&)` can publish virtual children with `AddChild(local_id, bounds, semantics, enabled)` and declare their actions through `AddAction` or `AddCustomAction`.
Keep local IDs stable and use the same availability predicate as actual input.
Runtime combines each child's availability with the mounted owner's `Enabled` state: disabled children remain discoverable but have no executable standard or custom actions.
Call `InvalidateSemantics()` when retained availability changes; this does not replace the control's pointer or keyboard checks.

## Paint commands

Use `Canvas` and existing `PaintCommand` variants for custom drawing.
A new platform-neutral command is a framework change: it must update the command variant, type-safe paint API, tests, public rendering documentation, and every renderer.

## Root services

Use a root service for a per-window capability shared by unrelated components, such as presentation, clipboard, files, HTTP, or a typed platform module.
Install custom root behavior through the existing RootHook boundary rather than adding another global registry.

## PlatformModule

`PlatformModule` exposes a typed nonvisual platform service to shared application code.
The application-facing service owns typed requests, results, and events; the platform implementation owns operating-system objects and UI-thread delivery.
`PlatformPayload` uses the top-level `Bytes` type for owned binary payloads, while `AsBytes()` returns a borrowed `std::span<const std::byte>`.
Keep strings for valid UTF-8 text and bytes for encoding-independent data; `PlatformPayload` does not convert between them.

iOS and macOS libraries may implement the platform side in Objective-C or Swift through the `HuxerUIPlatform` Clang module.
Their Objective-C++ RootHook passes the actual factory object to `ios::ObjectiveCPlatformModuleFactory` or `macos::ObjectiveCPlatformModuleFactory`, then wraps the resulting `PlatformChannel` in the library's typed service.
The adapter's `create` callback performs that strongly typed wrapping; PlatformView reserves `connect` and `disconnect` for Controller attachment.
The same platform headers retain the direct Objective-C++ factory path; factory class-name lookup and application-host registration are not used.

Use PlatformView only when a real platform visual control must participate in HuxerUI layout and ordering.
Use ExternalTexture when a producer supplies image frames rather than an interactive platform view.

## Platform adapters

Adding or changing a backend is framework work, not an application extension.
The adapter owns lifecycle, host events, text services, accessibility mapping, resource loading, frame scheduling, and replay of the shared committed scene.
It does not duplicate composition, layout, component state machines, or navigation policy.

See [Architecture Design](../design/architecture.md) for subsystem ownership.
