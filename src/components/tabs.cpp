#include <huxerui/view.h>

#include <algorithm>
#include <any>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/semantics.h>
#include <huxerui/animation.h>
#include <huxerui/theme.h>

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"
#include "resources/resource_internal.h"

namespace huxerui {

namespace detail {

std::optional<ResolvedImageAsset> InternalAccess::ResolveIcon(TabItem& item) {
  return ResolveOptionalControlIcon(item.icon_);
}

std::string InternalAccess::ResolveLabel(TabItem& item) {
  return UseString(std::move(item.label_));
}

bool InternalAccess::ShowsLabel(const TabItem& item) noexcept {
  return item.show_label_;
}

bool InternalAccess::IsEnabled(const TabItem& item) noexcept {
  return item.enabled_;
}

bool InternalAccess::HasIcon(const TabItem& item) noexcept {
  return item.icon_.has_value();
}

bool InternalAccess::HasBlankLiteralLabel(const TabItem& item) noexcept {
  return IsBlankStringVariantLiteral(item.label_);
}

void InternalAccess::ValidateIcon(const TabItem& item) {
  if (item.icon_.has_value()) {
    ValidateImageVariant(*item.icon_);
  }
}

} // namespace detail

namespace {

using detail::ResolveStyleOverride;

struct ResolvedTabItem {
  std::string label;
  std::optional<detail::ResolvedImageAsset> icon;
  bool show_label = true;
  bool enabled = true;
};

struct TabsExpandItems {
  using Value = bool;
};

struct TabsLayoutPolicy {
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    const std::size_t count = node.ChildCount();
    if (count == 0) {
      LayoutResult empty;
      empty.SetSize(constraints.Constrain({}));
      return empty;
    }

    const bool expand_items = node.LayoutValueOr<TabsExpandItems>(false);
    const Constraints item_constraints{
        0.0F,
        std::numeric_limits<float>::infinity(),
        0.0F,
        constraints.max_height,
    };
    std::vector<float> widths;
    widths.reserve(count);
    float natural_width = 0.0F;
    float height = 0.0F;
    for (ViewNode& child : node.Children()) {
      const Size size = context.Measure(child, item_constraints);
      widths.push_back(size.width);
      natural_width += size.width;
      height = std::max(height, size.height);
    }

    const float width = constraints.ConstrainWidth(std::max(natural_width, constraints.min_width));
    if (expand_items && natural_width <= constraints.min_width) {
      std::ranges::fill(widths, width / static_cast<float>(count));
    }
    height = constraints.ConstrainHeight(height);

    LayoutResult result;
    float x = 0.0F;
    for (std::size_t index = 0; index < count; ++index) {
      ViewNode& child = node.ChildAt(index);
      static_cast<void>(context.Measure(child, {widths[index], widths[index], height, height}));
      result.Place(child, {x, 0.0F});
      x += widths[index];
    }
    result.SetSize({width, height});
    return result;
  }
};

class TabLabel final : public View {
public:
  TabLabel(const ResolvedTabItem& item, const TabsStyle& style, bool selected, std::size_t index)
      : View(MakeSpec(item, style, selected, index)) {
    TextStyle text_style = style.label_style;
    text_style.foreground = selected ? style.selected_label : style.label_style.foreground;
    SetTextStyle(std::move(text_style));
  }

private:
  static std::shared_ptr<detail::ViewSpec>
  MakeSpec(const ResolvedTabItem& item, const TabsStyle& style, bool selected, std::size_t index) {
    auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
    spec->text = item.label;
    Size icon_size;
    if (item.icon.has_value()) {
      spec->image_properties.SetResolvedAsset(*item.icon);
      icon_size = {std::max(0.0F, style.icon_size), std::max(0.0F, style.icon_size)};
    }
    spec->layout_values.insert_or_assign(
        typeid(detail::LabelContentMetrics),
        detail::MakeErasedLayoutValue(detail::LabelContentMetrics{
            icon_size,
            std::max(0.0F, style.icon_spacing),
            item.show_label,
        })
    );
    spec->properties.padding = style.item_padding;
    spec->properties.disabled_foreground = style.disabled_label;
    spec->properties.disabled_opacity = 1.0F;
    spec->properties.frame.min_width = std::max(0.0F, style.minimum_item_width);
    spec->properties.frame.min_height = std::max(0.0F, style.minimum_height);
    spec->properties.text_layout_options = {
        .shaping = {},
        .align = TextAlign::Leading,
        .vertical_align = TextVerticalAlign::Center,
        .wrap = TextWrap::NoWrap,
    };
    spec->default_indication = style.indication;
    spec->component_semantics.role = SemanticRole::Tab;
    spec->component_semantics.label = item.label;
    spec->component_semantics.selected = selected;
    spec->component_semantics.collection_item.emplace();
    spec->component_semantics.collection_item->index = index;
    return spec;
  }
};

struct TabsBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  std::size_t selected_index;
  std::vector<bool> enabled_items;
  TabsStyle style;
  ScrollController scroll_controller;
  EventEmitter events;
};

class TabsBehaviorExtension final : public NodeExtension {
public:
  TabsBehaviorExtension(ViewNode& node, const TabsBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(ViewNode& node, const TabsBehavior& modifier) {
    const bool selection_changed = initialized_ && selected_index_ != modifier.selected_index;
    selected_index_ = modifier.selected_index;
    enabled_items_ = modifier.enabled_items;
    style_ = modifier.style;
    scroll_controller_ = modifier.scroll_controller;
    events_ = modifier.events;
    geometry_update_pending_ = geometry_update_pending_ || !initialized_ || selection_changed;
    animate_geometry_update_ = animate_geometry_update_ || selection_changed;
    if (selection_changed) {
      reveal_offset_.reset();
    }
    initialized_ = true;
    if (!node.IsEnabled()) {
      animate_geometry_update_ = false;
    }
  }

  FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_x = indicator_x_.Value();
    const float previous_width = indicator_width_.Value();
    const MotionAdvanceResult x_result = indicator_x_.Advance(frame);
    const MotionAdvanceResult width_result = indicator_width_.Advance(frame);
    if (indicator_x_.Value() != previous_x || indicator_width_.Value() != previous_width) {
      InvalidatePaint();
    }
    if (reveal_offset_.has_value()) {
      static_cast<void>(scroll_controller_.ScrollTo(*reveal_offset_));
      reveal_offset_.reset();
    }
    return {
        .needs_frame = geometry_update_pending_ || x_result.needs_frame || width_result.needs_frame,
        .wake_after = detail::EarliestWakeAfter(x_result.wake_after, width_result.wake_after),
    };
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    if (selected_index_ >= node.ChildCount()) {
      return PaintInvalidation::None;
    }
    const bool reveal_selection = geometry_update_pending_;
    const auto& selected = static_cast<const detail::MountedNode&>(node.ChildAt(selected_index_));
    const float item_x = selected.LayoutOffset().x;
    const float item_width = selected.LayoutSize().width;
    float target_width = item_width;
    if (style_.indicator_sizing == TabIndicatorSizing::Content) {
      target_width = std::clamp(
          std::max(std::max(0.0F, style_.indicator_min_width), ContentWidth(selected)),
          0.0F,
          item_width
      );
    }
    const float target_x = item_x + (item_width - target_width) * 0.5F;
    bool changed = false;
    if (!indicator_geometry_initialized_) {
      indicator_x_.Set(target_x);
      indicator_width_.Set(target_width);
      indicator_geometry_initialized_ = true;
      changed = true;
    } else if (indicator_x_.Target() != target_x || indicator_width_.Target() != target_width) {
      if (animate_geometry_update_ && style_.indicator_animation_duration > 0.0) {
        const TweenSpec animation{style_.indicator_animation_duration};
        indicator_x_.AnimateTo(target_x, animation);
        indicator_width_.AnimateTo(target_width, animation);
      } else {
        indicator_x_.Set(target_x);
        indicator_width_.Set(target_width);
      }
      changed = true;
    }

    const ScrollMetrics metrics = scroll_controller_.Metrics();
    if (reveal_selection && metrics.viewport_extent > 0.0F) {
      const float visible_start = metrics.offset;
      const float visible_end = visible_start + metrics.viewport_extent;
      const float selected_end = item_x + item_width;
      if (item_x < visible_start) {
        reveal_offset_ = item_x;
      } else if (selected_end > visible_end) {
        reveal_offset_ = selected_end - metrics.viewport_extent;
      }
    }
    geometry_update_pending_ = false;
    animate_geometry_update_ = false;
    return changed ? PaintInvalidation::Foreground : PaintInvalidation::None;
  }

  bool OnKey(ViewNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control ||
        event.modifiers.meta || enabled_items_.empty()) {
      return false;
    }
    std::optional<std::size_t> requested;
    if (event.key == Key::ArrowLeft) {
      requested = FindEnabled(selected_index_, -1);
    } else if (event.key == Key::ArrowRight) {
      requested = FindEnabled(selected_index_, 1);
    } else if (event.key == Key::Home) {
      requested = FindEdgeEnabled(false);
    } else if (event.key == Key::End) {
      requested = FindEdgeEnabled(true);
    }
    if (requested.has_value() && *requested != selected_index_) {
      events_.Emit<TabsEvents::Changed>(*requested);
    }
    return requested.has_value();
  }

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
    const Rect frame = node.Bounds();
    const float divider_height = std::clamp(style_.divider_height, 0.0F, frame.height);
    if (divider_height > 0.0F && style_.divider_color.alpha > 0.0F &&
        scroll_controller_.Metrics().maximum_offset <= 0.0F) {
      context.DrawRect(
          {frame.x, frame.y + frame.height - divider_height, frame.width, divider_height},
          style_.divider_color
      );
    }

    const float height = std::clamp(style_.indicator_height, 0.0F, frame.height);
    const float width = std::clamp(indicator_width_.Value(), 0.0F, frame.width);
    if (!indicator_geometry_initialized_ || height <= 0.0F || width <= 0.0F || style_.indicator.alpha <= 0.0F) {
      return;
    }
    context.DrawRect(
        {
            frame.x + indicator_x_.Value(),
            frame.y + frame.height - height,
            width,
            height,
        },
        style_.indicator,
        std::max(0.0F, style_.indicator_corner_radius)
    );
  }

private:
  [[nodiscard]] static float ContentWidth(const detail::MountedNode& node) {
    const detail::LabelContentMetrics metrics = node.LayoutValueOr<detail::LabelContentMetrics>({});
    float width = std::max(0.0F, metrics.icon_size.width);
    if (!metrics.show_label || node.text.PlainText().empty()) {
      return width;
    }
    const auto cached = node.layout_cache.find(typeid(detail::LabelLayoutCache));
    const auto* layout =
        cached == node.layout_cache.end() ? nullptr : std::any_cast<detail::LabelLayoutCache>(&cached->second);
    if (layout == nullptr) {
      return width;
    }
    if (width > 0.0F) {
      width += std::max(0.0F, metrics.icon_spacing);
    }
    return width + layout->text.size.width;
  }

  [[nodiscard]] std::optional<std::size_t> FindEnabled(std::size_t start, int direction) const {
    const std::size_t count = enabled_items_.size();
    for (std::size_t distance = 1; distance <= count; ++distance) {
      const std::size_t index = direction < 0 ? (start + count - distance % count) % count : (start + distance) % count;
      if (enabled_items_[index]) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> FindEdgeEnabled(bool from_end) const {
    for (std::size_t offset = 0; offset < enabled_items_.size(); ++offset) {
      const std::size_t index = from_end ? enabled_items_.size() - 1 - offset : offset;
      if (enabled_items_[index]) {
        return index;
      }
    }
    return std::nullopt;
  }

  MotionController indicator_x_;
  MotionController indicator_width_;
  std::vector<bool> enabled_items_;
  TabsStyle style_;
  ScrollController scroll_controller_;
  EventEmitter events_;
  std::optional<float> reveal_offset_;
  std::size_t selected_index_ = 0;
  bool initialized_ = false;
  bool indicator_geometry_initialized_ = false;
  bool geometry_update_pending_ = false;
  bool animate_geometry_update_ = false;
};

const detail::ModifierDescriptor& TabsBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<TabsBehavior, TabsBehaviorExtension>();
}

class TabsLayoutView final : public View {
public:
  TabsLayoutView(
      std::vector<View> items,
      std::size_t selected_index,
      std::vector<bool> enabled_items,
      TabsStyle style,
      ScrollController scroll_controller,
      EventEmitter events
  )
      : View(MakeSpec(
            std::move(items),
            selected_index,
            std::move(enabled_items),
            std::move(style),
            std::move(scroll_controller),
            std::move(events)
        )) {}

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(
      std::vector<View> items,
      std::size_t selected_index,
      std::vector<bool> enabled_items,
      TabsStyle style,
      ScrollController scroll_controller,
      EventEmitter events
  ) {
    auto spec = detail::MakeLayoutSpec(detail::LayoutDescriptorFor<TabsLayoutPolicy>(), std::move(items));
    spec->focusable = true;
    spec->component_semantics.role = SemanticRole::TabList;
    spec->component_semantics.collection.emplace();
    spec->component_semantics.collection->item_count = spec->children.size();
    spec->properties.background = style.background;
    spec->layout_values.insert_or_assign(typeid(TabsExpandItems), detail::MakeErasedLayoutValue(style.expand_items));
    spec->modifiers.push_back(
        detail::MakeModifierSpec(
            TabsBehavior{
                selected_index,
                std::move(enabled_items),
                std::move(style),
                std::move(scroll_controller),
                std::move(events),
            }
        )
    );
    return spec;
  }
};

std::shared_ptr<detail::ViewSpec> MakeTabsSpec(std::vector<TabItem> items, std::size_t selected_index) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI Tabs requires at least one item");
  }
  if (selected_index >= items.size()) {
    throw std::invalid_argument("HuxerUI Tabs selected index is out of range");
  }

  for (const TabItem& item : items) {
    if (detail::InternalAccess::HasBlankLiteralLabel(item)) {
      throw std::invalid_argument("HuxerUI Tabs item requires a non-empty semantic label");
    }
    if (!detail::InternalAccess::ShowsLabel(item) && !detail::InternalAccess::HasIcon(item)) {
      throw std::invalid_argument("HuxerUI icon-only Tabs item requires an icon and semantic label");
    }
    detail::InternalAccess::ValidateIcon(item);
  }

  return detail::MakeScopeSpec([items = std::move(items), selected_index]() -> View {
    std::vector<ResolvedTabItem> resolved_items;
    resolved_items.reserve(items.size());
    for (TabItem item : items) {
      ResolvedTabItem resolved{
          detail::InternalAccess::ResolveLabel(item),
          detail::InternalAccess::ResolveIcon(item),
          detail::InternalAccess::ShowsLabel(item),
          detail::InternalAccess::IsEnabled(item),
      };
      if (resolved.label.empty()) {
        throw std::invalid_argument("HuxerUI Tabs item requires a non-empty semantic label");
      }
      resolved_items.push_back(std::move(resolved));
    }

    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
    const TabsStyle style = ResolveStyleOverride<TabsStyle>(environment).value_or(detail::DefaultTabsStyle(theme));
    const ScrollController scroll_controller = UseScrollController();
    const EventEmitter events = UseEvents();

    std::vector<View> labels;
    labels.reserve(resolved_items.size());
    std::vector<bool> enabled_items;
    enabled_items.reserve(resolved_items.size());
    for (std::size_t index = 0; index < resolved_items.size(); ++index) {
      const bool enabled = resolved_items[index].enabled;
      enabled_items.push_back(enabled);
      labels.push_back(
          std::move(TabLabel(resolved_items[index], style, index == selected_index, index))
              .OnClick([events, index, selected_index] {
                if (index != selected_index) {
                  events.Emit<TabsEvents::Changed>(index);
                }
              })
              .With(huxerui::Enabled(enabled))
              .Key(index)
      );
    }

    return ScrollView(TabsLayoutView(
                          std::move(labels),
                          selected_index,
                          std::move(enabled_items),
                          style,
                          scroll_controller,
                          events
                      ))
        .ScrollAxis(Axis::Horizontal)
        .Controller(scroll_controller)
        .LayoutValue<detail::ScrollFillViewport>(true);
  });
}

std::shared_ptr<detail::ViewSpec> MakeTabsSpec(std::vector<StringVariant> labels, std::size_t selected_index) {
  std::vector<TabItem> items;
  items.reserve(labels.size());
  for (StringVariant& label : labels) {
    items.emplace_back(std::move(label));
  }
  return MakeTabsSpec(std::move(items), selected_index);
}

} // namespace

TabItem::TabItem(StringVariant label) : label_(std::move(label)) {}

TabItem::TabItem(ImageVariant icon, StringVariant label) : icon_(std::move(icon)), label_(std::move(label)) {
  detail::ValidateImageVariant(*icon_);
}

TabItem TabItem::IconOnly(ImageVariant icon, StringVariant semantic_label) {
  TabItem item(std::move(icon), std::move(semantic_label));
  item.show_label_ = false;
  return item;
}

TabItem TabItem::Enabled(bool enabled) && {
  enabled_ = enabled;
  return std::move(*this);
}

Tabs::Tabs(std::vector<StringVariant> labels, std::size_t selected_index)
    : detail::TypedView<Tabs>(MakeTabsSpec(std::move(labels), selected_index)) {}

Tabs::Tabs(std::vector<TabItem> items, std::size_t selected_index)
    : detail::TypedView<Tabs>(MakeTabsSpec(std::move(items), selected_index)) {}

} // namespace huxerui
