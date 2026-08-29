#include <huxerui/view.h>

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
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
#include <huxerui/vector.h>

#include "internal.h"
#include "indication_internal.h"

namespace huxerui {

namespace {

struct SelectPopupState {
  std::size_t selected_index = 0;
  std::size_t active_index = 0;
  State<std::optional<std::size_t>> active_index_state;
  std::optional<std::uint64_t> active_identity;
  bool has_active = true;
  bool reveal_active = true;
  std::vector<bool> enabled;
  std::vector<bool> observed;
  std::vector<std::uint64_t> identities;
  std::function<void(std::size_t)> commit;
};

struct SelectSession {
  std::optional<LayerId> layer;
  std::shared_ptr<SelectPopupState> popup_state;
  std::size_t selected_index = 0;
  float trigger_width = 0.0F;
};

struct SelectConfiguration {
  static const detail::ModifierDescriptor& Descriptor();

  detail::ViewItemSource source;
  std::size_t selected_index = 0;
  StringVariant label;
  ValidationResult validation;
};

std::optional<std::string> EffectiveSemanticLabel(const detail::MountedNode& node) {
  std::optional<std::string> label = node.component_semantics.label;
  if (node.author_semantics.has_value() && node.author_semantics->label.has_value()) {
    label = node.author_semantics->label;
  }
  return label;
}

bool HasIndependentSelectInteraction(const detail::MountedNode& node, bool is_choice_root = true) {
  const bool handles_pointer = static_cast<bool>(node.activation) ||
                               detail::HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
                               detail::HasEventBinding<ViewEvents::PointerDown>(node.event_bindings) ||
                               detail::HasEventBinding<ViewEvents::PointerMove>(node.event_bindings) ||
                               detail::HasEventBinding<ViewEvents::PointerUp>(node.event_bindings) ||
                               detail::HasEventBinding<ViewEvents::PointerCancel>(node.event_bindings);
  if (handles_pointer || (!is_choice_root && node.focusable)) {
    return true;
  }
  return std::ranges::any_of(node.children, [](const std::unique_ptr<detail::MountedNode>& child) {
    return HasIndependentSelectInteraction(*child, false);
  });
}

SelectStyle ResolveSelectStyle(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(SelectStyle))) {
    if (const auto* style = std::any_cast<SelectStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI Select style environment value has an invalid type");
  }
  return detail::DefaultSelectStyle(detail::ResolveThemeSpec(environment));
}

void ValidateSelectStyle(const SelectStyle& style) {
  const auto valid_insets = [](const EdgeInsets& insets) {
    return std::isfinite(insets.top) && insets.top >= 0.0F && std::isfinite(insets.right) && insets.right >= 0.0F &&
           std::isfinite(insets.bottom) && insets.bottom >= 0.0F && std::isfinite(insets.left) && insets.left >= 0.0F;
  };
  const bool valid_shadow = std::isfinite(style.popup_shadow.offset.x) && std::isfinite(style.popup_shadow.offset.y) &&
                            std::isfinite(style.popup_shadow.blur_radius) && style.popup_shadow.blur_radius >= 0.0F &&
                            std::isfinite(style.popup_shadow.spread);
  const bool valid = valid_insets(style.trigger_padding) && valid_insets(style.item_padding) &&
                     valid_insets(style.popup_padding) && valid_shadow && std::isfinite(style.content_spacing) &&
                     style.content_spacing >= 0.0F && std::isfinite(style.minimum_width) &&
                     style.minimum_width >= 0.0F &&
                     std::isfinite(style.minimum_height) && style.minimum_height >= 0.0F &&
                     std::isfinite(style.minimum_item_height) && style.minimum_item_height >= 0.0F &&
                     std::isfinite(style.maximum_popup_height) && style.maximum_popup_height > 0.0F &&
                     std::isfinite(style.indicator_size) && style.indicator_size >= 0.0F &&
                     std::isfinite(style.validation_spacing) && style.validation_spacing >= 0.0F &&
                     std::isfinite(style.corner_radius) && style.corner_radius >= 0.0F &&
                     std::isfinite(style.popup_corner_radius) && style.popup_corner_radius >= 0.0F &&
                     std::isfinite(style.border_width) && style.border_width >= 0.0F;
  if (!valid) {
    throw std::invalid_argument(
        "HuxerUI Select geometry and shadow must be finite with positive popup height and non-negative extents"
    );
  }
}

const VectorAsset& SelectIndicatorAsset() {
  static const VectorAsset asset = VectorAsset::Create({16.0F, 16.0F}, [](VectorBuilder& builder) {
    Path path;
    path.MoveTo({3.5F, 6.0F}).LineTo({8.0F, 10.5F}).LineTo({12.5F, 6.0F});
    builder.StrokePath(std::move(path), Color::Black(), 1.5F, StrokeCap::Round, StrokeJoin::Round);
  });
  return asset;
}

void ResizePopupState(const std::shared_ptr<SelectPopupState>& state, std::size_t item_count) {
  state->enabled.assign(item_count, true);
  state->observed.assign(item_count, false);
  state->identities.assign(item_count, 0);
  if (state->active_index >= item_count) {
    state->active_index = state->selected_index;
    if (state->active_index_state.IsValid()) {
      state->active_index_state = std::optional{state->active_index};
    }
    state->active_identity.reset();
  }
}

std::optional<std::size_t> FindEnabledEdge(const SelectPopupState& state, bool reverse) {
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

std::optional<std::size_t> FindNextEnabled(const SelectPopupState& state, int direction) {
  if (state.enabled.empty()) {
    return std::nullopt;
  }
  std::size_t index = state.active_index;
  while ((direction < 0 && index > 0) || (direction > 0 && index + 1 < state.enabled.size())) {
    index = direction < 0 ? index - 1 : index + 1;
    if (state.enabled[index]) {
      return index;
    }
  }
  return std::nullopt;
}

void SetActiveChoice(const std::shared_ptr<SelectPopupState>& state, std::size_t index) {
  if (!state || index >= state->identities.size()) {
    return;
  }
  state->active_index = index;
  state->has_active = true;
  state->active_identity = state->identities[index] == 0 ? std::nullopt
                                                         : std::optional{state->identities[index]};
  if (state->active_index_state.IsValid()) {
    state->active_index_state = std::optional{index};
  }
  state->reveal_active = true;
}

void DismissSelect(const std::shared_ptr<SelectSession>& session, const PopupHandle& popup) {
  if (!session->layer.has_value()) {
    return;
  }
  const LayerId layer = *session->layer;
  session->layer.reset();
  session->popup_state.reset();
  static_cast<void>(popup.Dismiss(layer));
}

struct SelectDisplayContent {
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const SelectDisplayContent&) const = default;
};

const detail::ModifierDescriptor& SelectDisplayContent::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec&, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        spec.pointer_events_enabled = false;
        spec.focusable = false;
      },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<SelectDisplayContent>(),
      nullptr,
  };
  return descriptor;
}

struct SelectItemBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  std::shared_ptr<SelectPopupState> state;
  Color active_background;
  Color selected_background;
  std::size_t index = 0;

  bool operator==(const SelectItemBehavior&) const = default;
};

class SelectItemBehaviorExtension final : public NodeExtension {
public:
  SelectItemBehaviorExtension(MountedNode& node, const SelectItemBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode&, const SelectItemBehavior& modifier) {
    state_ = modifier.state;
    if (active_background_ != modifier.active_background || selected_background_ != modifier.selected_background) {
      active_background_ = modifier.active_background;
      selected_background_ = modifier.selected_background;
      InvalidatePaint(PaintInvalidation::Content);
    }
    index_ = modifier.index;
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo&) override {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    if (HasIndependentSelectInteraction(mounted)) {
      throw std::invalid_argument(
          "HuxerUI Select item content must not provide independent pointer activation or focusable descendants"
      );
    }
    const std::optional<std::string> label = EffectiveSemanticLabel(mounted);
    if (!label.has_value() || label->empty()) {
      throw std::invalid_argument("HuxerUI Select item requires a non-empty semantic label on its root View");
    }
    if (label_ != label) {
      label_ = label;
      InvalidateSemantics();
    }
    if (state_ && index_ < state_->enabled.size()) {
      state_->enabled[index_] = node.IsEnabled();
      state_->observed[index_] = true;
      state_->identities[index_] = mounted.identity;
      if (state_->has_active && !state_->active_identity.has_value() && state_->active_index == index_) {
        state_->active_identity = mounted.identity;
      }
    }
    const bool active = state_ && state_->has_active && state_->active_identity == mounted.identity;
    const bool selected = state_ && state_->selected_index == index_;
    if (active_ != active || selected_ != selected) {
      active_ = active;
      selected_ = selected;
      InvalidatePaint(PaintInvalidation::Content);
    }
    return {};
  }

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (!state_ || !node.IsEnabled() || event.type != KeyEventType::Down || event.modifiers.alt ||
        event.modifiers.control || event.modifiers.meta) {
      return;
    }
    std::optional<std::size_t> requested;
    if (event.key == Key::ArrowUp) {
      requested = FindNextEnabled(*state_, -1);
    } else if (event.key == Key::ArrowDown) {
      requested = FindNextEnabled(*state_, 1);
    } else if (event.key == Key::Home) {
      requested = FindEnabledEdge(*state_, false);
    } else if (event.key == Key::End) {
      requested = FindEnabledEdge(*state_, true);
    } else if ((event.key == Key::Enter || event.key == Key::Space) && !event.repeat &&
               state_->active_index < state_->enabled.size() && state_->enabled[state_->active_index]) {
      if (state_->commit) {
        state_->commit(state_->active_index);
      }
      return;
    }
    if (requested.has_value()) {
      SetActiveChoice(state_, *requested);
    }
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down && !pointer_id_.has_value()) {
      pointer_id_ = event.pointer_id;
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      pointer_id_.reset();
      if (node.Bounds().Contains(event.position) && state_ && state_->commit) {
        state_->commit(index_);
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
    if (label_.has_value()) {
      semantics.label = *label_;
    }
    semantics.selected = selected_;
    semantics.collection_item = SemanticCollectionItem{.index = index_};
    semantics.descendants = SemanticDescendantPolicy::Exclude;
    builder.SetOwner(std::move(semantics));
    builder.AddAction(0, SemanticActionKind::Activate);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0 || action.kind != SemanticActionKind::Activate || !state_ || !state_->commit) {
      return false;
    }
    state_->commit(index_);
    return true;
  }

  void PaintBehindContent(const MountedNode& node, PaintContext& context) const override {
    Color color = Color::Transparent();
    if (active_) {
      color = active_background_;
    } else if (selected_) {
      color = selected_background_;
    }
    if (color.alpha > 0.0F) {
      context.DrawRect(node.Bounds(), color);
    }
  }

private:
  std::shared_ptr<SelectPopupState> state_;
  Color active_background_;
  Color selected_background_;
  std::optional<std::string> label_;
  std::optional<std::int64_t> pointer_id_;
  std::size_t index_ = 0;
  bool active_ = false;
  bool selected_ = false;
};

const detail::ModifierDescriptor& SelectItemBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<SelectItemBehavior, SelectItemBehaviorExtension>();
}

struct SelectPopupBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  std::shared_ptr<SelectPopupState> state;
  ScrollController scroll_controller;

  bool operator==(const SelectPopupBehavior&) const = default;
};

class SelectPopupBehaviorExtension final : public NodeExtension {
public:
  SelectPopupBehaviorExtension(MountedNode& node, const SelectPopupBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode&, const SelectPopupBehavior& modifier) {
    state_ = modifier.state;
    scroll_controller_ = modifier.scroll_controller;
  }

  FrameResult OnFrame(MountedNode&, const FrameInfo&) override {
    if (!state_ || state_->enabled.empty()) {
      return {};
    }
    if (!std::ranges::all_of(state_->observed, [](bool value) { return value; })) {
      return {.needs_frame = true};
    }
    if (state_->active_identity.has_value()) {
      const auto found = std::ranges::find(state_->identities, *state_->active_identity);
      if (found != state_->identities.end()) {
        const std::size_t index = static_cast<std::size_t>(std::distance(state_->identities.begin(), found));
        if (state_->active_index != index) {
          SetActiveChoice(state_, index);
        }
      } else {
        state_->active_identity.reset();
      }
    }
    if (!state_->active_identity.has_value() || !state_->enabled[state_->active_index]) {
      std::optional<std::size_t> fallback;
      if (state_->selected_index < state_->enabled.size() && state_->enabled[state_->selected_index]) {
        fallback = state_->selected_index;
      } else {
        fallback = FindEnabledEdge(*state_, false);
      }
      if (fallback.has_value()) {
        SetActiveChoice(state_, *fallback);
      } else {
        state_->active_identity.reset();
        state_->has_active = false;
        if (state_->active_index_state.IsValid()) {
          state_->active_index_state = std::optional<std::size_t>{};
        }
      }
    }
    return {};
  }

  PaintInvalidation PrepareGeometry(MountedNode& node) override {
    if (!state_ || !state_->reveal_active || node.ChildCount() == 0 || !scroll_controller_.IsConnected()) {
      return PaintInvalidation::None;
    }
    MountedNode& content = node.ChildAt(0);
    if (state_->active_index >= content.ChildCount()) {
      return PaintInvalidation::None;
    }
    state_->reveal_active = false;
    const MountedNode& active = content.ChildAt(state_->active_index);
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
  std::shared_ptr<SelectPopupState> state_;
  ScrollController scroll_controller_;
};

const detail::ModifierDescriptor& SelectPopupBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<SelectPopupBehavior, SelectPopupBehaviorExtension>();
}

View SelectPopupContent(detail::ViewItemSource source, SelectStyle style,
                        const std::shared_ptr<SelectPopupState>& state) {
  ResizePopupState(state, source.size);
  const State<std::optional<std::size_t>> active_index_state = UseState(std::optional{state->active_index});
  state->active_index_state = active_index_state;
  const std::optional<std::size_t> active_index = active_index_state.Get();
  state->has_active = active_index.has_value();
  if (active_index.has_value()) {
    state->active_index = *active_index;
  }
  const ScrollController scroll_controller = UseScrollController();
  std::vector<View> items;
  items.reserve(source.size);
  for (std::size_t index = 0; index < source.size; ++index) {
    View item = source.factory(index);
    if (!item) {
      throw std::invalid_argument("HuxerUI Select item factory must return a View");
    }
    item = std::move(item).With(
        Frame{.min_height = style.minimum_item_height},
        Padding{style.item_padding},
        Foreground{style.foreground},
        Focusable{active_index == index},
        SelectItemBehavior{
            .state = state,
            .active_background = style.active_item_background,
            .selected_background = style.selected_item_background,
            .index = index,
        }
    );
    item = std::move(item).With(detail::DefaultIndication{style.item_indication});
    items.push_back(std::move(item));
  }

  Semantics semantics;
  semantics.role = SemanticRole::List;
  semantics.collection = SemanticCollection{.item_count = source.size};
  return ScrollView(Column {std::move(items)}.With(CrossAlign{CrossAxisAlignment::Stretch}))
      .ScrollAxis(Axis::Vertical)
      .Controller(scroll_controller)
      .With(
          Frame{
              .min_width = style.minimum_width,
              .max_height = style.maximum_popup_height,
          },
          Padding{style.popup_padding},
          Background{style.popup_background},
          Foreground{style.foreground},
          CornerRadius{style.popup_corner_radius},
          ClipChildren{},
          style.popup_shadow,
          detail::BuiltInSemantics{std::move(semantics)},
          SelectPopupBehavior{.state = state, .scroll_controller = scroll_controller}
      );
}

struct SelectTriggerBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  detail::ViewItemSource source;
  SelectStyle style;
  EventEmitter events;
  PopupHandle popup;
  std::shared_ptr<SelectSession> session;
  std::string accessible_label;
  std::size_t selected_index = 0;
  bool invalid = false;
  std::string validation_message;

  bool operator==(const SelectTriggerBehavior&) const = default;
};

class SelectTriggerBehaviorExtension final : public NodeExtension {
public:
  SelectTriggerBehaviorExtension(MountedNode& node, const SelectTriggerBehavior& modifier) {
    Update(node, modifier);
  }

  ~SelectTriggerBehaviorExtension() override {
    if (session_ && popup_.has_value()) {
      DismissSelect(session_, *popup_);
    }
  }

  void Update(MountedNode&, const SelectTriggerBehavior& modifier) {
    source_ = modifier.source;
    style_ = modifier.style;
    events_ = modifier.events;
    popup_ = modifier.popup;
    session_ = modifier.session;
    selected_index_ = modifier.selected_index;
    accessible_label_ = modifier.accessible_label;
    invalid_ = modifier.invalid;
    validation_message_ = modifier.validation_message;
    session_->selected_index = selected_index_;
    UpdateOpenPopup();
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo&) override {
    if (!node.IsEnabled() && session_ && popup_.has_value() && session_->layer.has_value()) {
      pointer_id_.reset();
      DismissSelect(session_, *popup_);
      InvalidateSemantics();
    }
    if (node.ChildCount() == 0) {
      throw std::logic_error("HuxerUI Select trigger requires selected content");
    }
    const auto& content = static_cast<const detail::MountedNode&>(node.ChildAt(0));
    if (HasIndependentSelectInteraction(content)) {
      throw std::invalid_argument(
          "HuxerUI Select item content must not provide independent pointer activation or focusable descendants"
      );
    }
    const std::optional<std::string> label = EffectiveSemanticLabel(content);
    if (!label.has_value() || label->empty()) {
      throw std::invalid_argument("HuxerUI Select item requires a non-empty semantic label on its root View");
    }
    if (label_ != label) {
      label_ = label;
      InvalidateSemantics();
    }
    return {};
  }

  PaintInvalidation PrepareGeometry(MountedNode& node) override {
    if (!session_) {
      return PaintInvalidation::None;
    }
    const float width = node.Bounds().width;
    if (session_->trigger_width != width) {
      session_->trigger_width = width;
      UpdateOpenPopup();
    }
    return PaintInvalidation::None;
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down && !pointer_id_.has_value()) {
      pointer_id_ = event.pointer_id;
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      pointer_id_.reset();
      if (node.Bounds().Contains(event.position)) {
        Toggle();
      }
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      return PointerResult::Handled;
    }
    return PointerResult::Handled;
  }

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || event.type != KeyEventType::Down || event.repeat || event.modifiers.alt ||
        event.modifiers.control || event.modifiers.meta) {
      return;
    }
    if (event.key == Key::Enter || event.key == Key::Space || event.key == Key::ArrowDown ||
        event.key == Key::ArrowUp) {
      Open();
    }
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.role = SemanticRole::ComboBox;
    if (!accessible_label_.empty()) {
      semantics.label = accessible_label_;
    }
    if (label_.has_value()) {
      semantics.value = *label_;
    }
    const bool expanded = session_ && session_->layer.has_value();
    semantics.expanded = expanded;
    semantics.invalid = invalid_;
    semantics.read_only = true;
    if (!validation_message_.empty()) {
      semantics.error = validation_message_;
    }
    semantics.descendants = SemanticDescendantPolicy::Exclude;
    builder.SetOwner(std::move(semantics));
    builder.AddAction(0, SemanticActionKind::Activate);
    builder.AddAction(0, expanded ? SemanticActionKind::Collapse : SemanticActionKind::Expand);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0) {
      return false;
    }
    if (action.kind == SemanticActionKind::Activate) {
      Toggle();
      return true;
    }
    if (action.kind == SemanticActionKind::Expand) {
      Open();
      return true;
    }
    if (action.kind == SemanticActionKind::Collapse && session_ && popup_.has_value() && session_->layer.has_value()) {
      DismissSelect(session_, *popup_);
      return true;
    }
    return false;
  }

private:
  void ConfigurePopupState() {
    if (!session_ || !session_->popup_state || !popup_.has_value()) {
      return;
    }
    std::weak_ptr<SelectSession> weak_session = session_;
    const PopupHandle popup = *popup_;
    const EventEmitter events = events_;
    session_->popup_state->selected_index = selected_index_;
    ResizePopupState(session_->popup_state, source_.size);
    session_->popup_state->commit = [weak_session, popup, events](std::size_t index) {
      const std::shared_ptr<SelectSession> session = weak_session.lock();
      if (!session) {
        return;
      }
      const bool changed = index != session->selected_index;
      DismissSelect(session, popup);
      if (changed) {
        events.Emit<SelectEvents::Changed>(index);
      }
    };
  }

  PopupFactory PopupContentFactory() const {
    const detail::ViewItemSource source = source_;
    SelectStyle style = style_;
    style.minimum_width = std::max(style.minimum_width, session_ ? session_->trigger_width : 0.0F);
    const std::shared_ptr<SelectPopupState> state = session_->popup_state;
    return [source, style, state](PopupContext) { return SelectPopupContent(source, style, state); };
  }

  void UpdateOpenPopup() {
    if (!session_ || !popup_.has_value() || !session_->layer.has_value()) {
      return;
    }
    ConfigurePopupState();
    if (!popup_->Update(*session_->layer, PopupContentFactory())) {
      session_->layer.reset();
      session_->popup_state.reset();
    }
  }

  void Open() {
    if (!session_ || !popup_.has_value() || session_->layer.has_value()) {
      return;
    }
    session_->popup_state = std::make_shared<SelectPopupState>();
    session_->popup_state->selected_index = selected_index_;
    session_->popup_state->active_index = selected_index_;
    ResizePopupState(session_->popup_state, source_.size);
    ConfigurePopupState();
    std::weak_ptr<SelectSession> weak_session = session_;
    const PopupHandle popup = *popup_;
    PopupOptions options;
    options.trap_focus = true;
    options.on_dismiss_request = [weak_session, popup] {
      if (const std::shared_ptr<SelectSession> session = weak_session.lock()) {
        DismissSelect(session, popup);
      }
    };
    session_->layer = popup_->Show(PopupContentFactory(), std::move(options));
    InvalidateSemantics();
  }

  void Toggle() {
    if (session_ && popup_.has_value() && session_->layer.has_value()) {
      DismissSelect(session_, *popup_);
      InvalidateSemantics();
    } else {
      Open();
    }
  }

  detail::ViewItemSource source_;
  SelectStyle style_;
  EventEmitter events_;
  std::optional<PopupHandle> popup_;
  std::shared_ptr<SelectSession> session_;
  std::optional<std::string> label_;
  std::string accessible_label_;
  std::optional<std::int64_t> pointer_id_;
  std::size_t selected_index_ = 0;
  bool invalid_ = false;
  std::string validation_message_;
};

const detail::ModifierDescriptor& SelectTriggerBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<SelectTriggerBehavior, SelectTriggerBehaviorExtension>();
}

std::function<View()> MakeSelectScopeFactory(SelectConfiguration configuration) {
  return [configuration = std::move(configuration)]() -> View {
    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const SelectStyle style = ResolveSelectStyle(environment);
    ValidateSelectStyle(style);
    const EventEmitter events = UseEvents();
    const PopupHandle popup = UsePopup();
    auto session_state = UseState(std::make_shared<SelectSession>());
    const std::shared_ptr<SelectSession> session = session_state.Get();
    const std::string validation_message = UseString(configuration.validation.message);
    const std::string accessible_label = UseString(configuration.label);

    View selected = configuration.source.factory(configuration.selected_index);
    if (!selected) {
      throw std::invalid_argument("HuxerUI Select item factory must return a View");
    }
    selected = std::move(selected).With(SelectDisplayContent{}, Grow{});

    Frame trigger_frame;
    trigger_frame.min_width = style.minimum_width;
    trigger_frame.min_height = style.minimum_height;
    Frame indicator_frame;
    indicator_frame.width = style.indicator_size;
    indicator_frame.height = style.indicator_size;
    View indicator = Image(SelectIndicatorAsset())
                         .Fit(ImageFit::Contain)
                         .Tint(style.indicator)
                         .With(indicator_frame, detail::BuiltInSemantics{Semantics{.hidden = true}});

    View trigger = Row {
      std::move(selected),
      std::move(indicator),
    }.With(
        trigger_frame,
        Padding{style.trigger_padding},
        Spacing{style.content_spacing},
        CrossAlign{CrossAxisAlignment::Center},
        Background{style.background},
        Border{
            configuration.validation.IsInvalid() ? style.validation_error : style.border,
            style.border_width,
        },
        CornerRadius{style.corner_radius},
        ClipChildren{},
        Foreground{style.foreground}
    );
    trigger = std::move(trigger).With(detail::DefaultIndication{style.indication});
    trigger = std::move(trigger).With(
        Focusable{},
        popup.Anchor(),
        SelectTriggerBehavior{
            .source = configuration.source,
            .style = style,
            .events = events,
            .popup = popup,
            .session = session,
            .accessible_label = accessible_label,
            .selected_index = configuration.selected_index,
            .invalid = configuration.validation.IsInvalid(),
            .validation_message = validation_message,
        }
    );

    std::vector<View> content;
    content.push_back(std::move(trigger));
    if (!validation_message.empty() &&
        (configuration.validation.status == ValidationStatus::Invalid ||
         configuration.validation.status == ValidationStatus::Pending)) {
      content.push_back(Text(validation_message).Style(style.validation_text_style));
    }
    return Column {std::move(content)}.With(
        Spacing{style.validation_spacing}, CrossAlign{CrossAxisAlignment::Stretch}
    );
  };
}

const detail::ModifierDescriptor& SelectConfiguration::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& configuration = *static_cast<const SelectConfiguration*>(modifier.value.get());
        spec.scope_factory = MakeSelectScopeFactory(configuration);
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

std::shared_ptr<ViewSpec> MakeSelectSpec(ViewItemSource source, std::size_t selected_index) {
  if (source.size == 0) {
    throw std::invalid_argument("HuxerUI Select requires at least one item");
  }
  if (selected_index >= source.size) {
    throw std::invalid_argument("HuxerUI Select selected index is out of range");
  }
  if (!source.factory) {
    throw std::invalid_argument("HuxerUI Select item factory must not be empty");
  }

  return std::make_shared<ViewSpec>(NodeKind::Scope);
}

} // namespace detail

Select Select::Label(StringVariant value) && {
  label_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

Select Select::Validation(ValidationResult value) && {
  validation_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

void Select::UpdateModifier() {
  SetModifier(detail::MakeModifierSpec(SelectConfiguration{source_, selected_index_, label_, validation_}));
}

} // namespace huxerui
