#include <huxerui/huxerui.h>

#include <custom_editor_resources.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace huxerui;

namespace {

constexpr float inset = 24.0F;
constexpr float paragraph_gap = 14.0F;
constexpr float illustration_height = 78.0F;
constexpr Color ink = Color::Rgb(29, 40, 61);
constexpr Color accent = Color::Rgb(37, 89, 176);
constexpr Color code_background = Color::Rgb(232, 238, 246);
constexpr Color surface_border = Color::Rgb(218, 227, 239);

struct StyledRange {
  TextRange range;
  TextSpanStyle style;
  bool inline_code = false;
  bool operator==(const StyledRange&) const = default;
};

struct StyledText {
  AttributedText body;
  std::vector<StyledRange> styles{};
  bool operator==(const StyledText&) const = default;
};

StyledText StarterText() {
  static const StyledText text = [] {
    const TextSpanStyle emphasis{.font_weight = FontWeight::Bold, .foreground = accent};
    const AttributedText attributed{
        TextSpan("A small document studio").Style({.font_weight = FontWeight::Bold}),
        TextSpan("\nThis "),
        TextSpan("HuxerUI").Style(emphasis),
        TextSpan(" editor uses one "),
        TextSpan("IME").Style(emphasis),
        TextSpan(" session for the whole document. Drag across paragraphs to select and format text. "
                 "Press Enter to split a paragraph or Backspace at its start to join it to the previous one.\n"
                 "Try composing 日本語 or 中文, then select some text with Shift and the arrow keys. Each "),
        TextSpan("TextLayout").Style({.font = Font::Monospace()}),
        TextSpan(" positions the text, caret and selection. 🙂"),
    };
    StyledText result{AttributedText(attributed.PlainText())};
    for (const auto& run : attributed.StyleRanges()) {
      auto style = run.style;
      style.font.reset();
      result.styles.push_back({run.range, std::move(style), run.style.font.has_value()});
    }
    return result;
  }();
  return text;
}

struct CompositionOrigin {
  TextRange range;
  StyledText text;
  TextSelection selection;
};

struct Document {
  StyledText text = StarterText();
  TextSelection selection;
  std::optional<TextRange> composition;
  std::optional<CompositionOrigin> origin;
};

auto FirstStyleEndingAfter(const StyledText& text, TextOffset offset) {
  return std::ranges::upper_bound(text.styles, offset, {}, [](const StyledRange& run) { return run.range.end; });
}

void AppendStyle(std::vector<StyledRange>& styles, StyledRange run) {
  if (!run.inline_code && run.style == TextSpanStyle{}) {
    return;
  }
  if (!styles.empty() && styles.back().range.end == run.range.start &&
      styles.back().inline_code == run.inline_code && styles.back().style == run.style) {
    styles.back().range.end = run.range.end;
  } else {
    styles.push_back(std::move(run));
  }
}

// Document style ranges use UTF-16 offsets. Clip and rebase them when laying out or moving a slice.
void AppendStyles(std::vector<StyledRange>& styles, const StyledText& text, TextRange source,
                  TextOffset destination) {
  if (source.IsCollapsed()) {
    return;
  }
  for (auto run = FirstStyleEndingAfter(text, source.start);
       run != text.styles.end() && run->range.start < source.end; ++run) {
    const TextOffset start = std::max(run->range.start, source.start);
    const TextOffset finish = std::min(run->range.end, source.end);
    AppendStyle(styles, {{destination + (start - source.start), destination + (finish - source.start)},
                        run->style, run->inline_code});
  }
}

void Replace(StyledText& text, TextRange range, const StyledText& replacement) {
  if (!range.IsValid()) {
    throw std::invalid_argument("HuxerUI example edit range must be ordered");
  }
  std::string result = text.body.TextInRange({0, range.start});
  result += replacement.body.PlainText();
  result += text.body.TextInRange({range.end, text.body.Length()});

  std::vector<StyledRange> styles;
  AppendStyles(styles, text, {0, range.start}, 0);
  AppendStyles(styles, replacement, {0, replacement.body.Length()}, range.start);
  AppendStyles(styles, text, {range.end, text.body.Length()}, range.start + replacement.body.Length());
  text = {AttributedText(std::move(result)), std::move(styles)};
}

TextOffset ReplaceInput(StyledText& text, TextRange range, const std::string& replacement) {
  StyledText inserted{AttributedText(replacement)};
  if (text.body.TextInRange(range) != replacement) {
    const auto run = FirstStyleEndingAfter(text, range.start);
    if (inserted.body.Length() > 0 && run != text.styles.end() && run->range.start <= range.start) {
      inserted.styles.push_back({{0, inserted.body.Length()}, run->style, run->inline_code});
    }
    Replace(text, range, inserted);
  }
  return inserted.body.Length();
}

void ValidateSelection(const AttributedText& text, TextSelection selection) {
  std::ignore = text.TextInRange(selection.Range());
}

void BeginComposition(Document& document, TextRange range) {
  StyledText original{AttributedText(document.text.body.TextInRange(range))};
  AppendStyles(original.styles, document.text, range, 0);
  document.origin = CompositionOrigin{range, std::move(original), document.selection};
  document.composition = range;
}

void FinishComposition(Document& document) {
  document.composition.reset();
  document.origin.reset();
}

void CancelComposition(Document& document) {
  if (document.origin) {
    Replace(document.text, *document.composition, document.origin->text);
    document.selection = document.origin->selection;
    FinishComposition(document);
  }
}

TextOffset OffsetAfterDeletion(TextOffset offset, TextRange deletion) {
  return offset <= deletion.start ? offset : std::max(deletion.start, offset - deletion.Length());
}

bool DeleteRange(Document& document, TextRange deletion) {
  if (deletion.IsCollapsed()) {
    return true;
  }
  if (document.composition) {
    auto& composition = *document.composition;
    auto& origin = *document.origin;
    if (deletion.end <= composition.start || deletion.start >= composition.end) {
      const TextOffset shift = deletion.end <= composition.start ? 0 : origin.range.end - composition.end;
      const TextRange original{deletion.start + shift, deletion.end + shift};
      origin.range = {OffsetAfterDeletion(origin.range.start, original),
                      OffsetAfterDeletion(origin.range.end, original)};
      origin.selection.anchor = OffsetAfterDeletion(origin.selection.anchor, original);
      origin.selection.active = OffsetAfterDeletion(origin.selection.active, original);
    } else if (deletion.start < composition.start || deletion.end > composition.end) {
      return false;
    }
    composition = {OffsetAfterDeletion(composition.start, deletion), OffsetAfterDeletion(composition.end, deletion)};
  }
  Replace(document.text, deletion, {});
  document.selection.anchor = OffsetAfterDeletion(document.selection.anchor, deletion);
  document.selection.active = OffsetAfterDeletion(document.selection.active, deletion);
  return true;
}

bool DeleteSurrounding(Document& document, const TextInputCommand& command) {
  if (command.delete_before < 0 || command.delete_after < 0) {
    return false;
  }
  const TextRange selected = document.selection.Range();
  const TextOffset length = document.text.body.Length();
  TextOffset first = selected.start - std::min(selected.start, command.delete_before);
  TextOffset last = selected.end + std::min(length - selected.end, command.delete_after);
  if (command.delete_unit == TextInputUnit::UnicodeCodePoint) {
    std::vector<TextOffset> boundaries{0};
    for (unsigned char byte : document.text.body.PlainText()) {
      if ((byte & 0xC0U) != 0x80U) {
        boundaries.push_back(boundaries.back() + (byte >= 0xF0U ? 2 : 1));
      }
    }
    const auto before = std::lower_bound(boundaries.begin(), boundaries.end(), selected.start);
    const auto after = std::lower_bound(boundaries.begin(), boundaries.end(), selected.end);
    first = *std::prev(before, std::min<TextOffset>(before - boundaries.begin(), command.delete_before));
    last = *std::next(after, std::min<TextOffset>(boundaries.end() - after - 1, command.delete_after));
  }
  return DeleteRange(document, {selected.end, last}) && DeleteRange(document, {first, selected.start});
}

bool Apply(Document& document, const TextInputCommand& command) {
  const auto kind = command.kind;
  if (kind != TextInputCommandKind::DeleteSurrounding &&
      (command.delete_before || command.delete_after || command.delete_unit != TextInputUnit::Utf16CodeUnit)) {
    return false;
  }
  if (kind != TextInputCommandKind::UpdateComposition && command.coordinate_space != TextInputCoordinateSpace::Text) {
    return false;
  }
  const bool replacement = kind == TextInputCommandKind::CommitText || kind == TextInputCommandKind::UpdateComposition;
  if (!replacement && (!command.text.empty() || (command.target && kind != TextInputCommandKind::BeginComposition) ||
                       (command.selection_after && kind != TextInputCommandKind::SetSelection))) {
    return false;
  }
  if (replacement) {
    TextRange target = command.target.value_or(document.composition.value_or(document.selection.Range()));
    if (document.composition && command.target) {
      if (kind != TextInputCommandKind::UpdateComposition ||
          command.coordinate_space != TextInputCoordinateSpace::Composition || !target.IsValid() ||
          target.end > document.composition->Length()) {
        return false;
      }
      target = {target.start + document.composition->start, target.end + document.composition->start};
    } else if (command.coordinate_space != TextInputCoordinateSpace::Text) {
      return false;
    }
    if (kind == TextInputCommandKind::UpdateComposition && !document.composition) {
      BeginComposition(document, target);
    }
    const TextOffset inserted = ReplaceInput(document.text, target, command.text);
    TextOffset caret = target.start + inserted;
    if (kind == TextInputCommandKind::UpdateComposition) {
      document.composition->end += inserted - target.Length();
      caret = document.composition->end;
    } else {
      FinishComposition(document);
    }
    document.selection = command.selection_after.value_or(TextSelection{caret, caret});
  } else {
    switch (kind) {
    case TextInputCommandKind::SetSelection:
      if (!command.selection_after) {
        return false;
      }
      document.selection = *command.selection_after;
      break;
    case TextInputCommandKind::BeginComposition:
      if (!command.target || document.composition) {
        return false;
      }
      BeginComposition(document, *command.target);
      break;
    case TextInputCommandKind::FinishComposition:
      FinishComposition(document);
      break;
    case TextInputCommandKind::CancelComposition:
      CancelComposition(document);
      break;
    case TextInputCommandKind::DeleteSurrounding:
      if (!DeleteSurrounding(document, command)) {
        return false;
      }
      break;
    default:
      return false;
    }
  }
  ValidateSelection(document.text.body, document.selection);
  return true;
}

struct CodeBackground {
  Rect bounds;
  Color color;
};

struct Paragraph {
  TextOffset offset;
  TextOffset length;
  Point origin;
  TextLayout layout;
  std::vector<CodeBackground> code_backgrounds;

  float Bottom() const { return origin.y + layout.Metrics().size.height; }
};

struct EditorLayoutInputs {
  std::uint64_t document_revision = 0;
  float width_limit = std::numeric_limits<float>::infinity();
  bool operator==(const EditorLayoutInputs&) const = default;
};

struct EditorLayout {
  std::vector<Paragraph> paragraphs;
  Rect illustration;
  float preferred_width;
  float height;
  std::uint64_t document_revision;
};

struct SelectionFormat {
  bool has_text = false;
  bool bold = false;
  bool italic = false;
  bool inline_code = false;
  TextDecoration decoration = TextDecoration::None;
};

Rect OffsetRect(Rect rect, Point point) {
  rect.x += point.x;
  rect.y += point.y;
  return rect;
}

class EditorClient final : public TextInputClient {
public:
  explicit EditorClient(huxerui::State<std::uint64_t> changed) : changed_(std::move(changed)) {}

  TextInputConfiguration Configuration() const override { return {.multiline = true}; }

  TextInputState State() const override {
    return {session_, revision_, content_revision_, document_.selection, document_.composition};
  }

  TextInputState BeginTextInput(TextInputSessionId session) override {
    session_ = session;
    return State();
  }

  void EndTextInput(TextInputSessionId session, TextInputEndReason) override {
    if (session == session_) {
      FinishEditingComposition();
      session_ = 0;
    }
  }

  TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) override {
    if (batch.session_id != session_ || session_ == 0) {
      return {TextInputResultCode::SessionMismatch};
    }
    if (batch.commands.empty()) {
      return {TextInputResultCode::Rejected};
    }
    Document next = document_;
    try {
      for (const TextInputCommand& command : batch.commands) {
        if (!Apply(next, command)) {
          return {TextInputResultCode::Rejected};
        }
      }
      if (!next.composition) {
        Remember(document_, next);
      }
    } catch (const std::invalid_argument&) {
      return {TextInputResultCode::Rejected};
    }
    Publish(std::move(next));
    return {TextInputResultCode::Ok};
  }

  TextInputContext QueryTextInputContext(
      TextInputSessionId session, TextOffset start, TextOffset length) const override {
    if (session != session_ || session_ == 0) {
      return {TextInputResultCode::SessionMismatch, session};
    }
    if (start < 0 || length < 0) {
      return {TextInputResultCode::Rejected, session};
    }
    return {TextInputResultCode::Ok, session, 0, document_.text.body.Length(), document_.text.body.PlainText(),
            document_.selection, document_.composition};
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session, TextRange range) const override {
    if (session != session_ || session_ == 0) {
      return {TextInputResultCode::SessionMismatch, session, {}, {}};
    }
    if (!HasGeometry()) {
      return {TextInputResultCode::Rejected, session, {}, {}};
    }
    try {
      std::ignore = document_.text.body.TextInRange(range);
      const auto affinity = range.end == document_.selection.active ? document_.selection.affinity
                                                                    : TextAffinity::Downstream;
      return {TextInputResultCode::Ok, session, Caret({range.end, affinity}), RangeRects(range)};
    } catch (const std::invalid_argument&) {
      return {TextInputResultCode::Rejected, session, {}, {}};
    }
  }

  TextInputPositionResult QueryTextInputPosition(TextInputSessionId session, Point point) const override {
    if (session != session_ || session_ == 0) {
      return {TextInputResultCode::SessionMismatch, session};
    }
    if (!HasGeometry() || !std::isfinite(point.x) || !std::isfinite(point.y)) {
      return {TextInputResultCode::Rejected, session};
    }
    return {TextInputResultCode::Ok, session, Position(point)};
  }

  TextInputKeyResult HandleTextKey(const KeyEvent& event) override {
    if (event.type != KeyEventType::Down) {
      return TextInputKeyResult::Unhandled;
    }
    if (event.modifiers.control || event.modifiers.meta) {
      if (event.key == Key::A) {
        SelectAll();
      } else if (event.key == Key::Z) {
        if (event.modifiers.shift) {
          Redo();
        } else {
          Undo();
        }
      } else if (event.key == Key::Y && event.modifiers.control && !event.modifiers.shift) {
        Redo();
      } else if (event.key == Key::B && !event.modifiers.alt && !event.modifiers.shift) {
        if (!event.repeat) {
          ToggleBold();
        }
      } else {
        return TextInputKeyResult::Unhandled;
      }
      return TextInputKeyResult::Handled;
    }
    if (event.modifiers.alt) {
      return TextInputKeyResult::Unhandled;
    }

    const TextSelection selection = document_.selection;
    TextPosition caret{selection.active, selection.affinity};
    if (HasGeometry()) {
      const Paragraph& paragraph = ParagraphAt(caret.offset);
      const TextPosition local{caret.offset - paragraph.offset, caret.affinity};
      const auto lines = paragraph.layout.Lines();
      const std::size_t line = paragraph.layout.LineIndex(local);
      const Rect rect = Caret(caret);
      switch (event.key) {
      case Key::ArrowLeft:
      case Key::ArrowRight:
        caret = {Adjacent(caret.offset, event.key == Key::ArrowRight), TextAffinity::Downstream};
        break;
      case Key::Home:
        caret = {paragraph.offset + lines[line].range.start, TextAffinity::Downstream};
        break;
      case Key::End:
        caret = {paragraph.offset + lines[line].range.end, TextAffinity::Upstream};
        break;
      case Key::ArrowUp:
      case Key::ArrowDown:
        caret = VerticalPosition(paragraph, line, rect.x, event.key == Key::ArrowDown);
        break;
      default:
        return ApplyKey(event);
      }
      Select(caret, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    }
    return ApplyKey(event);
  }

  void Select(TextPosition caret, bool extend) {
    auto next = document_;
    FinishComposition(next);
    next.selection = {extend ? next.selection.anchor : caret.offset, caret.offset, caret.affinity};
    Remember(document_, next);
    Publish(std::move(next));
  }

  void SelectAll() {
    auto next = document_;
    FinishComposition(next);
    next.selection = {0, next.text.body.Length()};
    Remember(document_, next);
    Publish(std::move(next));
  }

  void ToggleBold() {
    FormatSelection([](std::span<StyledRange> runs, SelectionFormat current) {
      const auto weight = current.bold ? std::optional<FontWeight>{} : std::optional{FontWeight::Bold};
      for (auto& run : runs) {
        run.style.font_weight = weight;
      }
    });
  }

  void ToggleItalic() {
    FormatSelection([](std::span<StyledRange> runs, SelectionFormat current) {
      const auto slant = current.italic ? std::optional<FontSlant>{} : std::optional{FontSlant::Italic};
      for (auto& run : runs) {
        run.style.font_slant = slant;
      }
    });
  }

  void ToggleDecoration(TextDecoration decoration) {
    FormatSelection([decoration](std::span<StyledRange> runs, SelectionFormat current) {
      const bool remove = HasTextDecoration(current.decoration, decoration);
      const auto keep = static_cast<TextDecoration>(~static_cast<std::uint8_t>(decoration));
      for (auto& run : runs) {
        const auto previous = run.style.decoration.value_or(TextDecoration::None);
        const auto next = remove ? previous & keep : previous | decoration;
        run.style.decoration = next == TextDecoration::None ? std::nullopt : std::optional{next};
      }
    });
  }

  void SetFontSize(std::optional<float> size) {
    FormatSelection([size](std::span<StyledRange> runs, SelectionFormat) {
      for (auto& run : runs) {
        run.style.font_size = size;
      }
    });
  }

  void SetTextColor(std::optional<Color> color) {
    FormatSelection([color](std::span<StyledRange> runs, SelectionFormat) {
      for (auto& run : runs) {
        run.style.foreground = color;
      }
    });
  }

  void SetTextBackground(std::optional<Color> color) {
    FormatSelection([color](std::span<StyledRange> runs, SelectionFormat) {
      for (auto& run : runs) {
        run.style.background = color;
      }
    });
  }

  void ToggleInlineCode() {
    FormatSelection([](std::span<StyledRange> runs, SelectionFormat current) {
      for (auto& run : runs) {
        run.inline_code = !current.inline_code;
      }
    });
  }

  void ClearFormatting() {
    FormatSelection([](std::span<StyledRange> runs, SelectionFormat) {
      for (auto& run : runs) {
        run.style = {};
        run.inline_code = false;
      }
    });
  }

  SelectionFormat SelectedFormat() const { return DescribeSelection(SelectionStyles()); }

  bool CanUndo() const { return document_.composition.has_value() || !undo_.empty(); }

  bool CanRedo() const { return !document_.composition && !redo_.empty(); }

  void Undo() {
    FinishEditingComposition();
    Restore(undo_, redo_);
  }

  void Redo() {
    if (!document_.composition) {
      Restore(redo_, undo_);
    }
  }

  void Reset() {
    Document next;
    Remember(document_, next);
    Publish(std::move(next));
  }

  bool HasGeometry() const {
    return layout_ && layout_->document_revision == layout_inputs_.Get().document_revision;
  }

  TextPosition Position(Point point) const {
    const auto& paragraphs = layout_->paragraphs;
    auto paragraph = paragraphs.begin();
    for (auto next = std::next(paragraph); next != paragraphs.end(); ++next) {
      if (point.y < (paragraph->Bottom() + next->origin.y) * 0.5F) {
        break;
      }
      paragraph = next;
    }
    return Position(*paragraph, point);
  }

  std::shared_ptr<const EditorLayout> BuildLayout(TextMeasurer& measurer, float preferred_width) const {
    const auto& inputs = layout_inputs_.Get();
    const float width = std::max(1.0F, std::min(preferred_width, inputs.width_limit) - 2.0F * inset);
    auto result = std::make_shared<EditorLayout>();
    result->preferred_width = preferred_width;
    result->document_revision = inputs.document_revision;
    const std::string_view text = document_.text.body.PlainText();
    TextOffset offset = 0;
    float y = inset;
    for (std::size_t begin = 0;;) {
      const std::size_t end = text.find('\n', begin);
      const AttributedText paragraph(std::string(text.substr(begin, end == text.npos ? text.npos : end - begin)));
      std::vector<StyledRange> runs;
      AppendStyles(runs, document_.text, {offset, offset + paragraph.Length()}, 0);
      const bool title = begin == 0;
      const TextStyle base = Style(title);
      std::vector<TextStyleRange> styles;
      std::vector<std::pair<TextRange, Color>> code_ranges;
      for (auto& run : runs) {
        if (run.inline_code) {
          // Resolve the complete font here so the editable run keeps its individual overrides.
          run.style.font = Font::Monospace(base.font.Size());
          const Color color = run.style.background.value_or(code_background);
          run.style.background.reset();
          if (!code_ranges.empty() && code_ranges.back().first.end == run.range.start &&
              code_ranges.back().second == color) {
            code_ranges.back().first.end = run.range.end;
          } else {
            code_ranges.emplace_back(run.range, color);
          }
        }
        styles.push_back({run.range, std::move(run.style)});
      }
      auto layout = measurer.CreateTextLayout(paragraph.WithStyles(std::move(styles)), base, width);
      const Point origin{inset, y};
      std::vector<CodeBackground> backgrounds;
      for (const auto& [range, color] : code_ranges) {
        for (Rect rect : layout.RangeRects(range)) {
          const float margin = std::min(1.0F, rect.height * 0.25F);
          rect.y += margin;
          rect.height -= 2.0F * margin;
          backgrounds.push_back({OffsetRect(rect, origin), color});
        }
      }
      result->paragraphs.push_back({offset, paragraph.Length(), origin, std::move(layout), std::move(backgrounds)});
      y = result->paragraphs.back().Bottom() + paragraph_gap;
      if (title) {
        result->illustration = {inset, y, width, illustration_height};
        y += illustration_height + paragraph_gap;
      }
      if (end == text.npos) {
        break;
      }
      offset += paragraph.Length() + 1;
      begin = end + 1;
    }
    result->height = y + inset;
    return result;
  }

  void SetLayout(std::shared_ptr<const EditorLayout> layout) {
    layout_ = std::move(layout);
  }

  void ResolveWidth(float width) {
    // The frame keeps requesting its preferred width so the layout can grow again after a resize.
    const float limit = width < layout_->preferred_width ? width : std::numeric_limits<float>::infinity();
    layout_inputs_.Update([limit](EditorLayoutInputs& inputs) { inputs.width_limit = limit; });
  }

  void Paint(PaintContext& paint, bool focused) const {
    if (!HasGeometry()) {
      return;
    }
    for (const auto& paragraph : layout_->paragraphs) {
      for (const auto& background : paragraph.code_backgrounds) {
        paint.DrawRect(background.bounds, background.color, CornerRadii{3.0F});
      }
      paint.DrawText(paragraph.origin, paragraph.layout);
    }
    // Keep the selection visible over any inline text background.
    for (Rect rect : RangeRects(document_.selection.Range())) {
      paint.DrawRect(rect, Color::Rgb(74, 139, 239, 0.28F));
    }
    DrawIllustration(paint);
    if (document_.composition) {
      for (Rect rect : RangeRects(*document_.composition)) {
        paint.DrawRect({rect.x, rect.y + rect.height - 2.0F, rect.width, 2.0F}, accent);
      }
    }
    if (focused && document_.selection.IsCollapsed()) {
      Rect caret = Caret({document_.selection.active, document_.selection.affinity});
      caret.width = 1.5F;
      paint.DrawRect(caret, accent);
    }
  }

private:
  static TextStyle Style(bool title) {
    return {.font = Font::System(title ? 28.0F : 18.0F), .foreground = ink};
  }

  std::vector<StyledRange> SelectionStyles() const {
    const TextRange range = document_.selection.Range();
    std::vector<StyledRange> existing;
    AppendStyles(existing, document_.text, range, range.start);

    std::vector<StyledRange> styles;
    TextOffset start = range.start;
    for (const auto& run : existing) {
      if (start < run.range.start) {
        styles.push_back({{start, run.range.start}, {}});
      }
      styles.push_back(run);
      start = run.range.end;
    }
    if (start < range.end) {
      styles.push_back({{start, range.end}, {}});
    }
    return styles;
  }

  SelectionFormat DescribeSelection(std::span<const StyledRange> runs) const {
    SelectionFormat result{
        .bold = true,
        .italic = true,
        .inline_code = true,
        .decoration = TextDecoration::Underline | TextDecoration::StrikeThrough,
    };
    for (const auto& run : runs) {
      if (document_.text.body.TextInRange(run.range).find_first_not_of('\n') == std::string::npos) {
        continue;
      }
      result.has_text = true;
      result.bold = result.bold && run.style.font_weight == FontWeight::Bold;
      result.italic = result.italic && run.style.font_slant == FontSlant::Italic;
      result.inline_code = result.inline_code && run.inline_code;
      result.decoration = result.decoration & run.style.decoration.value_or(TextDecoration::None);
    }
    return result.has_text ? result : SelectionFormat{};
  }

  template <class Format>
  void FormatSelection(Format format) {
    auto selected = SelectionStyles();
    const auto current = DescribeSelection(selected);
    if (!current.has_text) {
      return;
    }
    format(selected, current);

    const auto& text = document_.text;
    const TextRange range = document_.selection.Range();
    std::vector<StyledRange> styles;
    AppendStyles(styles, text, {0, range.start}, 0);
    for (auto& run : selected) {
      AppendStyle(styles, std::move(run));
    }
    AppendStyles(styles, text, {range.end, text.body.Length()}, range.end);
    if (styles == text.styles) {
      return;
    }

    // Commit provisional input separately so one undo reverts just the formatting command.
    FinishEditingComposition();
    auto next = document_;
    next.text.styles = std::move(styles);
    Remember(document_, next);
    Publish(std::move(next));
  }

  const Paragraph& ParagraphAt(TextOffset offset) const {
    const auto& paragraphs = layout_->paragraphs;
    auto after = std::upper_bound(paragraphs.begin(), paragraphs.end(), offset,
        [](TextOffset value, const Paragraph& paragraph) { return value < paragraph.offset; });
    return *std::prev(after);
  }

  static TextPosition Position(const Paragraph& paragraph, Point point) {
    TextPosition caret = paragraph.layout.HitTest(point - paragraph.origin);
    caret.offset += paragraph.offset;
    return caret;
  }

  Rect Caret(TextPosition position) const {
    const Paragraph& paragraph = ParagraphAt(position.offset);
    position.offset -= paragraph.offset;
    return OffsetRect(paragraph.layout.CaretRect(position), paragraph.origin);
  }

  std::vector<Rect> RangeRects(TextRange range) const {
    std::vector<Rect> rects;
    for (const auto& paragraph : layout_->paragraphs) {
      const auto start = std::max(range.start, paragraph.offset);
      const auto end = std::min(range.end, paragraph.offset + paragraph.length);
      if (start < end) {
        for (Rect rect : paragraph.layout.RangeRects({start - paragraph.offset, end - paragraph.offset})) {
          rects.push_back(OffsetRect(rect, paragraph.origin));
        }
      }
    }
    return rects;
  }

  TextOffset Adjacent(TextOffset offset, bool forward) const {
    const Paragraph& paragraph = ParagraphAt(offset);
    const TextOffset local = offset - paragraph.offset;
    if (forward) {
      return local == paragraph.length ? std::min(offset + 1, document_.text.body.Length())
                                       : paragraph.offset + paragraph.layout.NextCaretOffset(local);
    }
    return local == 0 ? std::max<TextOffset>(0, offset - 1)
                      : paragraph.offset + paragraph.layout.PreviousCaretOffset(local);
  }

  TextPosition VerticalPosition(const Paragraph& paragraph, std::size_t line, float x, bool down) const {
    const Paragraph* target = &paragraph;
    auto lines = target->layout.Lines();
    if (down && line + 1 < lines.size()) {
      ++line;
    } else if (!down && line > 0) {
      --line;
    } else if (down && target != &layout_->paragraphs.back()) {
      ++target;
      line = 0;
    } else if (!down && target != &layout_->paragraphs.front()) {
      --target;
      line = target->layout.Lines().size() - 1;
    } else {
      return {document_.selection.active, document_.selection.affinity};
    }
    const auto& row = target->layout.Lines()[line].bounds;
    return Position(*target, {x, target->origin.y + row.y + row.height * 0.5F});
  }

  TextInputKeyResult ApplyKey(const KeyEvent& event) {
    TextInputCommand command;
    command.kind = TextInputCommandKind::CommitText;
    if (event.key == Key::Enter) {
      command.text = "\n";
    } else if (event.key == Key::Escape && document_.composition) {
      command.kind = TextInputCommandKind::CancelComposition;
    } else if (event.key == Key::Backspace || event.key == Key::Delete) {
      if (document_.selection.IsCollapsed() && HasGeometry() && !document_.composition) {
        const auto offset = document_.selection.active;
        const auto other = Adjacent(offset, event.key == Key::Delete);
        command.target = TextRange{std::min(offset, other), std::max(offset, other)};
      }
    } else if (!event.text.empty()) {
      command.text = event.text;
    } else {
      return TextInputKeyResult::Unhandled;
    }
    ApplyTextInput({session_, {std::move(command)}});
    return TextInputKeyResult::Handled;
  }

  void FinishEditingComposition() {
    if (document_.composition) {
      TextInputCommand command;
      command.kind = TextInputCommandKind::FinishComposition;
      ApplyTextInput({session_, {command}});
    }
  }

  void Remember(const Document& before, const Document& after) {
    Document original = before;
    CancelComposition(original);
    if (original.text != after.text) {
      if (undo_.size() == 32) {
        undo_.erase(undo_.begin());
      }
      undo_.push_back(std::move(original));
      redo_.clear();
    }
  }

  void Restore(std::vector<Document>& source, std::vector<Document>& destination) {
    if (source.empty()) {
      return;
    }
    destination.push_back(document_);
    auto next = std::move(source.back());
    source.pop_back();
    Publish(std::move(next));
  }

  void Publish(Document next) {
    const bool changed_document = document_.text != next.text;
    if (!changed_document && document_.selection == next.selection && document_.composition == next.composition) {
      document_ = std::move(next);
      return;
    }
    if (changed_document && document_.text.body.PlainText() != next.text.body.PlainText()) {
      ++content_revision_;
    }
    document_ = std::move(next);
    if (changed_document) {
      layout_inputs_.Update([](EditorLayoutInputs& inputs) { ++inputs.document_revision; });
    }
    changed_ = ++revision_;
  }

  void DrawIllustration(PaintContext& paint) const {
    const auto [x, y, width, height] = layout_->illustration;
    paint.DrawRect(layout_->illustration, Color::Rgb(225, 236, 251), {10.0F, 10.0F, 10.0F, 10.0F});
    for (int index = 0; index < 3; ++index) {
      const float center = x + width * (0.2F + 0.3F * static_cast<float>(index));
      paint.DrawCircle({center, y + height * 0.38F}, 8.0F, accent);
      paint.DrawRect({center - width * 0.075F, y + height * 0.62F, width * 0.15F, 3.0F}, Color::Rgb(118, 155, 210));
    }
  }

  huxerui::State<std::uint64_t> changed_;
  huxerui::State<EditorLayoutInputs> layout_inputs_{EditorLayoutInputs{}};
  Document document_;
  std::vector<Document> undo_;
  std::vector<Document> redo_;
  TextInputSessionId session_ = 0;
  std::uint64_t revision_ = 0;
  std::uint64_t content_revision_ = 0;
  std::shared_ptr<const EditorLayout> layout_;
};

struct EditorSurface;

class EditorExtension final : public NodeExtension {
public:
  EditorExtension(ViewNode& node, const EditorSurface& surface) { Update(node, surface); }

  void Update(ViewNode&, const EditorSurface& surface);

  std::shared_ptr<TextInputClient> GetTextInputClient() noexcept override { return editor_; }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    editor_->ResolveWidth(node.ContentBounds().width);
    return PaintInvalidation::None;
  }

  bool HitTest(ViewNode& node, Point position) const override {
    return node.ContentBounds().Contains(position);
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override {
    if (event.device_kind == PointerDeviceKind::Touch) {
      return OnTouch(node, event);
    }
    if (event.type == PointerEventType::Down && editor_->HasGeometry() &&
        (event.changed_button == PointerButton::Primary || event.changed_button == PointerButton::None)) {
      dragging_ = true;
      editor_->Select(editor_->Position(event.position), event.modifiers.shift);
      return PointerResult::Capture;
    }
    if (event.type == PointerEventType::Move && dragging_ && editor_->HasGeometry()) {
      editor_->Select(editor_->Position(event.position), true);
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      dragging_ = false;
    }
    return PointerResult::Ignored;
  }

  void OnFocusChanged(ViewNode&, bool, bool) override { InvalidatePaint(); }

  void PaintAboveContent(const ViewNode& node, PaintContext& paint) const override {
    editor_->Paint(paint, node.IsFocused());
  }

private:
  PointerResult OnTouch(ViewNode& node, const PointerEvent& event) {
    if (event.type == PointerEventType::Down && !touch_down_ && editor_->HasGeometry()) {
      touch_down_ = event;
      return PointerResult::Observe;
    }
    if (!touch_down_ || touch_down_->pointer_id != event.pointer_id) {
      return PointerResult::Ignored;
    }
    const Point delta = event.position - touch_down_->position;
    if (event.type == PointerEventType::Cancel ||
        std::max(std::abs(delta.x), std::abs(delta.y)) >= GestureSettings{}.pointer_slop) {
      touch_down_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      touch_down_.reset();
      if (editor_->HasGeometry() && HitTest(node, event.position)) {
        editor_->Select(editor_->Position(event.position), event.modifiers.shift);
        return PointerResult::Handled;
      }
      return PointerResult::Ignored;
    }
    return PointerResult::Observe;
  }

  std::shared_ptr<EditorClient> editor_;
  std::optional<PointerEvent> touch_down_;
  bool dragging_ = false;
};

struct EditorSurface {
  using Extension = EditorExtension;
  std::shared_ptr<EditorClient> editor;
  std::shared_ptr<const EditorLayout> layout;
  std::uint64_t revision;
  bool operator==(const EditorSurface&) const = default;
};

void EditorExtension::Update(ViewNode&, const EditorSurface& surface) {
  editor_ = surface.editor;
  editor_->SetLayout(surface.layout);
  InvalidatePaint();
}

[[huxerui::composable]]
View EditorCanvas(std::shared_ptr<EditorClient> editor, std::shared_ptr<const EditorLayout> layout,
                  State<std::uint64_t> changes) {
  return Canvas([](PaintContext&, Size) {}).With(
      Frame{.width = layout->preferred_width, .height = layout->height},
      Background(Color::White()),
      Border{.color = surface_border},
      CornerRadius(16.0F),
      ClipChildren(),
      Focusable{},
      EditorSurface{editor, layout, changes.Get()},
      Semantics{.role = SemanticRole::TextField, .label = "Document"}
  );
}

IconButton ToolbarButton(const ImageResource& icon, const char* label, bool available = true,
                         std::optional<bool> selected = {}) {
  // Leave the pointer route active so a swipe can start on an unavailable tool.
  return IconButton(icon, label).With(
      Tooltip(label),
      Focusable{available},
      Opacity(available ? 1.0F : 0.4F),
      Background(selected.value_or(false) ? Color::Rgb(220, 235, 255) : Color::Transparent()),
      CornerRadius(7.0F),
      Semantics{.state_description = available ? std::nullopt : std::optional<StringVariant>{"Unavailable"},
                .selected = selected}
  );
}

struct Swatch {
  const char* label;
  Color color;
};

constexpr Swatch highlight_colors[]{
    {"Amber", Color::Rgb(255, 236, 164)},
    {"Mint", Color::Rgb(201, 244, 224)},
    {"Sky", Color::Rgb(206, 231, 255)},
    {"Rose", Color::Rgb(255, 216, 227)},
};

constexpr Swatch text_colors[]{
    {"Ink", ink},
    {"Blue", accent},
    {"Forest", Color::Rgb(24, 105, 73)},
    {"Crimson", Color::Rgb(166, 41, 77)},
};

View ColorPalette(PopupContext popup, const char* label, std::span<const Swatch> colors,
                  const std::function<void(std::optional<Color>)>& apply) {
  return Column {
    Text(label, TextRole::Label),
    Row {
      ForEach(colors, [popup, apply](const Swatch& swatch) {
        return IconButton(custom_editor::images::swatch, swatch.label)
            .OnClick([popup, apply, color = swatch.color] {
              apply(color);
              popup.Dismiss();
            })
            .With(Background(swatch.color), CornerRadius(7.0F), Tooltip(swatch.label));
      }),
    }.With(Spacing(5.0F)),
    Button("Clear color").OnClick([popup, apply] {
      apply(std::nullopt);
      popup.Dismiss();
    }),
  }.With(
      Padding(12.0F),
      Spacing(8.0F),
      Background(Color::White()),
      CornerRadius(10.0F),
      Border{.color = surface_border},
      Shadow{.color = Color::Rgb(29, 40, 61, 0.16F), .offset = {0.0F, 4.0F}, .blur_radius = 12.0F}
  );
}

[[huxerui::composable]]
View ColorTool(const ImageResource& icon, const char* label, bool available, std::span<const Swatch> colors,
               std::function<void(std::optional<Color>)> apply) {
  auto palette = UsePopup();
  auto palette_target = UseViewHandle();
  return ToolbarButton(icon, label, available)
      .With(palette_target)
      .OnClick([palette, palette_target, label, available, colors, apply = std::move(apply)] {
        if (available) {
          palette.Show(
              palette_target,
              [label, colors, apply](PopupContext popup) { return ColorPalette(popup, label, colors, apply); },
              PopupOptions{.trap_focus = true}
          );
        }
      });
}

[[huxerui::composable]]
View EditorToolbar(std::shared_ptr<EditorClient> editor, State<std::uint64_t> changes) {
  auto size_menu = UseMenu();
  auto size_target = UseViewHandle();
  std::ignore = changes.Get();
  const auto format = editor->SelectedFormat();
  const bool has_text = format.has_text;
  const bool underlined = HasTextDecoration(format.decoration, TextDecoration::Underline);
  const bool struck_through = HasTextDecoration(format.decoration, TextDecoration::StrikeThrough);
  using namespace custom_editor::images;

  return ScrollView {
    Row {
      ToolbarButton(undo, "Undo", editor->CanUndo()).OnClick([editor] { editor->Undo(); }),
      ToolbarButton(redo, "Redo", editor->CanRedo()).OnClick([editor] { editor->Redo(); }),
      ToolbarButton(select_all, "Select all").OnClick([editor] { editor->SelectAll(); }),
      Divider(Axis::Vertical).With(Frame{.height = 22.0F}),
      ToolbarButton(bold, "Bold", has_text, format.bold).OnClick([editor] { editor->ToggleBold(); }),
      ToolbarButton(italic, "Italic", has_text, format.italic).OnClick([editor] { editor->ToggleItalic(); }),
      ToolbarButton(underline, "Underline", has_text, underlined)
          .OnClick([editor] { editor->ToggleDecoration(TextDecoration::Underline); }),
      ToolbarButton(strikethrough, "Strikethrough", has_text, struck_through)
          .OnClick([editor] { editor->ToggleDecoration(TextDecoration::StrikeThrough); }),
      ToolbarButton(inline_code, "Inline code", has_text, format.inline_code)
          .OnClick([editor] { editor->ToggleInlineCode(); }),
      ToolbarButton(font_size, "Font size", has_text)
          .With(size_target)
          .OnClick([editor, size_menu, size_target, has_text] {
            if (has_text) {
              size_menu.Show(size_target, {
                  MenuItem("Font size 14", [editor] { editor->SetFontSize(14.0F); }),
                  MenuItem("Font size 18", [editor] { editor->SetFontSize(18.0F); }),
                  MenuItem("Font size 24", [editor] { editor->SetFontSize(24.0F); }),
                  MenuSection{},
                  MenuItem("Default size", [editor] { editor->SetFontSize(std::nullopt); }),
              });
            }
          }),
      ColorTool(text_color, "Text color", has_text, text_colors,
          [editor](std::optional<Color> color) { editor->SetTextColor(color); }),
      ColorTool(highlight, "Text background", has_text, highlight_colors,
          [editor](std::optional<Color> color) { editor->SetTextBackground(color); }),
      ToolbarButton(clear_formatting, "Clear formatting", has_text).OnClick([editor] { editor->ClearFormatting(); }),
      Divider(Axis::Vertical).With(Frame{.height = 22.0F}),
      ToolbarButton(reset, "Reset document").OnClick([editor] { editor->Reset(); }),
    }.With(Padding(EdgeInsets::Symmetric(8.0F, 5.0F)), Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.ScrollAxis(Axis::Horizontal).With(
      Background(Color::White()),
      Border{.color = surface_border},
      CornerRadius(12.0F),
      ClipChildren(),
      ScrollBar(),
      Semantics{.label = "Formatting toolbar"}
  );
}

[[huxerui::composable]]
View EditorDemo() {
  auto changes = UseState(std::uint64_t{0});
  const auto editor = UseState(std::make_shared<EditorClient>(changes)).Get();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const float width = compact ? 420.0F : 620.0F;
  const auto layout = editor->BuildLayout(UseTextMeasurer(), width);
  return Column {
    EditorToolbar(editor, changes).With(Frame{.width = width}),
    ScrollView(EditorCanvas(editor, layout, changes)).With(Frame{.width = width}, Grow(), ScrollBar()),
  }.With(
      Spacing(compact ? 10.0F : 14.0F),
      Padding(compact ? 12.0F : 24.0F),
      CrossAlign(CrossAxisAlignment::Center),
      Background(Color::Rgb(239, 244, 251))
  );
}

View App() { return EditorDemo(); }

} // namespace

const Application application{
    App,
    {.window = {.title = "HuxerUI Custom Editor", .initial_size = {690.0F, 580.0F}}}
};
