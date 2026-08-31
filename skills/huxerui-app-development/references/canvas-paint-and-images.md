# Canvas, Paint, and Images

## Colors and fills

`Color` stores normalized red, green, blue, and alpha channels. Prefer `Color::Rgb(...)`, `Color::Transparent()`, `Color::Black()`, and `Color::White()` instead of mixing byte and normalized channel conventions.

`VisualFill` accepts a `Color`, `LinearGradient`, `RadialGradient`, or `ImageFill`. Gradient start, end, center, and radius values are normalized to the painted bounds; stops use offsets from `0.0F` to `1.0F`. Set the gradient's `transform` to rotate, scale, skew, or translate that normalized sampling space without moving the painted geometry; leave its identity default when no transform is needed. `ImageFill` adds fit, alignment, sampling, optional tint, and opacity to an `ImageVariant`. The same fill vocabulary is used by `Background` and interaction indication layers.

## Image sources

`ImageVariant` accepts the image forms exposed by the active SDK, including resource-backed and resolved assets. `Image` provides fit, alignment, sampling, and tint. Use `VectorAsset` for public vector data and `ExternalTexture` for frames produced outside HuxerUI.

## Canvas

`Canvas` receives a `PaintContext` and assigned `Size`. Give it explicit or parent-derived constraints. Draw in local coordinates and do not use Canvas to arbitrarily place `PlatformView` children.

`PaintContext` emits platform-neutral commands for rectangles, gradients, text, images, circles, lines, arcs, borders, shadows, paths, clips, and transforms. Balance every pushed clip or transform with a pop on every path. Call only public drawing methods; `PaintCommand`, `RenderScene`, and renderer integration are framework boundaries rather than application extension points.
`FillPath()` and `StrokePath()` accept a solid color, `LinearGradient`, or `RadialGradient`. Gradient geometry and its transform are normalized to exact Path bounds unless an explicit gradient rectangle is supplied; that rectangle defines coordinates and does not clip the fill or stroke. Use the explicit form when separate Paths must share one continuous gradient.

## Paths and text

Use `Path` and its public builder operations for filled or stroked geometry. `StrokeStyle` is the single stroke configuration accepted by `DrawLine()`, `DrawArc()`, `DrawBorder()`, and `StrokePath()`; do not pass width, cap, or join as parallel arguments. Dash entries alternate painted and skipped lengths in local logical units, an odd entry count repeats to form an even cycle, and each Path contour restarts at `dash_offset`.

`Path::ArcTo()` continues the active contour with an endpoint-based elliptical arc. Supply local x/y radii, x-axis rotation in radians, `ArcSize`, `ArcDirection`, and the endpoint; clockwise follows HuxerUI's downward-Y logical coordinates. Undersized radii scale to reach the endpoint, either zero radius produces a line, and a coincident endpoint adds no segment. Use two endpoint arcs for a complete ellipse. Use `PaintContext::DrawArc()` instead for an independent stroked circular arc that does not join a Path contour.

Use `Path::Contains(point, fill_rule)` to test a local point against the same filled geometry used by custom Canvas interaction. Match the rule passed to `FillPath()` or `PushPathClip()`; open contours close implicitly and their boundaries are included. This query does not test stroke width.

```cpp
paint.DrawLine({0.0F, 12.0F}, {120.0F, 12.0F}, Color::Black(),
               StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 4.0F}});
paint.StrokePath(path, LinearGradient{.stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}}},
                 StrokeStyle{.width = 3.0F, .join = StrokeJoin::Round});
```

Respect fill rules when filling, clipping, or querying paths. Text painting uses `TextStyle`, shaping options, and layout options. `UseTextMeasurer()` provides the active platform text measurer during composition; do not invent glyph metrics. Prefer `Text` for ordinary UI text because it owns layout and semantics.

## External textures

`ExternalTexture` is a retained frame source displayed through `Image(texture)`. Platform-specific public `ExternalTextureSource` types publish:

- Windows/Linux: copied RGBA/BGRA byte frames with explicit dimensions and stride;
- Android: a Java bitmap;
- iOS/macOS: a `CVPixelBufferRef`;
- Web: an `emscripten::val` video-frame source.

Create the source with an intrinsic DIP size, expose `Texture()` to UI, publish platform frames, and call `Finish()` when no more frames will arrive. Use `PlatformView` instead when a live platform control must own input, IME, accessibility, or platform lifecycle.
Objective-C and Swift on iOS or macOS use `ExternalTextureSource` from `HuxerUIPlatform`; it wraps the same platform source, publishes `CVPixelBuffer` frames, and may travel inside `PlatformPayload` without a public texture identifier or second registry.
