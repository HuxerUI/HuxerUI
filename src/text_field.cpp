#include "internal.h"
#include "text_field_internal.h"
#include "text_input_internal.h"
#include "text_layout_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace huxerui {
namespace {

bool IsSingleUtf8CodePoint(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  const std::uint8_t first = static_cast<std::uint8_t>(text.front());
  std::size_t length = 1;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
  }
  return text.size() == length;
}

class SecureTextLayout final : public detail::TextLayout {
public:
  SecureTextLayout(std::unique_ptr<detail::TextLayout> visual_layout, std::vector<TextOffset> boundaries)
      : visual_layout_(std::move(visual_layout)), boundaries_(std::move(boundaries)) {}

  Size Measure() const override {
    return visual_layout_->Measure();
  }

  TextPosition HitTest(Point point) const override {
    const TextPosition visual = visual_layout_->HitTest(point);
    const auto index = static_cast<std::size_t>(
        std::clamp<TextOffset>(visual.offset, 0, static_cast<TextOffset>(boundaries_.size() - 1))
    );
    return {
        boundaries_[index],
        visual.affinity,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    return visual_layout_->CaretRect(VisualOffset(offset, affinity == TextAffinity::Downstream), affinity);
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    if (range.IsCollapsed()) {
      return {};
    }
    return visual_layout_->RangeRects({
        VisualOffset(range.start, false),
        VisualOffset(range.end, true),
    });
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto found = std::lower_bound(boundaries_.begin(), boundaries_.end(), offset);
    return found == boundaries_.begin() ? boundaries_.front() : *std::prev(found);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto found = std::upper_bound(boundaries_.begin(), boundaries_.end(), offset);
    return found == boundaries_.end() ? boundaries_.back() : *found;
  }

private:
  TextOffset VisualOffset(TextOffset offset, bool round_up) const {
    const auto found = std::lower_bound(boundaries_.begin(), boundaries_.end(), offset);
    if (found == boundaries_.end()) {
      return static_cast<TextOffset>(boundaries_.size() - 1);
    }
    const TextOffset index = static_cast<TextOffset>(std::distance(boundaries_.begin(), found));
    if (*found == offset || round_up || found == boundaries_.begin()) {
      return index;
    }
    return index - 1;
  }

  std::unique_ptr<detail::TextLayout> visual_layout_;
  std::vector<TextOffset> boundaries_;
};

struct PreparedTextLayout {
  std::unique_ptr<detail::TextLayout> text_layout;
  std::string display_text;
};

std::vector<TextOffset> CollectGraphemeBoundaries(detail::TextLayout& layout, std::string_view text) {
  const TextOffset length = detail::Utf16Length(text).value_or(0);
  std::vector<TextOffset> boundaries{0};
  while (boundaries.back() < length) {
    const TextOffset next = layout.NextCaretOffset(boundaries.back());
    if (next <= boundaries.back() || next > length) {
      throw std::logic_error("HuxerUI platform returned an invalid text boundary");
    }
    boundaries.push_back(next);
  }
  return boundaries;
}

PreparedTextLayout PrepareTextFieldLayout(
    PlatformAdapter& platform,
    std::string_view text,
    const TextStyle& style,
    float max_width,
    bool secure,
    const TextLayoutOptions& options
) {
  if (!secure) {
    return {
        platform.CreateTextLayout(text, style, max_width, options),
        std::string(text),
    };
  }

  std::unique_ptr<detail::TextLayout> source = platform.CreateTextLayout(text, style, max_width, options);
  if (!source) {
    return {};
  }
  std::vector<TextOffset> boundaries = CollectGraphemeBoundaries(*source, text);

  std::string display_text;
  constexpr std::string_view bullet = "\xE2\x80\xA2";
  display_text.reserve((boundaries.size() - 1) * bullet.size());
  for (std::size_t index = 1; index < boundaries.size(); ++index) {
    display_text.append(bullet);
  }
  std::unique_ptr<detail::TextLayout> visual = platform.CreateTextLayout(display_text, style, max_width, options);
  if (!visual) {
    return {};
  }
  return {
      std::make_unique<SecureTextLayout>(std::move(visual), std::move(boundaries)),
      std::move(display_text),
  };
}

void ValidateConfiguration(const TextInputConfiguration& configuration) {
  if (configuration.secure && configuration.multiline) {
    throw std::invalid_argument("HuxerUI TextField cannot be both secure and multiline");
  }
  if (!configuration.multiline && configuration.action == TextInputAction::Newline) {
    throw std::invalid_argument("HuxerUI TextField newline action requires multiline input");
  }
}

void ValidateLimits(
    const TextInputConfiguration& configuration, std::size_t min_lines, const std::optional<std::size_t>& max_lines
) {
  if (min_lines == 0 || (max_lines.has_value() && *max_lines == 0)) {
    throw std::invalid_argument("HuxerUI TextField line limits must be greater than zero");
  }
  if (max_lines.has_value() && min_lines > *max_lines) {
    throw std::invalid_argument("HuxerUI TextField minimum lines cannot exceed maximum lines");
  }
  if (!configuration.multiline && (min_lines != 1 || (max_lines.has_value() && *max_lines != 1))) {
    throw std::invalid_argument("HuxerUI TextField line limits greater than one require multiline input");
  }
}

class TextFieldClient final : public TextInputClient, public TextSelectionClient {
public:
  void Attach(detail::MountedNode& node) noexcept {
    node_ = &node;
  }

  void Detach() noexcept {
    node_ = nullptr;
  }

  void Update(detail::MountedNode& node, const detail::TextFieldModifier& modifier) {
    ValidateConfiguration(modifier.configuration);
    ValidateLimits(modifier.configuration, modifier.min_lines, modifier.max_lines);
    if (!detail::IsValidTextEditingValue(modifier.value)) {
      throw std::invalid_argument("HuxerUI TextField value is invalid");
    }
    if (!detail::Utf16Length(modifier.placeholder).has_value()) {
      throw std::invalid_argument("HuxerUI TextField placeholder must contain valid UTF-8");
    }
    if (!detail::Utf16Length(modifier.validation.message).has_value()) {
      throw std::invalid_argument("HuxerUI TextField validation message must contain valid UTF-8");
    }
    if (!initialized_ && modifier.value.composition.has_value()) {
      throw std::invalid_argument("HuxerUI TextField initial value must not contain a composition");
    }

    Attach(node);
    event_bindings_ = node.event_bindings;
    const bool text_layout_mode_changed =
        initialized_ && (configuration_.multiline != modifier.configuration.multiline ||
                         configuration_.secure != modifier.configuration.secure);
    const bool length_limit_changed = initialized_ && max_length_ != modifier.max_length;
    const bool validation_changed = validation_ != modifier.validation;
    configuration_ = modifier.configuration;
    min_lines_ = modifier.min_lines;
    max_lines_ = modifier.max_lines;
    max_length_ = modifier.max_length;
    validation_ = modifier.validation;
    if (length_limit_changed) {
      ClearHistory();
    }
    ConfigureScrollNode(node);
    placeholder_ = modifier.placeholder;

    TextFieldStyle next_style = node.LayoutValueOr<detail::ResolvedTextFieldStyle>(TextFieldStyle::Default());
    next_style.background = node.properties.background.value_or(next_style.background);
    next_style.text_style = node.properties.text_style;
    next_style.corner_radius = node.properties.corner_radius;
    if (!initialized_ || text_layout_mode_changed || next_style.text_style.font != style_.text_style.font ||
        next_style.placeholder_style.font != style_.placeholder_style.font ||
        next_style.validation_text_style.font != style_.validation_text_style.font ||
        placeholder_ != laid_out_placeholder_ || validation_changed) {
      text_layout_.reset();
      placeholder_layout_.reset();
      validation_layout_.reset();
    }
    if (text_layout_mode_changed) {
      horizontal_scroll_offset_ = 0.0F;
      preferred_caret_x_.reset();
      RequestCaretReveal();
    }
    style_ = next_style;

    if (!initialized_) {
      editing_.value = modifier.value;
      initialized_ = true;
      return;
    }

    const bool acknowledged = last_emitted_.has_value() && modifier.value == *last_emitted_;
    if (acknowledged) {
      last_emitted_.reset();
      return;
    }
    if (modifier.value == editing_.value) {
      return;
    }
    if (modifier.value.composition.has_value()) {
      throw std::invalid_argument("HuxerUI TextField authoritative value must not introduce a composition");
    }

    const bool text_changed = modifier.value.text != editing_.value.text;
    if (text_changed) {
      ClearHistory();
    } else {
      BreakHistoryMerge();
    }
    composition_history_start_.reset();
    editing_ = {
        modifier.value,
        std::nullopt,
    };
    last_emitted_.reset();
    ++revision_;
    if (text_changed) {
      ++content_revision_;
      text_layout_.reset();
    }
    preferred_caret_x_.reset();
    RequestCaretReveal();
    ResetCaretBlink();
  }

  Size Measure(detail::MountedNode& node, PlatformAdapter& platform, Constraints constraints) {
    Attach(node);
    platform_ = &platform;
    const float next_layout_width = ResolveLayoutWidth(platform, constraints);
    if (next_layout_width != text_layout_width_) {
      text_layout_width_ = next_layout_width;
      text_layout_.reset();
      placeholder_layout_.reset();
      RequestCaretReveal();
    }
    const float next_validation_width =
        constraints.HasBoundedWidth() ? std::max(1.0F, constraints.max_width) : std::numeric_limits<float>::infinity();
    if (next_validation_width != validation_text_layout_width_) {
      validation_text_layout_width_ = next_validation_width;
      validation_layout_.reset();
    }
    EnsureLayouts(platform);
    const Size text_size = text_layout_->Measure();
    const Size placeholder_size = placeholder_layout_ ? placeholder_layout_->Measure() : Size{};
    const Size validation_size = validation_layout_ ? validation_layout_->Measure() : Size{};
    const float content_width = std::max({
        text_size.width,
        placeholder_size.width,
        validation_size.width,
    });
    const float full_editor_height = std::max(text_size.height, placeholder_size.height);
    if (node.scroll_state) {
      node.scroll_state->content_width = text_size.width;
      node.scroll_state->content_height = full_editor_height + ValidationAreaHeight();
    }
    float editor_height = full_editor_height;
    if (configuration_.multiline) {
      const float line_height = text_layout_->CaretRect(0, TextAffinity::Downstream).height;
      editor_height = std::max(editor_height, line_height * static_cast<float>(min_lines_));
      if (max_lines_.has_value()) {
        editor_height = std::min(editor_height, line_height * static_cast<float>(*max_lines_));
      }
    }
    const float minimum_editor_height = std::max(0.0F, style_.minimum_height - node.properties.padding.Vertical());
    editor_height = std::max(editor_height, minimum_editor_height);
    return constraints.Constrain({
        content_width,
        editor_height + ValidationAreaHeight(),
    });
  }

  bool UpdateEditorScrollOffset(detail::MountedNode& node) {
    const float previous_horizontal_offset = horizontal_scroll_offset_;
    const float previous_vertical_offset = node.scroll_state ? node.scroll_state->offset_y : 0.0F;
    UpdateScrollOffset(node, EditorContentRect(node));
    const bool changed = horizontal_scroll_offset_ != previous_horizontal_offset ||
                         (node.scroll_state && node.scroll_state->offset_y != previous_vertical_offset);
    if (changed) {
      ++revision_;
    }
    return changed;
  }

  void Paint(const detail::MountedNode& node, PaintContext& context) const {
    if (!text_layout_) {
      return;
    }

    const Rect editor_frame = EditorFrame(node);
    const Rect content = EditorContentRect(node);
    const Point origin = TextOrigin(node);
    context.PushClip(content, std::max(0.0F, style_.corner_radius));

    if (!editing_.value.selection.IsCollapsed()) {
      for (const Rect& rect : text_layout_->RangeRects(editing_.value.selection.Range())) {
        context.DrawRect(OffsetRect(rect, origin), style_.selection);
      }
    }

    if (editing_.value.text.empty() && !placeholder_.empty() && placeholder_layout_) {
      const Size size = placeholder_layout_->Measure();
      context.DrawText(
          {
              origin.x,
              origin.y,
              std::max(content.width, size.width),
              size.height,
          },
          placeholder_,
          style_.placeholder_style
      );
    } else {
      const Size size = text_layout_->Measure();
      context.DrawText(
          {
              origin.x,
              origin.y,
              std::max(content.width, size.width),
              size.height,
          },
          laid_out_text_,
          style_.text_style
      );
    }

    if (editing_.value.composition.has_value()) {
      for (const Rect& rect : text_layout_->RangeRects(*editing_.value.composition)) {
        const Rect translated = OffsetRect(rect, origin);
        context.DrawRect(
            {
                translated.x,
                translated.y + std::max(0.0F, translated.height - 1.0F),
                translated.width,
                1.0F,
            },
            style_.composition
        );
      }
    }

    if (node.focused && editing_.value.selection.IsCollapsed() && caret_visible_) {
      Rect caret =
          OffsetRect(text_layout_->CaretRect(editing_.value.selection.active, editing_.value.selection.affinity), origin);
      caret.width = std::max(1.0F, caret.width);
      context.DrawRect(caret, style_.caret);
    }
    context.PopClip();

    if (validation_layout_) {
      const Rect node_content = node.ContentBounds();
      const Size size = validation_layout_->Measure();
      TextStyle validation_style = style_.validation_text_style;
      if (!validation_.IsInvalid()) {
        validation_style.foreground = style_.placeholder_style.foreground;
      }
      context.PushClip(node.bounds, std::max(0.0F, style_.corner_radius));
      context.DrawText(
          {
              node_content.x,
              editor_frame.y + editor_frame.height + std::max(0.0F, style_.validation_spacing),
              node_content.width,
              size.height,
          },
          validation_.message,
          std::move(validation_style)
      );
      context.PopClip();
    }

    const bool invalid = validation_.IsInvalid();
    float border_width = std::max(0.0F, style_.border_width);
    Color border = style_.border;
    if (invalid) {
      border_width = std::max(0.0F, style_.validation_border_width);
      border = style_.validation_error;
    } else if (node.focused) {
      border_width = std::max(0.0F, style_.focused_border_width);
      border = style_.focused_border;
    }
    if (border_width > 0.0F && border.alpha > 0.0F) {
      context.DrawBorder(editor_frame, border, border_width, std::max(0.0F, style_.corner_radius));
    }
  }

  NodeExtension::FrameResult
  AdvanceCaret(const detail::MountedNode& node, const FrameInfo& frame, bool& paint_changed) {
    const bool previous_caret_visible = caret_visible_;
    const auto finish = [&](NodeExtension::FrameResult result) {
      paint_changed = caret_visible_ != previous_caret_visible;
      return result;
    };
    if (!node.focused || !editing_.value.selection.IsCollapsed()) {
      caret_epoch_.reset();
      caret_visible_ = false;
      return finish({});
    }
    const double interval = style_.caret_blink_interval;
    if (!std::isfinite(interval) || interval <= 0.0) {
      caret_epoch_.reset();
      caret_visible_ = true;
      return finish({});
    }
    if (!caret_epoch_.has_value() || caret_reset_pending_) {
      caret_epoch_ = frame.timestamp;
      caret_reset_pending_ = false;
    }
    const double elapsed = std::max(0.0, frame.timestamp - *caret_epoch_);
    const double phase = std::fmod(elapsed, interval);
    caret_visible_ = static_cast<std::uint64_t>(elapsed / interval) % 2 == 0;
    return finish({
        .wake_after = std::max(0.001, interval - phase),
    });
  }

  void FocusChanged(bool focused) {
    BreakHistoryMerge();
    if (focused) {
      RequestCaretReveal();
      ResetCaretBlink();
    } else {
      caret_epoch_.reset();
      caret_visible_ = false;
    }
  }

  NodeExtension::PointerResult Pointer(const PointerEvent& event) {
    if (!node_ || !text_layout_) {
      return NodeExtension::PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_anchor_.reset();
      pending_touch_selection_.reset();
      pending_touch_origin_.reset();
      return NodeExtension::PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      pointer_anchor_.reset();
      pending_touch_origin_.reset();
      if (event.device_kind == PointerDeviceKind::Touch && pending_touch_selection_.has_value()) {
        const TextPosition position = *pending_touch_selection_;
        pending_touch_selection_.reset();
        SetSelection({position.offset, position.offset, position.affinity});
      }
      return NodeExtension::PointerResult::Handled;
    }

    if (event.device_kind == PointerDeviceKind::Touch) {
      if (event.type == PointerEventType::Down) {
        pending_touch_selection_ = PositionAt(event.position);
        if (!pending_touch_selection_.has_value()) {
          pending_touch_origin_.reset();
          return NodeExtension::PointerResult::Ignored;
        }
        pending_touch_origin_ = event.position;
        return NodeExtension::PointerResult::Observe;
      }
      if (event.type == PointerEventType::Move && pending_touch_origin_.has_value()) {
        const float distance =
            std::hypot(event.position.x - pending_touch_origin_->x, event.position.y - pending_touch_origin_->y);
        if (distance >= detail::touch_gesture_slop) {
          pending_touch_selection_.reset();
          pending_touch_origin_.reset();
        }
      }
      return pending_touch_selection_.has_value() ? NodeExtension::PointerResult::Observe
                                                  : NodeExtension::PointerResult::Ignored;
    }

    if (event.type == PointerEventType::Move && pointer_anchor_.has_value()) {
      ScrollSelectionAtEdge(event.position);
    }
    const std::optional<TextPosition> position = PositionAt(event.position);
    if (!position.has_value()) {
      return NodeExtension::PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      pointer_anchor_ = position->offset;
      SetSelection({position->offset, position->offset, position->affinity});
      return NodeExtension::PointerResult::Observe;
    }
    if (event.type == PointerEventType::Move && pointer_anchor_.has_value()) {
      SetSelection({*pointer_anchor_, position->offset, position->affinity});
      return NodeExtension::PointerResult::Observe;
    }
    return NodeExtension::PointerResult::Ignored;
  }

  TextInputConfiguration Configuration() const override {
    return configuration_;
  }

  TextInputState State() const override {
    return {
        session_id_,
        revision_,
        content_revision_,
        editing_.value.selection,
        editing_.value.composition,
    };
  }

  TextInputState BeginTextInput(TextInputSessionId session_id) override {
    session_id_ = session_id;
    return State();
  }

  TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) override {
    if (batch.session_id != session_id_) {
      return {
          TextInputResultCode::SessionMismatch,
          TextInputSyncAction::None,
      };
    }
    if (configuration_.read_only) {
      return {
          TextInputResultCode::ReadOnly,
          TextInputSyncAction::None,
      };
    }
    return ApplyCommands(batch.commands);
  }

  TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const override {
    if (session_id != session_id_) {
      TextInputContext result;
      result.result_code = TextInputResultCode::SessionMismatch;
      result.session_id = session_id;
      return result;
    }
    if (start < 0 || length < 0) {
      TextInputContext result;
      result.result_code = TextInputResultCode::Rejected;
      result.session_id = session_id;
      return result;
    }
    return {
        TextInputResultCode::Ok,
        session_id,
        0,
        detail::Utf16Length(editing_.value.text).value_or(0),
        editing_.value.text,
        editing_.value.selection,
        editing_.value.composition,
    };
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const override {
    TextInputGeometry result;
    result.session_id = session_id;
    if (session_id != session_id_) {
      result.result_code = TextInputResultCode::SessionMismatch;
      return result;
    }
    if (!node_ || !text_layout_ || !IsValidRange(range)) {
      result.result_code = TextInputResultCode::Rejected;
      return result;
    }

    const Point origin = TextOrigin(*node_);
    result.result_code = TextInputResultCode::Ok;
    result.caret = OffsetRect(
        text_layout_->CaretRect(
            range.end,
            range.end == editing_.value.selection.active ? editing_.value.selection.affinity : TextAffinity::Downstream
        ),
        origin
    );
    for (const Rect& rect : text_layout_->RangeRects(range)) {
      result.range_rects.push_back(OffsetRect(rect, origin));
    }
    return result;
  }

  TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const override {
    TextInputPositionResult result;
    result.session_id = session_id;
    if (session_id != session_id_) {
      result.result_code = TextInputResultCode::SessionMismatch;
      return result;
    }
    if (!node_ || !text_layout_) {
      result.result_code = TextInputResultCode::Rejected;
      return result;
    }

    const std::optional<TextPosition> position = PositionAt(point);
    if (!position.has_value()) {
      result.result_code = TextInputResultCode::Rejected;
      return result;
    }
    result.result_code = TextInputResultCode::Ok;
    result.position = *position;
    return result;
  }

  bool CanPerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) const override {
    if (!node_ || !node_->enabled) {
      return false;
    }
    const TextRange selection = editing_.value.selection.Range();
    switch (action) {
    case TextEditingAction::Copy:
      return !configuration_.secure && !selection.IsCollapsed() && clipboard != nullptr;
    case TextEditingAction::SelectAll: {
      const TextOffset length = detail::Utf16Length(editing_.value.text).value_or(0);
      return length > 0 && selection != TextRange{0, length};
    }
    case TextEditingAction::Cut:
    case TextEditingAction::Paste:
      return false;
    }
    return false;
  }

  bool PerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) override {
    if (!CanPerformTextEditingAction(action, clipboard)) {
      return false;
    }
    if (action == TextEditingAction::SelectAll) {
      const TextOffset length = detail::Utf16Length(editing_.value.text).value_or(0);
      SetSelection({0, length});
      return true;
    }
    const std::optional<std::string> selected =
        detail::Utf8TextInRange(editing_.value.text, editing_.value.selection.Range());
    return selected.has_value() && clipboard->WriteText(*selected);
  }

  bool SelectWord(Point position) override {
    if (!node_ || !text_layout_) {
      return false;
    }
    const std::optional<TextPosition> text_position = PositionAt(position);
    if (!text_position.has_value()) {
      return false;
    }
    const std::optional<TextRange> range = detail::WordRangeAt(editing_.value.text, text_position->offset);
    if (!range.has_value()) {
      return false;
    }
    SetSelection({range->start, range->end, text_position->affinity});
    return true;
  }

  bool ExtendSelection(Point position, bool start_handle) override {
    if (!node_ || !text_layout_) {
      return false;
    }
    const TextRange range = editing_.value.selection.Range();
    if (range.IsCollapsed()) {
      return false;
    }
    ScrollSelectionAtEdge(position);
    const std::optional<TextPosition> text_position = PositionAt(position);
    if (!text_position.has_value()) {
      return false;
    }
    SetSelection(
        start_handle ? TextSelection{range.end, std::min(text_position->offset, range.end), text_position->affinity}
                     : TextSelection{range.start, std::max(text_position->offset, range.start), text_position->affinity}
    );
    return true;
  }

  bool QuerySelectionGeometry(Rect& start, Rect& end) const override {
    if (!node_ || !text_layout_) {
      return false;
    }
    const TextRange range = editing_.value.selection.Range();
    const Point origin = TextOrigin(*node_);
    start = OffsetRect(text_layout_->CaretRect(range.start, TextAffinity::Downstream), origin);
    end = OffsetRect(text_layout_->CaretRect(range.end, TextAffinity::Downstream), origin);
    return true;
  }

  Color SelectionHandleColor() const noexcept override {
    return style_.caret;
  }

  void ScrollActivity() noexcept {
    caret_reveal_pending_ = false;
    if (node_ && node_->scroll_state) {
      node_->scroll_state->allows_automatic_reveal = false;
      ++revision_;
    }
  }

  TextInputKeyResult HandleTextKey(const KeyEvent& event) override {
    if (event.type != KeyEventType::Down) {
      return TextInputKeyResult::Unhandled;
    }
    if (configuration_.read_only) {
      return TextInputKeyResult::Unhandled;
    }
    const bool history_shortcut = (event.modifiers.control || event.modifiers.meta) && !event.modifiers.alt;
    if (history_shortcut && event.key == Key::Z) {
      if (event.modifiers.shift) {
        Redo();
      } else {
        Undo();
      }
      return TextInputKeyResult::Handled;
    }
    if (history_shortcut && event.key == Key::Y && !event.modifiers.shift) {
      Redo();
      return TextInputKeyResult::Handled;
    }

    switch (event.key) {
    case Key::ArrowLeft:
      if (event.modifiers.meta) {
        MoveCaretToLineBoundary(false, event.modifiers.shift);
      } else if (event.modifiers.alt || event.modifiers.control) {
        MoveCaretByWord(false, event.modifiers.shift, event.modifiers.control && !event.modifiers.alt);
      } else {
        MoveCaret(false, event.modifiers.shift);
      }
      return TextInputKeyResult::Handled;
    case Key::ArrowRight:
      if (event.modifiers.meta) {
        MoveCaretToLineBoundary(true, event.modifiers.shift);
      } else if (event.modifiers.alt || event.modifiers.control) {
        MoveCaretByWord(true, event.modifiers.shift, event.modifiers.control && !event.modifiers.alt);
      } else {
        MoveCaret(true, event.modifiers.shift);
      }
      return TextInputKeyResult::Handled;
    case Key::ArrowUp:
      if (event.modifiers.meta) {
        MoveCaretToDocumentBoundary(false, event.modifiers.shift);
        return TextInputKeyResult::Handled;
      }
      if (event.modifiers.control || event.modifiers.alt) {
        return TextInputKeyResult::Unhandled;
      }
      if (!configuration_.multiline) {
        return TextInputKeyResult::Unhandled;
      }
      MoveCaretVertically(false, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::ArrowDown:
      if (event.modifiers.meta) {
        MoveCaretToDocumentBoundary(true, event.modifiers.shift);
        return TextInputKeyResult::Handled;
      }
      if (event.modifiers.control || event.modifiers.alt) {
        return TextInputKeyResult::Unhandled;
      }
      if (!configuration_.multiline) {
        return TextInputKeyResult::Unhandled;
      }
      MoveCaretVertically(true, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::Home:
      if (event.modifiers.alt) {
        return TextInputKeyResult::Unhandled;
      }
      if (event.modifiers.control || event.modifiers.meta || !configuration_.multiline) {
        MoveCaretToDocumentBoundary(false, event.modifiers.shift);
      } else {
        MoveCaretToLineBoundary(false, event.modifiers.shift);
      }
      return TextInputKeyResult::Handled;
    case Key::End:
      if (event.modifiers.alt) {
        return TextInputKeyResult::Unhandled;
      }
      if (event.modifiers.control || event.modifiers.meta || !configuration_.multiline) {
        MoveCaretToDocumentBoundary(true, event.modifiers.shift);
      } else {
        MoveCaretToLineBoundary(true, event.modifiers.shift);
      }
      return TextInputKeyResult::Handled;
    case Key::PageUp:
    case Key::PageDown:
      if (!configuration_.multiline || event.modifiers.control || event.modifiers.alt || event.modifiers.meta) {
        return TextInputKeyResult::Unhandled;
      }
      MoveCaretByPage(event.key == Key::PageDown, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::Backspace:
      if (event.modifiers.meta) {
        DeleteToLineBoundary(false);
      } else if (event.modifiers.alt || event.modifiers.control) {
        DeleteByWord(false, event.modifiers.control && !event.modifiers.alt);
      } else {
        DeleteAdjacent(false);
      }
      return TextInputKeyResult::Handled;
    case Key::Delete:
      if (event.modifiers.meta) {
        DeleteToLineBoundary(true);
      } else if (event.modifiers.alt || event.modifiers.control) {
        DeleteByWord(true, event.modifiers.control && !event.modifiers.alt);
      } else {
        DeleteAdjacent(true);
      }
      return TextInputKeyResult::Handled;
    case Key::Enter:
      if (event.modifiers.control || event.modifiers.alt || event.modifiers.meta) {
        return TextInputKeyResult::Unhandled;
      }
      if (configuration_.multiline &&
          (configuration_.action == TextInputAction::Default || configuration_.action == TextInputAction::Newline)) {
        TextInputCommand command;
        command.kind = TextInputCommandKind::CommitText;
        command.text = "\n";
        ApplyCommands({command});
      } else {
        BreakHistoryMerge();
        detail::EmitEvent<TextFieldEvents::Submitted>(event_bindings_);
        ResetCaretBlink();
      }
      return TextInputKeyResult::Handled;
    case Key::Escape:
      if (event.modifiers.control || event.modifiers.alt || event.modifiers.meta) {
        return TextInputKeyResult::Unhandled;
      }
      if (editing_.value.composition.has_value()) {
        TextInputCommand command;
        command.kind = TextInputCommandKind::CancelComposition;
        ApplyCommands({command});
        return TextInputKeyResult::Handled;
      }
      return TextInputKeyResult::Unhandled;
    default:
      break;
    }

    if (event.modifiers.control || event.modifiers.alt || event.modifiers.meta) {
      return TextInputKeyResult::Unhandled;
    }
    if (!event.text.empty()) {
      TextInputCommand command;
      command.kind = TextInputCommandKind::CommitText;
      command.text = event.text;
      ApplyCommands({command});
      return TextInputKeyResult::Handled;
    }
    return TextInputKeyResult::Unhandled;
  }

  void EndTextInput(TextInputSessionId session_id, TextInputEndReason reason) override {
    static_cast<void>(reason);
    if (session_id != session_id_) {
      return;
    }
    if (editing_.value.composition.has_value()) {
      TextInputCommand command;
      command.kind = TextInputCommandKind::FinishComposition;
      ApplyCommands({command});
    }
    session_id_ = 0;
  }

private:
  enum class HistoryMergeKind {
    None,
    Typing,
    BackwardDeletion,
    ForwardDeletion,
  };

  struct HistoryEntry {
    TextEditingValue before;
    TextEditingValue after;
    HistoryMergeKind merge_kind = HistoryMergeKind::None;
    double timestamp = 0.0;
  };

  static constexpr std::size_t kHistoryLimit = 100;
  static constexpr double kHistoryMergeInterval = 1.0;

  std::optional<TextPosition> PositionAt(Point point) const {
    if (!node_ || !text_layout_) {
      return std::nullopt;
    }
    const Point origin = TextOrigin(*node_);
    return text_layout_->HitTest({
        point.x - origin.x,
        point.y - origin.y,
    });
  }

  void EnsureLayouts(PlatformAdapter& platform) {
    const TextLayoutOptions text_layout_options{
        .wrap = configuration_.multiline ? TextWrap::Word : TextWrap::NoWrap,
    };
    if (!text_layout_) {
      PreparedTextLayout prepared = PrepareTextFieldLayout(
          platform,
          editing_.value.text,
          style_.text_style,
          text_layout_width_,
          configuration_.secure,
          text_layout_options
      );
      text_layout_ = std::move(prepared.text_layout);
      laid_out_text_ = std::move(prepared.display_text);
    }
    if (!text_layout_) {
      throw std::logic_error("HuxerUI platform does not provide editable text layout");
    }
    if (placeholder_.empty()) {
      placeholder_layout_.reset();
      laid_out_placeholder_.clear();
    } else if (!placeholder_layout_ || laid_out_placeholder_ != placeholder_) {
      placeholder_layout_ =
          platform.CreateTextLayout(placeholder_, style_.placeholder_style, text_layout_width_, text_layout_options);
      laid_out_placeholder_ = placeholder_;
      if (!placeholder_layout_) {
        throw std::logic_error("HuxerUI platform does not provide editable text layout");
      }
    }
    if (!ShowsSupportingMessage()) {
      validation_layout_.reset();
      laid_out_validation_message_.clear();
    } else if (!validation_layout_ || laid_out_validation_message_ != validation_.message) {
      validation_layout_ = platform.CreateTextLayout(
          validation_.message,
          style_.validation_text_style,
          validation_text_layout_width_,
          TextLayoutOptions{.wrap = TextWrap::Word}
      );
      laid_out_validation_message_ = validation_.message;
      if (!validation_layout_) {
        throw std::logic_error("HuxerUI platform does not provide validation message layout");
      }
    }
  }

  float ValidationAreaHeight() const {
    if (!validation_layout_) {
      return 0.0F;
    }
    return std::max(0.0F, style_.validation_spacing) + validation_layout_->Measure().height;
  }

  bool ShowsSupportingMessage() const {
    return !validation_.message.empty() &&
           (validation_.status == ValidationStatus::Invalid || validation_.status == ValidationStatus::Pending);
  }

  Rect EditorFrame(const detail::MountedNode& node) const {
    Rect frame = node.bounds;
    const float minimum_height = std::min(frame.height, std::max(0.0F, style_.minimum_height));
    frame.height = std::clamp(frame.height - ValidationAreaHeight(), minimum_height, frame.height);
    return frame;
  }

  Rect EditorContentRect(const detail::MountedNode& node) const {
    const Rect frame = EditorFrame(node);
    return {
        frame.x + node.properties.padding.left,
        frame.y + node.properties.padding.top,
        std::max(0.0F, frame.width - node.properties.padding.Horizontal()),
        std::max(0.0F, frame.height - node.properties.padding.Vertical()),
    };
  }

  HistoryMergeKind ResolveHistoryMergeKind(
      const std::vector<TextInputCommand>& commands, const TextEditingValue& before, const TextEditingValue& after
  ) const {
    if (commands.size() != 1 || before.composition.has_value() || after.composition.has_value()) {
      return HistoryMergeKind::None;
    }
    const TextInputCommand& command = commands.front();
    if (command.kind != TextInputCommandKind::CommitText || !before.selection.IsCollapsed()) {
      return HistoryMergeKind::None;
    }
    if (!command.target.has_value()) {
      return command.text.find('\n') == std::string::npos && IsSingleUtf8CodePoint(command.text)
                 ? HistoryMergeKind::Typing
                 : HistoryMergeKind::None;
    }
    if (!command.text.empty() || !text_layout_) {
      return HistoryMergeKind::None;
    }

    const TextOffset active = before.selection.active;
    if (*command.target == TextRange{text_layout_->PreviousCaretOffset(active), active}) {
      return HistoryMergeKind::BackwardDeletion;
    }
    if (*command.target == TextRange{active, text_layout_->NextCaretOffset(active)}) {
      return HistoryMergeKind::ForwardDeletion;
    }
    return HistoryMergeKind::None;
  }

  static void PushHistory(std::vector<HistoryEntry>& history, HistoryEntry entry) {
    if (history.size() == kHistoryLimit) {
      history.erase(history.begin());
    }
    history.push_back(std::move(entry));
  }

  void RecordHistory(TextEditingValue before, TextEditingValue after, HistoryMergeKind merge_kind) {
    redo_history_.clear();
    const double timestamp = platform_ ? platform_->Now() : 0.0;
    if (history_merge_allowed_ && merge_kind != HistoryMergeKind::None && !undo_history_.empty()) {
      HistoryEntry& previous = undo_history_.back();
      const double elapsed = timestamp - previous.timestamp;
      if (previous.merge_kind == merge_kind && previous.after == before && elapsed >= 0.0 &&
          elapsed <= kHistoryMergeInterval) {
        previous.after = std::move(after);
        previous.timestamp = timestamp;
        return;
      }
    }

    PushHistory(
        undo_history_,
        {
            std::move(before),
            std::move(after),
            merge_kind,
            timestamp,
        }
    );
    history_merge_allowed_ = merge_kind != HistoryMergeKind::None;
  }

  void BreakHistoryMerge() noexcept {
    history_merge_allowed_ = false;
  }

  void ClearHistory() {
    undo_history_.clear();
    redo_history_.clear();
    composition_history_start_.reset();
    BreakHistoryMerge();
  }

  bool RestoreHistoryValue(TextEditingValue value) {
    value.composition.reset();
    if (value == editing_.value) {
      return false;
    }

    const bool text_changed = value.text != editing_.value.text;
    editing_ = {
        std::move(value),
        std::nullopt,
    };
    composition_history_start_.reset();
    preferred_caret_x_.reset();
    RequestCaretReveal();
    ++revision_;
    if (text_changed) {
      ++content_revision_;
    }
    last_emitted_ = editing_.value;
    if (text_changed) {
      text_layout_.reset();
      if (platform_) {
        EnsureLayouts(*platform_);
      }
    }
    ResetCaretBlink();
    detail::EmitEvent<TextFieldEvents::Changed>(event_bindings_, editing_.value);
    return true;
  }

  bool Undo() {
    BreakHistoryMerge();
    if (editing_.value.composition.has_value()) {
      TextInputCommand command;
      command.kind = TextInputCommandKind::CancelComposition;
      const std::uint64_t previous_revision = revision_;
      static_cast<void>(ApplyCommands({command}));
      return revision_ != previous_revision;
    }
    if (undo_history_.empty()) {
      return false;
    }

    HistoryEntry entry = std::move(undo_history_.back());
    undo_history_.pop_back();
    TextEditingValue target = entry.before;
    PushHistory(redo_history_, std::move(entry));
    return RestoreHistoryValue(std::move(target));
  }

  bool Redo() {
    BreakHistoryMerge();
    if (editing_.value.composition.has_value() || redo_history_.empty()) {
      return false;
    }

    HistoryEntry entry = std::move(redo_history_.back());
    redo_history_.pop_back();
    TextEditingValue target = entry.after;
    PushHistory(undo_history_, std::move(entry));
    return RestoreHistoryValue(std::move(target));
  }

  std::vector<TextOffset> GraphemeBoundaries(std::string_view text) const {
    if (!platform_) {
      throw std::logic_error("HuxerUI TextField cannot resolve text boundaries before layout");
    }
    std::unique_ptr<detail::TextLayout> layout = platform_->CreateTextLayout(
        text,
        style_.text_style,
        std::numeric_limits<float>::infinity(),
        TextLayoutOptions{.wrap = TextWrap::NoWrap}
    );
    if (!layout) {
      throw std::logic_error("HuxerUI platform does not provide editable text layout");
    }
    return CollectGraphemeBoundaries(*layout, text);
  }

  std::size_t GraphemeCount(std::string_view text) const {
    return GraphemeBoundaries(text).size() - 1;
  }

  std::size_t LengthLimitBaseline(const detail::TextFieldEditingState& state) const {
    if (!state.value.composition.has_value()) {
      return GraphemeCount(state.value.text);
    }
    TextInputCommand cancel;
    cancel.kind = TextInputCommandKind::CancelComposition;
    const detail::TextInputReductionResult cancelled = detail::ReduceTextInputCommands(state, {cancel});
    if (cancelled.status != detail::TextInputReductionStatus::Accepted) {
      throw std::logic_error("HuxerUI TextField has an invalid composition baseline");
    }
    return GraphemeCount(cancelled.state.value.text);
  }

  TextInputCommand LimitCommitText(const detail::TextFieldEditingState& state, const TextInputCommand& command) const {
    if (!max_length_.has_value()) {
      return command;
    }

    const std::size_t allowed = std::max(*max_length_, LengthLimitBaseline(state));
    const auto preview = [&state](const TextInputCommand& candidate) {
      return detail::ReduceTextInputCommands(state, {candidate});
    };
    const detail::TextInputReductionResult full = preview(command);
    if (full.status != detail::TextInputReductionStatus::Accepted || GraphemeCount(full.state.value.text) <= allowed) {
      return command;
    }

    const std::vector<TextOffset> boundaries = GraphemeBoundaries(command.text);
    TextInputCommand limited = command;
    limited.selection_after.reset();
    std::size_t accepted = 0;
    std::size_t rejected = boundaries.size() - 1;
    while (accepted < rejected) {
      const std::size_t index = accepted + (rejected - accepted + 1) / 2;
      limited.text = detail::Utf8TextInRange(command.text, {0, boundaries[index]}).value_or(std::string{});
      const detail::TextInputReductionResult result = preview(limited);
      if (result.status == detail::TextInputReductionStatus::Accepted &&
          GraphemeCount(result.state.value.text) <= allowed) {
        accepted = index;
      } else {
        rejected = index - 1;
      }
    }
    limited.text = detail::Utf8TextInRange(command.text, {0, boundaries[accepted]}).value_or(std::string{});
    return limited;
  }

  TextInputCommand
  PrepareLimitedCommand(const detail::TextFieldEditingState& state, const TextInputCommand& command) const {
    if (!max_length_.has_value()) {
      return command;
    }
    if (command.kind == TextInputCommandKind::CommitText) {
      return LimitCommitText(state, command);
    }
    if (command.kind != TextInputCommandKind::FinishComposition || !state.value.composition.has_value()) {
      return command;
    }

    const std::size_t allowed = std::max(*max_length_, LengthLimitBaseline(state));
    if (GraphemeCount(state.value.text) <= allowed) {
      return command;
    }
    TextInputCommand commit;
    commit.kind = TextInputCommandKind::CommitText;
    commit.text = detail::Utf8TextInRange(state.value.text, *state.value.composition).value_or(std::string{});
    return LimitCommitText(state, commit);
  }

  detail::TextInputReductionResult
  ReduceCommands(const std::vector<TextInputCommand>& commands, std::vector<TextInputCommand>& applied_commands) const {
    if (commands.empty()) {
      return {};
    }
    detail::TextFieldEditingState state = editing_;
    applied_commands.reserve(commands.size());
    for (const TextInputCommand& command : commands) {
      TextInputCommand applied = PrepareLimitedCommand(state, command);
      detail::TextInputReductionResult reduced = detail::ReduceTextInputCommands(state, {applied});
      if (reduced.status != detail::TextInputReductionStatus::Accepted) {
        return reduced;
      }
      state = std::move(reduced.state);
      applied_commands.push_back(std::move(applied));
    }
    const bool changed = state != editing_;
    return {
        detail::TextInputReductionStatus::Accepted,
        std::move(state),
        changed,
    };
  }

  TextInputApplyResult ApplyCommands(const std::vector<TextInputCommand>& commands) {
    const detail::TextFieldEditingState before = editing_;
    std::vector<TextInputCommand> applied_commands;
    const detail::TextInputReductionResult reduced = ReduceCommands(commands, applied_commands);
    if (reduced.status != detail::TextInputReductionStatus::Accepted) {
      return {
          TextInputResultCode::Rejected,
          TextInputSyncAction::None,
      };
    }
    const bool selection_command = std::ranges::any_of(commands, [](const TextInputCommand& command) {
      return command.kind == TextInputCommandKind::SetSelection;
    });
    if (!reduced.changed) {
      if (selection_command) {
        BreakHistoryMerge();
      }
      return {
          TextInputResultCode::Ok,
          TextInputSyncAction::None,
      };
    }

    const bool was_composing = before.value.composition.has_value();
    const bool is_composing = reduced.state.value.composition.has_value();
    const bool text_changed = reduced.state.value.text != before.value.text;
    if (!was_composing && is_composing) {
      composition_history_start_ = before.value;
      BreakHistoryMerge();
    } else if (was_composing && !is_composing) {
      if (composition_history_start_.has_value() && composition_history_start_->text != reduced.state.value.text) {
        RecordHistory(*composition_history_start_, reduced.state.value, HistoryMergeKind::None);
      } else {
        BreakHistoryMerge();
      }
      composition_history_start_.reset();
    } else if (!was_composing && !is_composing && text_changed) {
      RecordHistory(
          before.value,
          reduced.state.value,
          ResolveHistoryMergeKind(applied_commands, before.value, reduced.state.value)
      );
    } else if (!text_changed || selection_command) {
      BreakHistoryMerge();
    }

    editing_ = reduced.state;
    preferred_caret_x_.reset();
    RequestCaretReveal();
    ++revision_;
    if (text_changed) {
      ++content_revision_;
    }
    last_emitted_ = editing_.value;
    if (text_changed) {
      text_layout_.reset();
      if (platform_) {
        EnsureLayouts(*platform_);
      }
    }
    ResetCaretBlink();
    detail::EmitEvent<TextFieldEvents::Changed>(event_bindings_, editing_.value);
    return {
        TextInputResultCode::Ok,
        TextInputSyncAction::Update,
    };
  }

  void SetSelection(TextSelection selection, bool preserve_preferred_x = false) {
    const std::optional<float> preferred_x = preserve_preferred_x ? preferred_caret_x_ : std::nullopt;
    TextInputCommand command;
    command.kind = TextInputCommandKind::SetSelection;
    command.selection_after = selection;
    ApplyCommands({command});
    if (preserve_preferred_x) {
      preferred_caret_x_ = preferred_x;
    }
  }

  void MoveCaret(bool forward, bool extend) {
    if (!text_layout_) {
      return;
    }
    const TextRange selection = editing_.value.selection.Range();
    TextOffset offset = editing_.value.selection.active;
    if (!extend && !selection.IsCollapsed()) {
      offset = forward ? selection.end : selection.start;
    } else {
      offset = forward ? text_layout_->NextCaretOffset(offset) : text_layout_->PreviousCaretOffset(offset);
    }
    MoveCaretTo(offset, extend);
  }

  void MoveCaretByWord(bool forward, bool extend, bool next_word_start) {
    const TextRange selection = editing_.value.selection.Range();
    TextOffset offset = editing_.value.selection.active;
    if (!extend && !selection.IsCollapsed()) {
      offset = forward ? selection.end : selection.start;
    } else {
      std::optional<TextOffset> boundary;
      if (!forward) {
        boundary = detail::PreviousWordStart(editing_.value.text, offset);
      } else if (next_word_start) {
        boundary = detail::NextWordStart(editing_.value.text, offset);
      } else {
        boundary = detail::NextWordEnd(editing_.value.text, offset);
      }
      if (!boundary.has_value()) {
        return;
      }
      offset = *boundary;
    }
    MoveCaretTo(offset, extend);
  }

  void MoveCaretTo(TextOffset offset, bool extend) {
    const TextOffset anchor = extend ? editing_.value.selection.anchor : offset;
    SetSelection({anchor, offset, TextAffinity::Downstream});
  }

  void MoveCaretTo(TextPosition position, bool extend, bool preserve_preferred_x) {
    const TextOffset anchor = extend ? editing_.value.selection.anchor : position.offset;
    SetSelection({anchor, position.offset, position.affinity}, preserve_preferred_x);
  }

  void MoveCaretVertically(bool forward, bool extend) {
    if (!text_layout_) {
      return;
    }
    const TextSelection selection = editing_.value.selection;
    const Rect caret = text_layout_->CaretRect(selection.active, selection.affinity);
    const float x = preferred_caret_x_.value_or(caret.x);
    preferred_caret_x_ = x;
    const float target_y = forward ? caret.y + caret.height * 1.5F : caret.y - caret.height * 0.5F;
    MoveCaretTo(text_layout_->HitTest({x, target_y}), extend, true);
  }

  void MoveCaretByPage(bool forward, bool extend) {
    if (!text_layout_ || !node_) {
      return;
    }
    const TextSelection selection = editing_.value.selection;
    const Rect caret = text_layout_->CaretRect(selection.active, selection.affinity);
    const float x = preferred_caret_x_.value_or(caret.x);
    preferred_caret_x_ = x;
    const float distance = std::max(caret.height, EditorContentRect(*node_).height);
    const float target_y = caret.y + caret.height * 0.5F + (forward ? distance : -distance);
    MoveCaretTo(text_layout_->HitTest({x, target_y}), extend, true);
  }

  TextPosition LineBoundary(bool end) const {
    const TextSelection selection = editing_.value.selection;
    const Rect caret = text_layout_->CaretRect(selection.active, selection.affinity);
    const float x = end ? text_layout_->Measure().width + 1.0F : -1.0F;
    return text_layout_->HitTest({x, caret.y + caret.height * 0.5F});
  }

  void MoveCaretToLineBoundary(bool end, bool extend) {
    if (!text_layout_) {
      return;
    }
    MoveCaretTo(LineBoundary(end), extend, false);
  }

  void MoveCaretToDocumentBoundary(bool end, bool extend) {
    MoveCaretTo(end ? detail::Utf16Length(editing_.value.text).value_or(0) : 0, extend);
  }

  void DeleteAdjacent(bool forward) {
    if (!text_layout_) {
      return;
    }
    TextRange target = editing_.value.selection.Range();
    if (target.IsCollapsed()) {
      const TextOffset active = editing_.value.selection.active;
      target = forward ? TextRange{active, text_layout_->NextCaretOffset(active)}
                       : TextRange{text_layout_->PreviousCaretOffset(active), active};
    }
    DeleteRange(target);
  }

  void DeleteByWord(bool forward, bool next_word_start) {
    TextRange target = editing_.value.selection.Range();
    if (target.IsCollapsed()) {
      const TextOffset active = editing_.value.selection.active;
      std::optional<TextOffset> boundary;
      if (!forward) {
        boundary = detail::PreviousWordStart(editing_.value.text, active);
      } else if (next_word_start) {
        boundary = detail::NextWordStart(editing_.value.text, active);
      } else {
        boundary = detail::NextWordEnd(editing_.value.text, active);
      }
      if (!boundary.has_value()) {
        return;
      }
      target = {
          std::min(active, *boundary),
          std::max(active, *boundary),
      };
    }
    DeleteRange(target);
  }

  void DeleteToLineBoundary(bool end) {
    if (!text_layout_) {
      return;
    }
    TextRange target = editing_.value.selection.Range();
    if (target.IsCollapsed()) {
      const TextOffset active = editing_.value.selection.active;
      const TextOffset boundary = LineBoundary(end).offset;
      target = {
          std::min(active, boundary),
          std::max(active, boundary),
      };
    }
    DeleteRange(target);
  }

  void DeleteRange(TextRange target) {
    if (target.IsCollapsed()) {
      BreakHistoryMerge();
      ResetCaretBlink();
      return;
    }
    TextInputCommand command;
    command.kind = TextInputCommandKind::CommitText;
    command.target = target;
    ApplyCommands({command});
  }

  bool IsValidRange(TextRange range) const {
    if (!range.IsValid()) {
      return false;
    }
    TextEditingValue probe = editing_.value;
    probe.selection = {range.start, range.end};
    probe.composition.reset();
    return detail::IsValidTextEditingValue(probe);
  }

  Point TextOrigin(const detail::MountedNode& node) const {
    const Rect content = EditorContentRect(node);
    const Size size = text_layout_ ? text_layout_->Measure() : Size{};
    if (configuration_.multiline) {
      return {
          content.x,
          content.y - ResolveVerticalScrollOffset(node, content),
      };
    }
    return {
        content.x - ResolveHorizontalScrollOffset(content),
        content.y + std::max(0.0F, (content.height - size.height) * 0.5F),
    };
  }

  float ResolveHorizontalScrollOffset(Rect content) const {
    if (!text_layout_ || content.width <= 0.0F) {
      return 0.0F;
    }
    const Rect caret = text_layout_->CaretRect(editing_.value.selection.active, editing_.value.selection.affinity);
    float scroll_offset = horizontal_scroll_offset_;
    if (caret.x < scroll_offset) {
      scroll_offset = std::max(0.0F, caret.x);
    } else if (caret.x + caret.width > scroll_offset + content.width) {
      scroll_offset = caret.x + caret.width - content.width;
    }
    const float maximum = std::max(0.0F, text_layout_->Measure().width + caret.width - content.width);
    return std::clamp(scroll_offset, 0.0F, maximum);
  }

  float ResolveVerticalScrollOffset(const detail::MountedNode& node, Rect content) const {
    if (!text_layout_ || !node.scroll_state || content.height <= 0.0F) {
      return 0.0F;
    }
    const Rect caret = text_layout_->CaretRect(editing_.value.selection.active, editing_.value.selection.affinity);
    const float maximum = std::max(0.0F, text_layout_->Measure().height - content.height);
    float scroll_offset = std::clamp(node.scroll_state->offset_y, 0.0F, maximum);
    if (caret_reveal_pending_) {
      if (caret.y < scroll_offset) {
        scroll_offset = std::max(0.0F, caret.y);
      } else if (caret.y + caret.height > scroll_offset + content.height) {
        scroll_offset = caret.y + caret.height - content.height;
      }
    }
    return std::clamp(scroll_offset, 0.0F, maximum);
  }

  void UpdateScrollOffset(detail::MountedNode& node, Rect content) {
    if (!text_layout_) {
      horizontal_scroll_offset_ = 0.0F;
      if (node.scroll_state) {
        node.scroll_state->offset_y = 0.0F;
      }
      return;
    }
    if (configuration_.multiline) {
      horizontal_scroll_offset_ = 0.0F;
      if (!node.scroll_state || content.height <= 0.0F) {
        if (node.scroll_state) {
          node.scroll_state->offset_y = 0.0F;
        }
        return;
      }
      node.scroll_state->offset_y = ResolveVerticalScrollOffset(node, content);
      caret_reveal_pending_ = false;
      return;
    }
    horizontal_scroll_offset_ = ResolveHorizontalScrollOffset(content);
  }

  void ConfigureScrollNode(detail::MountedNode& node) {
    if (!configuration_.multiline) {
      node.scroll_state.reset();
      return;
    }
    if (!node.scroll_state) {
      node.scroll_state = std::make_unique<detail::ScrollNodeState>();
    }
    node.scroll_state->axis = Axis::Vertical;
    node.scroll_state->touch_drag_only = true;
  }

  void ScrollSelectionAtEdge(Point position) {
    if (!configuration_.multiline || !node_ || !node_->scroll_state) {
      return;
    }
    const Rect content = EditorContentRect(*node_);
    float delta = 0.0F;
    if (position.y < content.y) {
      delta = position.y - content.y;
    } else if (position.y > content.y + content.height) {
      delta = position.y - (content.y + content.height);
    }
    if (detail::ScrollNodeBy(*node_, delta) != 0.0F) {
      caret_reveal_pending_ = false;
    }
  }

  float ResolveLayoutWidth(PlatformAdapter& platform, Constraints constraints) const {
    if (!configuration_.multiline) {
      return std::numeric_limits<float>::infinity();
    }
    if (constraints.HasBoundedWidth()) {
      return std::max(1.0F, constraints.max_width);
    }

    float width = std::max(1.0F, constraints.min_width);
    auto measure_lines = [&](std::string_view text) {
      std::size_t start = 0;
      do {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
        width = std::max(
            width,
            platform
                .MeasureText(
                    line,
                    style_.text_style,
                    std::numeric_limits<float>::infinity(),
                    TextLayoutOptions{.wrap = TextWrap::NoWrap}
                )
                .size.width
        );
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      } while (start <= text.size());
    };
    measure_lines(editing_.value.text);
    measure_lines(placeholder_);
    return width;
  }

  static Rect OffsetRect(Rect rect, Point offset) {
    rect.x += offset.x;
    rect.y += offset.y;
    return rect;
  }

  void ResetCaretBlink() noexcept {
    caret_reset_pending_ = true;
    caret_visible_ = true;
  }

  void RequestCaretReveal() noexcept {
    caret_reveal_pending_ = true;
    if (node_ && node_->scroll_state) {
      node_->scroll_state->allows_automatic_reveal = true;
    }
  }

  detail::MountedNode* node_ = nullptr;
  PlatformAdapter* platform_ = nullptr;
  detail::EventBindings event_bindings_;
  TextInputConfiguration configuration_;
  std::size_t min_lines_ = 1;
  std::optional<std::size_t> max_lines_;
  std::optional<std::size_t> max_length_;
  ValidationResult validation_;
  TextFieldStyle style_;
  detail::TextFieldEditingState editing_;
  std::optional<TextEditingValue> last_emitted_;
  std::optional<TextEditingValue> composition_history_start_;
  std::vector<HistoryEntry> undo_history_;
  std::vector<HistoryEntry> redo_history_;
  std::string placeholder_;
  std::string laid_out_text_;
  std::string laid_out_placeholder_;
  std::string laid_out_validation_message_;
  std::unique_ptr<detail::TextLayout> text_layout_;
  std::unique_ptr<detail::TextLayout> placeholder_layout_;
  std::unique_ptr<detail::TextLayout> validation_layout_;
  std::optional<TextOffset> pointer_anchor_;
  std::optional<TextPosition> pending_touch_selection_;
  std::optional<Point> pending_touch_origin_;
  std::optional<float> preferred_caret_x_;
  std::optional<double> caret_epoch_;
  TextInputSessionId session_id_ = 0;
  std::uint64_t revision_ = 0;
  std::uint64_t content_revision_ = 0;
  float horizontal_scroll_offset_ = 0.0F;
  float text_layout_width_ = std::numeric_limits<float>::infinity();
  float validation_text_layout_width_ = std::numeric_limits<float>::infinity();
  bool initialized_ = false;
  bool caret_reset_pending_ = true;
  bool caret_visible_ = true;
  bool caret_reveal_pending_ = true;
  bool history_merge_allowed_ = false;
};

class TextFieldExtension final : public NodeExtension {
public:
  TextFieldExtension(MountedNode& node, const detail::TextFieldModifier& modifier)
      : client_(std::make_shared<TextFieldClient>()) {
    Update(node, modifier);
  }

  ~TextFieldExtension() override {
    client_->Detach();
  }

  void Update(MountedNode& node, const detail::TextFieldModifier& modifier) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    client_->Update(mounted, modifier);
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool geometry_changed = client_->UpdateEditorScrollOffset(mounted);
    bool caret_changed = false;
    const FrameResult result = client_->AdvanceCaret(mounted, frame, caret_changed);
    if (geometry_changed || caret_changed) {
      mounted.foreground_paint_dirty = true;
    }
    return result;
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  void OnFocusChanged(MountedNode& node, bool focused) override {
    static_cast<void>(node);
    client_->FocusChanged(focused);
    InvalidatePaint();
  }

  void OnScrollActivity(MountedNode& node) override {
    static_cast<void>(node);
    client_->ScrollActivity();
    InvalidatePaint();
  }

  std::shared_ptr<TextInputClient> GetTextInputClient() noexcept override {
    return client_;
  }

  TextSelectionClient* GetTextSelectionClient() noexcept override {
    return client_.get();
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    static_cast<void>(node);
    const PointerResult result = client_->Pointer(event);
    if (result != PointerResult::Ignored) {
      InvalidatePaint();
    }
    return result;
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
    client_->Paint(static_cast<const detail::MountedNode&>(node), context);
  }

  Size Measure(detail::MountedNode& node, PlatformAdapter& platform, Constraints constraints) {
    return client_->Measure(node, platform, constraints);
  }

private:
  std::shared_ptr<TextFieldClient> client_;
};

TextFieldExtension& FindTextFieldExtension(detail::MountedNode& node) {
  for (detail::NodeExtensionEntry& entry : node.extensions) {
    if (entry.descriptor == &detail::TextFieldModifier::Descriptor() && entry.extension) {
      return static_cast<TextFieldExtension&>(*entry.extension);
    }
  }
  throw std::logic_error("HuxerUI TextField has no retained input extension");
}

std::shared_ptr<detail::ViewSpec> MakeTextFieldSpec() {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::TextField);
  spec->focusable = true;
  return spec;
}

} // namespace

namespace detail {

bool TextFieldModifier::LayoutEquals(const TextFieldModifier& left, const TextFieldModifier& right) {
  return left.value.text == right.value.text && left.placeholder == right.placeholder &&
         left.configuration.multiline == right.configuration.multiline &&
         left.configuration.secure == right.configuration.secure && left.min_lines == right.min_lines &&
         left.max_lines == right.max_lines && left.validation == right.validation;
}

const ModifierDescriptor& TextFieldModifier::Descriptor() {
  return ModifierDescriptorFor<TextFieldModifier, TextFieldExtension, true, TextFieldModifier::LayoutEquals>();
}

Size MeasureTextField(MountedNode& node, PlatformAdapter& platform, Constraints constraints) {
  return FindTextFieldExtension(node).Measure(node, platform, constraints);
}

} // namespace detail

TextFieldLineLimits TextFieldLineLimits::SingleLine() noexcept {
  return TextFieldLineLimits(false, 1, std::nullopt);
}

TextFieldLineLimits TextFieldLineLimits::MultiLine(std::size_t minimum) {
  if (minimum == 0) {
    throw std::invalid_argument("HuxerUI TextField minimum lines must be greater than zero");
  }
  return TextFieldLineLimits(true, minimum, std::nullopt);
}

TextFieldLineLimits TextFieldLineLimits::MultiLine(std::size_t minimum, std::size_t maximum) {
  if (minimum == 0 || maximum == 0) {
    throw std::invalid_argument("HuxerUI TextField line limits must be greater than zero");
  }
  if (minimum > maximum) {
    throw std::invalid_argument("HuxerUI TextField minimum lines cannot exceed maximum lines");
  }
  return TextFieldLineLimits(true, minimum, maximum);
}

TextField::TextField(TextEditingValue value)
    : detail::TypedView<TextField>(MakeTextFieldSpec()), value_(std::move(value)) {
  if (!detail::IsValidTextEditingValue(value_)) {
    throw std::invalid_argument("HuxerUI TextField value is invalid");
  }
  UpdateModifier();
}

TextField TextField::Placeholder(std::string value) && {
  if (!detail::Utf16Length(value).has_value()) {
    throw std::invalid_argument("HuxerUI TextField placeholder must contain valid UTF-8");
  }
  placeholder_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

TextField TextField::Placeholder(std::string_view value) && {
  return std::move(*this).Placeholder(std::string(value));
}

TextField TextField::Placeholder(const char* value) && {
  return std::move(*this).Placeholder(value == nullptr ? std::string{} : std::string(value));
}

TextField TextField::LineLimits(TextFieldLineLimits value) && {
  configuration_.multiline = value.IsMultiline();
  if (configuration_.multiline && configuration_.action == TextInputAction::Default) {
    configuration_.action = TextInputAction::Newline;
  } else if (!configuration_.multiline && configuration_.action == TextInputAction::Newline) {
    configuration_.action = TextInputAction::Default;
  }
  ValidateConfiguration(configuration_);
  line_limits_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

TextField TextField::MaxLength(std::size_t value) && {
  max_length_ = value;
  UpdateModifier();
  return std::move(*this);
}

TextField TextField::Validation(ValidationResult value) && {
  validation_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

TextField TextField::Secure() && {
  configuration_.secure = true;
  ValidateConfiguration(configuration_);
  UpdateModifier();
  return std::move(*this);
}

TextField TextField::InputConfiguration(TextInputConfiguration configuration) && {
  ValidateConfiguration(configuration);
  if (configuration.multiline != line_limits_.IsMultiline()) {
    line_limits_ = configuration.multiline ? TextFieldLineLimits::MultiLine() : TextFieldLineLimits::SingleLine();
  }
  configuration_ = configuration;
  UpdateModifier();
  return std::move(*this);
}

void TextField::UpdateModifier() {
  SetModifier(
      detail::MakeModifierSpec(
          detail::TextFieldModifier{
              value_,
              placeholder_,
              configuration_,
              line_limits_.Minimum(),
              line_limits_.Maximum(),
              max_length_,
              validation_,
          }
      )
  );
}

} // namespace huxerui
