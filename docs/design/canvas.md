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
It supports move, line, quadratic curve, cubic curve, endpoint-based elliptical arc, and close operations.
Copying a Path is inexpensive, and mutating one copy detaches it before changing the shared data.
A PaintCommand therefore retains a stable geometry snapshot even when the caller later changes the original Path.

Path coordinates must be finite.
Commands that extend or close a contour require a preceding `MoveTo()`.
`Close()` ends the active contour, so another contour starts with a new `MoveTo()`.
Move-only contours do not contribute to Path bounds because they produce no pixels.
Path equality compares geometry by value and uses shared storage identity as a fast path.
Bounds include curve extrema rather than only curve endpoints.

`ArcTo()` selects one of the four elliptical arcs between the current point and an endpoint through `ArcSize` and `ArcDirection`.
Its horizontal and vertical radii precede an explicit x-axis rotation in radians; positive rotation and `ArcDirection::Clockwise` follow HuxerUI's downward-Y logical coordinate system.
Undersized radii scale proportionally, either zero radius produces a line, and an endpoint equal to the current point produces no segment.
One endpoint arc cannot identify a complete ellipse, which is expressed by two arcs with distinct intermediate endpoints.

Path normalizes each non-degenerate arc immediately into at most four cubic elements whose absolute sweep does not exceed `pi / 2`.
The final cubic uses the declared endpoint exactly, and the canonical cubic sequence becomes the geometry observed by equality, bounds, fill, stroke, dash, shadow, clipping, and immutable PaintCommand snapshots.
The maximum radial deviation of a 90-degree circular segment is approximately `0.0273%` of its radius; applying the ellipse transform bounds the absolute deviation by the same fraction of the larger normalized radius.
No renderer receives an arc-specific Path element, while the independent circular `DrawArcCommand` remains available for direct stroked arcs.

`PathFillRule::NonZero` and `PathFillRule::EvenOdd` are operation inputs supplied by fill, clip, shadow, and containment queries.
They do not change the underlying geometry value.

`Path::Contains()` evaluates a local point against the same implicitly closed fill contours without consulting a renderer.
It adaptively subdivides quadratic and cubic elements through deterministic midpoint De Casteljau subdivision, then applies a half-open ray crossing rule to the resulting edges.
Non-zero filling tracks signed winding, while even-odd filling tracks crossing parity.
The boundary is contained, including recorded zero-length segments and explicit or implicit closing edges.
The base logical tolerance is `0.0001`; the effective tolerance is the greater of that value and four single-precision units at the geometry's coordinate scale.
Curve flatness uses half the effective tolerance and a maximum subdivision depth of 24.
The query retains no mutable cache and does not alter shared Path storage or PaintCommand snapshots.

## Paint commands

`PaintContext` records Path operations through explicit methods:

```cpp
paint.FillPath(path, color, PathFillRule::NonZero);
paint.FillPath(path, LinearGradient{
    .start = {0.0F, 0.0F},
    .end = {1.0F, 1.0F},
    .stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}},
});
paint.StrokePath(path, color,
                 StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round,
                             .dash_pattern = {8.0F, 4.0F}});
paint.DrawPathShadow(path, shadow_color, offset, blur_radius);
paint.PushPathClip(path, PathFillRule::EvenOdd);
paint.PopClip();
```

Solid and gradient fills record `FillPathCommand`, `FillLinearGradientPathCommand`, or `FillRadialGradientPathCommand` as one atomic geometry-and-paint operation.
Strokes, shadows, and clips continue to use `StrokePathCommand`, `DrawPathShadowCommand`, and `PushPathClipCommand`.
The overload without an explicit gradient rectangle evaluates normalized gradient coordinates relative to exact Path bounds.
An explicit gradient rectangle lets several Paths share one coordinate space without clipping geometry to that rectangle.
Damage remains based on transformed and clipped Path bounds rather than the gradient coordinate rectangle.
Renderers map the atomic command to native Path filling where available; Core Graphics uses an equivalent renderer-local Path clip around one gradient draw rather than exposing clip expansion in the shared command sequence.
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

The Path surface does not include relative commands, boolean geometry operations, path metrics, gradient strokes, stroke containment, or a reusable hit-shape modifier.
ImageAsset, DrawImage, and DrawImageRect extend the same PaintSequence and are specified in [App Resources, Images, and Localization Design](resources.md).
Rectangle and Path linear and radial gradients use explicit PaintCommands; there is no generic Brush abstraction.
