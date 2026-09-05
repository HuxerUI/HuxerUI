#include <huxerui/view.h>

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include <huxerui/environment.h>
#include <huxerui/presentation.h>
#include <huxerui/semantics.h>
#include <huxerui/state.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "indication_internal.h"
#include "runtime/mounted_node_internal.h"
#include "resources/resource_internal.h"
#include "runtime/semantics_internal.h"
#include "text/text_input_internal.h"

namespace huxerui {

namespace {

struct ComboBoxPopupState {
  State<std::uint64_t> recomposition_revision;
  std::optional<std::size_t> active_index;
  std::vector<bool> enabled;
  std::vector<bool> observed;
  std::vector<std::string> text;
  std::function<void(std::size_t, std::uint64_t)> commit;
  std::uint64_t generation = 0;
  bool reveal_active = false;
};

struct ComboBoxSession {
  detail::ComboBoxSuggestionSource source;
  ComboBoxStyle style;
  EventEmitter events;
  std::optional<PopupHandle> popup;
  std::optional<LayerId> layer;
  std::shared_ptr<ComboBoxPopupState> popup_state;
  ViewFactory empty_content;
  std::optional<std::uint64_t> dismissed_revision;
  std::uint64_t query_revision = 0;
  std::uint64_t generation = 0;
  float field_width = 0.0F;
  bool focused = false;
  bool composing = false;
};

struct ComboBoxConfiguration {
  static const detail::ModifierDescriptor& Descriptor();

  detail::ComboBoxSuggestionSource source;
  TextEditingValue value;
  StringVariant label;
  StringVariant placeholder;
  std::optional<ImageVariant> leading_icon;
  std::optional<ImageVariant> trailing_icon;
  std::optional<TextFieldVariant> variant;
  TextAlign text_align = TextAlign::Leading;
  std::optional<std::size_t> max_length;
  ValidationResult validation;
  TextInputConfiguration input_configuration;
  ViewFactory empty_content;
};

ComboBoxStyle ResolveComboBoxStyle(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(ComboBoxStyle))) {
    if (const auto* style = std::any_cast<ComboBoxStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI ComboBox style environment value has an invalid type");
  }
  return detail::DefaultComboBoxStyle(detail::ResolveThemeSpec(environment));
}

void ValidateComboBoxStyle(const ComboBoxStyle& style) {
  const auto valid_insets = [](const EdgeInsets& insets) {
    return std::isfinite(insets.top) && insets.top >= 0.0F && std::isfinite(insets.right) && insets.right >= 0.0F &&
           std::isfinite(insets.bottom) && insets.bottom >= 0.0F && std::isfinite(insets.left) && insets.left >= 0.0F;
  };
  const bool valid_shadow = std::isfinite(style.popup_shadow.offset.x) && std::isfinite(style.popup_shadow.offset.y) &&
                            std::isfinite(style.popup_shadow.blur_radius) && style.popup_shadow.blur_radius >= 0.0F &&
                            std::isfinite(style.popup_shadow.spread);
  const bool valid = valid_insets(style.item_padding) && valid_insets(style.popup_padding) && valid_shadow &&
                     std::isfinite(style.minimum_item_height) && style.minimum_item_height >= 0.0F &&
                     std::isfinite(style.maximum_popup_height) && style.maximum_popup_height > 0.0F &&
                     std::isfinite(style.popup_corner_radius) && style.popup_corner_radius >= 0.0F;
  if (!valid) {
    throw std::invalid_argument(
        "HuxerUI ComboBox geometry and shadow must be finite with positive popup height and non-negative extents"
    );
  }
}

void ValidateComboBoxInputConfiguration(const TextInputConfiguration& configuration) {
  if (configuration.multiline) {
    throw std::invalid_argument("HuxerUI ComboBox requires single-line input");
  }
  if (configuration.secure) {
    throw std::invalid_argument("HuxerUI ComboBox does not support secure input");
  }
  if (configuration.read_only) {
    throw std::invalid_argument("HuxerUI ComboBox is editable; use Select for read-only choices");
  }
  if (configuration.action == TextInputAction::Newline) {
    throw std::invalid_argument("HuxerUI ComboBox does not support the newline input action");
  }
}

bool HasIndependentPopupInteraction(const detail::MountedNode& node) {
  const bool handles_pointer = static_cast<bool>(node.activation) ||
                               detail::HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
                               detail::HasEventBinding<ViewEvents::Pointer>(node.event_bindings);
  if (handles_pointer || node.focusable) {
    return true;
  }
  return std::ranges::any_of(node.children, [](const std::unique_ptr<detail::MountedNode>& child) {
    return HasIndependentPopupInteraction(*child);
  });
}

bool HasActiveTextComposition(detail::MountedNode& node) {
  for (detail::NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    if (const std::shared_ptr<TextInputClient> client = entry.extension->GetTextInputClient()) {
      return client->State().composition.has_value();
    }
  }
  return false;
}

void ResizePopupState(const std::shared_ptr<ComboBoxPopupState>& state, std::size_t item_count) {
  state->enabled.assign(item_count, true);
  state->observed.assign(item_count, false);
  state->text.assign(item_count, {});
  if (state->active_index.has_value() && *state->active_index >= item_count) {
    state->active_index.reset();
    state->reveal_active = false;
  }
}

std::optional<std::size_t> FindEnabledEdge(const ComboBoxPopupState& state, bool reverse) {
  if (reverse) {
    for (std::size_t index = state.enabled.size(); index > 0; --index) {
      if (state.enabled[index - 1]) {
        return index - 1;
      }
    }
    return std::nullopt;
  }
  for (std::size_t index = 0; index < state.enabled.size(); ++index) {
    if (state.enabled[index]) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindNextEnabled(const ComboBoxPopupState& state, int direction) {
  if (!state.active_index.has_value()) {
    return FindEnabledEdge(state, direction < 0);
  }
  std::size_t index = *state.active_index;
  while ((direction < 0 && index > 0) || (direction > 0 && index + 1 < state.enabled.size())) {
    index = direction < 0 ? index - 1 : index + 1;
    if (state.enabled[index]) {
      return index;
    }
  }
  return std::nullopt;
}

void SetActiveSuggestion(const std::shared_ptr<ComboBoxPopupState>& state, std::optional<std::size_t> index) {
  if (!state || state->active_index == index || (index.has_value() && *index >= state->enabled.size())) {
    return;
  }
  state->active_index = index;
  if (state->recomposition_revision.IsValid()) {
    state->recomposition_revision = state->recomposition_revision.Get() + 1;
  }
  state->reveal_active = index.has_value();
}

bool HasPopupContent(const std::shared_ptr<ComboBoxSession>& session) {
  return session && (session->source.size > 0 || static_cast<bool>(session->empty_content));
}

void SetPopupLayer(const std::shared_ptr<ComboBoxSession>& session, std::optional<LayerId> layer) {
  const bool was_expanded = session->layer.has_value();
  session->layer = layer;
  const bool expanded = session->layer.has_value();
  if (was_expanded != expanded) {
    session->events.Emit<ComboBoxEvents::ExpandedChanged>(expanded);
  }
}

void DismissComboBox(const std::shared_ptr<ComboBoxSession>& session, bool suppress_reopen) {
  if (!session) {
    return;
  }
  if (suppress_reopen) {
    session->dismissed_revision = session->query_revision;
  }
  if (!session->layer.has_value()) {
    session->popup_state.reset();
    return;
  }
  const LayerId layer = *session->layer;
  if (session->popup.has_value()) {
    static_cast<void>(session->popup->Dismiss(layer));
  }
  session->popup_state.reset();
  SetPopupLayer(session, std::nullopt);
}

struct ComboBoxSuggestionBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  std::shared_ptr<ComboBoxPopupState> state;
  Color active_background;
  std::string text;
  std::size_t index = 0;
  std::uint64_t generation = 0;

  bool operator==(const ComboBoxSuggestionBehavior&) const = default;
};

class ComboBoxSuggestionBehaviorExtension final : public NodeExtension {
public:
  ComboBoxSuggestionBehaviorExtension(MountedNode& node, const ComboBoxSuggestionBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode&, const ComboBoxSuggestionBehavior& modifier) {
    state_ = modifier.state;
    if (active_background_ != modifier.active_background) {
      active_background_ = modifier.active_background;
      InvalidatePaint(PaintInvalidation::Content);
    }
    if (text_ != modifier.text) {
      text_ = modifier.text;
      InvalidateSemantics();
    }
    index_ = modifier.index;
    generation_ = modifier.generation;
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo&) override {
    if (state_ && index_ < state_->enabled.size()) {
      state_->enabled[index_] = node.IsEnabled();
      state_->observed[index_] = true;
      state_->text[index_] = text_;
    }
    const bool active = state_ && state_->active_index == index_;
    if (active_ != active) {
      active_ = active;
      InvalidatePaint(PaintInvalidation::Content);
      InvalidateSemantics();
    }
    return {};
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    if (node.IsEnabled() && event.type != HoverEventType::Leave) {
      SetActiveSuggestion(state_, index_);
    }
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down && !pointer_id_.has_value()) {
      pointer_id_ = event.pointer_id;
      pressed_generation_ = generation_;
      SetActiveSuggestion(state_, index_);
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      pointer_id_.reset();
      if (node.Bounds().Contains(event.position) && state_ && state_->commit) {
        state_->commit(index_, pressed_generation_);
      }
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      return PointerResult::Handled;
    }
    return PointerResult::Handled;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.role = SemanticRole::ListItem;
    semantics.label = text_;
    semantics.selected = active_;
    semantics.collection_item = SemanticCollectionItem{.index = index_};
    semantics.descendants = SemanticDescendantPolicy::Exclude;
    builder.SetOwner(std::move(semantics));
    builder.AddAction(0, SemanticActionKind::Activate);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0 || action.kind != SemanticActionKind::Activate || !state_ || !state_->commit) {
      return false;
    }
    state_->commit(index_, generation_);
    return true;
  }

  void PaintBehindContent(const MountedNode& node, PaintContext& context) const override {
    if (active_ && active_background_.alpha > 0.0F) {
      context.DrawRect(node.Bounds(), active_background_);
    }
  }

private:
  std::shared_ptr<ComboBoxPopupState> state_;
  Color active_background_;
  std::string text_;
  std::optional<std::int64_t> pointer_id_;
  std::size_t index_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t pressed_generation_ = 0;
  bool active_ = false;
};

const detail::ModifierDescriptor& ComboBoxSuggestionBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<ComboBoxSuggestionBehavior, ComboBoxSuggestionBehaviorExtension>();
}

struct ComboBoxPopupBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  std::shared_ptr<ComboBoxPopupState> state;
  ScrollController scroll_controller;

  bool operator==(const ComboBoxPopupBehavior&) const = default;
};

class ComboBoxPopupBehaviorExtension final : public NodeExtension {
public:
  ComboBoxPopupBehaviorExtension(MountedNode& node, const ComboBoxPopupBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode&, const ComboBoxPopupBehavior& modifier) {
    state_ = modifier.state;
    scroll_controller_ = modifier.scroll_controller;
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo&) override {
    if (HasIndependentPopupInteraction(static_cast<const detail::MountedNode&>(node))) {
      throw std::invalid_argument(
          "HuxerUI ComboBox popup content must not provide independent pointer activation or focusable descendants"
      );
    }
    if (!state_ || state_->enabled.empty() ||
        !std::ranges::all_of(state_->observed, [](bool value) { return value; })) {
      return state_ && !state_->enabled.empty() ? FrameResult{.needs_frame = true} : FrameResult{};
    }
    if (state_->active_index.has_value() && !state_->enabled[*state_->active_index]) {
      SetActiveSuggestion(state_, std::nullopt);
    }
    return {};
  }

  PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
    if (!state_ || !state_->reveal_active || !state_->active_index.has_value() || node.ChildCount() == 0 ||
        !scroll_controller_.IsConnected()) {
      return PaintInvalidation::None;
    }
    MountedNode& content = node.ChildAt(0);
    if (*state_->active_index >= content.ChildCount()) {
      return PaintInvalidation::None;
    }
    state_->reveal_active = false;
    const MountedNode& active = content.ChildAt(*state_->active_index);
    const float top = active.LayoutOffset().y;
    const float bottom = top + active.LayoutSize().height;
    const ScrollMetrics metrics = scroll_controller_.Metrics();
    if (top < metrics.offset) {
      static_cast<void>(scroll_controller_.ScrollTo(top));
    } else if (bottom > metrics.offset + metrics.viewport_extent) {
      static_cast<void>(scroll_controller_.ScrollTo(bottom - metrics.viewport_extent));
    }
    return PaintInvalidation::None;
  }

private:
  std::shared_ptr<ComboBoxPopupState> state_;
  ScrollController scroll_controller_;
};

const detail::ModifierDescriptor& ComboBoxPopupBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<ComboBoxPopupBehavior, ComboBoxPopupBehaviorExtension>();
}

View ComboBoxPopupContent(detail::ComboBoxSuggestionSource source, ComboBoxStyle style, ViewFactory empty_content,
                          const std::shared_ptr<ComboBoxPopupState>& state, float minimum_width) {
  ResizePopupState(state, source.size);
  state->recomposition_revision = UseState(std::uint64_t{0});
  static_cast<void>(state->recomposition_revision.Get());

  std::vector<View> items;
  items.reserve(source.size);
  for (std::size_t index = 0; index < source.size; ++index) {
    detail::ComboBoxSuggestionDeclaration declaration = source.factory(index);
    if (!declaration.content) {
      throw std::invalid_argument("HuxerUI ComboBox suggestion content factory must return a View");
    }
    const std::string text = UseString(declaration.text);
    View item = std::move(declaration.content).With(
        Frame{.min_height = style.minimum_item_height},
        Padding{style.item_padding},
        Foreground{style.foreground},
        ComboBoxSuggestionBehavior{
            .state = state,
            .active_background = style.active_item_background,
            .text = text,
            .index = index,
            .generation = state->generation,
        }
    );
    item = std::move(item).With(detail::DefaultIndication{style.item_indication});
    items.push_back(std::move(item));
  }

  View content;
  if (items.empty()) {
    content = empty_content ? empty_content() : View{};
    if (!content) {
      throw std::logic_error("HuxerUI ComboBox popup requires suggestions or empty content");
    }
  } else {
    content = Column {std::move(items)}.With(CrossAlign{CrossAxisAlignment::Stretch});
  }

  const ScrollController scroll_controller = UseScrollController();
  Semantics semantics;
  semantics.role = SemanticRole::List;
  semantics.collection = SemanticCollection{.item_count = source.size};
  return ScrollView(std::move(content))
      .ScrollAxis(Axis::Vertical)
      .Controller(scroll_controller)
      .With(
          Frame{.min_width = minimum_width, .max_height = style.maximum_popup_height},
          Padding{style.popup_padding},
          Background{style.popup_background},
          Foreground{style.foreground},
          CornerRadius{style.popup_corner_radius},
          ClipChildren{},
          style.popup_shadow,
          detail::BuiltInSemantics{std::move(semantics)},
          ComboBoxPopupBehavior{.state = state, .scroll_controller = scroll_controller}
      );
}

PopupFactory ComboBoxPopupContentFactory(const std::shared_ptr<ComboBoxSession>& session) {
  const detail::ComboBoxSuggestionSource source = session->source;
  const ComboBoxStyle style = session->style;
  const ViewFactory empty_content = session->empty_content;
  const std::shared_ptr<ComboBoxPopupState> state = session->popup_state;
  const float minimum_width = session->field_width;
  return [source, style, empty_content, state, minimum_width](PopupContext) {
    return ComboBoxPopupContent(source, style, empty_content, state, minimum_width);
  };
}

void ConfigurePopupState(const std::shared_ptr<ComboBoxSession>& session) {
  if (!session || !session->popup_state || !session->popup.has_value()) {
    return;
  }
  const std::weak_ptr<ComboBoxSession> weak_session = session;
  session->popup_state->generation = session->generation;
  ResizePopupState(session->popup_state, session->source.size);
  session->popup_state->commit = [weak_session](std::size_t index, std::uint64_t generation) {
    const std::shared_ptr<ComboBoxSession> session = weak_session.lock();
    if (!session || generation != session->generation || !session->popup_state ||
        index >= session->popup_state->text.size() || !session->popup_state->observed[index] ||
        !session->popup_state->enabled[index]) {
      return;
    }
    const TextEditingValue next = TextEditingValue::FromText(session->popup_state->text[index]);
    DismissComboBox(session, true);
    session->events.Emit<ComboBoxEvents::Selected>(index, next);
  };
}

void UpdateOpenPopup(const std::shared_ptr<ComboBoxSession>& session) {
  if (!session || !session->popup.has_value() || !session->layer.has_value()) {
    return;
  }
  if (!HasPopupContent(session)) {
    DismissComboBox(session, false);
    return;
  }
  ConfigurePopupState(session);
  if (!session->popup->Update(*session->layer, ComboBoxPopupContentFactory(session))) {
    session->popup_state.reset();
    SetPopupLayer(session, std::nullopt);
  }
}

void OpenComboBox(const std::shared_ptr<ComboBoxSession>& session, bool allow_unfocused = false) {
  if (!session || !session->popup.has_value() || session->layer.has_value() ||
      (!session->focused && !allow_unfocused) ||
      session->dismissed_revision == session->query_revision ||
      !HasPopupContent(session)) {
    return;
  }
  session->popup_state = std::make_shared<ComboBoxPopupState>();
  session->popup_state->generation = session->generation;
  ResizePopupState(session->popup_state, session->source.size);
  ConfigurePopupState(session);
  const std::weak_ptr<ComboBoxSession> weak_session = session;
  PopupOptions options;
  options.trap_focus = false;
  options.retain_anchor_focus = true;
  options.on_dismiss_request = [weak_session] {
    if (const std::shared_ptr<ComboBoxSession> session = weak_session.lock()) {
      DismissComboBox(session, true);
    }
  };
  SetPopupLayer(session, session->popup->Show(ComboBoxPopupContentFactory(session), std::move(options)));
}

void HandleEditedValue(const std::shared_ptr<ComboBoxSession>& session, const TextEditingValue& value) {
  if (!session) {
    return;
  }
  session->composing = value.composition.has_value();
  ++session->query_revision;
  session->dismissed_revision.reset();
  if (session->popup_state) {
    SetActiveSuggestion(session->popup_state, std::nullopt);
  }
  if (session->layer.has_value()) {
    UpdateOpenPopup(session);
  } else {
    OpenComboBox(session);
  }
  session->events.Emit<ComboBoxEvents::Changed>(value);
}

bool HandleComboBoxKey(const std::shared_ptr<ComboBoxSession>& session, const KeyEvent& event) {
  if (!session || event.type != KeyEventType::Down || event.modifiers.control || event.modifiers.meta ||
      event.modifiers.shift || session->composing) {
    return false;
  }
  if (event.modifiers.alt && event.key != Key::ArrowDown && event.key != Key::ArrowUp) {
    return false;
  }
  if (event.key == Key::Escape) {
    if (!session->layer.has_value()) {
      return false;
    }
    DismissComboBox(session, true);
    return true;
  }
  if (event.key != Key::ArrowDown && event.key != Key::ArrowUp && event.key != Key::Enter) {
    return false;
  }
  if (event.key == Key::Enter) {
    if (!session->popup_state || !session->popup_state->active_index.has_value()) {
      return false;
    }
    if (!event.repeat) {
      const std::size_t index = *session->popup_state->active_index;
      session->popup_state->commit(index, session->generation);
    }
    return true;
  }
  if (event.modifiers.alt && event.key == Key::ArrowUp) {
    if (session->layer.has_value()) {
      DismissComboBox(session, true);
      return true;
    }
    return false;
  }
  if (!session->layer.has_value()) {
    session->dismissed_revision.reset();
    OpenComboBox(session);
  }
  if (!session->popup_state || session->popup_state->enabled.empty()) {
    return false;
  }
  const std::optional<std::size_t> requested = FindNextEnabled(*session->popup_state,
                                                               event.key == Key::ArrowUp ? -1 : 1);
  if (requested.has_value()) {
    SetActiveSuggestion(session->popup_state, requested);
  }
  return true;
}

struct ComboBoxFieldBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  detail::ComboBoxSuggestionSource source;
  ComboBoxStyle style;
  EventEmitter events;
  PopupHandle popup;
  std::shared_ptr<ComboBoxSession> session;
  ViewFactory empty_content;
};

class ComboBoxFieldBehaviorExtension final : public NodeExtension {
public:
  ComboBoxFieldBehaviorExtension(MountedNode& node, const ComboBoxFieldBehavior& modifier) {
    Update(node, modifier);
  }

  ~ComboBoxFieldBehaviorExtension() override {
    DismissComboBox(session_, false);
  }

  void Update(MountedNode& node, const ComboBoxFieldBehavior& modifier) {
    session_ = modifier.session;
    session_->source = modifier.source;
    session_->style = modifier.style;
    session_->events = modifier.events;
    session_->popup = modifier.popup;
    session_->empty_content = modifier.empty_content;
    session_->focused = node.IsFocused();
    session_->composing = HasActiveTextComposition(static_cast<detail::MountedNode&>(node));
    ++session_->generation;

    if (session_->popup_state) {
      SetActiveSuggestion(session_->popup_state, std::nullopt);
    }
    if (session_->layer.has_value()) {
      UpdateOpenPopup(session_);
    } else {
      OpenComboBox(session_);
    }
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo&) override {
    session_->composing = HasActiveTextComposition(static_cast<detail::MountedNode&>(node));
    if (!node.IsEnabled() && session_ && session_->layer.has_value()) {
      DismissComboBox(session_, false);
    }
    const bool expanded = session_ && session_->layer.has_value();
    if (expanded_ != expanded) {
      expanded_ = expanded;
      InvalidateSemantics();
    }
    return {};
  }

  PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
    if (!session_) {
      return PaintInvalidation::None;
    }
    const float width = node.Bounds().width;
    if (session_->field_width != width) {
      session_->field_width = width;
      UpdateOpenPopup(session_);
    }
    return PaintInvalidation::None;
  }

  void OnFocusChanged(MountedNode&, bool focused, bool) override {
    if (!session_) {
      return;
    }
    session_->focused = focused;
    if (focused) {
      session_->dismissed_revision.reset();
      OpenComboBox(session_);
    } else {
      DismissComboBox(session_, false);
    }
    InvalidateSemantics();
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    const bool expanded = session_ && session_->layer.has_value();
    Semantics semantics;
    semantics.role = SemanticRole::ComboBox;
    semantics.expanded = expanded;
    semantics.read_only = false;
    builder.SetOwner(std::move(semantics));
    if (expanded) {
      builder.AddAction(0, SemanticActionKind::Collapse);
    } else if (HasPopupContent(session_)) {
      builder.AddAction(0, SemanticActionKind::Expand);
    }
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0 || !session_) {
      return false;
    }
    if (action.kind == SemanticActionKind::Expand && HasPopupContent(session_)) {
      session_->dismissed_revision.reset();
      OpenComboBox(session_, true);
      return true;
    }
    if (action.kind == SemanticActionKind::Collapse && session_->layer.has_value()) {
      DismissComboBox(session_, true);
      return true;
    }
    return false;
  }

private:
  std::shared_ptr<ComboBoxSession> session_;
  bool expanded_ = false;
};

const detail::ModifierDescriptor& ComboBoxFieldBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<ComboBoxFieldBehavior, ComboBoxFieldBehaviorExtension>();
}

std::function<View()> MakeComboBoxScopeFactory(ComboBoxConfiguration configuration) {
  return [configuration = std::move(configuration)]() -> View {
    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const ComboBoxStyle style = ResolveComboBoxStyle(environment);
    ValidateComboBoxStyle(style);
    const EventEmitter events = UseEvents();
    const PopupHandle popup = UsePopup();
    auto session_state = UseState(std::make_shared<ComboBoxSession>());
    const std::shared_ptr<ComboBoxSession> session = session_state.Get();

    TextField field(configuration.value);
    field = std::move(field)
                .Label(configuration.label)
                .Placeholder(configuration.placeholder)
                .Align(configuration.text_align)
                .Validation(configuration.validation)
                .InputConfiguration(configuration.input_configuration);
    if (configuration.leading_icon.has_value()) {
      field = std::move(field).LeadingIcon(*configuration.leading_icon);
    }
    const ImageVariant trailing_icon =
        configuration.trailing_icon.value_or(ImageVariant(images::dropdown_indicator));
    field = std::move(field).TrailingIcon(trailing_icon);
    if (configuration.variant.has_value()) {
      field = std::move(field).Variant(*configuration.variant);
    }
    if (configuration.max_length.has_value()) {
      field = std::move(field).MaxLength(*configuration.max_length);
    }

    field = std::move(field)
                .On<TextFieldEvents::Changed>(
                    [session](const TextEditingValue& value) { HandleEditedValue(session, value); }
                )
                .On<TextFieldEvents::Submitted>([session] {
                  DismissComboBox(session, true);
                  session->events.Emit<ComboBoxEvents::Submitted>();
                })
                .On<ViewEvents::KeyIntercept>(
                    [session](const KeyEvent& event) { return HandleComboBoxKey(session, event); }
                )
                .With(
                    popup.Anchor(),
                    ComboBoxFieldBehavior{
                        .source = configuration.source,
                        .style = style,
                        .events = events,
                        .popup = popup,
                        .session = session,
                        .empty_content = configuration.empty_content,
                    }
                );
    return field;
  };
}

const detail::ModifierDescriptor& ComboBoxConfiguration::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& configuration = *static_cast<const ComboBoxConfiguration*>(modifier.value.get());
        spec.scope_factory = MakeComboBoxScopeFactory(configuration);
      },
      nullptr,
      nullptr,
      false,
      nullptr,
      nullptr,
  };
  return descriptor;
}

} // namespace

namespace detail {

std::shared_ptr<ViewSpec> MakeComboBoxSpec(ComboBoxSuggestionSource source, TextEditingValue value) {
  if (!source.factory) {
    throw std::invalid_argument("HuxerUI ComboBox suggestion factory must not be empty");
  }
  if (!IsValidTextEditingValue(value)) {
    throw std::invalid_argument("HuxerUI ComboBox value is invalid");
  }
  return std::make_shared<ViewSpec>(NodeKind::Scope);
}

} // namespace detail

ComboBox ComboBox::Label(StringVariant value) && {
  label_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::Placeholder(StringVariant value) && {
  placeholder_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::LeadingIcon(ImageVariant icon) && {
  detail::ValidateImageVariant(icon);
  leading_icon_ = std::move(icon);
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::TrailingIcon(ImageVariant icon) && {
  detail::ValidateImageVariant(icon);
  trailing_icon_ = std::move(icon);
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::Variant(TextFieldVariant value) && {
  variant_ = value;
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::Align(TextAlign value) && {
  text_align_ = value;
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::MaxLength(std::size_t value) && {
  max_length_ = value;
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::Validation(ValidationResult value) && {
  validation_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::InputConfiguration(TextInputConfiguration configuration) && {
  ValidateComboBoxInputConfiguration(configuration);
  configuration_ = configuration;
  UpdateModifier();
  return std::move(*this);
}

ComboBox ComboBox::EmptyContent(std::function<View()> content) && {
  if (!content) {
    throw std::invalid_argument("HuxerUI ComboBox empty content factory must not be empty");
  }
  empty_content_ = std::move(content);
  UpdateModifier();
  return std::move(*this);
}

void ComboBox::UpdateModifier() {
  SetModifier(detail::MakeModifierSpec(ComboBoxConfiguration{
      source_, value_, label_, placeholder_, leading_icon_, trailing_icon_, variant_, text_align_, max_length_,
      validation_, configuration_, empty_content_,
  }));
}

} // namespace huxerui
