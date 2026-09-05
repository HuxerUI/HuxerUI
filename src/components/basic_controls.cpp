#include <huxerui/view.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "runtime/view_internal.h"
#include "indication_internal.h"
#include "resources/resource_internal.h"

namespace huxerui {

namespace {

using detail::ResolveStyleOverride;
using detail::ResolveComponentLabel;

void ApplyButtonDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const ButtonStyle style = ResolveStyleOverride<ButtonStyle>(environment).value_or(detail::DefaultButtonStyle(theme));
  spec.properties.padding = style.padding;
  spec.properties.background = style.background;
  spec.properties.disabled_background = style.disabled_background;
  spec.properties.text_style = style.label_style;
  spec.properties.text_layout_options.align = TextAlign::Center;
  spec.properties.text_layout_options.vertical_align = TextVerticalAlign::Center;
  spec.properties.text_layout_options.wrap = TextWrap::NoWrap;
  spec.properties.disabled_foreground = style.disabled_label;
  spec.properties.corner_radii = style.corner_radius;
  spec.properties.frame.min_width = std::max(0.0F, style.minimum_width);
  spec.properties.frame.min_height = std::max(0.0F, style.minimum_height);
  spec.default_indication = style.indication;
  spec.properties.disabled_opacity = 1.0F;
  ResolveComponentLabel(spec);
}

void ApplyIconButtonDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const IconButtonStyle style =
      ResolveStyleOverride<IconButtonStyle>(environment).value_or(detail::DefaultIconButtonStyle(theme));
  const float icon_size = std::max(0.0F, style.icon_size);
  const float interactive_size = std::max(icon_size, std::max(0.0F, style.minimum_interactive_size));
  const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), interactive_size);
  const float corner_radius = std::max(0.0F, style.corner_radius);
  spec.properties.text_style = TextStyle{Font::System(theme.typography.label_large), style.foreground};
  spec.properties.text_layout_options.align = TextAlign::Leading;
  spec.properties.text_layout_options.vertical_align = TextVerticalAlign::Center;
  spec.properties.text_layout_options.wrap = TextWrap::NoWrap;
  spec.properties.disabled_foreground = style.disabled_foreground;
  spec.layout_values.insert_or_assign(
      typeid(detail::LabelContentMetrics),
      detail::MakeErasedLayoutValue(detail::LabelContentMetrics{{icon_size, icon_size}, 0.0F, false})
  );
  spec.properties.corner_radii = corner_radius;
  spec.properties.frame.min_width = interactive_size;
  spec.properties.frame.min_height = interactive_size;
  Indication indication = style.indication.value_or(theme.interactions.indication);
  indication.geometry.layer_size = Size{state_layer_size, state_layer_size};
  indication.geometry.clip_corner_radii = CornerRadii{std::min(corner_radius, state_layer_size * 0.5F)};
  spec.default_indication = std::move(indication);
  if (spec.image_properties.IsVector()) {
    spec.properties.disabled_opacity = 1.0F;
  }
  ResolveComponentLabel(spec);
  if (detail::StringLiteral(spec.text).find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
    throw std::invalid_argument("HuxerUI IconButton requires a non-empty semantic label");
  }
}

void ApplyChipDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const ChipStyle style = ResolveStyleOverride<ChipStyle>(environment).value_or(detail::DefaultChipStyle(theme));
  const bool selected = spec.chip_selection.value_or(false);
  spec.properties.padding = style.padding;
  spec.properties.background = selected ? style.selected_background : style.background;
  spec.properties.disabled_background = selected ? style.disabled_selected_background : style.disabled_background;
  spec.properties.border = Border{
      selected ? style.selected_border : style.border,
      std::max(0.0F, style.border_width),
  };
  spec.properties.disabled_border = Border{
      selected ? style.disabled_selected_border : style.disabled_border,
      std::max(0.0F, style.border_width),
  };
  spec.properties.text_style = style.label_style;
  spec.properties.text_style.foreground = selected ? style.selected_label : style.label_style.foreground;
  spec.properties.text_layout_options.align =
      spec.image_properties.HasValue() ? TextAlign::Leading : TextAlign::Center;
  spec.properties.text_layout_options.vertical_align = TextVerticalAlign::Center;
  spec.properties.text_layout_options.wrap = TextWrap::NoWrap;
  spec.properties.disabled_foreground = selected ? style.disabled_selected_label : style.disabled_label;
  if (spec.image_properties.HasValue()) {
    spec.layout_values.insert_or_assign(
        typeid(detail::LabelContentMetrics),
        detail::MakeErasedLayoutValue(detail::LabelContentMetrics{
            {std::max(0.0F, style.icon_size), std::max(0.0F, style.icon_size)},
            std::max(0.0F, style.icon_spacing),
            true,
        })
    );
  }
  spec.properties.corner_radii = style.corner_radius;
  spec.properties.frame.min_height = std::max(0.0F, style.minimum_height);
  spec.default_indication =
      selected && style.selected_indication.has_value() ? style.selected_indication : style.indication;
  spec.properties.disabled_opacity = 1.0F;
  ResolveComponentLabel(spec);
  if (spec.image_properties.HasValue() && detail::StringLiteral(spec.text).empty()) {
    throw std::invalid_argument("HuxerUI Chip with an icon requires a non-empty label");
  }
}

void ApplyDividerDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const DividerStyle style =
      ResolveStyleOverride<DividerStyle>(environment).value_or(detail::DefaultDividerStyle(theme));
  spec.properties.background = style.color;
  spec.layout_values.insert_or_assign(
      typeid(detail::DividerThicknessBinding),
      detail::MakeErasedLayoutValue(std::max(0.0F, style.thickness))
  );
}

void ActivateClick(const detail::EventBindings& bindings) {
  detail::EmitEvent<ViewEvents::Click>(bindings);
}

std::shared_ptr<detail::ViewSpec> MakeButtonSpec(StringVariant label) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Button);
  spec->defaults = ApplyButtonDefaults;
  spec->text = std::move(label);
  spec->focusable = true;
  spec->activation = ActivateClick;
  spec->component_semantics.role = SemanticRole::Button;
  spec->modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeIconButtonSpec(ImageVariant icon, StringVariant semantic_label) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::IconButton);
  spec->defaults = ApplyIconButtonDefaults;
  spec->text = std::move(semantic_label);
  spec->image_properties.SetImage(std::move(icon));
  spec->focusable = true;
  spec->activation = ActivateClick;
  spec->component_semantics.role = SemanticRole::Button;
  spec->modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeChipSpec(
    StringVariant label,
    std::optional<bool> selection,
    std::optional<ImageVariant> icon = std::nullopt
) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Chip);
  spec->defaults = ApplyChipDefaults;
  spec->text = std::move(label);
  if (icon.has_value()) {
    spec->image_properties.SetImage(std::move(*icon));
  }
  spec->focusable = true;
  spec->chip_selection = selection;
  spec->component_semantics.role = SemanticRole::Button;
  spec->component_semantics.selected = selection;
  if (selection.has_value()) {
    const bool selected = *selection;
    spec->activation = [selected](const detail::EventBindings& bindings) {
      detail::EmitEvent<ViewEvents::Click>(bindings);
      detail::EmitEvent<ToggleEvents::Changed>(bindings, !selected);
    };
  } else {
    spec->activation = ActivateClick;
  }
  spec->modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeDividerSpec(Axis axis) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Divider);
  spec->defaults = ApplyDividerDefaults;
  spec->layout_values.insert_or_assign(typeid(detail::DividerAxisBinding), detail::MakeErasedLayoutValue(axis));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeCanvasSpec(CanvasPainter painter) {
  if (!painter) {
    throw std::invalid_argument("HuxerUI canvas painter must not be empty");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Canvas);
  spec->canvas_painter = std::move(painter);
  return spec;
}

} // namespace

std::shared_ptr<detail::ViewSpec> MakeImageSpec(ImageVariant image) {
  detail::ValidateImageVariant(image);
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Image);
  spec->image_properties.SetImage(std::move(image));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeExternalTextureSpec(std::shared_ptr<ExternalTexture> texture) {
  if (!texture) {
    throw std::invalid_argument("HuxerUI image view asset must not be empty");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Image);
  spec->image_properties.source = std::move(texture);
  return spec;
}

Button::Button(StringVariant label) : View(MakeButtonSpec(std::move(label))) {}

IconButton::IconButton(ImageVariant icon, StringVariant semantic_label)
    : detail::TypedView<IconButton>([&] {
        detail::ValidateImageVariant(icon);
        if (detail::IsBlankStringVariantLiteral(semantic_label)) {
          throw std::invalid_argument("HuxerUI IconButton requires a non-empty semantic label");
        }
        return MakeIconButtonSpec(std::move(icon), std::move(semantic_label));
      }()) {}

Chip::Chip(StringVariant label) : detail::TypedView<Chip>(MakeChipSpec(std::move(label), std::nullopt)) {}

Chip::Chip(StringVariant label, bool selected)
    : detail::TypedView<Chip>(MakeChipSpec(std::move(label), selected)) {}

Chip::Chip(ImageVariant icon, StringVariant label)
    : detail::TypedView<Chip>([&] {
        detail::ValidateImageVariant(icon);
        if (detail::IsBlankStringVariantLiteral(label)) {
          throw std::invalid_argument("HuxerUI Chip with an icon requires a non-empty label");
        }
        return MakeChipSpec(std::move(label), std::nullopt, std::move(icon));
      }()) {}

Chip::Chip(ImageVariant icon, StringVariant label, bool selected)
    : detail::TypedView<Chip>([&] {
        detail::ValidateImageVariant(icon);
        if (detail::IsBlankStringVariantLiteral(label)) {
          throw std::invalid_argument("HuxerUI Chip with an icon requires a non-empty label");
        }
        return MakeChipSpec(std::move(label), selected, std::move(icon));
      }()) {}

Divider::Divider(Axis axis) : View(MakeDividerSpec(axis)) {}

Image::Image(ImageVariant image) : View(MakeImageSpec(std::move(image))) {}

Image::Image(std::shared_ptr<ExternalTexture> texture) : View(MakeExternalTextureSpec(std::move(texture))) {}

Image Image::Fit(ImageFit fit) && {
  SetImageFit(fit);
  return std::move(*this);
}

Image Image::Align(HorizontalAlignment horizontal, VerticalAlignment vertical) && {
  SetImageAlignment(horizontal, vertical);
  return std::move(*this);
}

Image Image::Sampling(ImageSampling sampling) && {
  SetImageSampling(sampling);
  return std::move(*this);
}

Image Image::Tint(Color tint) && {
  SetImageTint(tint);
  return std::move(*this);
}

Canvas::Canvas(CanvasPainter painter) : View(MakeCanvasSpec(std::move(painter))) {}

} // namespace huxerui
