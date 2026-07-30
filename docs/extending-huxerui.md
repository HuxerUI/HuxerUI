# Extending HuxerUI

HuxerUI exposes typed extension points for component events, layout, virtualization, retained modifiers, root services, and platform hosts. Prefer the narrowest extension point that owns the behavior.

## Custom components and events

Custom components remain ordinary functions returning `View`. Use a scope only when the component owns local state:

```cpp
struct Submitted : Event<std::string> {};

[[huxerui::scope]]
View SearchBox() {
  auto events = UseEvents();

  return Button("Submit").OnClick([events] {
    events.Emit<Submitted>("query");
  });
}
```

## Custom layouts

A custom layout derives from `Layout<Derived>` and implements `Measure()`:

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
      x += size.width + node.Spacing();
      height = std::max(height, size.height);
    }

    result.SetSize(constraints.Constrain({x, height}));
    return result;
  }
};
```

`LayoutContext::Measure()` measures a child. `LayoutResult` records the container size and child placements. `MountedNode::Cache<T>()` provides per-mounted-node layout cache storage, while typed `LayoutValue<Key>()` values carry container-specific child metadata.

## Custom virtual layouts

`VirtualLayout<Derived>` exposes a demand-driven logical item source. `VirtualLayoutContext::Item(index)` materializes or reuses one item, and the result retains only items included in the final placements.

The runtime owns item reconciliation, duplicate-key checks, saved state, clipping, input routing, scrolling, and cleanup. A custom virtual layout owns its visible-range calculation and placement algorithm. It can opt in to `ScrollController::ScrollToItem()` by implementing the same static `ScrollOffsetForItem()` contract as `VirtualList` and `VirtualGrid`.

## Custom modifiers

A property modifier applies a value directly to `ViewSpec`. A retained modifier pairs a small declarative value with a persistent `NodeExtension`:

```cpp
class GlowExtension;

struct Glow {
  using Extension = GlowExtension;

  Color color;
  float radius = 12.0F;
};
```

```cpp
class GlowExtension final : public NodeExtension {
public:
  GlowExtension(MountedNode& node, const Glow& spec);

  void Update(MountedNode& node, const Glow& spec);

  void Paint(const MountedNode& node, DisplayList& display_list) const override;
};
```

The framework detects `Glow::Extension`, performs type erasure, and reconciles compatible extensions by descriptor and declaration position. `Update()` refreshes declarative configuration without discarding retained animation or gesture state.

`NodeExtension` can receive frame, scroll, pointer, hover, focus, key, and paint callbacks. It returns frame scheduling needs from `OnFrame()` and must not retain raw node or child references across reconciliation.

## Root hooks and services

A root hook installs per-window services without replacing application content. Services can use `LayerController` for presentation above the application tree and are consumed through typed `UseService<Service>()`.

Built-in Toast and Dialog services install automatically. Third-party services should preserve per-window ownership and capture the Environment needed by deferred content.

## Platform hosts

`PlatformHost` is the native boundary for a HuxerUI host view. It provides frame scheduling, time, text measurement, text input, clipboard behavior, and native rendering integration while sharing the same `Runtime`.

Native feature modules should expose typed services and controllers rather than adding feature-specific branches to Runtime. Embedded native UI is designed as a real leaf View, not a Modifier or NodeExtension.

Detailed contracts are documented in:

- [Architecture Design](design/architecture.md)
- [Scope Code Generation Design](design/scope-codegen.md)
- [SDK, CLI, and Module Design](design/sdk-cli.md)

