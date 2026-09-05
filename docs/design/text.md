# Text and Font Design

This document defines the shared text values, measurement boundary, paint commands, and platform resource ownership used by Text, TextField, Canvas, and text-oriented component libraries.
Localized string resolution and the root Locale Environment are defined in [App Resources, Images, and Localization Design](resources.md).

## Ownership

The shared C++ layer owns platform-neutral font identity, text style, shaping intent, layout options, measured geometry, and retained paint commands.
Platform adapters own platform font resolution, shaping, paragraph layout, glyph drawing, and bounded platform caches.
Platform font, glyph, layout, and brush objects never enter a View, PaintCommand, RenderScene, or application state value.

`AttributedText`, `Font`, `TextStyle`, `TextShapingOptions`, and `TextLayoutOptions` compare by value.
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
Size belongs to Font because platform font metrics and shaped glyph advances depend on it.
Weight and slant also belong to Font because they select the platform face.
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
Horizontal and vertical text alignment and wrapping belong to `TextLayoutOptions`, while direction and locale shaping hints belong to `TextShapingOptions`.
This keeps one TextStyle reusable across a paragraph, a TextField, and exact editor runs.

`TextAlign` places each line horizontally using semantic leading and trailing directions.
`TextVerticalAlign` places the complete paragraph vertically inside its final rectangle using `Top`, `Center`, or `Bottom`.
The default layout uses leading horizontal alignment, top vertical alignment, and word wrapping.
Vertical alignment does not change intrinsic paragraph measurement and applies only when the final rectangle has unused height.
When the paragraph is taller than the rectangle, the remaining height is clamped to zero so alignment never introduces a negative vertical origin.

`TextDirection::Auto` resolves from the first strong directional character and falls back to left-to-right when the text contains no strong character.
Measurement and drawing use the same resolution rule on every platform.

Mounted Text, built-in control labels, TextField, and Canvas text use the effective inherited `Locale` when their `TextShapingOptions::locale` is empty.
`Text(...).Shaping(options)` and `TextField(...).Shaping(options)` provide complete component-local shaping overrides; a non-empty shaping locale wins without subscribing that declaration to the inherited Locale for shaping.
TextField applies one resolved shaping value to its controlled value, label, placeholder, validation message, selection and caret geometry, secure grapheme processing, measurement, and retained drawing.
The PaintContext passed to a Canvas fills an empty locale for `DrawText`, `DrawTextRun`, and each `DrawTextRuns` entry from that Canvas node while preserving every explicit run locale.
An independently constructed PaintContext has no inherited Environment and therefore leaves empty shaping locales unchanged.

Localized resource selection and text shaping remain separate concerns.
An explicit shaping locale does not change which StringResource or ImageResource variant a declaration resolves, and a resource Locale change can still update resource-backed content whose shaping locale is explicit.
TextMeasurer remains stateless and explicit: custom composition code reads `Locale` with `UseEnvironment<Locale>()` and passes its language tag when it wants inherited shaping.

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

The active `PlatformAdapter` is exposed through a private root text-measurer service whose lifetime is owned by Runtime.
Components can obtain it with `UseTextMeasurer()` without depending on PlatformAdapter or platform API types.
Built-in layout and editing code use the same service contract directly through the host boundary.
`TextMeasurer` calls occur synchronously on the Runtime and platform host thread.
`NodeExtension::PrepareGeometry()` receives the same active measurer after final presentation geometry is resolved.
Callers may retain returned value metrics, but they must not retain the service reference beyond the active composition, layout, or geometry-preparation operation.

Measurement results are authoritative geometry.
A caller that positions exact runs translates the returned run bounds to its chosen baseline and records both values.
The renderer may reuse a platform-shaped object and query glyph internals required for replay, but it must not measure the text again to move, resize, wrap, or clip the supplied run.

## Paragraph and run commands

`DrawText()` is the paragraph operation:

```cpp
paint.DrawText(
    {0.0F, 0.0F, size.width, size.height},
    text,
    style,
    {
        .align = TextAlign::Leading,
        .vertical_align = TextVerticalAlign::Top,
        .wrap = TextWrap::Word,
    }
);
```

Its rectangle is a layout constraint, so the renderer creates a platform paragraph layout using the same TextStyle and TextLayoutOptions that were used for measurement.
`TextAlign` controls each hard line's horizontal placement, and `TextVerticalAlign` controls the paragraph's vertical placement independently of wrapping.
When an unwrapped line is wider than the rectangle, Center and semantic trailing alignment may place its origin before the rectangle's leading edge; the rectangle clips the overflow without changing alignment.
`DrawTextCommand::paragraph_offset` applies a post-layout translation inside that fixed rectangle.
It does not affect paragraph measurement, alignment, cache identity, or clipping, and lets a scrolling editor keep retained text geometry and rendered text in the same coordinate system.
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

## Platform caches

Platform caches are private to one platform renderer or host view.
A resolved-font or metrics key contains family kind and name, size, weight, and slant; a backend may omit size only when its cached platform typeface is explicitly size independent.
A paragraph or shaped-run key additionally contains text, shaping options, width, alignment, and wrapping.
Color may be excluded when the platform API applies it at draw time.
Decoration remains in the key whenever it changes platform layout state or the conservative visual bounds.

Caches retain bounded reusable entries rather than unbounded histories and use values rather than pointers into RenderScene.
Changing a Font, TextStyle, text string, shaping option, or layout width naturally misses the old entry.
Device-dependent resources are cleared when the platform drawing device is recreated.
Host destruction releases all remaining entries.
System font database changes may clear font-dependent entries at the platform boundary without invalidating shared Runtime state.

Cache hits are an optimization only.
They cannot change measurement, line breaking, baseline placement, selection geometry, or damage bounds.
Secure TextField source text must not be retained in a global text or paragraph cache; only its masked display text may use ordinary renderer caches.

## TextField and selection

TextField uses complete TextStyle values for editable text, placeholder text, and validation text.
The platform TextLayout created for editing supplies hit testing, caret rectangles, selection rectangles, and grapheme movement from the same style and layout options used to paint the field.
TextField applies horizontal alignment through that retained TextLayout and derives value, placeholder, caret, selection, hit-test, and IME geometry from one aligned origin.
Single-line fields vertically center editable content by default, while multiline fields default to top alignment; an explicit TextField vertical alignment overrides that adaptive default.
Floating labels, icons, and supporting messages retain their own component geometry and are not moved by editable-text alignment.
IME geometry therefore derives from the committed TextLayout and the node's local-to-host transform rather than from a renderer-side measurement.

Selection overlays remain Runtime-owned RenderNodes.
Their labels use paragraph text commands and Theme-resolved TextStyle values.
They do not introduce a second platform text ownership path.

## Platform mapping

- Windows keeps bounded DirectWrite font metrics, paragraph layout, and exact-run layout caches.
- macOS and iOS keep bounded CoreText font, CTFrame paragraph, and CTLine exact-run caches.
- Android maps Font to cached Typeface values, keeps a bounded StaticLayout paragraph cache, and submits each exact-run batch through one JNI call for baseline-positioned Canvas replay.
- Linux configures one Pango attributed layout for each measurement, geometry, or drawing operation.
- Web retains Canvas-backed paragraph geometry with the deliberate shaping limitations below.

All mappings consume logical coordinates and UTF-8 shared text at the host boundary.
Platform adapters perform their required UTF-16 or platform-string conversion locally.

## Attributed paragraphs and logical selection

### Scope and ownership

The feature extends the existing `Text` node rather than adding a parallel rich-text component or drawing channel.
Its scope includes mixed character styles, links, continuous selection, virtualized logical text, and bounded streaming update work.
Markdown and HTML parsing, rich-text editing, undo history, arbitrary inline Views, document-format conversion, and virtualization inside one paragraph remain outside this feature.
A paragraph is text with one set of layout options and may contain explicit line breaks; headings, list items, code groups, and other document blocks remain application-owned composition.

Three contracts remain separate:

- `AttributedText` is an immutable paragraph value containing text and attributes, not a mutable document or mounted interaction state.
- Platform paragraph layout supplies measurement, drawing, hit testing, caret positions, and range geometry from equivalent inputs.
- Logical selection reads application-owned text independently of which paragraphs are currently mounted.

An eventual editor may produce attributed paragraph snapshots and expose its content through the read-only selection source.
Editing operations, transaction history, insertion styles, and precise edit-position mapping do not belong in the paragraph value or selection source.

### Attributed values and construction

`AttributedText` owns validated UTF-8 text, normalized style ranges, independent link ranges, cached UTF-16 length, and comparison metadata through shared immutable storage.
Text storage can be shared independently of attributes so recoloring does not require another copy of the body.
Copying a value into State, a View declaration, a mounted payload, or a PaintCommand shares its storage.
Constructing a changed continuous text body may still take time proportional to that body; immutable sharing is not a constant-time append guarantee.
Equality first compares storage identity, then cached comparison information and actual normalized content; a hash match alone is never equality.

`TextSpanStyle` supplies optional complete Font, size, weight, slant, foreground, background, and decoration overrides.
Resolution starts with the Text's final base style, applies an explicitly supplied complete Font, and then applies individual font and visual overrides.
An omitted field inherits, `TextDecoration::None` explicitly clears decoration, and a transparent background produces no visible background.
The existing complete-replacement meaning of `Text::Style()` remains unchanged.
Paragraph alignment, wrapping, direction, and shaping locale remain paragraph properties rather than character attributes.

`TextSpan` is a construction fragment with direct UTF-8 text, optional style overrides, and an optional Uri.
It contains no callbacks, Views, platform objects, or retained state.
Fragment construction concatenates once, discards empty fragments, omits ineffective style ranges, and merges adjacent equivalent visual ranges.
`AttributedText::FromRanges()` accepts an already assembled body with ordered non-overlapping style ranges and ordered non-overlapping link ranges.
This avoids requiring parsers and highlighters to split and concatenate an existing body solely to add attributes.
Replacing style ranges on an existing value can reuse the body; a public mutable Builder and arbitrary overlapping-style precedence are not required.

Style and link ranges are independent projections and may cross each other's boundaries.
One logical link may contain several visual styles without becoming several focus or accessibility targets.
Distinct adjacent links do not merge merely because their targets match.
The ranges inside either projection must be in bounds and must not split a UTF-16 surrogate pair.
Validation and batched UTF-8/UTF-16 conversion scan the body without rescanning it from the beginning for every range.
Caller input errors use `std::invalid_argument` and the existing English HuxerUI diagnostic prefix.

User selection respects grapheme boundaries rather than treating style boundaries as extra caret stops.
Combining sequences and emoji sequences must not be split by pointer or handle movement.
Range backgrounds follow visual fragments across wrapping and bidirectional ordering; they are not one enclosing rectangle and introduce no inline padding or box layout.

`Text(StringVariant)` remains the localized plain-text declaration path, while attributed values contain resolved direct text.
Localization may reorder a sentence, so independently translated fragments are not a sentence-localization strategy.
A localized attributed resource format is outside this feature.

### Unified paragraph boundary

Plain and attributed declarations normalize into the same mounted paragraph representation without retaining two authoritative bodies.
The generic State formatting constructor excludes attributed values, which have a dedicated Text input path.
Ordinary controls retain their existing public plain-label inputs even when internal paragraph services accept attributed data.

TextMeasurer, PlatformAdapter text-layout creation, and DrawText consume the same attributed paragraph contract.
Plain-string convenience calls delegate to that contract rather than retaining a second virtual implementation.
DrawTextRuns remains the exact-run operation for callers that already own line layout and is not an attributed-paragraph fallback.

Platform-private paragraph construction applies equivalent text, font ranges, options, and constraints to measurement, interaction geometry, and painting.
Selection and links share one retained geometry layout on their mounted Text; geometry caches never pin unmounted blocks.
Measurement and painting may create separate platform layout objects because they have different lifetimes and secure-text retention policies.
Android uses `HuxerUITextLayout` to construct the StaticLayout for all three paths; renderer cache lookup precedes span construction and StaticLayout creation, while editing geometry remains outside that cache.
Intrinsic and final width constraints can legitimately produce different layouts.
Vertical placement, scrolling translation, and clipping remain presentation inputs where they do not affect shaping or line breaking.
Platform font and layout objects never enter PaintCommands or application values, and previously published cached layouts are not mutated for another paragraph's appearance.

Invalidation compares the projections actually consumed by each stage:

| Change | Required invalidation |
|---|---|
| Text, effective font ranges, width, or wrapping policy | Layout, content paint, and affected interaction geometry |
| Foreground, background, or decoration | Paint, without ancestor remeasurement |
| Link target only | Interaction and semantics, without paragraph reflow |
| Selection, hover, or pressed state | Affected retained interaction paint |
| Position, scrolling, or opacity | Presentation, without reshaping |

A full attributed-value comparison must not turn every paint-only change into a layout change.
Caches have bounded memory retention as well as appropriate entry limits, and superseded streaming versions cannot accumulate indefinitely.
Renderer paragraph caches admit at most 256 entries and charge an estimated eight-megabyte retention budget; oversized entries are used without retention.
The budget includes source text, attributes, and conservative platform glyph bookkeeping estimates, not a guarantee about the platform allocator's exact resident bytes.
Linux does not retain a renderer paragraph cache.
Unifying paragraph services must not put secure TextField source text into ordinary renderer caches.

### Logical selection source

Ordinary `SelectionArea(content)` continues to discover descendant Text nodes and insert a newline between their copied bodies.
Discovery maintains entries by mounted identity and reuses unchanged paragraph geometry instead of clearing all entries and rebuilding a concatenated document at each measurement.
Nested selection areas remain independent owners.

For virtualized content, `SelectionArea::Source()` accepts a shared read-only `TextSelectionSource` snapshot.
The source exposes logical block count, block identity by index, index lookup by identity, and a block's attributed text and following separator.
It does not create Views, fetch resources, parse markup, own platform geometry, or introduce another change-notification system.
The application publishes replacement snapshots through existing State and keeps the source and rendered Text values derived from the same block snapshots.
Unchanged bodies, attributes, ordering, and lookup indexes remain shared; replacing a tail paragraph does not require copying the full block list or rebuilding its index.
Structural edits may update ordering and indexes.
Copy and geometry queries use the same committed source version; new source text cannot be combined with stale visible paragraph geometry.

`Text::SelectionBlock()` binds a complete Text to one stable logical block ID.
That ID is unique within the selection source and is independent of View::Key's sibling-local reconciliation identity.
One logical block has at most one mounted Text representation within its selection owner, and its rendered plain text must match the source body.
In explicit-source mode, unbound decorative Text nodes do not silently become part of copied document content.
Mapping arbitrary slices of one logical block to several Text nodes is outside the initial contract; independently keyed line groups cover line-oriented content.

The separator belongs to the logical source, not the layout tree.
Copy includes separators only when traversing between selected blocks and never appends the final block's separator merely because it exists.
Select All establishes logical endpoints without materializing every block or flattening the document.
Copy reads the selected logical blocks, including unmounted blocks, and assembles only the requested result.
Copy never starts network work to obtain content outside the published source.

Selection positions contain block identity, paragraph-local UTF-16 offset, and layout affinity rather than a global offset that shifts whenever preceding text changes.
An unmounted block loses geometry, not its place in the selection.
Only currently laid-out selected fragments contribute highlight geometry; selected offscreen content does not pin mounted Views or platform paragraph layouts.

### Selection updates and gestures

Snapshot changes follow deterministic read-only selection rules:

- Style-only changes preserve selection.
- Appending text preserves existing endpoint offsets, including an endpoint at the previous end; newly appended text is not automatically selected.
- Inserting or moving blocks preserves endpoints by block identity and uses the newly committed logical order.
- Removing an intermediate block changes the copied content without discarding surviving endpoints.
- Removing either endpoint block clears selection instead of guessing a replacement block.
- Replacing an endpoint block's body maps positions through a conservative replacement interval derived from common prefix and suffix boundaries: positions before it remain, positions after it shift, and positions inside it converge to the replacement boundary.
- Insertion-boundary bias preserves the pre-insertion position for streaming append; layout affinity is not reused as insertion bias.
- Mapped positions are normalized to valid text boundaries before use.
- Replacing the document's selection-owner identity clears its old selection.

Only changed endpoint blocks need old/new text comparison; the complete transcript is not diffed.
This conservative snapshot mapping is not an editor transaction model and can later be replaced by precise edit-operation mapping in an editor.

TextSelectionClient geometry must represent the two endpoint rectangles independently as optional values and provide an appropriate visible-selection anchor for the toolbar.
One offscreen endpoint must not hide the other visible handle or force an offscreen paragraph to mount.
When the complete selection is offscreen, its presentation can hide while its logical state remains available for Copy.
All geometry follows the same content origin, vertical alignment, transforms, and effective clips as the paragraph drawing.
At an automatic line break, the same logical offset has an upstream position at the preceding line's end and a downstream position at the next line's start; hit testing preserves the selected visual line.
An explicit line separator has its own source offsets, so a position after it stays on the next line regardless of affinity.
Directional boundaries continue to use the platform layout's bidi geometry; a strong/weak cursor distinction alone does not resolve soft-wrap affinity.
Apple NoWrap geometry applies the same available-width alignment translation as drawing, including paragraphs with several explicit lines; UIKit and AppKit retain separate implementations.
Android selection rectangles retain Layout's disjoint selection-path fragments through Region's integer-coordinate boundary using a 1/64-logical-pixel grid, not a DPI scale.
That conversion uses line-local coordinates, clips away next-line continuation paths, and checks the supported integer range before rasterizing.

Mouse dragging, double-click word selection, Shift-click extension, touch long press, touch handles, Copy, and Select All belong to this feature.
Full-document insertion-caret keyboard navigation does not.
Dragging near a viewport edge requests frame-driven scrolling through the existing scroll state and resumes hit testing after new virtual placements are committed.
Cancellation, disablement, pointer release, owner removal, and exhausted scroll capacity end the corresponding activity without leaving continuous frame requests.
Runtime coordinates selection capabilities, not concrete Text, SelectionArea, or Agent component types.

### Links and semantics

TextEvents::LinkActivated carries the target Uri and its current paragraph-local TextRange.
The Text event binding owns the callback, and pointer, keyboard, and accessibility activation share that typed event.
The framework never opens a Uri automatically; scheme policy and navigation remain application-owned.
Link appearance uses existing Theme colors and focus resources, with explicit range styles taking precedence, rather than expanding ColorScheme for this feature.

Hit testing uses the actual visual range fragments, not the link's enclosing rectangle or only the nearest text offset.
Link activation and selection participate in the same pointer arbitration: drag selection, scrolling, long press, Cancel, and disabled input cannot also activate the link.
A pressed target is canceled when its text or logical link association no longer matches the committed content.
Tab and Shift-Tab visit actionable links; Enter activates the current link.
The existing NodeExtension focus-change callback receives reverse traversal intent so a composite node can enter at its last actionable item without Runtime knowing its component type.
Copy and Select All can resolve the nearest selection-capability owner when a descendant link owns keyboard focus, without bypassing an active editable text-input client.

Virtual semantic children expose plain segments and logical links once in reading order.
Visual style boundaries do not create semantic children or split a link's identity.
Disabled links publish no executable action, and author semantics retain their existing authority.
Virtualized content does not eagerly materialize every paragraph for accessibility.
Streaming text is not a per-token live announcement by default.

### Streaming display

The public-API [streaming example](../../examples/streaming_text/main.cpp) demonstrates a virtual transcript with shared completed blocks, an independently observed tail, batched publication, one latest-version highlighting task, and conditional bottom following.
Its mock transport is a scope-owned coroutine; external transport callbacks should accumulate deltas and schedule at most one pending UI delivery rather than posting one callback per token.
Messages and generation tasks are owned outside evictable virtual rows so scrolling does not cancel generation or discard text.
Applications keep stable completed blocks and publish new values only for changing blocks.
State reads are localized to the changing block's composition lifetime; a stable Key alone does not create an independent subscription boundary.
Very long messages can virtualize blocks rather than treating each complete message as one indivisible row.

Transport deltas are buffered and published at a display-appropriate cadence instead of writing State for every token.
Pending UI delivery is bounded: a slow display consumes the latest accumulated content rather than replaying an unbounded queue of obsolete snapshots, without dropping source bytes.
Completion, failure, and cancellation have explicit final-delivery behavior.
Background highlighting returns source-versioned results and discards obsolete output.

Applications preserve a stable visible block and its viewport-relative position when updating content before the viewport; tail-only updates below the viewport need no offset correction.
Bottom following continues only while the user is following the end; it stops when the user scrolls away.
Offset correction uses the existing authoritative scroll state rather than another writable offset.

The text core does not assume that only the final block can change.
Markdown reference definitions and other syntax can affect earlier content; stability decisions, dependency tracking, and block identity belong to the parser above this layer.
Code and log content can use stable natural-line groups, while ordinary paragraphs must not be arbitrarily split by character count because that changes shaping and wrapping.
Appending to a single indefinitely growing paragraph can still require complete paragraph reflow; no O(delta) guarantee is made for that case.

### Platform limits and verification

Non-Web backends provide the complete paragraph, selection, and interaction contracts through their platform text boundary; different platform font engines need not produce pixel-identical output.
Web deliberately retains the existing Canvas backend without adding a paragraph-engine dependency.
It supports attributed styles, wrapping, links, and basic selection, but does not promise parity with non-Web backends for mixed bidirectional layout, ligatures across style boundaries, or contextual shaping across separately drawn runs.
Web measurement, drawing, and selection must use the same fallback paragraph geometry, including mixed font metrics; independently positioned span views are not a substitute for this shared layout.
LF, CRLF, and CR terminate a visual line without rewriting the source text or its UTF-16 offsets.
Measurement-only layouts omit caret-stop construction, while interactive layouts reuse prefix advances already measured during wrapping.
Those advances remain shaped-prefix measurements rather than sums of independently measured character widths; complete paragraph reflow can still be expensive for a long unbroken line.
These limitations are part of the documented Web contract and must be covered by backend-specific tests rather than treated as shaping support equivalent to non-Web backends.

Validation includes public header checks, invalid text and range construction, fragment/range equivalence, source/rendered-body consistency, style and link independence, and plain/attributed compatible recomposition.
Selection tests cover fully unmounted intermediate blocks, one visible endpoint, edge scrolling, copying separators, append/replacement/removal/movement, transforms, clipping, and disabled or canceled input.
Link tests cover pointer arbitration, keyboard traversal, accessibility action identity, and stale-target rejection.
TextField and secure-input tests protect existing editing and platform-session behavior when the shared paragraph and selection boundaries change.

Deterministic counters verify that tail updates do not reshape unchanged historical paragraphs, paint-only changes do not remeasure ancestors, and unchanged selected content reuses its layouts.
Copying a cross-viewport range must produce the complete expected logical text without realizing intermediate items.
Performance investigations should measure allocation, layout creation, paint recording, cache retention, and elapsed-time distributions for short labels and long transcripts; FPS alone is not evidence of bounded work.
Visible-node comparisons, required ancestor layout work, and platform scene replay are permitted; the contract does not promise that every backend repaints only changed pixels.

Changes to this boundary require paragraph, logical selection, gesture, link, example, and affected renderer validation together.
