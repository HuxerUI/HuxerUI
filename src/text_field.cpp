#include "internal.h"
#include "text_field_internal.h"
#include "text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

#include <huxerui/indication.h>

namespace huxerui {
namespace {

Color ApplyOpacity(Color color, float opacity) {
  color.alpha *= std::clamp(opacity, 0.0F, 1.0F);
  return color;
}

Rect ContentRect(const detail::MountedNode& node) {
  return {
      node.frame.x + node.style.padding.left,
      node.frame.y + node.style.padding.top,
      std::max(0.0F, node.frame.width - node.style.padding.Horizontal()),
      std::max(0.0F, node.frame.height - node.style.padding.Vertical()),
  };
}

void ValidateConfiguration(const TextInputConfiguration& configuration) {
  if (configuration.multiline) {
    throw std::invalid_argument("HuxerUI TextField currently supports single-line input only");
  }
  if (configuration.secure) {
    throw std::invalid_argument("HuxerUI TextField secure input is not implemented");
  }
}

class TextFieldClient final : public TextInputClient {
public:
  void Attach(detail::MountedNode& node) noexcept {
    node_ = &node;
  }

  void Detach(detail::MountedNode& node) noexcept {
    if (node_ == &node) {
      node_ = nullptr;
    }
  }

  void Update(detail::MountedNode& node, const detail::TextFieldModifier& modifier) {
    ValidateConfiguration(modifier.configuration);
    if (!detail::IsValidTextEditingValue(modifier.value)) {
      throw std::invalid_argument("HuxerUI TextField value is invalid");
    }
    if (!detail::Utf16Length(modifier.placeholder).has_value()) {
      throw std::invalid_argument("HuxerUI TextField placeholder must contain valid UTF-8");
    }
    if (!initialized_ && modifier.value.composition.has_value()) {
      throw std::invalid_argument("HuxerUI TextField initial value must not contain a composition");
    }

    Attach(node);
    event_bindings_ = node.event_bindings;
    configuration_ = modifier.configuration;
    placeholder_ = modifier.placeholder;

    TextFieldStyle next_style = node.LayoutValueOr<detail::ResolvedTextFieldStyle>(TextFieldStyleKey::Default());
    next_style.background = node.style.background.value_or(next_style.background);
    next_style.foreground = node.style.foreground.value_or(next_style.foreground);
    next_style.font_size = node.style.font_size.value_or(next_style.font_size);
    next_style.corner_radius = node.style.corner_radius;
    if (!initialized_ || next_style.font_size != style_.font_size || placeholder_ != layout_placeholder_) {
      layout_.reset();
      placeholder_layout_.reset();
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

    editing_ = {
        modifier.value,
        std::nullopt,
    };
    last_emitted_.reset();
    ++revision_;
    layout_.reset();
    ResetCaretBlink();
  }

  Size Measure(detail::MountedNode& node, PlatformHost& platform, Constraints constraints) {
    Attach(node);
    platform_ = &platform;
    EnsureLayouts(platform);
    const Size text_size = layout_->Measure();
    const Size placeholder_size = placeholder_layout_ ? placeholder_layout_->Measure() : Size{};
    return constraints.Constrain({
        std::max(text_size.width, placeholder_size.width),
        std::max(text_size.height, placeholder_size.height),
    });
  }

  void Paint(const detail::MountedNode& node, DisplayList& display_list) const {
    if (!layout_) {
      return;
    }

    const Rect content = ContentRect(node);
    const Point origin = TextOrigin(node);
    const float opacity = node.PresentationOpacity();
    display_list.PushClip(content, std::max(0.0F, style_.corner_radius));

    if (!editing_.value.selection.IsCollapsed()) {
      for (const Rect& rect : layout_->RangeRects(editing_.value.selection.Range())) {
        display_list.DrawRect(OffsetRect(rect, origin), ApplyOpacity(style_.selection, opacity));
      }
    }

    if (editing_.value.text.empty() && !placeholder_.empty() && placeholder_layout_) {
      const Size size = placeholder_layout_->Measure();
      display_list.DrawText(
          {
              origin.x,
              origin.y,
              std::max(content.width, size.width),
              size.height,
          },
          placeholder_,
          ApplyOpacity(style_.placeholder, opacity),
          style_.font_size
      );
    } else {
      const Size size = layout_->Measure();
      display_list.DrawText(
          {
              origin.x,
              origin.y,
              std::max(content.width, size.width),
              size.height,
          },
          editing_.value.text,
          ApplyOpacity(style_.foreground, opacity),
          style_.font_size
      );
    }

    if (editing_.value.composition.has_value()) {
      for (const Rect& rect : layout_->RangeRects(*editing_.value.composition)) {
        const Rect translated = OffsetRect(rect, origin);
        display_list.DrawRect(
            {
                translated.x,
                translated.y + std::max(0.0F, translated.height - 1.0F),
                translated.width,
                1.0F,
            },
            ApplyOpacity(style_.composition, opacity)
        );
      }
    }

    if (node.focused && editing_.value.selection.IsCollapsed() && caret_visible_) {
      Rect caret =
          OffsetRect(layout_->CaretRect(editing_.value.selection.active, editing_.value.selection.affinity), origin);
      caret.width = std::max(1.0F, caret.width);
      display_list.DrawRect(caret, ApplyOpacity(style_.caret, opacity));
    }
    display_list.PopClip();

    const float border_width =
        node.focused ? std::max(0.0F, style_.focused_border_width) : std::max(0.0F, style_.border_width);
    const Color border = node.focused ? style_.focused_border : style_.border;
    if (border_width > 0.0F && border.alpha > 0.0F) {
      display_list
          .DrawBorder(node.frame, ApplyOpacity(border, opacity), border_width, std::max(0.0F, style_.corner_radius));
    }
  }

  NodeExtension::FrameResult AdvanceCaret(const detail::MountedNode& node, const FrameInfo& frame) {
    if (!node.focused || !editing_.value.selection.IsCollapsed()) {
      caret_epoch_.reset();
      caret_visible_ = false;
      return {};
    }
    const double interval = style_.caret_blink_interval;
    if (!std::isfinite(interval) || interval <= 0.0) {
      caret_epoch_.reset();
      caret_visible_ = true;
      return {};
    }
    if (!caret_epoch_.has_value() || caret_reset_pending_) {
      caret_epoch_ = frame.timestamp;
      caret_reset_pending_ = false;
    }
    const double elapsed = std::max(0.0, frame.timestamp - *caret_epoch_);
    const double phase = std::fmod(elapsed, interval);
    caret_visible_ = static_cast<std::uint64_t>(elapsed / interval) % 2 == 0;
    return {
        .wake_after = std::max(0.001, interval - phase),
    };
  }

  void FocusChanged(bool focused) {
    if (focused) {
      ResetCaretBlink();
    } else {
      caret_epoch_.reset();
      caret_visible_ = false;
    }
  }

  NodeExtension::PointerResult Pointer(const PointerEvent& event) {
    if (!node_ || !layout_) {
      return NodeExtension::PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_anchor_.reset();
      return NodeExtension::PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      pointer_anchor_.reset();
      return NodeExtension::PointerResult::Handled;
    }

    const Point origin = TextOrigin(*node_);
    const detail::TextHit hit = layout_->HitTest({
        event.position.x - origin.x,
        event.position.y - origin.y,
    });
    if (event.type == PointerEventType::Down) {
      pointer_anchor_ = hit.offset;
      SetSelection({hit.offset, hit.offset, hit.affinity});
      return NodeExtension::PointerResult::Observe;
    }
    if (event.type == PointerEventType::Move && pointer_anchor_.has_value()) {
      SetSelection({*pointer_anchor_, hit.offset, hit.affinity});
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
          false,
      };
    }
    if (configuration_.read_only) {
      return {
          TextInputResultCode::ReadOnly,
          TextInputSyncAction::None,
          false,
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
    if (!node_ || !layout_ || !IsValidRange(range)) {
      result.result_code = TextInputResultCode::Rejected;
      return result;
    }

    const Point origin = TextOrigin(*node_);
    result.result_code = TextInputResultCode::Ok;
    result.caret = ToHostRect(
        *node_,
        OffsetRect(
            layout_->CaretRect(
                range.end,
                range.end == editing_.value.selection.active ? editing_.value.selection.affinity
                                                             : TextAffinity::Downstream
            ),
            origin
        )
    );
    for (const Rect& rect : layout_->RangeRects(range)) {
      result.range_rects.push_back(ToHostRect(*node_, OffsetRect(rect, origin)));
    }
    return result;
  }

  bool SelectWord(Point position) {
    if (!node_ || !layout_) {
      return false;
    }
    const Point origin = TextOrigin(*node_);
    const detail::TextHit hit = layout_->HitTest({
        position.x - origin.x,
        position.y - origin.y,
    });
    const std::optional<TextRange> range = detail::WordRangeAt(editing_.value.text, hit.offset);
    if (!range.has_value()) {
      return false;
    }
    SetSelection({range->start, range->end, hit.affinity});
    return true;
  }

  bool ExtendSelection(Point position, bool start_handle) {
    if (!node_ || !layout_) {
      return false;
    }
    const TextRange range = editing_.value.selection.Range();
    if (range.IsCollapsed()) {
      return false;
    }
    const Point origin = TextOrigin(*node_);
    const detail::TextHit hit = layout_->HitTest({
        position.x - origin.x,
        position.y - origin.y,
    });
    SetSelection(
        start_handle ? TextSelection{range.end, std::min(hit.offset, range.end), hit.affinity}
                     : TextSelection{range.start, std::max(hit.offset, range.start), hit.affinity}
    );
    return true;
  }

  bool QuerySelectionGeometry(Rect& start, Rect& end) const {
    if (!node_ || !layout_) {
      return false;
    }
    const TextRange range = editing_.value.selection.Range();
    const Point origin = TextOrigin(*node_);
    start = ToHostRect(*node_, OffsetRect(layout_->CaretRect(range.start, TextAffinity::Downstream), origin));
    end = ToHostRect(*node_, OffsetRect(layout_->CaretRect(range.end, TextAffinity::Downstream), origin));
    return true;
  }

  Color HandleColor() const noexcept {
    Color color = style_.caret;
    color.alpha *= node_ == nullptr ? 1.0F : std::clamp(node_->PresentationOpacity(), 0.0F, 1.0F);
    return color;
  }

  TextInputKeyResult HandleTextKey(const KeyEvent& event) override {
    if (event.type != KeyEventType::Down) {
      return TextInputKeyResult::Unhandled;
    }
    if (configuration_.read_only) {
      return TextInputKeyResult::Unhandled;
    }
    if (event.modifiers.control || event.modifiers.alt || event.modifiers.meta) {
      return TextInputKeyResult::Unhandled;
    }

    switch (event.key) {
    case Key::ArrowLeft:
      MoveCaret(false, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::ArrowRight:
      MoveCaret(true, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::Home:
      MoveCaretTo(0, event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::End:
      MoveCaretTo(detail::Utf16Length(editing_.value.text).value_or(0), event.modifiers.shift);
      return TextInputKeyResult::Handled;
    case Key::Backspace:
      DeleteAdjacent(false);
      return TextInputKeyResult::Handled;
    case Key::Delete:
      DeleteAdjacent(true);
      return TextInputKeyResult::Handled;
    case Key::Enter:
      detail::EmitEvent<TextFieldEvents::Submitted>(event_bindings_);
      ResetCaretBlink();
      return TextInputKeyResult::Handled;
    case Key::Escape:
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
  void EnsureLayouts(PlatformHost& platform) {
    if (!layout_) {
      layout_ = platform.CreateTextLayout(editing_.value.text, style_.font_size);
    }
    if (!layout_) {
      throw std::logic_error("HuxerUI platform does not provide editable text layout");
    }
    if (placeholder_.empty()) {
      placeholder_layout_.reset();
      layout_placeholder_.clear();
    } else if (!placeholder_layout_ || layout_placeholder_ != placeholder_) {
      placeholder_layout_ = platform.CreateTextLayout(placeholder_, style_.font_size);
      layout_placeholder_ = placeholder_;
      if (!placeholder_layout_) {
        throw std::logic_error("HuxerUI platform does not provide editable text layout");
      }
    }
  }

  TextInputApplyResult ApplyCommands(const std::vector<TextInputCommand>& commands) {
    const detail::TextInputReductionResult reduced = detail::ReduceTextInputCommands(editing_, commands);
    if (reduced.status != detail::TextInputReductionStatus::Accepted) {
      return {
          TextInputResultCode::Rejected,
          TextInputSyncAction::None,
          false,
      };
    }
    if (!reduced.changed) {
      return {
          TextInputResultCode::Ok,
          TextInputSyncAction::None,
          false,
      };
    }

    const bool text_changed = reduced.state.value.text != editing_.value.text;
    editing_ = reduced.state;
    ++revision_;
    last_emitted_ = editing_.value;
    if (text_changed) {
      layout_.reset();
      if (platform_) {
        EnsureLayouts(*platform_);
      }
    }
    ResetCaretBlink();
    detail::EmitEvent<TextFieldEvents::Changed>(event_bindings_, editing_.value);
    return {
        TextInputResultCode::Ok,
        TextInputSyncAction::Update,
        true,
    };
  }

  void SetSelection(TextSelection selection) {
    TextInputCommand command;
    command.kind = TextInputCommandKind::SetSelection;
    command.selection_after = selection;
    ApplyCommands({command});
  }

  void MoveCaret(bool forward, bool extend) {
    if (!layout_) {
      return;
    }
    const TextRange selection = editing_.value.selection.Range();
    TextOffset offset = editing_.value.selection.active;
    if (!extend && !selection.IsCollapsed()) {
      offset = forward ? selection.end : selection.start;
    } else {
      offset = forward ? layout_->NextCaretOffset(offset) : layout_->PreviousCaretOffset(offset);
    }
    MoveCaretTo(offset, extend);
  }

  void MoveCaretTo(TextOffset offset, bool extend) {
    const TextOffset anchor = extend ? editing_.value.selection.anchor : offset;
    SetSelection({anchor, offset, TextAffinity::Downstream});
  }

  void DeleteAdjacent(bool forward) {
    if (!layout_) {
      return;
    }
    TextRange target = editing_.value.selection.Range();
    if (target.IsCollapsed()) {
      const TextOffset active = editing_.value.selection.active;
      target = forward ? TextRange{active, layout_->NextCaretOffset(active)}
                       : TextRange{layout_->PreviousCaretOffset(active), active};
    }
    if (target.IsCollapsed()) {
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
    const Rect content = ContentRect(node);
    const Size size = layout_ ? layout_->Measure() : Size{};
    const_cast<TextFieldClient*>(this)->UpdateScrollOffset(content);
    return {
        content.x - horizontal_scroll_offset_,
        content.y + std::max(0.0F, (content.height - size.height) * 0.5F),
    };
  }

  void UpdateScrollOffset(Rect content) {
    if (!layout_ || content.width <= 0.0F) {
      horizontal_scroll_offset_ = 0.0F;
      return;
    }
    const Rect caret = layout_->CaretRect(editing_.value.selection.active, editing_.value.selection.affinity);
    if (caret.x < horizontal_scroll_offset_) {
      horizontal_scroll_offset_ = std::max(0.0F, caret.x);
    } else if (caret.x + caret.width > horizontal_scroll_offset_ + content.width) {
      horizontal_scroll_offset_ = caret.x + caret.width - content.width;
    }
    const float maximum = std::max(0.0F, layout_->Measure().width + caret.width - content.width);
    horizontal_scroll_offset_ = std::clamp(horizontal_scroll_offset_, 0.0F, maximum);
  }

  static Rect OffsetRect(Rect rect, Point offset) {
    rect.x += offset.x;
    rect.y += offset.y;
    return rect;
  }

  static Rect ToHostRect(const detail::MountedNode& node, Rect rect) {
    return detail::TransformBounds(node.presentation.resolved_transform, rect);
  }

  void ResetCaretBlink() noexcept {
    caret_reset_pending_ = true;
    caret_visible_ = true;
  }

  detail::MountedNode* node_ = nullptr;
  PlatformHost* platform_ = nullptr;
  detail::EventBindings event_bindings_;
  TextInputConfiguration configuration_;
  TextFieldStyle style_;
  detail::TextFieldEditingState editing_;
  std::optional<TextEditingValue> last_emitted_;
  std::string placeholder_;
  std::string layout_placeholder_;
  std::unique_ptr<detail::TextLayout> layout_;
  std::unique_ptr<detail::TextLayout> placeholder_layout_;
  std::optional<TextOffset> pointer_anchor_;
  std::optional<double> caret_epoch_;
  TextInputSessionId session_id_ = 0;
  std::uint64_t revision_ = 0;
  float horizontal_scroll_offset_ = 0.0F;
  bool initialized_ = false;
  bool caret_reset_pending_ = true;
  bool caret_visible_ = true;
};

class TextFieldExtension final : public NodeExtension {
public:
  TextFieldExtension(MountedNode& node, const detail::TextFieldModifier& modifier)
      : client_(std::make_shared<TextFieldClient>()) {
    Update(node, modifier);
  }

  ~TextFieldExtension() override {
    if (node_) {
      client_->Detach(*node_);
    }
  }

  void Update(MountedNode& node, const detail::TextFieldModifier& modifier) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (node_ && node_ != &mounted) {
      client_->Detach(*node_);
    }
    node_ = &mounted;
    client_->Update(mounted, modifier);
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    return client_->AdvanceCaret(static_cast<const detail::MountedNode&>(node), frame);
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Frame().Contains(position);
  }

  void OnFocusChanged(MountedNode& node, bool focused) override {
    static_cast<void>(node);
    client_->FocusChanged(focused);
  }

  std::shared_ptr<TextInputClient> GetTextInputClient() noexcept override {
    return client_;
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    static_cast<void>(node);
    return client_->Pointer(event);
  }

  void Paint(const MountedNode& node, DisplayList& display_list) const override {
    client_->Paint(static_cast<const detail::MountedNode&>(node), display_list);
  }

  Size Measure(detail::MountedNode& node, PlatformHost& platform, Constraints constraints) {
    return client_->Measure(node, platform, constraints);
  }

  bool SelectWord(Point position) {
    return client_->SelectWord(position);
  }

  bool ExtendSelection(Point position, bool start_handle) {
    return client_->ExtendSelection(position, start_handle);
  }

  bool QuerySelectionGeometry(Rect& start, Rect& end) const {
    return client_->QuerySelectionGeometry(start, end);
  }

  Color HandleColor() const noexcept {
    return client_->HandleColor();
  }

private:
  detail::MountedNode* node_ = nullptr;
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

const ModifierDescriptor& TextFieldModifier::Descriptor() {
  return ModifierDescriptorFor<TextFieldModifier, TextFieldExtension>();
}

Size MeasureTextField(MountedNode& node, PlatformHost& platform, Constraints constraints) {
  return FindTextFieldExtension(node).Measure(node, platform, constraints);
}

bool SelectTextFieldWord(MountedNode& node, Point position) {
  const std::optional<Point> local = node.presentation.resolved_transform.Inverse(position);
  return local.has_value() && FindTextFieldExtension(node).SelectWord(*local);
}

bool ExtendTextFieldSelection(MountedNode& node, Point position, bool start_handle) {
  const std::optional<Point> local = node.presentation.resolved_transform.Inverse(position);
  return local.has_value() && FindTextFieldExtension(node).ExtendSelection(*local, start_handle);
}

bool QueryTextFieldSelectionGeometry(const MountedNode& node, Rect& start, Rect& end) {
  return FindTextFieldExtension(const_cast<MountedNode&>(node)).QuerySelectionGeometry(start, end);
}

Color TextFieldSelectionHandleColor(const MountedNode& node) {
  return FindTextFieldExtension(const_cast<MountedNode&>(node)).HandleColor();
}

} // namespace detail

TextField::TextField(TextEditingValue value)
    : detail::TypedView<TextField>(MakeTextFieldSpec()), value_(std::move(value)) {
  if (!detail::IsValidTextEditingValue(value_)) {
    throw std::invalid_argument("HuxerUI TextField value is invalid");
  }
  UpdateModifier();
  AddModifier(detail::MakeModifierSpec(detail::DefaultIndication{}));
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

TextField TextField::InputConfiguration(TextInputConfiguration configuration) && {
  ValidateConfiguration(configuration);
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
          }
      )
  );
}

} // namespace huxerui
