# Extending HuxerUI

HuxerUI exposes typed extension points for component events, layout, virtualization, retained modifiers, root services, and platform adapters. Prefer the narrowest extension point that owns the behavior.

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

A virtual layout that represents an accessible collection submits the collection and its default roles with the same result that commits placement:

```cpp
VirtualLayoutResult result;
result.SetCollectionSemantics(
    SemanticRole::Grid,
    SemanticRole::GridCell,
    SemanticCollection{
        .item_count = context.ItemCount(),
        .row_count = row_count,
        .column_count = column_count,
    }
);
```

Its realized placements include the logical collection position:

```cpp
result.Place(
    item,
    offset,
    SemanticCollectionItem{
        .index = index,
        .row_index = row,
        .column_index = column,
        .column_span = span,
    }
);
```

The ordinary two-argument `Place()` remains appropriate for non-collection layouts and decorative realized items.
Runtime attaches this metadata to the real mounted item root, preserves any existing component role, and never materializes an accessibility-only item.

## Custom modifiers

A property modifier applies a value directly to `ViewSpec`. A retained modifier pairs a small declarative value with a persistent `NodeExtension`:

```cpp
class GlowExtension;

struct Glow {
  using Extension = GlowExtension;

  Color color;
  float radius = 12.0F;

  bool operator==(const Glow&) const = default;
};
```

```cpp
class GlowExtension final : public NodeExtension {
public:
  GlowExtension(MountedNode& node, const Glow& spec);

  void Update(MountedNode& node, const Glow& spec);

  void Paint(const MountedNode& node, PaintContext& context) const override;
};
```

The framework detects `Glow::Extension`, performs type erasure, and reconciles compatible extensions by descriptor and declaration position.
An equality-comparable modifier skips `Update()` when its declarative value and relevant node inputs are unchanged.
`Update()` refreshes changed declarative configuration without discarding retained animation or gesture state.

`NodeExtension` can receive frame, resolved-geometry, scroll, pointer, hover, focus, key, and paint callbacks. `HoverHitTest()` selects hover-aware extensions and `OnHoverChanged()` reports entry and exit. It returns frame scheduling needs from `OnFrame()` and must not retain raw node or child references across reconciliation.
`PrepareGeometry()` runs after final presentation transforms are resolved and reports whether changed geometry requires foreground rerecording.
After retained visual state changes, an extension calls the protected `InvalidatePaint()` operation so Runtime rerecords its foreground PaintSequence.
Invalidation outside frame construction requests a frame, while invalidation from `OnFrame()` is consumed by the current frame and follow-up scheduling remains the responsibility of `FrameResult`.
Paint commands use node-local coordinates and may extend beyond `MountedNode::Bounds()` unless the extension pushes an explicit clip. Runtime uses the recorded command bounds, rather than the layout bounds, for render visibility and damage.

Custom editable or selectable components expose `TextInputClient`, `TextSelectionClient`, or both through their retained extension. Text input owns IME sessions; text selection owns word selection, handle geometry, and selection-menu actions without starting an IME session.

## Root hooks and services

A root hook installs per-window services without replacing application content. Services can use `LayerController` for presentation above the application tree and are consumed through typed `UseService<Service>()`.

Built-in Toast, Tooltip, Dialog, BottomSheet, Popup, and Menu services install automatically. Command-oriented typed handles capture the current Environment, while Tooltip captures it through its retained target modifier. A third-party service using `LayerController` directly receives the root Environment unless its own typed handle or modifier explicitly captures and provides narrower values needed by deferred content.

## Platform adapters

`PlatformAdapter` is the native boundary for a HuxerUI host view. It provides frame scheduling, time, text measurement, text input, clipboard behavior, and native rendering integration while sharing the same `Runtime`.

Native feature modules should expose typed services and controllers rather than adding feature-specific branches to Runtime. Embedded native UI is designed as a real leaf View, not a Modifier or NodeExtension.

Detailed contracts are documented in:

- [Architecture Design](design/architecture.md)
- [Scope Code Generation Design](design/scope-codegen.md)
- [SDK, CLI, and Module Design](design/sdk-cli.md)
