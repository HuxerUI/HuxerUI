#include <huxerui/navigation.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <huxerui/environment.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "internal.h"
#include "resource_internal.h"

namespace huxerui::detail {

namespace {

std::optional<ResolvedImageAsset> ResolveNavigationIcon(const std::optional<ImageVariant>& value) {
  return value.has_value() ? std::optional<ResolvedImageAsset>{UseImageVariant(*value)} : std::nullopt;
}

} // namespace

struct NavigationItemAccess {
  static std::string ResolveLabel(NavigationItem& item) {
    return UseString(std::move(item.label_));
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

  static bool HasIcon(const NavigationItem& item) noexcept {
    return item.icon_.has_value();
  }

  static bool HasSelectedIcon(const NavigationItem& item) noexcept {
    return item.selected_icon_.has_value();
  }

  static bool HasBlankLiteralLabel(const NavigationItem& item) noexcept {
    return IsBlankStringVariantLiteral(item.label_);
  }

  static void ValidateImages(const NavigationItem& item) {
    if (item.icon_.has_value()) {
      ValidateImageVariant(*item.icon_);
    }
    if (item.selected_icon_.has_value()) {
      ValidateImageVariant(*item.selected_icon_);
    }
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

struct TopAppBarConfiguration {
  TopAppBarStyle style;
  TopAppBarTitleAlignment title_alignment = TopAppBarTitleAlignment::Start;

  bool operator==(const TopAppBarConfiguration&) const = default;
};

struct TopAppBarConfigurationValue {
  using Value = TopAppBarConfiguration;
};

template <class Style>
Style ResolveNavigationStyle(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(Style))) {
    if (const auto* style = std::any_cast<Style>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI navigation style environment value has an invalid type");
  }
  return Style::Default();
}

template <class Style> Style ResolveNavigationStyle() {
  return ResolveNavigationStyle<Style>(detail::CurrentEnvironment());
}

template <EnvironmentValue Value>
Value ResolveNavigationEnvironmentValue(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindEnvironmentValue(environment, typeid(Value))) {
    if (const auto* typed = std::any_cast<Value>(value)) {
      return *typed;
    }
    throw std::logic_error("HuxerUI navigation environment value has an invalid type");
  }
  return Value::Default();
}

void ApplyTopAppBarDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment);
void ApplyTopAppBarTitleDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment);
void ApplyTopAppBarActionsDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment);

template <class Layout>
std::shared_ptr<detail::ViewSpec> MakeNavigationLayoutSpec(
    std::vector<View> children,
    detail::ViewDefaults defaults
) {
  auto spec = detail::MakeLayoutSpec(detail::LayoutDescriptorFor<Layout>(), std::move(children));
  spec->defaults = defaults;
  return spec;
}

class TopAppBarTitle final : public View {
public:
  explicit TopAppBarTitle(StringVariant title) : View(MakeSpec(std::move(title))) {}

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(StringVariant title) {
    auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
    spec->text = std::move(title);
    spec->text_role = TextRole::Title;
    spec->properties.text_layout_options = {
        .shaping = {},
        .vertical_align = TextVerticalAlign::Center,
        .wrap = TextWrap::NoWrap,
    };
    spec->component_semantics.role = SemanticRole::Heading;
    spec->component_semantics.heading_level = 1;
    spec->defaults = ApplyTopAppBarTitleDefaults;
    return spec;
  }
};

class TopAppBarActions final : public View {
public:
  explicit TopAppBarActions(std::vector<View> actions) : View(MakeSpec(std::move(actions))) {}

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(std::vector<View> actions) {
    auto spec = detail::MakeLayoutSpec(detail::LayoutDescriptorFor<Row>(), std::move(actions));
    spec->defaults = ApplyTopAppBarActionsDefaults;
    spec->modifiers.push_back(detail::MakeModifierSpec(CrossAlign(CrossAxisAlignment::Center)));
    spec->modifiers.push_back(detail::MakeModifierSpec(ClipChildren{}));
    return spec;
  }
};

float IndicatorCornerRadius(float shortest_side, float radius) noexcept {
  return std::min(std::max(0.0F, radius), shortest_side * 0.5F);
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

void ValidateNavigationDeclarations(
    const std::vector<NavigationItem>& items, std::size_t selected_index, bool require_icons
) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI navigation requires at least one item");
  }
  if (selected_index >= items.size()) {
    throw std::invalid_argument("HuxerUI navigation selected index is out of range");
  }
  for (const NavigationItem& item : items) {
    if (detail::NavigationItemAccess::HasBlankLiteralLabel(item)) {
      throw std::invalid_argument("HuxerUI NavigationItem requires a non-empty semantic label");
    }
    if (detail::NavigationItemAccess::HasSelectedIcon(item) && !detail::NavigationItemAccess::HasIcon(item)) {
      throw std::invalid_argument("HuxerUI NavigationItem selected icon requires a regular icon");
    }
    if (require_icons && !detail::NavigationItemAccess::HasIcon(item)) {
      throw std::invalid_argument("HuxerUI navigation items require icons");
    }
    detail::NavigationItemAccess::ValidateImages(item);
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

void ApplyTopAppBarDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const TopAppBarStyle style = ResolveNavigationStyle<TopAppBarStyle>(environment);
  const auto configuration_it = spec.layout_values.find(typeid(TopAppBarConfigurationValue));
  if (configuration_it == spec.layout_values.end()) {
    throw std::logic_error("HuxerUI TopAppBar is missing its layout configuration");
  }
  auto* configuration = std::any_cast<TopAppBarConfiguration>(&configuration_it->second.value);
  if (configuration == nullptr) {
    throw std::logic_error("HuxerUI TopAppBar layout configuration has an invalid type");
  }
  configuration->style = style;
  spec.properties.background = style.background;
  SystemBarsAppearance system_bars = ResolveNavigationEnvironmentValue<SystemBarsAppearance>(environment);
  system_bars.status_bar_background = style.background;
  spec.properties.system_bars_appearance = system_bars;
}

void ApplyTopAppBarTitleDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const std::string& title = detail::StringLiteral(spec.text);
  if (title.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
    throw std::invalid_argument("HuxerUI TopAppBar requires a non-empty title");
  }
  if (!spec.component_semantics.label.has_value()) {
    spec.component_semantics.label = title;
  }
  spec.properties.text_style = ResolveNavigationStyle<TopAppBarStyle>(environment).title_style;
}

void ApplyTopAppBarActionsDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  spec.properties.spacing = std::max(0.0F, ResolveNavigationStyle<TopAppBarStyle>(environment).action_spacing);
}

void ApplyPreserveDisabledAppearance(detail::ViewSpec& spec, const PreserveDisabledAppearance&) {
  spec.properties.disabled_opacity = 1.0F;
}

const detail::ModifierDescriptor& PreserveDisabledAppearance::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        ApplyPreserveDisabledAppearance(
            spec, *static_cast<const PreserveDisabledAppearance*>(modifier.value.get())
        );
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

  PaintInvalidation PrepareGeometry(MountedNode& node) override {
    if (!reveal_selection_ || axis_ != Axis::Vertical || selected_index_ >= node.ChildCount() ||
        !scroll_controller_.IsConnected()) {
      return PaintInvalidation::None;
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
    return PaintInvalidation::None;
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

View ApplyNavigationInteraction(View item, std::optional<Indication> indication, bool enabled) {
  item = std::move(item).With(PreserveDisabledAppearance{}, Enabled(enabled));
  if (indication.has_value()) {
    item = std::move(item).With(*indication);
  }
  return item;
}

Semantics NavigationContainerSemantics(std::size_t item_count) {
  Semantics semantics;
  semantics.role = SemanticRole::Navigation;
  semantics.collection.emplace();
  semantics.collection->item_count = item_count;
  return semantics;
}

Semantics NavigationItemSemantics(const std::string& label, std::size_t index, bool selected) {
  Semantics semantics;
  semantics.role = SemanticRole::Button;
  semantics.label = label;
  semantics.selected = selected;
  semantics.collection_item.emplace();
  semantics.collection_item->index = index;
  semantics.descendants = SemanticDescendantPolicy::Exclude;
  return semantics;
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
                        Grow(),
                        detail::BuiltInSemantics{NavigationItemSemantics(item.label, index, selected)}
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
      Align(expanded ? HorizontalAlignment::Stretch : HorizontalAlignment::Center, VerticalAlignment::Center),
      detail::BuiltInSemantics{NavigationItemSemantics(item.label, index, selected)}
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

std::function<View()> NavigationBarFactory(std::vector<NavigationItem> declarations, std::size_t selected_index) {
  return [declarations = std::move(declarations), selected_index] {
    const std::shared_ptr<const detail::ResolvedNavigationItems> items = ResolveItems(declarations);
    ValidateSelectedIndex(*items, selected_index);
    ValidateNavigationBarItems(*items);
    const NavigationBarStyle style = ResolveNavigationStyle<NavigationBarStyle>();
    SystemBarsAppearance system_bars = UseEnvironment<SystemBarsAppearance>();
    system_bars.navigation_bar_background = style.background;
    const EventEmitter events = UseEvents();
    std::vector<View> children;
    children.reserve(items->values.size());
    for (std::size_t index = 0; index < items->values.size(); ++index) {
      children.push_back(BuildBarItem(items->values[index], style, index == selected_index, index, events));
    }
    View items_view = Row(std::move(children))
                          .With(
                              Frame{.height = std::max(0.0F, style.height)},
                              CrossAlign(CrossAxisAlignment::Stretch),
                              Focusable{},
                              detail::BuiltInSemantics{NavigationContainerSemantics(items->values.size())},
                              NavigationSelectionBehavior{
                                  selected_index,
                                  EnabledItems(*items),
                                  Axis::Horizontal,
                                  ScrollController{},
                                  events,
                              }
                          );
    return Stack {
        std::move(items_view),
    }.With(
        Background(style.background),
        SafeAreaPadding{.top = false},
        system_bars
    );
  };
}

std::function<View()> NavigationPaneFactory(
    std::vector<NavigationItem> declarations, std::size_t selected_index, bool expanded
) {
  return [declarations = std::move(declarations), selected_index, expanded] {
    const std::shared_ptr<const detail::ResolvedNavigationItems> items = ResolveItems(declarations);
    ValidateSelectedIndex(*items, selected_index);
    ValidateNavigationPaneItems(*items, expanded);
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
                           detail::BuiltInSemantics{NavigationContainerSemantics(items->values.size())},
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

struct DrawerStyleBinding {
  using Value = DrawerStyle;
};

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

  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const DrawerPresentation&) const = default;
};

class DrawerPresentationExtension final : public NodeExtension {
public:
  DrawerPresentationExtension(MountedNode& node, const DrawerPresentation& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const DrawerPresentation& modifier) {
    state_ = modifier.state;
    role_ = modifier.role;
    if (role_ == DrawerPresentationRole::Scrim) {
      modal_shadow_.reset();
      modal_corner_radii_ = {};
      return;
    }
    const DrawerStyle style = node.LayoutValueOr<DrawerStyleBinding>(DrawerStyle::Default());
    modal_shadow_ = style.shadow;
    if (role_ == DrawerPresentationRole::StartPanel) {
      modal_corner_radii_ = {0.0F, style.corner_radius, style.corner_radius, 0.0F};
    } else if (role_ == DrawerPresentationRole::EndPanel) {
      modal_corner_radii_ = {style.corner_radius, 0.0F, 0.0F, style.corner_radius};
    }
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
    static_cast<void>(modifier);
    const DrawerOverlayConfiguration* configuration = node.LayoutValue<DrawerOverlayValue>();
    if (configuration == nullptr) {
      throw std::logic_error("HuxerUI drawer overlay is missing its compiled configuration");
    }
    configuration_ = *configuration;
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
        progress_.AnimateTo(target_visible ? 1.0F : 0.0F, Motion(target_visible));
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
    const MotionAdvanceResult result = progress_.Advance(frame);
    if (configuration_.presentation) {
      configuration_.presentation->progress = progress_.Value();
    }
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool trap_focus = target_visible || progress_.Value() > 0.001F;
    const bool focus_changed = mounted.trap_focus != trap_focus;
    mounted.trap_focus = trap_focus;
    return {result.needs_frame || focus_changed, result.wake_after};
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
        progress_.AnimateTo(0.0F, Motion(false));
      } else if (dragging_) {
        const bool next_open = progress_.Value() >= 0.5F;
        EmitOpenChanged(node, next_open);
        target_visible_ = next_open;
        progress_.AnimateTo(next_open ? 1.0F : 0.0F, Motion(next_open));
      }
      ResetPointer();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Cancel) {
      target_visible_ = TargetVisible();
      progress_.AnimateTo(target_visible_ ? 1.0F : 0.0F, Motion(target_visible_));
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
      progress_.AnimateTo(1.0F, Motion(true));
      return true;
    case BackPhase::Commit:
      interactive_ = false;
      EmitOpenChanged(node, false);
      target_visible_ = false;
      progress_.AnimateTo(0.0F, Motion(false));
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
  MotionController progress_;
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

template <class Value>
Value& MutableNavigationLayoutValue(detail::ViewSpec& spec, std::type_index key, const char* message) {
  const auto value_it = spec.layout_values.find(key);
  if (value_it == spec.layout_values.end()) {
    throw std::logic_error(message);
  }
  auto* value = std::any_cast<Value>(&value_it->second.value);
  if (value == nullptr) {
    throw std::logic_error("HuxerUI navigation layout value has an invalid type");
  }
  return *value;
}

void ApplyDrawerSurfaceDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment,
                                DrawerSide side) {
  const DrawerStyle style = ResolveNavigationStyle<DrawerStyle>(environment);
  spec.layout_values.insert_or_assign(typeid(DrawerStyleBinding), detail::MakeErasedLayoutValue(style));
  spec.properties.frame.width = std::max(0.0F, style.preferred_width);
  spec.properties.background = style.background;
  spec.properties.safe_area_padding = side == DrawerSide::Start ? SafeAreaPadding{.right = false}
                                                                 : SafeAreaPadding{.left = false};
}

void ApplyStartDrawerDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  ApplyDrawerSurfaceDefaults(spec, environment, DrawerSide::Start);
}

void ApplyEndDrawerDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  ApplyDrawerSurfaceDefaults(spec, environment, DrawerSide::End);
}

void ApplyDrawerScrimDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  spec.properties.background = ResolveNavigationStyle<DrawerStyle>(environment).scrim;
}

void ApplyDrawerOverlayDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  DrawerOverlayConfiguration& configuration = MutableNavigationLayoutValue<DrawerOverlayConfiguration>(
      spec,
      typeid(DrawerOverlayValue),
      "HuxerUI drawer overlay is missing its configuration"
  );
  const DrawerStyle style = ResolveNavigationStyle<DrawerStyle>(environment);
  configuration.modal_content_reveal = style.modal_content_reveal;
  configuration.edge_drag_width = style.edge_drag_width;
  configuration.motion = style.motion;
}

void ApplyDrawerLayoutDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  DrawerLayoutConfiguration& configuration = MutableNavigationLayoutValue<DrawerLayoutConfiguration>(
      spec,
      typeid(DrawerLayoutValue),
      "HuxerUI DrawerLayout is missing its responsive configuration"
  );
  configuration.viewport_class = ResolveNavigationEnvironmentValue<detail::ViewportEnvironment>(environment).value;
  const DrawerStyle style = ResolveNavigationStyle<DrawerStyle>(environment);
  if (configuration.start.has_value()) {
    configuration.start->style = style;
  }
  if (configuration.end.has_value()) {
    configuration.end->style = style;
  }
}

class DrawerScrimView final : public View {
public:
  explicit DrawerScrimView(std::shared_ptr<DrawerPresentationState> presentation)
      : View(MakeSpec(std::move(presentation))) {}

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(std::shared_ptr<DrawerPresentationState> presentation) {
    auto spec = detail::MakeLayoutSpec(detail::LayoutDescriptorFor<Stack>(), {});
    spec->defaults = ApplyDrawerScrimDefaults;
    spec->modifiers.push_back(
        detail::MakeModifierSpec(DrawerPresentation{std::move(presentation), DrawerPresentationRole::Scrim})
    );
    return spec;
  }
};

class DrawerOverlayView final : public View {
public:
  DrawerOverlayView(DrawerSide side, View panel, bool open)
      : DrawerOverlayView(Build(side, std::move(panel), open)) {}

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

  static Construction Build(DrawerSide side, View panel, bool open) {
    auto presentation = std::make_shared<DrawerPresentationState>();
    presentation->progress = open ? 1.0F : 0.0F;
    DrawerOverlayConfiguration configuration{
        .side = side,
        .requested_open = open,
        .presentation = presentation,
    };
    View scrim = DrawerScrimView(presentation);
    panel = std::move(panel).With(
        PreserveDisabledAppearance{},
        DrawerPresentation{
            presentation,
            side == DrawerSide::Start ? DrawerPresentationRole::StartPanel : DrawerPresentationRole::EndPanel,
        }
    );
    auto spec = detail::MakeLayoutSpec(
        detail::LayoutDescriptorFor<DrawerOverlayLayout>(),
        std::vector<View>{std::move(scrim), std::move(panel)}
    );
    spec->properties.clip_children = true;
    spec->layout_values.insert_or_assign(typeid(DrawerOverlayValue), detail::MakeErasedLayoutValue(configuration));
    spec->defaults = ApplyDrawerOverlayDefaults;
    spec->modifiers.push_back(detail::MakeModifierSpec(configuration));
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

struct TopAppBar::Construction {
  std::vector<View> children;
};

TopAppBar::Construction TopAppBar::Build(
    StringVariant title,
    std::optional<View> leading,
    std::vector<View> actions
) {
  Construction construction;
  if (leading.has_value() && !*leading) {
    throw std::invalid_argument("HuxerUI TopAppBar leading view must not be empty");
  }
  if (std::ranges::any_of(actions, [](const View& action) { return !action; })) {
    throw std::invalid_argument("HuxerUI TopAppBar actions must not contain empty views");
  }

  construction.children.reserve(3);
  construction.children.push_back(leading.has_value() ? std::move(*leading) : View{Spacer()});
  construction.children.push_back(TopAppBarTitle(std::move(title)));
  construction.children.push_back(TopAppBarActions(std::move(actions)));
  return construction;
}

TopAppBar::TopAppBar(StringVariant title, std::optional<View> leading, std::vector<View> actions)
    : TopAppBar([&] {
        if (detail::IsBlankStringVariantLiteral(title)) {
          throw std::invalid_argument("HuxerUI TopAppBar requires a non-empty title");
        }
        if (leading.has_value() && !*leading) {
          throw std::invalid_argument("HuxerUI TopAppBar leading view must not be empty");
        }
        if (std::ranges::any_of(actions, [](const View& action) { return !action; })) {
          throw std::invalid_argument("HuxerUI TopAppBar actions must not contain empty views");
        }
        return Build(std::move(title), std::move(leading), std::move(actions));
      }()) {}

TopAppBar::TopAppBar(Construction construction)
    : Layout<TopAppBar>(
          MakeNavigationLayoutSpec<TopAppBar>(std::move(construction.children), ApplyTopAppBarDefaults)
      ) {
  ApplyModifiers(SafeAreaPadding{.bottom = false}, ClipChildren{});
  UpdateConfiguration();
}

TopAppBar TopAppBar::TitleAlignment(TopAppBarTitleAlignment alignment) && {
  title_alignment_ = alignment;
  UpdateConfiguration();
  return std::move(*this);
}

void TopAppBar::UpdateConfiguration() {
  ApplyLayoutValue<TopAppBarConfigurationValue>(
      TopAppBarConfiguration{TopAppBarStyle::Default(), title_alignment_}
  );
}

LayoutResult TopAppBar::Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
  if (node.ChildCount() != 3) {
    throw std::logic_error("HuxerUI TopAppBar must contain leading, title, and actions slots");
  }
  const TopAppBarConfiguration* configuration = node.LayoutValue<TopAppBarConfigurationValue>();
  if (configuration == nullptr) {
    throw std::logic_error("HuxerUI TopAppBar is missing its layout configuration");
  }

  const float desired_height = std::max(0.0F, configuration->style.height);
  const float height = constraints.Constrain({0.0F, desired_height}).height;
  const float padding = std::max(0.0F, configuration->style.horizontal_padding);
  const float title_inset = std::max(padding, std::max(0.0F, configuration->style.title_inset));
  const float title_spacing = std::max(0.0F, configuration->style.title_spacing);
  const bool bounded_width = constraints.HasBoundedWidth();
  const float bounded_layout_width = bounded_width ? constraints.max_width : 0.0F;
  const float bounded_content_width = bounded_width ? std::max(0.0F, bounded_layout_width - padding * 2.0F) : 0.0F;

  const Constraints leading_constraints{
      0.0F,
      bounded_width ? bounded_content_width : std::numeric_limits<float>::infinity(),
      0.0F,
      height,
  };
  const Size leading_size = context.Measure(node.ChildAt(0), leading_constraints);
  const float leading_gap = leading_size.width > 0.0F ? title_spacing : 0.0F;
  const float action_max_width = bounded_width
                                     ? std::max(0.0F, bounded_content_width - leading_size.width - leading_gap)
                                     : std::numeric_limits<float>::infinity();
  const Size actions_size = context.Measure(node.ChildAt(2), {0.0F, action_max_width, 0.0F, height});
  const float action_gap = actions_size.width > 0.0F ? title_spacing : 0.0F;

  float width = bounded_layout_width;
  float title_start = leading_size.width > 0.0F ? padding + leading_size.width + leading_gap : title_inset;
  float title_end = bounded_width ? width - padding - actions_size.width - action_gap
                                  : std::numeric_limits<float>::infinity();
  const float title_max_width = bounded_width ? std::max(0.0F, title_end - title_start)
                                               : std::numeric_limits<float>::infinity();
  const Size title_size = context.Measure(node.ChildAt(1), {0.0F, title_max_width, 0.0F, height});

  if (!bounded_width) {
    const float leading_extent = leading_size.width > 0.0F ? padding + leading_size.width + leading_gap : title_inset;
    const float action_extent = actions_size.width > 0.0F ? action_gap + actions_size.width + padding : padding;
    const float natural_width = configuration->title_alignment == TopAppBarTitleAlignment::Center
                                    ? title_size.width + std::max(leading_extent, action_extent) * 2.0F
                                    : leading_extent + title_size.width + action_extent;
    width = constraints.Constrain({natural_width, height}).width;
    title_start = leading_size.width > 0.0F ? padding + leading_size.width + leading_gap : title_inset;
    title_end = width - padding - actions_size.width - action_gap;
  }

  float title_x = title_start;
  if (configuration->title_alignment == TopAppBarTitleAlignment::Center) {
    const float ideal = (width - title_size.width) * 0.5F;
    title_x = std::clamp(ideal, title_start, std::max(title_start, title_end - title_size.width));
  }
  const float actions_x = std::max(padding, width - padding - actions_size.width);

  LayoutResult result;
  result.Place(node.ChildAt(0), {padding, std::max(0.0F, (height - leading_size.height) * 0.5F)});
  result.Place(node.ChildAt(1), {title_x, std::max(0.0F, (height - title_size.height) * 0.5F)});
  result.Place(node.ChildAt(2), {actions_x, std::max(0.0F, (height - actions_size.height) * 0.5F)});
  return result.SetSize(constraints.Constrain({width, height}));
}

TopAppBarStyle TopAppBarStyle::Default() {
  return {};
}

NavigationBarStyle NavigationBarStyle::Default() {
  return {};
}

NavigationPaneStyle NavigationPaneStyle::Default() {
  return {};
}

DrawerStyle DrawerStyle::Default() {
  return {};
}

NavigationItem::NavigationItem(StringVariant label) : label_(std::move(label)) {}

NavigationItem::NavigationItem(ImageVariant icon, StringVariant label)
    : icon_(std::move(icon)), label_(std::move(label)) {
  detail::ValidateImageVariant(*icon_);
}

NavigationItem NavigationItem::SelectedIcon(ImageVariant icon) && {
  detail::ValidateImageVariant(icon);
  selected_icon_ = std::move(icon);
  return std::move(*this);
}

NavigationItem NavigationItem::Enabled(bool enabled) && {
  enabled_ = enabled;
  return std::move(*this);
}

NavigationBar::NavigationBar(std::vector<NavigationItem> items, std::size_t selected_index)
    : detail::TypedView<NavigationBar>([&] {
        ValidateNavigationDeclarations(items, selected_index, true);
        return detail::MakeScopeSpec(NavigationBarFactory(std::move(items), selected_index));
      }()) {}

NavigationPane::NavigationPane(std::vector<NavigationItem> items, std::size_t selected_index, bool expanded)
    : detail::TypedView<NavigationPane>([&] {
        ValidateNavigationDeclarations(items, selected_index, !expanded);
        return detail::MakeScopeSpec(NavigationPaneFactory(std::move(items), selected_index, expanded));
      }()) {}

StartDrawer::StartDrawer(View content)
    : Layout<StartDrawer>(MakeNavigationLayoutSpec<StartDrawer>(
          DrawerPanelChild(std::move(content), "StartDrawer"),
          ApplyStartDrawerDefaults
      )) {
  ApplyModifiers(ClipChildren{});
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
    : Layout<EndDrawer>(MakeNavigationLayoutSpec<EndDrawer>(
          DrawerPanelChild(std::move(content), "EndDrawer"),
          ApplyEndDrawerDefaults
      )) {
  ApplyModifiers(ClipChildren{});
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
    ViewportClass viewport_class
) {
  DrawerOverlayView drawer(side, std::move(panel), open);
  const std::shared_ptr<DrawerPresentationState> presentation = drawer.Presentation();
  presentation->placement = PreferredDrawerPlacement(viewport_class, side);
  presentation->target_visible = presentation->placement == DrawerPlacement::Inline || open;
  presentation->progress = presentation->target_visible ? 1.0F : 0.0F;
  const std::size_t child_index = children.size();
  children.push_back(std::move(drawer));
  return {child_index, open, DrawerStyle::Default(), presentation};
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
  construction.configuration.viewport_class = ViewportClass::Compact;
  if (start.has_value()) {
    const bool open = start->open_;
    construction.configuration.start = AppendDrawer(
        construction.children,
        DrawerSide::Start,
        std::move(*start),
        open,
        construction.configuration.viewport_class
    );
  }
  if (end.has_value()) {
    const bool open = end->open_;
    construction.configuration.end = AppendDrawer(
        construction.children,
        DrawerSide::End,
        std::move(*end),
        open,
        construction.configuration.viewport_class
    );
  }
  return construction;
}

DrawerLayout::DrawerLayout(Construction construction)
    : Layout<DrawerLayout>(MakeNavigationLayoutSpec<DrawerLayout>(
          std::move(construction.children),
          ApplyDrawerLayoutDefaults
      )) {
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
