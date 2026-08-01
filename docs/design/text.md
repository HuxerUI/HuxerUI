# Text and Font Design

Status: implemented

This document defines the shared text values, measurement boundary, paint commands, and native resource ownership used by Text, TextField, Canvas, and text-oriented component libraries.
Localized string resolution and the root Locale Environment are defined in [App Resources, Images, and Localization Design](resources.md); applying the inherited Locale automatically to otherwise-unspecified text shaping remains deferred.

## Ownership

The shared C++ layer owns platform-neutral font identity, text style, shaping intent, layout options, measured geometry, and retained paint commands.
Platform hosts own native font resolution, shaping, paragraph layout, glyph drawing, and bounded native caches.
Native font, glyph, layout, and brush objects never enter a View, PaintCommand, RenderScene, or application state value.

`Font`, `TextStyle`, `TextShapingOptions`, and `TextLayoutOptions` compare by value.
A value change therefore participates naturally in View reconciliation, layout invalidation, PaintSequence invalidation, and renderer cache lookup without a separate generation counter or manual invalidation API.

## Font and style

`Font` is a complete immutable font request:

```cpp
Font body = Font::System(16.0F);
Font code = Font::Monospace(14.0F);
Font title = Font::Named("Inter", 28.0F)
                 .WithWeight(FontWeight::Bold)
                 .WithSlant(FontSlant::Italic);
```

The family is system, monospace, or explicitly named.
Size belongs to Font because native font metrics and shaped glyph advances depend on it.
Weight and slant also belong to Font because they select the native face.
The platform may fall back when a requested named family or exact face is unavailable, but the declarative Font value remains unchanged.

`TextStyle` combines the font with foreground color and composable decoration flags:

```cpp
TextStyle diagnostic_style{
    Font::Monospace(14.0F),
    Color::Rgb(207, 34, 46),
    TextDecoration::Underline | TextDecoration::StrikeThrough,
};
```

`Text(...).Style(style)` replaces the complete theme-resolved style for that Text value.
Later `Foreground` and `FontSize` modifiers update the corresponding members while preserving the remaining font and decoration identity.

Underline and strike-through are style properties rather than separate drawing commands.
Selection, composition, diagnostics, and other decorations that require custom geometry can still use ordinary PaintCommands.

Paragraph alignment and wrapping are not visual style.
They belong to `TextLayoutOptions`, while direction and locale shaping hints belong to `TextShapingOptions`.
This keeps one TextStyle reusable across a paragraph, a TextField, and exact editor runs.

`TextDirection::Auto` resolves from the first strong directional character and falls back to left-to-right when the text contains no strong character.
Measurement and drawing use the same resolution rule on every platform.

## Measurement

`TextMeasurer` is the only public geometry source:

```cpp
FontMetrics font_metrics = measurer.Metrics(style.font);
TextRunMetrics run = measurer.MeasureRun(text, style, shaping);
TextLayoutMetrics paragraph = measurer.MeasureText(text, style, max_width, options);
```

`FontMetrics` describes one resolved font.
`TextRunMetrics` contains advance, a conservative visual bound, and the resolved font metrics for one unwrapped shaped run.
An exact run does not contain line breaks; line ownership remains with the caller.
Its bounds use a baseline origin at `(0, 0)`, so ascent occupies negative y coordinates.
`TextLayoutMetrics` contains the paragraph size, first and last baseline, and line count.
It intentionally does not expose per-line geometry; editable controls and editor components retain their own line model
when hit testing, selection, or individual line metrics are required.

`TextWrap::NoWrap` disables automatic wrapping but preserves explicit line breaks in the source text.
A finite `max_width` still constrains the reported paragraph width; it does not change the natural line geometry retained by an editable layout.
`TextWrap::Word` additionally introduces automatic line breaks to satisfy a finite `max_width`.
Both modes report all resulting hard and automatic lines through `line_count`, `first_baseline`, and `last_baseline`.

The active `PlatformHost` is exposed through a private root text-measurer service whose lifetime is owned by Runtime.
Components can obtain it with `UseTextMeasurer()` without depending on PlatformHost or native types.
Built-in layout and editing code use the same service contract directly through the host boundary.
`TextMeasurer` calls occur synchronously on the Runtime and native host thread.
Callers may retain returned value metrics, but they must not retain the service reference beyond the active composition or layout operation.

Measurement results are authoritative geometry.
A caller that positions exact runs translates the returned run bounds to its chosen baseline and records both values.
The renderer may reuse a native shaped object and query glyph internals required for replay, but it must not measure the text again to move, resize, wrap, or clip the supplied run.

## Paragraph and run commands

`DrawText()` is the paragraph operation:

```cpp
paint.DrawText(
    {0.0F, 0.0F, size.width, size.height},
    text,
    style,
    {.align = TextAlign::Leading, .wrap = TextWrap::Word}
);
```

Its rectangle is a layout constraint, so the renderer creates a native paragraph layout using the same TextStyle and TextLayoutOptions that were used for measurement.
Wrapped paragraphs start at the top of the rectangle; an unwrapped paragraph is vertically centered while TextAlign controls each hard line's horizontal placement.
When an unwrapped line is wider than the rectangle, Center and semantic trailing alignment may place its origin before the rectangle's leading edge; the rectangle clips the overflow without changing alignment.
This path serves Text, buttons, labels, validation text, and other ordinary UI paragraphs.

`DrawTextRun()` is the exact baseline-positioned operation:

```cpp
TextRunMetrics measured = measurer.MeasureRun(token, style);
Point baseline{line_x, line_baseline};
Rect bounds{
    baseline.x + measured.visual_bounds.x,
    baseline.y + measured.visual_bounds.y,
    measured.visual_bounds.width,
    measured.visual_bounds.height,
};
paint.DrawTextRun(bounds, baseline, token, style);
```

The bounds exist for culling, damage, and visibility.
The baseline origin controls placement.
An empty string or empty bound records no run because it cannot contribute visible pixels or damage.
Multiple adjacent calls coalesce into one `DrawTextRunsCommand`, and `DrawTextRuns()` records a prepared vector in one operation.
This command supports editor lines, syntax spans, diagnostic text, and other callers that already own line layout without introducing a second Canvas or render-object model.

`DrawTextCommand` remains necessary because paragraph layout and exact run replay have different ownership.
Replacing it with runs would force ordinary components to implement wrapping and alignment themselves.

## Native caches

Native caches are private to one platform renderer or host view.
A resolved-font or metrics key contains family kind and name, size, weight, and slant; a backend may omit size only when its cached native typeface is explicitly size independent.
A paragraph or shaped-run key additionally contains text, shaping options, width, alignment, and wrapping.
Color may be excluded when the native API applies it at draw time.
Decoration remains in the key whenever it changes native layout state or the conservative visual bounds.

Caches are bounded keyed lookups rather than linear histories and use values rather than pointers into RenderScene.
Changing a Font, TextStyle, text string, shaping option, or layout width naturally misses the old entry.
Device-dependent resources are cleared when the native drawing device is recreated.
Host destruction releases all remaining entries.
System font database changes may clear font-dependent entries at the platform boundary without invalidating shared Runtime state.

Cache hits are an optimization only.
They cannot change measurement, line breaking, baseline placement, selection geometry, or damage bounds.
Secure TextField source text must not be retained in a global text or paragraph cache; only its masked display text may use ordinary renderer caches.

## TextField and selection

TextField uses complete TextStyle values for editable text, placeholder text, and validation text.
The native TextLayout created for editing supplies hit testing, caret rectangles, selection rectangles, and grapheme movement from the same style and layout options used to paint the field.
IME geometry therefore derives from the committed TextLayout and the node's local-to-host transform rather than from a renderer-side measurement.

Selection overlays remain Runtime-owned RenderNodes.
Their labels use paragraph text commands and Theme-resolved TextStyle values.
They do not introduce a second native text ownership path.

## Platform mapping

- Android maps Font to cached Typeface values, keeps a bounded StaticLayout paragraph cache, and submits each exact-run batch through one JNI call for baseline-positioned Canvas replay.
- macOS keeps bounded CoreText font, CTFrame paragraph, and CTLine exact-run caches.
- Windows keeps bounded DirectWrite font metrics, paragraph layout, and exact-run layout caches.

All mappings consume logical coordinates and UTF-8 shared text at the host boundary.
Native adapters perform their required UTF-16 or platform-string conversion locally.
