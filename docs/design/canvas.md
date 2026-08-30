# Canvas and Path Design

This document defines HuxerUI's platform-neutral Path value, custom Canvas component, and their integration with retained PaintSequences.

## Goals

- Express arbitrary filled, stroked, clipped, and shadowed vector geometry in shared application code.
- Keep Canvas on the existing View, layout, paint invalidation, RenderScene, and platform renderer path.
- Preserve node-local logical coordinates and conservative damage bounds.
- Keep platform geometry and blur resources inside platform renderers.
- Avoid a second imperative rendering surface or public render-object hierarchy.

## Path

`Path` is a copy-on-write value containing one or more contours.
It supports move, line, quadratic curve, cubic curve, and close elements.
Copying a Path is inexpensive, and mutating one copy detaches it before changing the shared data.
A PaintCommand therefore retains a stable geometry snapshot even when the caller later changes the original Path.

Path coordinates must be finite.
Commands that extend or close a contour require a preceding `MoveTo()`.
`Close()` ends the active contour, so another contour starts with a new `MoveTo()`.
Move-only contours do not contribute to Path bounds because they produce no pixels.
Path equality compares geometry by value and uses shared storage identity as a fast path.
Bounds include curve extrema rather than only curve endpoints.

`PathFillRule::NonZero` and `PathFillRule::EvenOdd` are painting properties supplied by fill, clip, and shadow commands.
They do not change the underlying geometry value.

## Paint commands

`PaintContext` records Path operations through explicit methods:

```cpp
paint.FillPath(path, color, PathFillRule::NonZero);
paint.StrokePath(path, color,
                 StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round,
                             .dash_pattern = {8.0F, 4.0F}});
paint.DrawPathShadow(path, shadow_color, offset, blur_radius);
paint.PushPathClip(path, PathFillRule::EvenOdd);
paint.PopClip();
```

The corresponding immutable commands are `FillPathCommand`, `StrokePathCommand`, `DrawPathShadowCommand`, and `PushPathClipCommand`.
Path clips share the existing balanced clip stack and `PopClipCommand`.
Stroke bounds conservatively include cap, join, width, and miter-limit overflow.
Path shadows include offset and blur overflow but intentionally do not expose spread.
Reliable spread for an arbitrary path requires a separately defined geometry-offset operation for concave contours and holes.

`StrokeStyle` is the single stroke contract used by `DrawLine()`, `DrawArc()`, `DrawBorder()`, and `StrokePath()`.
It owns width, cap, join, miter limit, dash pattern, and dash offset; commands do not expose parallel flat stroke parameters.
Recording requires finite values, non-negative width and dash lengths, and a miter limit of at least one.
`DrawRect()` remains fill-only rather than duplicating border behavior.
`DrawLineCommand` preserves directed endpoints, while `VectorBuilder` stays Path-based because vector assets already represent line geometry as Path data.

Dash lengths and offsets use the geometry's local logical units and therefore follow the same transforms as the stroke.
Pattern entries alternate painted and skipped lengths, beginning with paint; an odd entry count repeats once to produce an even cycle.
An empty or all-zero pattern is canonicalized to a solid stroke, zero entries inside a nonzero cycle are retained, and negative offsets wrap into the cycle.
The selected cap applies to open contour endpoints and to each painted dash segment.
Every Path contour restarts from the normalized offset.
Lines begin at their `start`, arcs begin at `start_angle` and advance with the sweep direction, and borders begin on the top edge after the top-left corner and proceed clockwise.

Blurred Path shadows exclude the shifted caster interior, matching rectangular shadow semantics and avoiding a second solid shape.

Canvas uses the same text vocabulary as built-in controls.
`DrawText()` lays out a paragraph inside a rectangle, while `DrawTextRun()` and `DrawTextRuns()` replay baseline-positioned runs whose bounds were produced by `TextMeasurer`.
`TextLayoutOptions` controls horizontal alignment, vertical placement, and wrapping independently for paragraph drawing.
`DrawTextCommand::paragraph_offset` translates the laid-out paragraph inside its fixed layout and clip rectangle without changing measurement or alignment.
The run path is intended for syntax highlighting, diagnostics, and other callers that already own line layout.
See [Text and Font Design](text.md) for the measurement and replay contract.

## Canvas

`Canvas` is an ordinary leaf View with a pure painter callback:

```cpp
Canvas([](PaintContext& paint, Size size) {
  // Paint in local coordinates from (0, 0) to (size.width, size.height).
});
```

Canvas has no intrinsic size.
Its size comes from `Frame`, `Grow`, or parent constraints.
The painter receives the content size after Padding and always uses a content-local origin of `(0, 0)`.
For the uncommon case where Canvas itself has Padding, Runtime records a translation around the callback; a zero-Padding Canvas records no extra transform.

The node's Shadow and Background paint before the callback and continue to use the complete node bounds.
Foreground NodeExtensions and focus visuals paint afterward.
Canvas drawing is not clipped to its size automatically, allowing intentional overflow whose bounds participate in visibility and damage.
The painter can use `PushClip()` or `PushPathClip()` when clipping is required.

Canvas remains a normal interaction node.
Pointer hit testing uses its rectangular node bounds and does not infer a hit shape from painted Paths.

## Retention and invalidation

The painter runs when the Canvas content PaintSequence is dirty.
Mounting, reconciling a new painter, changing Canvas size, or changing a content-paint modifier marks that sequence dirty.
An unchanged frame reuses the recorded commands without invoking the callback.
Presentation-only changes such as offset, scale, rotation, opacity, and scrolling continue to reuse the same PaintSequence.

Painter callables are not compared by captured value.
When a scope recomposes and produces the same Canvas node, its content is conservatively rerecorded because the new callback may capture different declarative inputs.
This invalidation remains local to that Canvas and does not repaint clean siblings.

Canvas painters may append commands only to the supplied PaintContext.
They must not retain the context, mutate layout or interaction state, schedule frames, or query platform coordinates.
Animated retained behavior continues to use NodeExtension frame callbacks and paint invalidation rather than a Canvas-specific scheduler.

## Platform rendering

Platform renderers convert Path elements to platform geometry while preserving fill rules, stroke styles, transforms, and clip balance:

- Windows uses Direct2D path geometry.
- macOS uses Core Graphics paths.
- Android uses `android.graphics.Path` and the host Canvas.
- iOS uses Core Graphics paths.
- Linux uses Cairo paths.
- Web uses Canvas 2D paths.

Each renderer maps `StrokeStyle` into its native stroke state before replay.
Uniform solid borders may use a native rounded-rectangle operation; dashed or otherwise non-default border geometry lowers to the same inset Path used for asymmetric corners, so there is still one border shape contract.

Path shadow masks reuse each backend's existing blur machinery.
Platform geometry, masks, layers, and device-dependent caches never enter shared Runtime state.

## Unsupported capabilities

The Path surface does not include arcs, relative commands, boolean geometry operations, path metrics, gradient path fills, or Path-based pointer hit testing.
ImageAsset, DrawImage, and DrawImageRect extend the same PaintSequence and are specified in [App Resources, Images, and Localization Design](resources.md).
Rectangle linear and radial gradients use dedicated PaintCommands; there is no generic Brush abstraction.
