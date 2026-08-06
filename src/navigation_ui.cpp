#include <huxerui/navigation.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <huxerui/environment.h>
#include <huxerui/theme.h>

#include "internal.h"
#include "resource_internal.h"

namespace huxerui::detail {

namespace {

template <class IconVariant> std::optional<ResolvedImageAsset> ResolveNavigationIcon(IconVariant& value) {
  return std::visit(
      [](auto& icon) -> std::optional<ResolvedImageAsset> {
        using Icon = std::decay_t<decltype(icon)>;
        if constexpr (std::same_as<Icon, std::monostate>) {
          return std::nullopt;
        } else if constexpr (std::same_as<Icon, ImageResource>) {
          return UseImageResource(std::move(icon));
        } else {
          if (!icon.HasValue()) {
            throw std::invalid_argument("HuxerUI NavigationItem icon must not be empty");
          }
          return ResolvedImageAsset{std::move(icon)};
        }
      },
      value
  );
}

} // namespace

struct NavigationItemAccess {
  static std::string ResolveLabel(NavigationItem& item) {
    return ResolveStringVariant(std::move(item.label_));
  }

  static std::optional<ResolvedImageAsset> ResolveIcon(NavigationItem& item) {
    return ResolveNavigationIcon(item.icon_);
  }

  static std::optional<ResolvedImageAsset> ResolveSelectedIcon(NavigationItem& item) {
    return ResolveNavigationIcon(item.selected_icon_);
  }

  static bool IsEnabled(const NavigationItem& item) noexcept {
    return item.enabled_;
  }
};

struct ResolvedNavigationItem {
  std::string label;
  std::optional<ResolvedImageAsset> icon;
  std::optional<ResolvedImageAsset> selected_icon;
  bool enabled = true;
};

struct ResolvedNavigationItems {
  std::vector<ResolvedNavigationItem> values;
};

} // namespace huxerui::detail

namespace huxerui {

namespace {

enum class DrawerSide {
  Start,
  End,
};

enum class DrawerPlacement {
  Modal,
  Inline,
};

float IndicatorCornerRadius(float shortest_side, float radius) noexcept {
  return std::min(std::max(0.0F, radius), shortest_side * 0.5F);
}

template <class Style> Style ResolveNavigationStyle() {
  const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(Style))) {
    if (const auto* style = std::any_cast<Style>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI navigation style environment value has an invalid type");
  }
  return Style::Default();
}

std::shared_ptr<const detail::ResolvedNavigationItems> ResolveItems(std::vector<NavigationItem> items) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI navigation requires at least one item");
  }
  auto resolved = std::make_shared<detail::ResolvedNavigationItems>();
  resolved->values.reserve(items.size());
  for (NavigationItem& item : items) {
    detail::ResolvedNavigationItem value{
        detail::NavigationItemAccess::ResolveLabel(item),
        detail::NavigationItemAccess::ResolveIcon(item),
        detail::NavigationItemAccess::ResolveSelectedIcon(item),
        detail::NavigationItemAccess::IsEnabled(item),
    };
    if (value.label.empty()) {
      throw std::invalid_argument("HuxerUI NavigationItem requires a non-empty semantic label");
    }
    if (value.selected_icon.has_value() && !value.icon.has_value()) {
      throw std::invalid_argument("HuxerUI NavigationItem selected icon requires a regular icon");
    }
    resolved->values.push_back(std::move(value));
  }
  return resolved;
}

void ValidateSelectedIndex(const detail::ResolvedNavigationItems& items, std::size_t selected_index) {
  if (selected_index >= items.values.size()) {
    throw std::invalid_argument("HuxerUI navigation selected index is out of range");
  }
}

bool HasMissingNavigationItemIcon(const detail::ResolvedNavigationItems& items) {
  return std::ranges::any_of(items.values, [](const detail::ResolvedNavigationItem& item) {
    return !item.icon.has_value();
  });
}

void ValidateNavigationBarItems(const detail::ResolvedNavigationItems& items) {
  if (HasMissingNavigationItemIcon(items)) {
    throw std::invalid_argument("HuxerUI NavigationBar items require icons");
  }
}

void ValidateNavigationPaneItems(const detail::ResolvedNavigationItems& items, bool expanded) {
  if (!expanded && HasMissingNavigationItemIcon(items)) {
    throw std::invalid_argument("HuxerUI compact NavigationPane items require icons");
  }
}

View BuildNavigationIcon(detail::ResolvedImageAsset icon, float size, Color tint) {
  return std::visit(
      [size, tint](auto asset) -> View {
        using Asset = std::decay_t<decltype(asset)>;
        if constexpr (std::same_as<Asset, VectorAsset>) {
          return Image(std::move(asset)).Tint(tint).With(Frame{.width = size, .height = size});
        } else {
          return Image(std::move(asset))
              .With(Frame{.width = size, .height = size}, Opacity(std::clamp(tint.alpha, 0.0F, 1.0F)));
        }
      },
      std::move(icon)
  );
}

struct PreserveDisabledAppearance {
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const PreserveDisabledAppearance&) const = default;
};

void ApplyPreserveDisabledAppearance(detail::ViewSpec& spec, const PreserveDisabledAppearance&) {
  spec.properties.disabled_opacity = 1.0F;
}

const detail::ModifierDescriptor& PreserveDisabledAppearance::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, const void* value) {
        ApplyPreserveDisabledAppearance(spec, *static_cast<const PreserveDisabledAppearance*>(value));
      },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<PreserveDisabledAppearance>(),
      nullptr,
  };
  return descriptor;
}

struct NavigationSelectionBehavior {
  std::size_t selected_index = 0;
  std::vector<bool> enabled_items;
  Axis axis = Axis::Horizontal;
  ScrollController scroll_controller;
  EventEmitter events;

  static const detail::ModifierDescriptor& Descriptor();
};

class NavigationSelectionExtension final : public NodeExtension {
public:
  NavigationSelectionExtension(MountedNode& node, const NavigationSelectionBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const NavigationSelectionBehavior& modifier) {
    static_cast<void>(node);
    reveal_selection_ = reveal_selection_ || selected_index_ != modifier.selected_index;
    selected_index_ = modifier.selected_index;
    enabled_items_ = modifier.enabled_items;
    axis_ = modifier.axis;
    scroll_controller_ = modifier.scroll_controller;
    events_ = modifier.events;
  }

  bool PrepareGeometry(MountedNode& node) override {
    if (!reveal_selection_ || axis_ != Axis::Vertical || selected_index_ >= node.ChildCount() ||
        !scroll_controller_.IsConnected()) {
      return false;
    }
    reveal_selection_ = false;
    const MountedNode& selected = node.ChildAt(selected_index_);
    const float top = selected.LayoutOffset().y;
    const float bottom = top + selected.LayoutSize().height;
    const ScrollMetrics metrics = scroll_controller_.Metrics();
    if (top < metrics.offset) {
      static_cast<void>(scroll_controller_.ScrollTo(top));
    } else if (bottom > metrics.offset + metrics.viewport_extent) {
      static_cast<void>(scroll_controller_.ScrollTo(bottom - metrics.viewport_extent));
    }
    return false;
  }

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control ||
        event.modifiers.meta || enabled_items_.empty()) {
      return;
    }
    std::optional<std::size_t> requested;
    if (event.key == Key::Home) {
      requested = FindEdge(false);
    } else if (event.key == Key::End) {
      requested = FindEdge(true);
    } else if (axis_ == Axis::Horizontal && event.key == Key::ArrowLeft) {
      requested = FindNext(-1);
    } else if (axis_ == Axis::Horizontal && event.key == Key::ArrowRight) {
      requested = FindNext(1);
    } else if (axis_ == Axis::Vertical && event.key == Key::ArrowUp) {
      requested = FindNext(-1);
    } else if (axis_ == Axis::Vertical && event.key == Key::ArrowDown) {
      requested = FindNext(1);
    }
    if (requested.has_value() && *requested != selected_index_) {
      events_.Emit<NavigationEvents::Changed>(*requested);
    }
  }

private:
  std::optional<std::size_t> FindNext(int direction) const {
    const std::size_t count = enabled_items_.size();
    for (std::size_t distance = 1; distance <= count; ++distance) {
      const std::size_t index =
          direction < 0 ? (selected_index_ + count - distance % count) % count : (selected_index_ + distance) % count;
      if (enabled_items_[index]) {
        return index;
      }
    }
    return std::nullopt;
  }

  std::optional<std::size_t> FindEdge(bool from_end) const {
    for (std::size_t offset = 0; offset < enabled_items_.size(); ++offset) {
      const std::size_t index = from_end ? enabled_items_.size() - 1 - offset : offset;
      if (enabled_items_[index]) {
        return index;
      }
    }
    return std::nullopt;
  }

  std::size_t selected_index_ = 0;
  std::vector<bool> enabled_items_;
  Axis axis_ = Axis::Horizontal;
  ScrollController scroll_controller_;
  EventEmitter events_;
  bool reveal_selection_ = true;
};

const detail::ModifierDescriptor& NavigationSelectionBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<NavigationSelectionBehavior, NavigationSelectionExtension>();
}

View NavigationIconLayer(
    const detail::ResolvedNavigationItem& item,
    bool selected,
    Color content,
    Color indicator,
    Size indicator_size,
    float indicator_corner_radius,
    float icon_size,
    const AnimationSpec& animation
) {
  View indicator_view = Stack {}.With(
      Frame{.width = indicator_size.width, .height = indicator_size.height},
      Background(indicator),
      CornerRadius{CornerRadii{IndicatorCornerRadius(
          std::min(std::max(0.0F, indicator_size.width), std::max(0.0F, indicator_size.height)),
          indicator_corner_radius
      )}},
      Opacity(AnimateTo(selected ? 1.0F : 0.0F, animation))
  );
  const auto& icon = selected && item.selected_icon.has_value() ? item.selected_icon : item.icon;
  if (!icon.has_value()) {
    return indicator_view;
  }
  return Stack {
    std::move(indicator_view),
    BuildNavigationIcon(*icon, std::max(0.0F, icon_size), content),
  }.With(
      Frame{.width = indicator_size.width, .height = indicator_size.height},
      Align(HorizontalAlignment::Center, VerticalAlignment::Center)
  );
}

View ApplyNavigationInteraction(View item, std::optional<IndicationSpec> indication, bool enabled) {
  item = std::move(item).With(PreserveDisabledAppearance{}, Enabled(enabled));
  if (indication.has_value()) {
    item = std::move(item).With(Indication{*indication});
  }
  return item;
}

View BuildBarItem(
    const detail::ResolvedNavigationItem& item,
    const NavigationBarStyle& style,
    bool selected,
    std::size_t index,
    EventEmitter events
) {
  const Color content =
      item.enabled ? (selected ? style.selected_content : style.label_style.foreground) : style.disabled_content;
  std::vector<View> children;
  children.push_back(NavigationIconLayer(
      item,
      selected,
      content,
      style.indicator,
      style.indicator_size,
      style.indicator_corner_radius,
      style.icon_size,
      style.selection_animation
  ));
  if (selected || style.show_unselected_labels) {
    TextStyle label_style = style.label_style;
    label_style.foreground = content;
    children.push_back(Text(item.label).Style(std::move(label_style)));
  }
  View result = Column(std::move(children))
                    .With(
                        Frame{
                            .min_width = std::max(0.0F, style.minimum_item_width),
                        },
                        Padding(style.item_padding),
                        Spacing(std::max(0.0F, style.icon_spacing)),
                        MainAlign(MainAxisAlignment::Center),
                        CrossAlign(CrossAxisAlignment::Center),
                        Grow()
                    )
                    .OnClick([events, index, selected] {
                      if (!selected) {
                        events.Emit<NavigationEvents::Changed>(index);
                      }
                    })
                    .Key(index);
  return ApplyNavigationInteraction(std::move(result), style.indication, item.enabled);
}

View BuildPaneItem(
    const detail::ResolvedNavigationItem& item,
    const NavigationPaneStyle& style,
    bool selected,
    bool expanded,
    std::size_t index,
    EventEmitter events
) {
  const Color content =
      item.enabled ? (selected ? style.selected_content : style.label_style.foreground) : style.disabled_content;
  View content_view;
  const auto& icon = selected && item.selected_icon.has_value() ? item.selected_icon : item.icon;
  if (expanded) {
    std::vector<View> row_children;
    if (icon.has_value()) {
      row_children.push_back(BuildNavigationIcon(*icon, std::max(0.0F, style.icon_size), content));
    }
    TextStyle label_style = style.label_style;
    label_style.foreground = content;
    row_children.push_back(Text(item.label).Style(std::move(label_style)));
    content_view = Row(std::move(row_children))
                       .With(
                           Padding(style.item_padding),
                           Spacing(std::max(0.0F, style.icon_spacing)),
                           CrossAlign(CrossAxisAlignment::Center)
                       );
  } else {
    content_view = BuildNavigationIcon(*icon, std::max(0.0F, style.icon_size), content);
  }

  const Size compact_indicator_size{
      std::max(0.0F, style.compact_indicator_size.width),
      std::max(0.0F, style.compact_indicator_size.height),
  };
  const float indicator_height = expanded ? std::max(0.0F, style.item_height) : compact_indicator_size.height;
  const float indicator_corner_radius = IndicatorCornerRadius(
      expanded ? indicator_height : std::min(compact_indicator_size.width, compact_indicator_size.height),
      style.indicator_corner_radius
  );
  Frame indicator_frame{.height = indicator_height};
  if (!expanded) {
    indicator_frame.width = compact_indicator_size.width;
  }
  View indicator = Stack {}.With(
      indicator_frame,
      Background(style.indicator),
      CornerRadius{CornerRadii{indicator_corner_radius}},
      Opacity(AnimateTo(selected ? 1.0F : 0.0F, style.selection_animation))
  );
  View surface = Stack {
    std::move(indicator),
    std::move(content_view),
  }.With(
      Frame{.height = std::max(0.0F, style.item_height)},
      CornerRadius{CornerRadii{indicator_corner_radius}},
      Align(expanded ? HorizontalAlignment::Stretch : HorizontalAlignment::Center, VerticalAlignment::Center)
  ).OnClick([events, index, selected] {
    if (!selected) {
      events.Emit<NavigationEvents::Changed>(index);
    }
  });
  surface = ApplyNavigationInteraction(std::move(surface), style.indication, item.enabled);

  return Stack {
    std::move(surface),
  }.With(
      Frame{
          .height = std::max(0.0F, style.item_height),
          .min_width = std::max(0.0F, expanded ? style.expanded_min_width : style.compact_width),
      },
      Padding(style.item_margin),
      Align(HorizontalAlignment::Stretch, VerticalAlignment::Center)
  ).Key(index);
}

std::vector<bool> EnabledItems(const detail::ResolvedNavigationItems& items) {
  std::vector<bool> enabled;
  enabled.reserve(items.values.size());
  for (const detail::ResolvedNavigationItem& item : items.values) {
    enabled.push_back(item.enabled);
  }
  return enabled;
}

std::function<View()>
NavigationBarFactory(std::shared_ptr<const detail::ResolvedNavigationItems> items, std::size_t selected_index) {
  return [items = std::move(items), selected_index] {
    const NavigationBarStyle style = ResolveNavigationStyle<NavigationBarStyle>();
    const EventEmitter events = UseEvents();
    std::vector<View> children;
    children.reserve(items->values.size());
    for (std::size_t index = 0; index < items->values.size(); ++index) {
      children.push_back(BuildBarItem(items->values[index], style, index == selected_index, index, events));
    }
    return Row(std::move(children))
        .With(
            Frame{.height = std::max(0.0F, style.height)},
            Background(style.background),
            CrossAlign(CrossAxisAlignment::Stretch),
            Focusable{},
            NavigationSelectionBehavior{
                selected_index,
                EnabledItems(*items),
                Axis::Horizontal,
                ScrollController{},
                events,
            }
        );
  };
}

std::function<View()> NavigationPaneFactory(
    std::shared_ptr<const detail::ResolvedNavigationItems> items, std::size_t selected_index, bool expanded
) {
  return [items = std::move(items), selected_index, expanded] {
    const NavigationPaneStyle style = ResolveNavigationStyle<NavigationPaneStyle>();
    const EventEmitter events = UseEvents();
    const ScrollController scroll = UseScrollController();
    std::vector<View> children;
    children.reserve(items->values.size());
    for (std::size_t index = 0; index < items->values.size(); ++index) {
      children.push_back(BuildPaneItem(items->values[index], style, index == selected_index, expanded, index, events));
    }
    Frame content_frame;
    if (expanded) {
      content_frame.min_width = std::max(0.0F, style.expanded_min_width);
    } else {
      content_frame.width = std::max(0.0F, style.compact_width);
    }
    View content = Column(std::move(children))
                       .With(
                           content_frame,
                           Background(style.background),
                           CrossAlign(CrossAxisAlignment::Stretch),
                           Focusable{},
                           NavigationSelectionBehavior{
                               selected_index,
                               EnabledItems(*items),
                               Axis::Vertical,
                               scroll,
                               events,
                           }
                       );
    return ScrollView(std::move(content)).ScrollAxis(Axis::Vertical).Controller(scroll);
  };
}

Size MeasureSingleChild(LayoutContext& context, MountedNode& node, Constraints constraints, const char* name) {
  if (node.ChildCount() != 1) {
    throw std::logic_error(std::string("HuxerUI ") + name + " must contain exactly one child");
  }
  Constraints child_constraints = constraints;
  if (constraints.HasBoundedWidth()) {
    child_constraints = child_constraints.TightWidth(constraints.max_width);
  }
  if (constraints.HasBoundedHeight()) {
    child_constraints = child_constraints.TightHeight(constraints.max_height);
  }
  return context.Measure(node.ChildAt(0), child_constraints);
}

std::vector<View> DrawerPanelChild(View content, const char* name) {
  if (!content) {
    throw std::invalid_argument(std::string("HuxerUI ") + name + " content must not be empty");
  }
  std::vector<View> children;
  children.push_back(std::move(content));
  return children;
}

struct DrawerPresentationState {
  DrawerPlacement placement = DrawerPlacement::Modal;
  float progress = 0.0F;
  bool target_visible = false;
  bool allow_open_gesture = true;
};

struct DrawerOverlayConfiguration {
  DrawerSide side = DrawerSide::Start;
  bool requested_open = false;
  float modal_content_reveal = 56.0F;
  float edge_drag_width = 24.0F;
  std::optional<DrawerMotion> motion;
  std::shared_ptr<DrawerPresentationState> presentation;

  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const DrawerOverlayConfiguration&) const = default;
};

enum class DrawerPresentationRole {
  Scrim,
  StartPanel,
  EndPanel,
};

struct DrawerPresentation {
  std::shared_ptr<DrawerPresentationState> state;
  DrawerPresentationRole role = DrawerPresentationRole::Scrim;
  std::optional<Shadow> modal_shadow;
  CornerRadii modal_corner_radii;

  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const DrawerPresentation&) const = default;
};

class DrawerPresentationExtension final : public NodeExtension {
public:
  DrawerPresentationExtension(MountedNode& node, const DrawerPresentation& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const DrawerPresentation& modifier) {
    static_cast<void>(node);
    state_ = modifier.state;
    role_ = modifier.role;
    modal_shadow_ = modifier.modal_shadow;
    modal_corner_radii_ = modifier.modal_corner_radii;
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(frame);
    if (!state_) {
      return {};
    }
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const float progress = std::clamp(state_->progress, 0.0F, 1.0F);
    if (role_ == DrawerPresentationRole::Scrim) {
      mounted.presentation.local_opacity *= state_->placement == DrawerPlacement::Modal ? progress : 0.0F;
      return {};
    }
    const bool inline_placement = state_->placement == DrawerPlacement::Inline;
    const std::optional<Shadow> shadow = inline_placement ? std::nullopt : modal_shadow_;
    const CornerRadii corner_radii = inline_placement ? CornerRadii{} : modal_corner_radii_;
    // Final placement can change from the viewport preference after local constraint fallback, so modal-only paint
    // properties follow the retained placement state instead of rebuilding the drawer subtree.
    if (mounted.properties.shadow != shadow || mounted.properties.corner_radii != corner_radii) {
      mounted.properties.shadow = shadow;
      mounted.properties.corner_radii = corner_radii;
      mounted.content_paint_dirty = true;
      mounted.foreground_paint_dirty = true;
    }
    if (inline_placement) {
      return {};
    }
    const float closed =
        role_ == DrawerPresentationRole::StartPanel ? -mounted.LayoutSize().width : mounted.LayoutSize().width;
    mounted.presentation.local_transform = detail::ComposeTransform(
        detail::TranslationTransform({closed * (1.0F - progress), 0.0F}),
        mounted.presentation.local_transform
    );
    return {};
  }

private:
  std::shared_ptr<DrawerPresentationState> state_;
  DrawerPresentationRole role_ = DrawerPresentationRole::Scrim;
  std::optional<Shadow> modal_shadow_;
  CornerRadii modal_corner_radii_;
};

const detail::ModifierDescriptor& DrawerPresentation::Descriptor() {
  return detail::ModifierDescriptorFor<DrawerPresentation, DrawerPresentationExtension>();
}

struct DrawerOverlayValue {
  using Value = DrawerOverlayConfiguration;
};

class DrawerOverlayLayout final : public Layout<DrawerOverlayLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    if (node.ChildCount() != 2) {
      throw std::logic_error("HuxerUI drawer overlay must contain a scrim and panel");
    }
    const DrawerOverlayConfiguration* configuration = node.LayoutValue<DrawerOverlayValue>();
    if (configuration == nullptr || !configuration->presentation) {
      throw std::logic_error("HuxerUI drawer overlay is missing its configuration");
    }
    const Size size = constraints.Constrain({
        constraints.HasBoundedWidth() ? constraints.max_width : constraints.min_width,
        constraints.HasBoundedHeight() ? constraints.max_height : constraints.min_height,
    });
    const bool inline_placement = configuration->presentation->placement == DrawerPlacement::Inline;
    LayoutResult result;
    const float scrim_width = inline_placement ? 0.0F : size.width;
    static_cast<void>(context.Measure(node.ChildAt(0), {scrim_width, scrim_width, size.height, size.height}));
    result.Place(node.ChildAt(0), {});

    float panel_width = size.width;
    if (!inline_placement) {
      panel_width = std::max(0.0F, size.width - std::max(0.0F, configuration->modal_content_reveal));
    }
    const Size panel_size = context.Measure(node.ChildAt(1), {0.0F, panel_width, size.height, size.height});
    const float x = configuration->side == DrawerSide::Start ? 0.0F : size.width - panel_size.width;
    result.Place(node.ChildAt(1), {x, 0.0F});
    static_cast<detail::MountedNode&>(node.ChildAt(1)).local_enabled = configuration->presentation->target_visible;
    static_cast<detail::MountedNode&>(node).trap_focus =
        !inline_placement &&
        (configuration->presentation->target_visible || configuration->presentation->progress > 0.001F);
    return result.SetSize(size);
  }
};

class DrawerOverlayExtension final : public NodeExtension {
public:
  DrawerOverlayExtension(MountedNode& node, const DrawerOverlayConfiguration& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const DrawerOverlayConfiguration& modifier) {
    static_cast<void>(node);
    configuration_ = modifier;
    if (!initialized_) {
      placement_ = Placement();
      target_visible_ = TargetVisible();
      progress_.Set(target_visible_ ? 1.0F : 0.0F);
      initialized_ = true;
    }
    if (configuration_.presentation) {
      configuration_.presentation->progress = progress_.Value();
    }
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    const bool target_visible = TargetVisible();
    const DrawerPlacement placement = Placement();
    if (placement_ != placement) {
      placement_ = placement;
      interactive_ = false;
      target_visible_ = target_visible;
      progress_.Set(target_visible ? 1.0F : 0.0F);
    } else if (!interactive_ && target_visible_ != target_visible) {
      target_visible_ = target_visible;
      if (IsModal()) {
        progress_.Update(target_visible ? 1.0F : 0.0F, Motion(target_visible));
      } else {
        progress_.Set(target_visible ? 1.0F : 0.0F);
      }
    }
    if (!IsModal()) {
      progress_.Set(target_visible ? 1.0F : 0.0F);
      if (configuration_.presentation) {
        configuration_.presentation->progress = progress_.Value();
      }
      auto& mounted = static_cast<detail::MountedNode&>(node);
      const bool focus_changed = mounted.trap_focus;
      mounted.trap_focus = false;
      return {focus_changed, std::nullopt};
    }
    const bool running = progress_.Advance(frame.timestamp, frame.delta_time);
    if (configuration_.presentation) {
      configuration_.presentation->progress = progress_.Value();
    }
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool trap_focus = target_visible || progress_.Value() > 0.001F;
    const bool focus_changed = mounted.trap_focus != trap_focus;
    mounted.trap_focus = trap_focus;
    return {running || focus_changed, std::nullopt};
  }

  bool HitTest(MountedNode& node, Point position) const override {
    if (!IsModal() || !node.IsEnabled() || !node.Bounds().Contains(position)) {
      return false;
    }
    if (progress_.Value() > 0.001F || TargetVisible()) {
      return true;
    }
    if (!configuration_.presentation || !configuration_.presentation->allow_open_gesture) {
      return false;
    }
    const float edge = std::max(0.0F, configuration_.edge_drag_width);
    return configuration_.side == DrawerSide::Start ? position.x <= edge : position.x >= node.Bounds().width - edge;
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!IsModal() || !node.IsEnabled()) {
      ResetPointer();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      pointer_id_ = event.pointer_id;
      drag_origin_ = event.position.x;
      drag_origin_progress_ = progress_.Value();
      const Rect panel = PresentedPanelBounds(node);
      outside_press_ = progress_.Value() > 0.001F && !panel.Contains(event.position);
      const float handle = std::max(0.0F, configuration_.edge_drag_width);
      const bool close_handle = configuration_.side == DrawerSide::Start
                                    ? std::abs(event.position.x - (panel.x + panel.width)) <= handle
                                    : std::abs(event.position.x - panel.x) <= handle;
      dragging_ = !outside_press_ && (progress_.Value() <= 0.001F || close_handle);
      return outside_press_ || dragging_ ? PointerResult::Capture : PointerResult::Ignored;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Move && dragging_) {
      const float width = std::max(1.0F, PanelWidth(node));
      const float delta =
          configuration_.side == DrawerSide::Start ? event.position.x - drag_origin_ : drag_origin_ - event.position.x;
      progress_.Set(std::clamp(drag_origin_progress_ + delta / width, 0.0F, 1.0F));
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      if (outside_press_) {
        EmitOpenChanged(node, false);
        target_visible_ = false;
        progress_.Update(0.0F, Motion(false));
      } else if (dragging_) {
        const bool next_open = progress_.Value() >= 0.5F;
        EmitOpenChanged(node, next_open);
        target_visible_ = next_open;
        progress_.Update(next_open ? 1.0F : 0.0F, Motion(next_open));
      }
      ResetPointer();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Cancel) {
      target_visible_ = TargetVisible();
      progress_.Update(target_visible_ ? 1.0F : 0.0F, Motion(target_visible_));
      ResetPointer();
      return PointerResult::Handled;
    }
    return PointerResult::Handled;
  }

  bool OnBack(MountedNode& node, const BackEvent& event) override {
    if (!IsModal()) {
      return false;
    }
    if (!TargetVisible() && !interactive_) {
      return progress_.Value() > 0.001F;
    }
    switch (event.phase) {
    case BackPhase::Begin:
      interactive_ = true;
      progress_.Set(1.0F);
      return true;
    case BackPhase::Update:
      if (interactive_) {
        progress_.Set(1.0F - std::clamp(event.progress, 0.0F, 1.0F));
      }
      return true;
    case BackPhase::Cancel:
      interactive_ = false;
      progress_.Update(1.0F, Motion(true));
      return true;
    case BackPhase::Commit:
      interactive_ = false;
      EmitOpenChanged(node, false);
      target_visible_ = false;
      progress_.Update(0.0F, Motion(false));
      return true;
    }
    return false;
  }

private:
  bool IsModal() const noexcept {
    return Placement() == DrawerPlacement::Modal;
  }

  DrawerPlacement Placement() const noexcept {
    return configuration_.presentation ? configuration_.presentation->placement : DrawerPlacement::Modal;
  }

  bool TargetVisible() const noexcept {
    return configuration_.presentation ? configuration_.presentation->target_visible : configuration_.requested_open;
  }

  AnimationSpec Motion(bool opening) const {
    if (!configuration_.motion.has_value()) {
      return SnapSpec{};
    }
    return opening ? configuration_.motion->open : configuration_.motion->close;
  }

  float PanelWidth(MountedNode& node) const {
    return node.ChildCount() < 2 ? 0.0F : node.ChildAt(1).LayoutSize().width;
  }

  Rect PresentedPanelBounds(MountedNode& node) const {
    if (node.ChildCount() < 2) {
      return {};
    }
    const MountedNode& panel = node.ChildAt(1);
    const float width = panel.LayoutSize().width;
    const float closed = configuration_.side == DrawerSide::Start ? -width : width;
    return {
        panel.LayoutOffset().x + closed * (1.0F - progress_.Value()),
        panel.LayoutOffset().y,
        width,
        panel.LayoutSize().height,
    };
  }

  void EmitOpenChanged(MountedNode& node, bool open) const {
    if (node.ChildCount() < 2 || open == configuration_.requested_open) {
      return;
    }
    const auto& panel = static_cast<const detail::MountedNode&>(node.ChildAt(1));
    static_cast<void>(detail::EmitEvent<DrawerEvents::OpenChanged>(panel.event_bindings, open));
  }

  void ResetPointer() noexcept {
    pointer_id_.reset();
    dragging_ = false;
    outside_press_ = false;
  }

  DrawerOverlayConfiguration configuration_;
  detail::AnimatedValue<float> progress_;
  std::optional<std::int64_t> pointer_id_;
  float drag_origin_ = 0.0F;
  float drag_origin_progress_ = 0.0F;
  bool initialized_ = false;
  bool dragging_ = false;
  bool outside_press_ = false;
  bool interactive_ = false;
  bool target_visible_ = false;
  DrawerPlacement placement_ = DrawerPlacement::Modal;
};

const detail::ModifierDescriptor& DrawerOverlayConfiguration::Descriptor() {
  return detail::ModifierDescriptorFor<DrawerOverlayConfiguration, DrawerOverlayExtension>();
}

struct DrawerLayoutSlot {
  std::size_t child_index = 0;
  bool requested_open = false;
  DrawerStyle style;
  std::shared_ptr<DrawerPresentationState> presentation;

  bool operator==(const DrawerLayoutSlot&) const = default;
};

struct DrawerLayoutConfiguration {
  ViewportClass viewport_class = ViewportClass::Compact;
  std::optional<DrawerLayoutSlot> start;
  std::optional<DrawerLayoutSlot> end;

  bool operator==(const DrawerLayoutConfiguration&) const = default;
};

struct DrawerLayoutValue {
  using Value = DrawerLayoutConfiguration;
};

class DrawerOverlayView final : public View {
public:
  DrawerOverlayView(DrawerSide side, View panel, bool open, DrawerStyle style)
      : DrawerOverlayView(Build(side, std::move(panel), open, std::move(style))) {}

  const std::shared_ptr<DrawerPresentationState>& Presentation() const noexcept {
    return presentation_;
  }

private:
  struct Construction {
    std::shared_ptr<detail::ViewSpec> spec;
    std::shared_ptr<DrawerPresentationState> presentation;
  };

  explicit DrawerOverlayView(Construction construction)
      : View(std::move(construction.spec)), presentation_(std::move(construction.presentation)) {}

  static Construction Build(DrawerSide side, View panel, bool open, DrawerStyle style) {
    auto presentation = std::make_shared<DrawerPresentationState>();
    presentation->progress = open ? 1.0F : 0.0F;
    DrawerOverlayConfiguration configuration{
        side,
        open,
        style.modal_content_reveal,
        style.edge_drag_width,
        style.motion,
        presentation,
    };
    View scrim = Stack {}.With(
        Background(style.scrim),
        DrawerPresentation{presentation, DrawerPresentationRole::Scrim}
    );
    panel = std::move(panel).With(
        PreserveDisabledAppearance{},
        DrawerPresentation{
            presentation,
            side == DrawerSide::Start ? DrawerPresentationRole::StartPanel : DrawerPresentationRole::EndPanel,
            style.shadow,
            side == DrawerSide::Start ? CornerRadii{0.0F, style.corner_radius, style.corner_radius, 0.0F}
                                      : CornerRadii{style.corner_radius, 0.0F, 0.0F, style.corner_radius},
        }
    );
    auto spec = detail::MakeLayoutSpec(
        detail::LayoutDescriptorFor<DrawerOverlayLayout>(),
        std::vector<View>{std::move(scrim), std::move(panel)}
    );
    spec->properties.clip_children = true;
    spec->layout_values.insert_or_assign(typeid(DrawerOverlayValue), detail::MakeErasedLayoutValue(configuration));
    spec->retained_modifiers.push_back(detail::MakeModifierSpec(configuration));
    return {std::move(spec), std::move(presentation)};
  }

  std::shared_ptr<DrawerPresentationState> presentation_;
};

DrawerPlacement PreferredDrawerPlacement(ViewportClass viewport_class, DrawerSide side) noexcept {
  if (viewport_class == ViewportClass::Compact) {
    return DrawerPlacement::Modal;
  }
  if (viewport_class == ViewportClass::Medium && side == DrawerSide::End) {
    return DrawerPlacement::Modal;
  }
  return DrawerPlacement::Inline;
}

} // namespace

NavigationBarStyle NavigationBarStyle::Default() {
  return {};
}

NavigationPaneStyle NavigationPaneStyle::Default() {
  return {};
}

DrawerStyle DrawerStyle::Default() {
  return {};
}

NavigationItem::NavigationItem(Icon icon, StringVariant label) : icon_(std::move(icon)), label_(std::move(label)) {}

NavigationItem::NavigationItem(StringVariant label) : NavigationItem(Icon{}, std::move(label)) {}

NavigationItem::NavigationItem(ImageResource icon, StringVariant label)
    : NavigationItem(Icon{std::move(icon)}, std::move(label)) {}

NavigationItem::NavigationItem(ImageAsset icon, StringVariant label)
    : NavigationItem(Icon{std::move(icon)}, std::move(label)) {}

NavigationItem::NavigationItem(VectorAsset icon, StringVariant label)
    : NavigationItem(Icon{std::move(icon)}, std::move(label)) {}

NavigationItem NavigationItem::SelectedIcon(ImageResource icon) && {
  selected_icon_ = std::move(icon);
  return std::move(*this);
}

NavigationItem NavigationItem::SelectedIcon(ImageAsset icon) && {
  selected_icon_ = std::move(icon);
  return std::move(*this);
}

NavigationItem NavigationItem::SelectedIcon(VectorAsset icon) && {
  selected_icon_ = std::move(icon);
  return std::move(*this);
}

NavigationItem NavigationItem::Enabled(bool enabled) && {
  enabled_ = enabled;
  return std::move(*this);
}

NavigationBar::NavigationBar(std::vector<NavigationItem> items, std::size_t selected_index)
    : detail::TypedView<NavigationBar>([&] {
        auto resolved = ResolveItems(std::move(items));
        ValidateSelectedIndex(*resolved, selected_index);
        ValidateNavigationBarItems(*resolved);
        return detail::MakeScopeSpec(NavigationBarFactory(std::move(resolved), selected_index));
      }()) {}

NavigationPane::NavigationPane(std::vector<NavigationItem> items, std::size_t selected_index, bool expanded)
    : detail::TypedView<NavigationPane>([&] {
        auto resolved = ResolveItems(std::move(items));
        ValidateSelectedIndex(*resolved, selected_index);
        ValidateNavigationPaneItems(*resolved, expanded);
        return detail::MakeScopeSpec(NavigationPaneFactory(std::move(resolved), selected_index, expanded));
      }()) {}

StartDrawer::StartDrawer(View content)
    : Layout<StartDrawer>(DrawerPanelChild(std::move(content), "StartDrawer")),
      style_(ResolveNavigationStyle<DrawerStyle>()) {
  ApplyModifiers(
      Frame{.width = std::max(0.0F, style_.preferred_width)},
      Background(style_.background),
      ClipChildren{}
  );
}

StartDrawer StartDrawer::Open(bool open) && {
  open_ = open;
  return std::move(*this);
}

LayoutResult StartDrawer::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  LayoutResult result;
  const Size size = MeasureSingleChild(context, node, constraints, "StartDrawer");
  result.Place(node.ChildAt(0), {});
  return result.SetSize(constraints.Constrain(size));
}

EndDrawer::EndDrawer(View content)
    : Layout<EndDrawer>(DrawerPanelChild(std::move(content), "EndDrawer")),
      style_(ResolveNavigationStyle<DrawerStyle>()) {
  ApplyModifiers(
      Frame{.width = std::max(0.0F, style_.preferred_width)},
      Background(style_.background),
      ClipChildren{}
  );
}

EndDrawer EndDrawer::Open(bool open) && {
  open_ = open;
  return std::move(*this);
}

LayoutResult EndDrawer::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  LayoutResult result;
  const Size size = MeasureSingleChild(context, node, constraints, "EndDrawer");
  result.Place(node.ChildAt(0), {});
  return result.SetSize(constraints.Constrain(size));
}

struct DrawerLayout::Construction {
  std::vector<View> children;
  DrawerLayoutConfiguration configuration;
};

namespace {

DrawerLayoutSlot AppendDrawer(
    std::vector<View>& children,
    DrawerSide side,
    View panel,
    bool open,
    DrawerStyle style,
    ViewportClass viewport_class
) {
  DrawerStyle layout_style = style;
  DrawerOverlayView drawer(side, std::move(panel), open, std::move(style));
  const std::shared_ptr<DrawerPresentationState> presentation = drawer.Presentation();
  presentation->placement = PreferredDrawerPlacement(viewport_class, side);
  presentation->target_visible = presentation->placement == DrawerPlacement::Inline || open;
  presentation->progress = presentation->target_visible ? 1.0F : 0.0F;
  const std::size_t child_index = children.size();
  children.push_back(std::move(drawer));
  return {child_index, open, std::move(layout_style), presentation};
}

float DrawerMinimumWidth(const DrawerLayoutSlot& slot) noexcept {
  return std::min(std::max(0.0F, slot.style.minimum_width), std::max(0.0F, slot.style.preferred_width));
}

float DrawerPreferredWidth(const DrawerLayoutSlot& slot) noexcept {
  return std::max(DrawerMinimumWidth(slot), std::max(0.0F, slot.style.preferred_width));
}

void AllocateInlineWidths(
    float available,
    const std::optional<DrawerLayoutSlot>& start,
    const std::optional<DrawerLayoutSlot>& end,
    bool start_inline,
    bool end_inline,
    float& start_width,
    float& end_width
) {
  start_width = 0.0F;
  end_width = 0.0F;
  if (start_inline && !end_inline) {
    start_width = std::min(DrawerPreferredWidth(*start), available);
    return;
  }
  if (end_inline && !start_inline) {
    end_width = std::min(DrawerPreferredWidth(*end), available);
    return;
  }
  if (!start_inline || !end_inline) {
    return;
  }

  start_width = DrawerMinimumWidth(*start);
  end_width = DrawerMinimumWidth(*end);
  float remaining = std::max(0.0F, available - start_width - end_width);
  const float shared = remaining * 0.5F;
  const float start_add = std::min(shared, DrawerPreferredWidth(*start) - start_width);
  const float end_add = std::min(shared, DrawerPreferredWidth(*end) - end_width);
  start_width += start_add;
  end_width += end_add;
  remaining -= start_add + end_add;
  const float start_remainder = std::min(remaining, DrawerPreferredWidth(*start) - start_width);
  start_width += start_remainder;
  remaining -= start_remainder;
  end_width += std::min(remaining, DrawerPreferredWidth(*end) - end_width);
}

} // namespace

DrawerLayout::Construction DrawerLayout::Build(
    View content, std::optional<StartDrawer> start, std::optional<EndDrawer> end
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI DrawerLayout content must not be empty");
  }
  Construction construction;
  construction.children.push_back(std::move(content));
  construction.configuration.viewport_class = UseViewportClass();
  if (start.has_value()) {
    const bool open = start->open_;
    DrawerStyle style = start->style_;
    construction.configuration.start = AppendDrawer(
        construction.children,
        DrawerSide::Start,
        std::move(*start),
        open,
        std::move(style),
        construction.configuration.viewport_class
    );
  }
  if (end.has_value()) {
    const bool open = end->open_;
    DrawerStyle style = end->style_;
    construction.configuration.end = AppendDrawer(
        construction.children,
        DrawerSide::End,
        std::move(*end),
        open,
        std::move(style),
        construction.configuration.viewport_class
    );
  }
  return construction;
}

DrawerLayout::DrawerLayout(Construction construction)
    : Layout<DrawerLayout>(std::move(construction.children)) {
  SetLayoutValue(typeid(DrawerLayoutValue), std::move(construction.configuration));
  ApplyModifiers(ClipChildren{});
}

DrawerLayout::DrawerLayout(View content)
    : DrawerLayout(Build(std::move(content), std::nullopt, std::nullopt)) {}

DrawerLayout::DrawerLayout(View content, StartDrawer start)
    : DrawerLayout(Build(std::move(content), std::move(start), std::nullopt)) {}

DrawerLayout::DrawerLayout(View content, EndDrawer end)
    : DrawerLayout(Build(std::move(content), std::nullopt, std::move(end))) {}

DrawerLayout::DrawerLayout(View content, StartDrawer start, EndDrawer end)
    : DrawerLayout(Build(std::move(content), std::move(start), std::move(end))) {}

LayoutResult DrawerLayout::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  if (node.ChildCount() == 0) {
    throw std::logic_error("HuxerUI DrawerLayout is missing its content");
  }
  const DrawerLayoutConfiguration* configuration = node.LayoutValue<DrawerLayoutValue>();
  if (configuration == nullptr) {
    throw std::logic_error("HuxerUI DrawerLayout is missing its responsive configuration");
  }

  const float bounded_width = constraints.HasBoundedWidth() ? constraints.max_width : 0.0F;
  bool start_inline = configuration->start.has_value() &&
                      PreferredDrawerPlacement(configuration->viewport_class, DrawerSide::Start) ==
                          DrawerPlacement::Inline;
  bool end_inline = configuration->end.has_value() &&
                    PreferredDrawerPlacement(configuration->viewport_class, DrawerSide::End) ==
                        DrawerPlacement::Inline;
  const auto resolve_minimum_content_width = [&] {
    float value = 0.0F;
    if (start_inline) {
      value = std::max(value, configuration->start->style.minimum_content_width);
    }
    if (end_inline) {
      value = std::max(value, configuration->end->style.minimum_content_width);
    }
    return std::max(0.0F, value);
  };
  float minimum_content_width = resolve_minimum_content_width();

  if (constraints.HasBoundedWidth() && start_inline && end_inline &&
      bounded_width < minimum_content_width + DrawerMinimumWidth(*configuration->start) +
                          DrawerMinimumWidth(*configuration->end)) {
    end_inline = false;
    minimum_content_width = resolve_minimum_content_width();
  }
  if (constraints.HasBoundedWidth() && start_inline &&
      bounded_width < minimum_content_width + DrawerMinimumWidth(*configuration->start)) {
    start_inline = false;
    minimum_content_width = resolve_minimum_content_width();
  }
  if (constraints.HasBoundedWidth() && end_inline &&
      bounded_width < minimum_content_width + DrawerMinimumWidth(*configuration->end)) {
    end_inline = false;
    minimum_content_width = resolve_minimum_content_width();
  }

  const bool start_modal_visible =
      configuration->start.has_value() && !start_inline && configuration->start->requested_open;
  const bool end_modal_visible = configuration->end.has_value() && !end_inline && configuration->end->requested_open;
  if (configuration->start.has_value()) {
    configuration->start->presentation->allow_open_gesture = !end_modal_visible;
  }
  if (configuration->end.has_value()) {
    configuration->end->presentation->allow_open_gesture = !start_modal_visible;
  }

  const float inline_available = constraints.HasBoundedWidth()
                                     ? std::max(0.0F, bounded_width - minimum_content_width)
                                     : (start_inline ? DrawerPreferredWidth(*configuration->start) : 0.0F) +
                                           (end_inline ? DrawerPreferredWidth(*configuration->end) : 0.0F);
  float start_width = 0.0F;
  float end_width = 0.0F;
  AllocateInlineWidths(
      inline_available,
      configuration->start,
      configuration->end,
      start_inline,
      end_inline,
      start_width,
      end_width
  );

  Constraints content_constraints = constraints;
  if (constraints.HasBoundedWidth()) {
    const float content_width = std::max(0.0F, bounded_width - start_width - end_width);
    content_constraints.min_width = content_width;
    content_constraints.max_width = content_width;
  }
  if (constraints.HasBoundedHeight()) {
    content_constraints = content_constraints.TightHeight(constraints.max_height);
  }
  const Size content_size = context.Measure(node.ChildAt(0), content_constraints);
  Size size = constraints.Constrain({
      constraints.HasBoundedWidth() ? constraints.max_width : content_size.width + start_width + end_width,
      constraints.HasBoundedHeight() ? constraints.max_height : content_size.height,
  });

  LayoutResult result;
  result.Place(node.ChildAt(0), {start_width, 0.0F});

  const auto place_drawer = [&](const std::optional<DrawerLayoutSlot>& slot,
                                DrawerSide side,
                                bool inline_placement,
                                float width) {
    if (!slot.has_value() || slot->child_index >= node.ChildCount()) {
      return;
    }
    slot->presentation->placement = inline_placement ? DrawerPlacement::Inline : DrawerPlacement::Modal;
    slot->presentation->target_visible = inline_placement || slot->requested_open;
    const float drawer_width = inline_placement ? width : size.width;
    MountedNode& drawer = node.ChildAt(slot->child_index);
    static_cast<void>(context.Measure(drawer, {drawer_width, drawer_width, size.height, size.height}));
    const float x = inline_placement && side == DrawerSide::End ? size.width - drawer_width : 0.0F;
    result.Place(drawer, {x, 0.0F});
  };

  place_drawer(configuration->start, DrawerSide::Start, start_inline, start_width);
  place_drawer(configuration->end, DrawerSide::End, end_inline, end_width);
  return result.SetSize(size);
}

} // namespace huxerui
