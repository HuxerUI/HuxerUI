#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/data.h>
#include <huxerui/event.h>
#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
struct InternalAccess;
}

/// An offset measured in UTF-16 code units, not UTF-8 bytes or grapheme clusters.
///
/// Public strings use UTF-8, but text ranges and input protocols use UTF-16 offsets. A supplementary Unicode
/// scalar occupies two units; a user-perceived character can contain several scalars. Do not use std::string::size()
/// as a text offset. Use AttributedText::Length() when the body is already an attributed value.
using TextOffset = std::int64_t;

/// Chooses the visual side of a logical offset at a line or directional boundary.
enum class TextAffinity {
  /// Attaches to the preceding text, such as the end of the previous line at a soft wrap.
  Upstream,
  /// Attaches to the following text, such as the start of the next line at a soft wrap.
  Downstream,
};

/// A logical insertion position with enough information to disambiguate its visual caret.
struct TextPosition {
  /// UTF-16 offset in the owning text.
  TextOffset offset = 0;
  /// Visual side to use when offset has more than one caret location.
  TextAffinity affinity = TextAffinity::Downstream;

  bool operator==(const TextPosition&) const = default;
};

/// An ordered, half-open range [start, end) in UTF-16 code units.
///
/// This value does not own text or validate boundaries against a body. APIs consuming a range validate its bounds
/// and reject split surrogate pairs. Use TextSelection when the direction of a selection matters.
/// @code
/// const AttributedText text("A\xF0\x9F\x98\x80" "B");
/// const std::string emoji = text.TextInRange({1, 3});
/// @endcode
struct TextRange {
  /// Inclusive starting offset.
  TextOffset start = 0;
  /// Exclusive ending offset; equal to start for an empty range.
  TextOffset end = 0;

  /// @return Whether the offsets are nonnegative and ordered; does not check text bounds or Unicode boundaries.
  [[nodiscard]] bool IsValid() const noexcept { return start >= 0 && end >= start; }
  /// @return Whether both offsets are equal, without validating the range.
  [[nodiscard]] bool IsCollapsed() const noexcept { return start == end; }
  /// @return end - start in UTF-16 units. The caller must provide a valid range.
  [[nodiscard]] TextOffset Length() const noexcept { return end - start; }

  bool operator==(const TextRange&) const = default;
};

/// Selects how a platform resolves a font family.
enum class FontFamilyKind {
  /// The platform's default proportional UI family.
  System,
  /// The platform's default fixed-width family.
  Monospace,
  /// An explicitly named family, with platform fallback when unavailable.
  Named,
};

/// Standard font-weight requests; the platform resolves the closest available face.
enum class FontWeight : std::uint16_t {
  /// Very thin strokes.
  Thin = 100,
  /// Lighter than Light.
  ExtraLight = 200,
  /// Lighter than regular body text.
  Light = 300,
  /// Normal body-text weight.
  Regular = 400,
  /// Slightly heavier than Regular.
  Medium = 500,
  /// Emphasis between Medium and Bold.
  SemiBold = 600,
  /// Bold emphasis.
  Bold = 700,
  /// Heavier than Bold.
  ExtraBold = 800,
  /// The heaviest standard weight.
  Black = 900,
};

/// Upright or italic face selection.
enum class FontSlant {
  /// Upright glyphs.
  Normal,
  /// Italic glyphs, using platform face selection or synthesis.
  Italic,
};

/// A platform-neutral font request with family, logical size, weight, and slant.
///
/// WithSize(), WithWeight(), and WithSlant() return modified values without changing the original.
/// A missing family or face may fall back on the platform; this does not change the requested Font value.
/// @code
/// const Font title = Font::Named("Inter", 28.0F).WithWeight(FontWeight::Bold);
/// const Font code = Font::Monospace(14.0F).WithSlant(FontSlant::Italic);
/// @endcode
class Font {
public:
  Font() = default;

  /// Requests the platform's default proportional UI font.
  /// @param size Finite, positive font size in logical units, not physical pixels.
  /// @return A regular, upright system font request.
  /// @throws std::invalid_argument If size is not finite and positive.
  static Font System(float size = 14.0F);
  /// Requests the platform's default fixed-width font.
  /// @param size Finite, positive font size in logical units.
  /// @return A regular, upright monospace font request.
  /// @throws std::invalid_argument If size is not finite and positive.
  static Font Monospace(float size = 14.0F);
  /// Requests a named family without loading or bundling a font asset.
  /// @param family Nonempty platform font-family name.
  /// @param size Finite, positive font size in logical units.
  /// @return A regular, upright named font request.
  /// @throws std::invalid_argument If family is empty or size is not finite and positive.
  static Font Named(std::string family, float size = 14.0F);

  /// Returns a resized request, preserving family, weight, and slant.
  /// @param size Finite, positive font size in logical units.
  /// @return The modified font value.
  /// @throws std::invalid_argument If size is not finite and positive.
  [[nodiscard]] Font WithSize(float size) const;
  /// Changes the requested weight without changing other font properties.
  /// @param weight One of the declared FontWeight values.
  /// @return The modified font value.
  [[nodiscard]] Font WithWeight(FontWeight weight) const;
  /// Changes the requested slant without changing other font properties.
  /// @param slant One of the declared FontSlant values.
  /// @return The modified font value.
  [[nodiscard]] Font WithSlant(FontSlant slant) const;

  /// @return The requested family category, not the platform's resolved fallback.
  [[nodiscard]] FontFamilyKind FamilyKind() const noexcept {
    return family_kind_;
  }

  /// @return The requested name for Named fonts, or an empty view for System and Monospace.
  /// The view remains valid only while this Font's family-name storage remains alive and unchanged.
  [[nodiscard]] std::string_view FamilyName() const noexcept {
    return family_name_;
  }

  /// @return The requested font size in logical units.
  [[nodiscard]] float Size() const noexcept {
    return size_;
  }

  /// @return The requested font weight.
  [[nodiscard]] FontWeight Weight() const noexcept {
    return weight_;
  }

  /// @return The requested upright or italic slant.
  [[nodiscard]] FontSlant Slant() const noexcept {
    return slant_;
  }

  bool operator==(const Font&) const = default;

private:
  Font(FontFamilyKind family_kind, std::string family_name, float size);

  FontFamilyKind family_kind_ = FontFamilyKind::System;
  std::string family_name_;
  float size_ = 14.0F;
  FontWeight weight_ = FontWeight::Regular;
  FontSlant slant_ = FontSlant::Normal;
};

/// Composable character-decoration flags; combine them with operator|.
enum class TextDecoration : std::uint8_t {
  /// No decoration; explicitly clears inherited decorations in TextSpanStyle.
  None = 0,
  /// Draws a line below the text using the resolved font metrics.
  Underline = 1 << 0,
  /// Draws a line through the text using the resolved font metrics.
  StrikeThrough = 1 << 1,
};

/// Combines decoration flags.
/// @param left First set of flags.
/// @param right Additional flags.
/// @return The union of both sets.
constexpr TextDecoration operator|(TextDecoration left, TextDecoration right) noexcept {
  return static_cast<TextDecoration>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

/// Intersects decoration flags.
/// @param left First set of flags.
/// @param right Flags to retain.
/// @return Flags present in both sets.
constexpr TextDecoration operator&(TextDecoration left, TextDecoration right) noexcept {
  return static_cast<TextDecoration>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
}

/// Tests whether any requested decoration flag is present.
/// @param value Flags to inspect.
/// @param decoration Flags to test; None always returns false.
/// @return True if the intersection is nonempty, not necessarily if all requested flags are present.
constexpr bool HasTextDecoration(TextDecoration value, TextDecoration decoration) noexcept {
  return (value & decoration) != TextDecoration::None;
}

/// A complete text style, separate from paragraph alignment, wrapping, and shaping.
///
/// Text::Style() replaces the complete base style; use TextSpanStyle for partial character overrides.
struct TextStyle {
  /// Complete font request used for measurement and drawing.
  Font font = Font::System();
  /// Text and decoration color.
  Color foreground = Color::Rgb(31, 35, 40);
  /// Underline and strike-through flags.
  TextDecoration decoration = TextDecoration::None;

  /// @return The text style from ThemeSpec::Default(), not the active composition's inherited Theme.
  static TextStyle Default();

  bool operator==(const TextStyle&) const = default;
};

/// Character overrides applied after the paragraph's base TextStyle.
/// A complete font is applied before size, weight, and slant overrides; omitted fields inherit.
///
/// Use a partial font property to preserve the paragraph's other font properties, rather than replacing its Font.
/// Alignment, wrapping, direction, and shaping locale remain paragraph-wide.
/// @code
/// const TextSpanStyle emphasis{.font_weight = FontWeight::Bold};
/// const TextSpanStyle plain{.decoration = TextDecoration::None};
/// @endcode
struct TextSpanStyle {
  /// Optional complete font replacement, applied before the individual font overrides.
  std::optional<Font> font{};
  /// Finite, positive logical size; preserves the effective family, weight, and slant.
  std::optional<float> font_size{};
  /// Weight override that preserves the other effective font properties.
  std::optional<FontWeight> font_weight{};
  /// Slant override that preserves the other effective font properties.
  std::optional<FontSlant> font_slant{};
  /// Text and decoration color override.
  std::optional<Color> foreground{};
  /// Background behind visual text fragments, without padding; transparent means no visible background.
  std::optional<Color> background{};
  /// Complete decoration replacement; None clears decorations rather than inheriting them.
  std::optional<TextDecoration> decoration{};

  bool operator==(const TextSpanStyle&) const = default;
};

/// Character overrides for one paragraph-local range, validated when added to AttributedText.
struct TextStyleRange {
  /// In-bounds UTF-16 range whose endpoints must not split surrogate pairs.
  TextRange range;
  /// Overrides resolved against the paragraph's final base style.
  TextSpanStyle style;

  bool operator==(const TextStyleRange&) const = default;
};

/// A logical link independent of visual style boundaries. Adjacent links remain distinct.
struct TextLinkRange {
  /// In-bounds paragraph-local UTF-16 range; may span several visual style ranges.
  TextRange range;
  /// Application-owned navigation target, never opened automatically by Text.
  Uri target;

  bool operator==(const TextLinkRange&) const = default;
};

/// A direct UTF-8 construction fragment; localization is resolved before creating fragments.
class TextSpan final {
public:
  explicit TextSpan(std::string text);

  /// Replaces this fragment's character overrides.
  /// @param style Overrides resolved against the containing paragraph's base style.
  /// @return The modified fragment for further chaining or AttributedText construction.
  /// @throws std::invalid_argument If a supplied attribute is invalid.
  [[nodiscard]] TextSpan Style(TextSpanStyle style) &&;
  /// Associates this fragment with one link; it does not open the target automatically.
  /// @param target Application-owned navigation target.
  /// @return The modified fragment; a previous link target is replaced.
  [[nodiscard]] TextSpan Link(Uri target) &&;

private:
  friend class AttributedText;
  std::string text_;
  TextSpanStyle style_;
  std::optional<Uri> target_;
};

/// An immutable paragraph value with shared text storage and independent style and link ranges.
/// Copies share storage. Replacing styles shares the existing UTF-8 body. Text must be valid UTF-8; every range is
/// paragraph-local UTF-16 and must be in bounds without splitting a surrogate pair. Styles and links are ordered
/// and non-overlapping within their own lists, but may cross each other's boundaries. Invalid input throws
/// std::invalid_argument. Range validation does not require whole grapheme clusters.
///
/// A paragraph may contain explicit line breaks. This value is not a markup parser, editor, or collection of Views.
/// @code
/// const AttributedText text{
///     TextSpan("Build "),
///     TextSpan("completed").Style({.font_weight = FontWeight::Bold}),
///     TextSpan(". View report").Link(Uri("https://example.com/report")),
/// };
/// @endcode
class AttributedText final {
public:
  AttributedText();
  explicit AttributedText(std::string text);
  AttributedText(std::initializer_list<TextSpan> spans);
  explicit AttributedText(std::span<const TextSpan> spans);

  /// Adds attributes to an already assembled body without fragment concatenation.
  /// @param text Valid UTF-8 body.
  /// @param styles Ordered, non-overlapping UTF-16 ranges. Adjacent equal styles are merged.
  /// @param links Ordered, non-overlapping UTF-16 link ranges, independent of styles.
  /// @return An immutable value with normalized styles and distinct logical links; empty ranges are omitted.
  /// @throws std::invalid_argument For invalid UTF-8, attributes, or ranges, including split surrogate pairs.
  /// @code
  /// const auto message = AttributedText::FromRanges("Build completed",
  ///     {TextStyleRange{.range = {6, 15}, .style = {.font_weight = FontWeight::Bold}}});
  /// @endcode
  [[nodiscard]] static AttributedText
  FromRanges(std::string text, std::vector<TextStyleRange> styles, std::vector<TextLinkRange> links = {});

  /// Replaces visual attributes while sharing the body and preserving logical links.
  /// @param styles Ordered, non-overlapping UTF-16 style ranges; an empty list removes all explicit styles.
  /// @return A new value preserving text and links; this value is unchanged.
  /// @throws std::invalid_argument For invalid attributes or ranges.
  [[nodiscard]] AttributedText WithStyles(std::vector<TextStyleRange> styles) const;

  /// @return The resolved UTF-8 body, not markup. The reference borrows this value's shared text storage.
  /// Keep an owning AttributedText alive while using the reference; do not borrow from a temporary.
  [[nodiscard]] const std::string& PlainText() const noexcept;
  /// @return The cached UTF-16 length, not the UTF-8 byte length.
  [[nodiscard]] TextOffset Length() const noexcept;
  /// @return Normalized visual ranges; gaps inherit the paragraph's base style.
  /// The span borrows this value's shared attribute storage and does not retain it.
  [[nodiscard]] std::span<const TextStyleRange> StyleRanges() const noexcept;
  /// @return Logical links, without merging adjacent equal targets.
  /// The span borrows this value's shared attribute storage and does not retain it.
  [[nodiscard]] std::span<const TextLinkRange> LinkRanges() const noexcept;
  /// Copies a UTF-16 range without rescanning the body from its beginning.
  /// @param range In-bounds range whose endpoints do not split surrogate pairs.
  /// @return An owning UTF-8 substring, or an empty string for a collapsed range.
  /// @throws std::invalid_argument For invalid range boundaries.
  [[nodiscard]] std::string TextInRange(TextRange range) const;

  [[nodiscard]] bool operator==(const AttributedText& other) const noexcept;

private:
  friend struct detail::InternalAccess;
  struct Storage;
  explicit AttributedText(std::shared_ptr<const Storage> storage);
  std::shared_ptr<const Storage> storage_;
};

/// Stable logical identity within one SelectionArea; independent of a View's sibling Key.
///
/// IDs must be unique within a source snapshot and stay attached to the same logical block across updates and moves.
/// Zero is a valid ID; do not use a changing list index as an identity when blocks can be inserted or reordered.
using TextBlockId = std::uint64_t;

/// Immutable content supplied by a logical selection source.
struct TextSelectionBlock {
  /// Complete paragraph whose plain body matches the corresponding Text declaration.
  /// Visual styles may differ, but the declaration must not bind only a substring of this body.
  AttributedText text;
  /// Valid UTF-8 inserted after this block only when selection continues into another block.
  /// Copy never appends the final selected block's separator; an empty string joins blocks without a delimiter.
  std::string separator = "\n";
};

/// A read-only document snapshot whose text remains available when Views are virtualized away.
///
/// Publish replacement snapshots through application State. Keep unchanged blocks and lookup indexes shared;
/// a tail update should not rebuild the complete document. Methods must not mutate the snapshot, create Views,
/// fetch data, parse markup, or depend on mounted nodes. Each block maps to one complete Text declaration.
///
/// IdAt() and IndexOf() must agree, with a unique stable ID per block. Bind mounted Text nodes with
/// Text::SelectionBlock() and supply the same snapshot to SelectionArea::Source(). A block can have at most one
/// mounted binding in its area. Explicit-source mode excludes unbound decorative Text from selection.
///
/// Stable IDs preserve endpoints across moves. Removing an endpoint block clears the selection; appending text does
/// not automatically extend an existing selection. Offscreen blocks remain available for Copy without mounting Views.
/// With a shared immutable snapshot and the View APIs from `<huxerui/huxerui.h>`:
/// @code
/// View Document(std::shared_ptr<const TextSelectionSource> snapshot) {
///   return SelectionArea(
///       VirtualList(snapshot->Count(), [snapshot](std::size_t index) {
///         const auto id = snapshot->IdAt(index);
///         return Text(snapshot->BlockAt(index).text).SelectionBlock(id).Key(id);
///       })
///   ).Source(snapshot);
/// }
/// @endcode
class TextSelectionSource {
public:
  virtual ~TextSelectionSource() = default;

  /// @return The number of logical blocks, including blocks outside the viewport.
  [[nodiscard]] virtual std::size_t Count() const noexcept = 0;
  /// Returns the stable, unique identity at a logical index.
  /// @param index Index smaller than Count().
  /// @return The block's ID, whose IndexOf() lookup must return index.
  [[nodiscard]] virtual TextBlockId IdAt(std::size_t index) const = 0;
  /// Looks up a stable identity without scanning all block contents.
  /// @param id Identity within this selection owner.
  /// @return Current logical index, or no value if the ID is not present in this snapshot.
  [[nodiscard]] virtual std::optional<std::size_t> IndexOf(TextBlockId id) const = 0;
  /// Returns an inexpensive shared paragraph snapshot and its following separator.
  /// @param index Index smaller than Count().
  /// @return The immutable block content and separator, independent of its mounted state.
  [[nodiscard]] virtual TextSelectionBlock BlockAt(std::size_t index) const = 0;
};

/// Paragraph base direction used by shaping and semantic leading/trailing alignment.
enum class TextDirection {
  /// Uses the first strong directional character, or left-to-right if none exists.
  Auto,
  /// Requests a left-to-right base direction without disabling bidirectional shaping.
  LeftToRight,
  /// Requests a right-to-left base direction without reversing the source text.
  RightToLeft,
};

/// Paragraph-wide shaping hints, separate from font choice and visual style.
struct TextShapingOptions {
  /// Base direction for shaping and horizontal alignment.
  TextDirection direction = TextDirection::Auto;
  /// Language tag such as "en-US" or "ar" used for shaping, not localized resource lookup.
  /// Empty uses inherited Locale in mounted Text, TextField, and Canvas; TextMeasurer does not read Environment.
  std::string locale{};

  bool operator==(const TextShapingOptions&) const = default;
};

/// Horizontal placement of each line within the available paragraph width.
enum class TextAlign {
  /// Left in a left-to-right paragraph, right in a right-to-left paragraph.
  Leading,
  /// Centers each line.
  Center,
  /// Right in a left-to-right paragraph, left in a right-to-left paragraph.
  Trailing,
};

/// Vertical placement of the complete paragraph when its final rectangle has unused height.
///
/// This does not change intrinsic measurement. Overflowing paragraphs retain a top origin.
enum class TextVerticalAlign {
  /// Places the paragraph at the top.
  Top,
  /// Centers the paragraph in the available extra height.
  Center,
  /// Places the paragraph at the bottom using the available extra height.
  Bottom,
};

/// Controls automatic line breaking; both modes preserve explicit source line breaks.
enum class TextWrap {
  /// Disables automatic wrapping, not explicit line breaks or overflow clipping.
  NoWrap,
  /// Allows the platform paragraph layout to wrap at a finite width.
  Word,
};

/// Paragraph shaping and placement options shared by measurement and drawing.
///
/// Pass equivalent options and base styles to both operations to keep geometry consistent.
struct TextLayoutOptions {
  /// Paragraph direction and shaping locale.
  TextShapingOptions shaping{};
  /// Horizontal placement of each line.
  TextAlign align = TextAlign::Leading;
  /// Vertical placement inside the final drawing rectangle, without changing intrinsic height.
  TextVerticalAlign vertical_align = TextVerticalAlign::Top;
  /// Automatic wrapping policy in addition to explicit source line breaks.
  TextWrap wrap = TextWrap::Word;

  bool operator==(const TextLayoutOptions&) const = default;
};

/// Resolved font distances in logical units, independent of a particular string.
///
/// With a downward-positive drawing axis, the ascent lies above the baseline and the descent below it.
struct FontMetrics {
  /// Distance from the baseline to the font's upper extent.
  float ascent = 0.0F;
  /// Distance from the baseline to the font's lower extent.
  float descent = 0.0F;
  /// Additional line spacing supplied by the font.
  float leading = 0.0F;
  /// Underline offset below the baseline; draw at baseline_y + underline_position.
  float underline_position = 0.0F;
  /// Recommended underline thickness.
  float underline_thickness = 0.0F;
  /// Strike-through offset above the baseline; draw at baseline_y - strike_through_position.
  float strike_through_position = 0.0F;
  /// Recommended strike-through thickness.
  float strike_through_thickness = 0.0F;

  /// @return ascent + descent + leading, not the measured height of a particular paragraph.
  [[nodiscard]] float LineHeight() const noexcept {
    return ascent + descent + leading;
  }

  bool operator==(const FontMetrics&) const = default;
};

/// Geometry of one unwrapped shaped run in logical units, positioned relative to its baseline.
struct TextRunMetrics {
  /// Horizontal layout advance; not necessarily the width of painted glyph bounds.
  float advance = 0.0F;
  /// Conservative painted bounds with baseline origin (0, 0); ascent extends into negative y coordinates.
  Rect visual_bounds;
  /// Resolved font metrics used by the run.
  FontMetrics font_metrics;

  bool operator==(const TextRunMetrics&) const = default;
};

/// Intrinsic paragraph geometry in logical units, not per-line hit-testing or selection geometry.
struct TextLayoutMetrics {
  /// Measured paragraph extent; a finite maximum width constrains the reported width even in NoWrap mode.
  Size size;
  /// First line's baseline offset from the paragraph's top, before external vertical placement.
  float first_baseline = 0.0F;
  /// Last line's baseline offset from the paragraph's top, before external vertical placement.
  float last_baseline = 0.0F;
  /// Number of measured lines, including explicit line breaks and automatic wraps.
  std::size_t line_count = 0;

  bool operator==(const TextLayoutMetrics&) const = default;
};

/// Identifies a link requested by pointer, keyboard, or accessibility activation.
/// Applications own URI scheme policy and navigation; HuxerUI does not open the target automatically.
struct TextLinkActivation {
  /// The link target from the current paragraph snapshot.
  Uri target;
  /// The activated paragraph-local half-open UTF-16 range.
  TextRange range;
};

/// Typed events emitted by Text nodes containing attributed links.
struct TextEvents {
  /// Requests activation of a link in the current paragraph.
  /// The handler receives a TextLinkActivation valid for the callback; copy it if needed afterward.
  /// In this example, content is an AttributedText and Navigate is application-provided navigation:
  /// @code
  /// Text(content).On<TextEvents::LinkActivated>([](const TextLinkActivation& link) {
  ///   Navigate(link.target);
  /// });
  /// @endcode
  struct LinkActivated : Event<void(const TextLinkActivation&)> {};
};

/// Synchronous platform text measurement shared by components, layout, and custom drawing.
///
/// Calls run on the Runtime's host thread. Retain returned value metrics, not the service reference, beyond the
/// current composition, layout, or geometry-preparation operation. Platform font engines may differ in metrics.
/// Web's Canvas backend does not promise mixed bidirectional or cross-run shaping equivalent to non-Web backends.
/// Measurement reads only the supplied shaping options, not the active Environment.
class TextMeasurer {
public:
  virtual ~TextMeasurer() = default;

  /// Resolves font-level distances without measuring a string.
  /// @param font Platform-neutral font request.
  /// @return Metrics in logical units for the resolved font.
  [[nodiscard]] virtual FontMetrics Metrics(const Font& font) = 0;
  /// Shapes one exact run without paragraph wrapping; the caller owns line breaking and baseline placement.
  /// @param text Valid UTF-8 containing no line breaks.
  /// @param style Complete font, foreground, and decoration style.
  /// @param options Explicit direction and locale hints.
  /// @return Advance and conservative painted bounds relative to baseline (0, 0), plus font metrics.
  [[nodiscard]] virtual TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) = 0;
  /// Measures a complete attributed paragraph, including explicit breaks and optional automatic wrapping.
  /// @param text Immutable paragraph with UTF-16 character ranges.
  /// @param style Base style inherited by character attributes that are not overridden.
  /// @param max_width Maximum width in logical units, or positive infinity for unconstrained width.
  /// @param options Paragraph shaping, alignment, and wrapping; vertical alignment does not affect intrinsic size.
  /// @return Paragraph size, first and last baselines, and line count.
  /// A finite width constrains the reported width even with NoWrap; it does not shorten the source text.
  [[nodiscard]] virtual TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style,
      float max_width = std::numeric_limits<float>::infinity(), const TextLayoutOptions& options = {}) = 0;

  /// Measures plain text through the same paragraph path as AttributedText.
  /// @param text Valid UTF-8 body, possibly containing explicit line breaks.
  /// @param style Complete base style.
  /// @param max_width Maximum width in logical units, or positive infinity for unconstrained width.
  /// @param options Paragraph shaping, alignment, and wrapping options.
  /// @return Paragraph size, baseline offsets, and line count.
  /// @throws std::invalid_argument If text is not valid UTF-8.
  [[nodiscard]] TextLayoutMetrics MeasureText(std::string_view text, const TextStyle& style,
      float max_width = std::numeric_limits<float>::infinity(), const TextLayoutOptions& options = {}) {
    return MeasureText(AttributedText(std::string(text)), style, max_width, options);
  }
};

/// Obtains the current Runtime's text measurer during composition.
///
/// Use this from an application root, composable component, or a custom hook called by one.
/// Do not retain the returned reference in State or an asynchronous callback; keep the returned metrics instead.
/// @return The active host's synchronous text-measurement service.
/// @throws std::logic_error If no active service is available or the service is disconnected.
/// @code
/// TextLayoutMetrics UseLabelMetrics(std::string_view label) {
///   return UseTextMeasurer().MeasureText(label, TextStyle::Default(), 240.0F);
/// }
/// @endcode
TextMeasurer& UseTextMeasurer();

} // namespace huxerui
