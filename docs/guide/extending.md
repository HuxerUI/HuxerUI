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

```cpp
class SimpleRow final : public Layout<SimpleRow> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    LayoutResult result;
    float x = 0.0F;
    float height = 0.0F;

    for (MountedNode& child : node.Children()) {
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

`PrepareGeometry(MountedNode&, TextMeasurer&)` runs after final presentation geometry is resolved and before paint recording.
Use its borrowed measurer only for synchronous text-dependent retained geometry, and retain only the resulting value metrics.
Return the exact `PaintInvalidation` phases whose recorded inputs changed; text that affects measurement belongs in `Layout` because this callback does not provide another layout pass.

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
