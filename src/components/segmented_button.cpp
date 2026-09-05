#include <huxerui/view.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"
#include "indication_internal.h"
#include "resources/resource_internal.h"

namespace huxerui {

namespace detail {

std::optional<ResolvedImageAsset> InternalAccess::ResolveIcon(SegmentedButtonItem& item) {
  return ResolveOptionalControlIcon(item.icon_);
}

std::string InternalAccess::ResolveLabel(SegmentedButtonItem& item) {
  return UseString(std::move(item.label_));
}

bool InternalAccess::ShowsLabel(const SegmentedButtonItem& item) noexcept {
  return item.show_label_;
}

bool InternalAccess::HasIcon(const SegmentedButtonItem& item) noexcept {
  return item.icon_.has_value();
}

bool InternalAccess::HasBlankLiteralLabel(const SegmentedButtonItem& item) noexcept {
  return IsBlankStringVariantLiteral(item.label_);
}

void InternalAccess::ValidateIcon(const SegmentedButtonItem& item) {
  if (item.icon_.has_value()) {
    ValidateImageVariant(*item.icon_);
  }
}

} // namespace detail

namespace {

using detail::ResolveStyleOverride;

struct SegmentedButtonBorderWidth {
  using Value = float;
};

struct SegmentedButtonBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  std::size_t selected_index;
  EventEmitter events;
};

class SegmentedButtonBehaviorExtension final : public NodeExtension {
public:
  SegmentedButtonBehaviorExtension(MountedNode& node, const SegmentedButtonBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode&, const SegmentedButtonBehavior& modifier) {
    selected_index_ = modifier.selected_index;
    events_ = modifier.events;
  }

  bool OnKey(MountedNode& node, const KeyEvent& event) override {
    const std::size_t segment_count = node.ChildCount();
    if (!node.IsEnabled() || segment_count == 0 || event.type != KeyEventType::Down || event.modifiers.alt ||
        event.modifiers.control || event.modifiers.meta) {
      return false;
    }
    std::optional<std::size_t> requested;
    if (event.key == Key::ArrowLeft) {
      requested = selected_index_ == 0 ? segment_count - 1 : selected_index_ - 1;
    } else if (event.key == Key::ArrowRight) {
      requested = selected_index_ + 1 == segment_count ? 0 : selected_index_ + 1;
    } else if (event.key == Key::Home) {
      requested = 0;
    } else if (event.key == Key::End) {
      requested = segment_count - 1;
    }
    if (requested.has_value() && *requested != selected_index_) {
      events_.Emit<SegmentedButtonEvents::Changed>(*requested);
    }
    return requested.has_value();
  }

private:
  EventEmitter events_;
  std::size_t selected_index_ = 0;
};

const detail::ModifierDescriptor& SegmentedButtonBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<SegmentedButtonBehavior, SegmentedButtonBehaviorExtension>();
}

struct SegmentedButtonLayoutPolicy {
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    const std::size_t count = node.ChildCount();
    if (count == 0) {
      LayoutResult empty;
      empty.SetSize(constraints.Constrain({}));
      return empty;
    }

    float maximum_width = 0.0F;
    float maximum_height = 0.0F;
    for (MountedNode& child : node.Children()) {
      const Size size = context.Measure(child, constraints.Loose());
      maximum_width = std::max(maximum_width, size.width);
      maximum_height = std::max(maximum_height, size.height);
    }

    const float requested_overlap = std::max(0.0F, node.LayoutValueOr<SegmentedButtonBorderWidth>(0.0F));
    const float count_value = static_cast<float>(count);
    const float natural_width =
        maximum_width * count_value - std::min(requested_overlap, maximum_width) * static_cast<float>(count - 1);
    const float width = constraints.ConstrainWidth(natural_width);
    const float overlap = std::min(requested_overlap, std::max(0.0F, width));
    const float segment_width = (width + overlap * static_cast<float>(count - 1)) / count_value;
    const float height = constraints.ConstrainHeight(maximum_height);

    LayoutResult result;
    float x = 0.0F;
    for (MountedNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, {segment_width, segment_width, height, height}));
      result.Place(child, {x, 0.0F});
      x += segment_width - overlap;
    }
    result.SetSize({width, height});
    return result;
  }
};

CornerRadii SegmentCornerRadii(std::size_t index, std::size_t count, float radius) {
  const float value = std::max(0.0F, radius);
  if (count <= 1) {
    return CornerRadii{value};
  }
  if (index == 0) {
    return {value, 0.0F, 0.0F, value};
  }
  if (index + 1 == count) {
    return {0.0F, value, value, 0.0F};
  }
  return {};
}

struct ResolvedSegmentedButtonItem {
  std::string label;
  std::optional<detail::ResolvedImageAsset> icon;
  bool show_label = true;
};

class SegmentedButtonLabel final : public View {
public:
  SegmentedButtonLabel(
      std::string label,
      std::optional<detail::ResolvedImageAsset> icon,
      bool show_label,
      const SegmentedButtonStyle& style,
      bool selected,
      std::size_t index,
      std::size_t count
  )
      : View(MakeSpec(std::move(label), std::move(icon), show_label, style, selected, index, count)) {
    TextStyle text_style = style.label_style;
    if (selected) {
      text_style.foreground = style.selected_label;
    }
    SetTextStyle(std::move(text_style));
  }

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(
      std::string label,
      std::optional<detail::ResolvedImageAsset> icon,
      bool show_label,
      const SegmentedButtonStyle& style,
      bool selected,
      std::size_t index,
      std::size_t count
  ) {
    auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
    spec->text = std::move(label);
    if (icon.has_value()) {
      spec->image_properties.SetResolvedAsset(std::move(*icon));
      spec->layout_values.insert_or_assign(
          typeid(detail::LabelContentMetrics),
          detail::MakeErasedLayoutValue(detail::LabelContentMetrics{
              {std::max(0.0F, style.icon_size), std::max(0.0F, style.icon_size)},
              std::max(0.0F, style.icon_spacing),
              show_label,
          })
      );
    }
    spec->properties.padding = style.padding;
    spec->properties.background = selected ? style.selected_background : style.background;
    spec->properties.border = Border{
        selected ? style.selected_border : style.border,
        std::max(0.0F, style.border_width),
    };
    spec->properties.corner_radii = SegmentCornerRadii(index, count, style.corner_radius);
    spec->properties.frame.min_width = std::max(0.0F, style.minimum_segment_width);
    spec->properties.frame.min_height = std::max(0.0F, style.minimum_height);
    spec->properties.text_layout_options = {
        .shaping = {},
        .align = spec->image_properties.HasValue() ? TextAlign::Leading : TextAlign::Center,
        .vertical_align = TextVerticalAlign::Center,
        .wrap = TextWrap::NoWrap,
    };
    spec->default_indication =
        selected && style.selected_indication.has_value() ? style.selected_indication : style.indication;
    spec->component_semantics.role = SemanticRole::RadioButton;
    spec->component_semantics.label = detail::StringLiteral(spec->text);
    spec->component_semantics.checked =
        selected ? SemanticCheckedState::Checked : SemanticCheckedState::Unchecked;
    spec->component_semantics.selected = selected;
    spec->component_semantics.collection_item = SemanticCollectionItem{
        .index = index,
        .row_index = 0,
        .column_index = index,
    };
    spec->modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{spec->default_indication}));
    return spec;
  }
};

class SegmentedButtonLayoutView final : public View {
public:
  SegmentedButtonLayoutView(
      std::vector<View> segments,
      std::size_t selected_index,
      const SegmentedButtonStyle& style,
      EventEmitter events
  )
      : View(MakeSpec(std::move(segments), selected_index, style, std::move(events))) {}

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(
      std::vector<View> segments,
      std::size_t selected_index,
      const SegmentedButtonStyle& style,
      EventEmitter events
  ) {
    auto spec = detail::MakeLayoutSpec(detail::LayoutDescriptorFor<SegmentedButtonLayoutPolicy>(), std::move(segments));
    spec->focusable = true;
    spec->component_semantics.collection = SemanticCollection{
        .item_count = spec->children.size(),
        .row_count = 1,
        .column_count = spec->children.size(),
    };
    spec->properties.corner_radii = std::max(0.0F, style.corner_radius);
    spec->properties.clip_children = true;
    spec->layout_values.insert_or_assign(
        typeid(SegmentedButtonBorderWidth),
        detail::MakeErasedLayoutValue(std::max(0.0F, style.border_width))
    );
    spec->modifiers.push_back(detail::MakeModifierSpec(SegmentedButtonBehavior{
        selected_index,
        std::move(events),
    }));
    return spec;
  }
};

std::shared_ptr<detail::ViewSpec>
MakeSegmentedButtonSpec(std::vector<SegmentedButtonItem> items, std::size_t selected_index) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI SegmentedButton requires at least one item");
  }
  if (selected_index >= items.size()) {
    throw std::invalid_argument("HuxerUI SegmentedButton selected index is out of range");
  }

  for (const SegmentedButtonItem& item : items) {
    if (detail::InternalAccess::HasBlankLiteralLabel(item)) {
      throw std::invalid_argument("HuxerUI SegmentedButton item requires a non-empty semantic label");
    }
    if (!detail::InternalAccess::ShowsLabel(item) && !detail::InternalAccess::HasIcon(item)) {
      throw std::invalid_argument("HuxerUI icon-only SegmentedButton item requires an icon and semantic label");
    }
    detail::InternalAccess::ValidateIcon(item);
  }

  return detail::MakeScopeSpec([items = std::move(items), selected_index]() -> View {
    std::vector<ResolvedSegmentedButtonItem> resolved_items;
    resolved_items.reserve(items.size());
    for (SegmentedButtonItem item : items) {
      ResolvedSegmentedButtonItem resolved{
          detail::InternalAccess::ResolveLabel(item),
          detail::InternalAccess::ResolveIcon(item),
          detail::InternalAccess::ShowsLabel(item),
      };
      if (resolved.label.empty()) {
        throw std::invalid_argument("HuxerUI SegmentedButton item requires a non-empty semantic label");
      }
      resolved_items.push_back(std::move(resolved));
    }

    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
    const SegmentedButtonStyle style =
        ResolveStyleOverride<SegmentedButtonStyle>(environment).value_or(detail::DefaultSegmentedButtonStyle(theme));
    const EventEmitter events = UseEvents();

    std::vector<View> segments;
    segments.reserve(resolved_items.size());
    for (std::size_t index = 0; index < resolved_items.size(); ++index) {
      segments.push_back(
          std::move(SegmentedButtonLabel(
                        resolved_items[index].label,
                        resolved_items[index].icon,
                        resolved_items[index].show_label,
                        style,
                        index == selected_index,
                        index,
                        resolved_items.size()
                    ))
              .OnClick([events, index, selected_index] {
                if (index != selected_index) {
                  events.Emit<SegmentedButtonEvents::Changed>(index);
                }
              })
              .Key(index)
      );
    }

    return SegmentedButtonLayoutView(std::move(segments), selected_index, style, events);
  });
}

std::shared_ptr<detail::ViewSpec>
MakeSegmentedButtonSpec(std::vector<StringVariant> labels, std::size_t selected_index) {
  std::vector<SegmentedButtonItem> items;
  items.reserve(labels.size());
  for (StringVariant& label : labels) {
    items.emplace_back(std::move(label));
  }
  return MakeSegmentedButtonSpec(std::move(items), selected_index);
}

} // namespace

SegmentedButtonItem::SegmentedButtonItem(StringVariant label) : label_(std::move(label)) {}

SegmentedButtonItem::SegmentedButtonItem(ImageVariant icon, StringVariant label)
    : icon_(std::move(icon)), label_(std::move(label)) {
  detail::ValidateImageVariant(*icon_);
}

SegmentedButtonItem SegmentedButtonItem::IconOnly(ImageVariant icon, StringVariant semantic_label) {
  SegmentedButtonItem item(std::move(icon), std::move(semantic_label));
  item.show_label_ = false;
  return item;
}

SegmentedButton::SegmentedButton(std::vector<StringVariant> labels, std::size_t selected_index)
    : detail::TypedView<SegmentedButton>(MakeSegmentedButtonSpec(std::move(labels), selected_index)) {}

SegmentedButton::SegmentedButton(std::vector<SegmentedButtonItem> items, std::size_t selected_index)
    : detail::TypedView<SegmentedButton>(MakeSegmentedButtonSpec(std::move(items), selected_index)) {}

} // namespace huxerui
