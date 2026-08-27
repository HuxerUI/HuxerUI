# Canvas, Paint, and Images

## Colors and fills

`Color` stores normalized red, green, blue, and alpha channels. Prefer `Color::Rgb(...)`, `Color::Transparent()`, `Color::Black()`, and `Color::White()` instead of mixing byte and normalized channel conventions.

`VisualFill` accepts a `Color`, `LinearGradient`, `RadialGradient`, or `ImageFill`. Gradient start, end, center, and radius values are normalized to the painted bounds; stops use offsets from `0.0F` to `1.0F`. `ImageFill` adds fit, alignment, sampling, optional tint, and opacity to an `ImageVariant`. The same fill vocabulary is used by `Background` and interaction indication layers.

## Image sources

`ImageVariant` accepts the image forms exposed by the active SDK, including resource-backed and resolved assets. `Image` provides fit, alignment, sampling, and tint. Use `VectorAsset` for public vector data and `ExternalTexture` for frames produced outside HuxerUI.

## Canvas

`Canvas` receives a `PaintContext` and assigned `Size`. Give it explicit or parent-derived constraints. Draw in local coordinates and do not use Canvas to arbitrarily place `PlatformView` children.

`PaintContext` emits platform-neutral commands for rectangles, gradients, text, images, circles, arcs, borders, shadows, paths, clips, and transforms. Balance every pushed clip or transform with a pop on every path. Call only public drawing methods; `PaintCommand`, `RenderScene`, and renderer integration are framework boundaries rather than application extension points.

## Paths and text

Use `Path` and its public builder operations for filled or stroked geometry. Respect fill rules, stroke cap/join, and radians for arcs. Text painting uses `TextStyle`, shaping options, and layout options. `UseTextMeasurer()` provides the active platform text measurer during composition; do not invent glyph metrics. Prefer `Text` for ordinary UI text because it owns layout and semantics.

## External textures

`ExternalTexture` is a retained frame source displayed through `Image(texture)`. Platform-specific public `ExternalTextureSource` types publish:

- Windows/Linux: copied RGBA/BGRA byte frames with explicit dimensions and stride;
- Android: a Java bitmap;
- iOS/macOS: a `CVPixelBufferRef`;
- Web: an `emscripten::val` video-frame source.

Create the source with an intrinsic DIP size, expose `Texture()` to UI, publish platform frames, and call `Finish()` when no more frames will arrive. Use `PlatformView` instead when a live platform control must own input, IME, accessibility, or platform lifecycle.
