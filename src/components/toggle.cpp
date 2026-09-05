#include <huxerui/view.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include <huxerui/semantics.h>
#include <huxerui/animation.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "runtime/mounted_node_internal.h"
#include "indication_internal.h"
#include "resources/resource_internal.h"

namespace huxerui {

namespace {

using detail::ResolveStyleOverride;
using detail::ResolveComponentLabel;
using detail::AppliesDisabledAppearance;

enum class ToggleVisualKind {
  Checkbox,
  RadioButton,
  Switch,
};

struct CheckboxStyleBinding {
  using Value = CheckboxStyle;
};

struct RadioButtonStyleBinding {
  using Value = RadioButtonStyle;
};

struct SwitchStyleBinding {
  using Value = SwitchStyle;
};

struct ToggleVisual {
  static const detail::ModifierDescriptor& Descriptor();

  ToggleVisualKind kind;
  bool checked;
  std::optional<ImageVariant> checkmark;

  bool operator==(const ToggleVisual&) const = default;
};

ToggleVisual CompileToggleVisual(
    const ToggleVisual& declaration,
    const std::shared_ptr<const Environment>& environment,
    detail::AppResources& resources
);

class ToggleVisualExtension final : public NodeExtension {
public:
  ToggleVisualExtension(ViewNode& node, const ToggleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(ViewNode& node, const ToggleVisual& modifier) {
    kind_ = modifier.kind;
    if (kind_ == ToggleVisualKind::Checkbox) {
      if (!modifier.checkmark.has_value() || !std::holds_alternative<VectorAsset>(*modifier.checkmark)) {
        throw std::logic_error("HuxerUI Checkbox checkmark must resolve to a vector image");
      }
      checkmark_ = std::get<VectorAsset>(*modifier.checkmark);
      checkbox_style_ = node.LayoutValueOr<CheckboxStyleBinding>(CheckboxStyle::Default());
    } else if (kind_ == ToggleVisualKind::RadioButton) {
      checkmark_ = {};
      radio_button_style_ = node.LayoutValueOr<RadioButtonStyleBinding>(RadioButtonStyle::Default());
    } else {
      checkmark_ = {};
      switch_style_ = node.LayoutValueOr<SwitchStyleBinding>(SwitchStyle::Default());
    }
    if (!initialized_) {
      checked_ = modifier.checked;
      progress_.Set(checked_ ? 1.0F : 0.0F);
      initialized_ = true;
      return;
    }
    if (checked_ != modifier.checked) {
      checked_ = modifier.checked;
      target_pending_ = true;
    }
  }

  NodeExtension::FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_progress = progress_.Value();
    if (kind_ == ToggleVisualKind::Checkbox) {
      progress_.Set(checked_ ? 1.0F : 0.0F);
      target_pending_ = false;
      if (progress_.Value() != previous_progress) {
        InvalidatePaint();
      }
      return {};
    }
    if (target_pending_) {
      const double duration = kind_ == ToggleVisualKind::RadioButton ? radio_button_style_.animation_duration
                                                                     : switch_style_.animation_duration;
      progress_.AnimateTo(checked_ ? 1.0F : 0.0F, TweenSpec{duration});
      target_pending_ = false;
    }
    const MotionAdvanceResult result = progress_.Advance(frame);
    if (progress_.Value() != previous_progress) {
      InvalidatePaint();
    }
    return {
        .needs_frame = result.needs_frame,
        .wake_after = result.wake_after,
    };
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    std::optional<Rect> indication_bounds;
    if (kind_ == ToggleVisualKind::Switch) {
      const Rect track = detail::ResolveToggleControlBounds(mounted);
      const float state_layer_size =
          std::min(std::max(0.0F, switch_style_.state_layer_size), std::min(node.Bounds().width, node.Bounds().height));
      const float thumb_center_x =
          track.x + track.height * 0.5F + std::max(0.0F, track.width - track.height) * progress_.Value();
      indication_bounds = Rect{
          thumb_center_x - state_layer_size * 0.5F,
          track.y + (track.height - state_layer_size) * 0.5F,
          state_layer_size,
          state_layer_size,
      };
    } else {
      const Rect control = detail::ResolveToggleControlBounds(mounted);
      const detail::ToggleLayoutMetrics metrics = node.LayoutValueOr<detail::ToggleLayoutMetrics>({});
      const float configured_state_layer_size =
          kind_ == ToggleVisualKind::Checkbox ? checkbox_style_.state_layer_size : radio_button_style_.state_layer_size;
      const float size = std::min(
          std::max(0.0F, configured_state_layer_size),
          std::min(metrics.interactive_size.width, metrics.interactive_size.height)
      );
      indication_bounds = Rect{
          control.x + control.width * 0.5F - size * 0.5F,
          control.y + control.height * 0.5F - size * 0.5F,
          size,
          size,
      };
    }
    mounted.indication_bounds_override = indication_bounds;
    return PaintInvalidation::None;
  }

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
    if (kind_ == ToggleVisualKind::Checkbox) {
      PaintCheckbox(node, context);
    } else if (kind_ == ToggleVisualKind::RadioButton) {
      PaintRadioButton(node, context);
    } else {
      PaintSwitch(node, context);
    }
  }

private:
  void PaintCheckbox(const ViewNode& node, PaintContext& context) const {
    const Rect frame = detail::ResolveToggleControlBounds(static_cast<const detail::MountedNode&>(node));
    const bool disabled = AppliesDisabledAppearance(node);
    if (checked_) {
      const Color background =
          disabled ? checkbox_style_.disabled_checked_background : checkbox_style_.checked_background;
      const Color checkmark = disabled ? checkbox_style_.disabled_checkmark : checkbox_style_.checkmark;
      context.DrawRect(frame, background, std::max(0.0F, checkbox_style_.corner_radius));
      context.DrawImage(checkmark_, frame, checkmark);
      return;
    }
    context.DrawBorder(
        frame,
        disabled ? checkbox_style_.disabled_unchecked_border : checkbox_style_.unchecked_border,
        StrokeStyle{.width = std::max(0.0F, checkbox_style_.border_width)},
        std::max(0.0F, checkbox_style_.corner_radius)
    );
  }

  void PaintRadioButton(const ViewNode& node, PaintContext& context) const {
    constexpr float full_circle = 6.28318530717958647692F;
    const Rect frame = detail::ResolveToggleControlBounds(static_cast<const detail::MountedNode&>(node));
    const float progress = progress_.Value();
    const bool disabled = AppliesDisabledAppearance(node);
    const Color unselected =
        disabled ? radio_button_style_.disabled_unselected_color : radio_button_style_.unselected_color;
    const Color selected = disabled ? radio_button_style_.disabled_selected_color : radio_button_style_.selected_color;
    const Color color = detail::InterpolateColor(unselected, selected, progress);
    const float maximum_radius = std::max(0.0F, std::min(frame.width, frame.height) * 0.5F);
    const float border_width = std::clamp(radio_button_style_.border_width, 0.0F, maximum_radius);
    const Point center{
        frame.x + frame.width * 0.5F,
        frame.y + frame.height * 0.5F,
    };
    context.DrawArc(center, std::max(0.0F, maximum_radius - border_width * 0.5F), 0.0F, full_circle, color,
                    StrokeStyle{.width = border_width});
    const float dot_radius = std::clamp(radio_button_style_.dot_radius * progress, 0.0F, maximum_radius);
    if (dot_radius > 0.0F) {
      context.DrawCircle(center, dot_radius, color);
    }
  }

  void PaintSwitch(const ViewNode& node, PaintContext& context) const {
    const Rect frame = detail::ResolveToggleControlBounds(static_cast<const detail::MountedNode&>(node));
    const float progress = progress_.Value();
    const bool disabled = AppliesDisabledAppearance(node);
    const Color track =
        disabled
            ? detail::InterpolateColor(
                  switch_style_.disabled_unchecked_track, switch_style_.disabled_checked_track, progress)
            : detail::InterpolateColor(switch_style_.unchecked_track, switch_style_.checked_track, progress);
    const Color border =
        disabled ? detail::InterpolateColor(
                       switch_style_.disabled_unchecked_track_border,
                       switch_style_.disabled_checked_track_border,
                       progress
                   )
                 : detail::InterpolateColor(
                       switch_style_.unchecked_track_border, switch_style_.checked_track_border, progress);
    context.DrawRect(frame, track, std::max(0.0F, switch_style_.corner_radius));

    if (switch_style_.track_border_width > 0.0F && border.alpha > 0.0F) {
      context.DrawBorder(frame, border, StrokeStyle{.width = switch_style_.track_border_width},
                         std::max(0.0F, switch_style_.corner_radius));
    }

    const float maximum_radius = std::max(0.0F, std::min(frame.width, frame.height) * 0.5F);
    const float radius = std::clamp(
        switch_style_.unchecked_thumb_radius +
            (switch_style_.checked_thumb_radius - switch_style_.unchecked_thumb_radius) * progress,
        0.0F,
        maximum_radius
    );
    const float start_x = frame.x + frame.height * 0.5F;
    const float travel = std::max(0.0F, frame.width - frame.height);
    const Color thumb =
        disabled
            ? detail::InterpolateColor(
                  switch_style_.disabled_unchecked_thumb, switch_style_.disabled_checked_thumb, progress)
            : detail::InterpolateColor(switch_style_.unchecked_thumb, switch_style_.checked_thumb, progress);
    context.DrawCircle(
        {
            start_x + travel * progress,
            frame.y + frame.height * 0.5F,
        },
        radius,
        thumb
    );
  }

  ToggleVisualKind kind_ = ToggleVisualKind::Checkbox;
  CheckboxStyle checkbox_style_;
  RadioButtonStyle radio_button_style_;
  SwitchStyle switch_style_;
  VectorAsset checkmark_;
  MotionController progress_;
  bool checked_ = false;
  bool initialized_ = false;
  bool target_pending_ = false;
};

const detail::ModifierDescriptor& ToggleVisual::Descriptor() {
  static const detail::ModifierDescriptor descriptor = [] {
    detail::ModifierDescriptor result = detail::ModifierDescriptorFor<ToggleVisual, ToggleVisualExtension>();
    result.compile = [](detail::ViewSpec&,
                        detail::ModifierSpec& modifier,
                        const std::shared_ptr<const Environment>& environment,
                        detail::AppResources& resources) {
      const auto& declaration = *static_cast<const ToggleVisual*>(modifier.value.get());
      if (!declaration.checkmark.has_value() || !detail::NeedsResourceResolution(*declaration.checkmark)) {
        return;
      }
      modifier.value = std::make_shared<ToggleVisual>(
          CompileToggleVisual(declaration, environment, resources)
      );
    };
    return result;
  }();
  return descriptor;
}

ToggleVisual CompileToggleVisual(
    const ToggleVisual& declaration,
    const std::shared_ptr<const Environment>& environment,
    detail::AppResources& resources
) {
  if (!declaration.checkmark.has_value() || !detail::NeedsResourceResolution(*declaration.checkmark)) {
    return declaration;
  }
  const Locale locale = detail::ResolveResourceLocale(environment, resources);
  detail::ResolvedImageAsset resolved = detail::ResolveImage(*declaration.checkmark, resources, locale);
  if (!std::holds_alternative<VectorAsset>(resolved)) {
    throw std::invalid_argument("HuxerUI Checkbox checkmark resource must contain a vector image");
  }
  ToggleVisual compiled = declaration;
  compiled.checkmark = std::get<VectorAsset>(std::move(resolved));
  return compiled;
}

void ApplyToggleLayoutDefaults(
    detail::ViewSpec& spec,
    const std::shared_ptr<const Environment>& environment,
    const ThemeSpec& theme,
    detail::ToggleLayoutMetrics metrics
) {
  metrics.visual_size.width = std::max(0.0F, metrics.visual_size.width);
  metrics.visual_size.height = std::max(0.0F, metrics.visual_size.height);
  metrics.interactive_size.width = std::max(metrics.visual_size.width, metrics.interactive_size.width);
  metrics.interactive_size.height = std::max(metrics.visual_size.height, metrics.interactive_size.height);
  metrics.label_spacing = std::max(0.0F, metrics.label_spacing);
  spec.layout_values.insert_or_assign(typeid(detail::ToggleLayoutMetrics), detail::MakeErasedLayoutValue(metrics));
  if (detail::StringLiteral(spec.text).empty()) {
    spec.properties.frame.width = metrics.interactive_size.width;
    spec.properties.frame.height = metrics.interactive_size.height;
    return;
  }

  spec.properties.frame.min_width = metrics.interactive_size.width;
  spec.properties.frame.min_height = metrics.interactive_size.height;
  spec.properties.text_style =
      ResolveStyleOverride<TextStyle>(environment).value_or(detail::DefaultTextStyle(theme, TextRole::Body));
  spec.properties.text_layout_options = {
      .shaping = {},
      .vertical_align = TextVerticalAlign::Center,
      .wrap = TextWrap::NoWrap,
  };
  Color disabled_label = spec.properties.text_style.foreground;
  disabled_label.alpha *= spec.properties.disabled_opacity;
  spec.properties.disabled_foreground = disabled_label;
}

void ApplyCheckboxDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const CheckboxStyle style =
      ResolveStyleOverride<CheckboxStyle>(environment).value_or(detail::DefaultCheckboxStyle(theme));
  spec.layout_values.insert_or_assign(typeid(CheckboxStyleBinding), detail::MakeErasedLayoutValue(style));
  const float interactive_size = std::max(0.0F, std::max(style.size, style.minimum_interactive_size));
  const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), interactive_size);
  ApplyToggleLayoutDefaults(
      spec,
      environment,
      theme,
      {{style.size, style.size}, {interactive_size, interactive_size}, theme.spacing.small}
  );
  spec.properties.corner_radii = state_layer_size * 0.5F;
  Indication indication = theme.interactions.indication;
  indication.geometry.layer_size = Size{state_layer_size, state_layer_size};
  indication.geometry.clip_corner_radii = CornerRadii{state_layer_size * 0.5F};
  spec.default_indication = std::move(indication);
  spec.properties.disabled_opacity = 1.0F;
  ResolveComponentLabel(spec);
}

void ApplyRadioButtonDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const RadioButtonStyle style =
      ResolveStyleOverride<RadioButtonStyle>(environment).value_or(detail::DefaultRadioButtonStyle(theme));
  spec.layout_values.insert_or_assign(typeid(RadioButtonStyleBinding), detail::MakeErasedLayoutValue(style));
  const float interactive_size = std::max(0.0F, std::max(style.size, style.minimum_interactive_size));
  const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), interactive_size);
  ApplyToggleLayoutDefaults(
      spec,
      environment,
      theme,
      {{style.size, style.size}, {interactive_size, interactive_size}, theme.spacing.small}
  );
  spec.properties.corner_radii = state_layer_size * 0.5F;
  Indication indication = theme.interactions.indication;
  indication.geometry.layer_size = Size{state_layer_size, state_layer_size};
  indication.geometry.clip_corner_radii = CornerRadii{state_layer_size * 0.5F};
  spec.default_indication = std::move(indication);
  spec.properties.disabled_opacity = 1.0F;
  ResolveComponentLabel(spec);
}

void ApplySwitchDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const SwitchStyle style = ResolveStyleOverride<SwitchStyle>(environment).value_or(detail::DefaultSwitchStyle(theme));
  spec.layout_values.insert_or_assign(typeid(SwitchStyleBinding), detail::MakeErasedLayoutValue(style));
  const float width = std::max(0.0F, style.width);
  const float height = std::max(0.0F, std::max(style.height, style.minimum_interactive_height));
  const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), std::min(width, height));
  ApplyToggleLayoutDefaults(
      spec,
      environment,
      theme,
      {{style.width, style.height}, {width, height}, theme.spacing.small}
  );
  spec.properties.corner_radii = state_layer_size * 0.5F;
  Indication indication = theme.interactions.indication;
  indication.geometry.layer_size = Size{state_layer_size, state_layer_size};
  indication.geometry.clip_corner_radii = CornerRadii{state_layer_size * 0.5F};
  spec.default_indication = std::move(indication);
  spec.properties.disabled_opacity = 1.0F;
  ResolveComponentLabel(spec);
}

std::shared_ptr<detail::ViewSpec>
MakeToggleSpec(detail::NodeKind kind, ToggleVisualKind visual_kind, bool checked, StringVariant label = {}) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  if (visual_kind == ToggleVisualKind::Checkbox) {
    spec->defaults = ApplyCheckboxDefaults;
  } else if (visual_kind == ToggleVisualKind::RadioButton) {
    spec->defaults = ApplyRadioButtonDefaults;
  } else {
    spec->defaults = ApplySwitchDefaults;
  }
  spec->text = std::move(label);
  spec->focusable = true;
  spec->component_semantics.role = visual_kind == ToggleVisualKind::Checkbox      ? SemanticRole::Checkbox
                                   : visual_kind == ToggleVisualKind::RadioButton ? SemanticRole::RadioButton
                                                                                  : SemanticRole::Switch;
  spec->component_semantics.checked = checked ? SemanticCheckedState::Checked : SemanticCheckedState::Unchecked;
  spec->activation = [visual_kind, checked](const detail::EventBindings& bindings) {
    detail::EmitEvent<ViewEvents::Click>(bindings);
    if (visual_kind == ToggleVisualKind::RadioButton && checked) {
      return;
    }
    detail::EmitEvent<ToggleEvents::Changed>(bindings, !checked);
  };
  spec->modifiers.push_back(
      detail::MakeModifierSpec(ToggleVisual{
          visual_kind,
          checked,
          visual_kind == ToggleVisualKind::Checkbox ? std::optional<ImageVariant>{images::check} : std::nullopt,
      })
  );
  spec->modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
  return spec;
}

} // namespace

namespace detail {

float ToggleLabelLeading(const ToggleLayoutMetrics& metrics) noexcept {
  return metrics.visual_size.width + metrics.label_spacing;
}

Rect ResolveToggleControlBounds(const MountedNode& node) noexcept {
  const ToggleLayoutMetrics metrics = node.LayoutValueOr<ToggleLayoutMetrics>({});
  const Rect content = node.ContentBounds();
  const float width = std::min(metrics.visual_size.width, content.width);
  const float height = std::min(metrics.visual_size.height, content.height);
  const float requested_horizontal_offset =
      node.text.PlainText().empty() ? (content.width - metrics.visual_size.width) * 0.5F : 0.0F;
  return {
      content.x + std::clamp(requested_horizontal_offset, 0.0F, content.width - width),
      content.y + std::max(0.0F, (content.height - height) * 0.5F),
      width,
      height,
  };
}

Rect ResolveToggleLabelBounds(const MountedNode& node) noexcept {
  if (node.text.PlainText().empty()) {
    return {};
  }
  const ToggleLayoutMetrics metrics = node.LayoutValueOr<ToggleLayoutMetrics>({});
  const Rect content = node.ContentBounds();
  const float leading = std::min(content.width, ToggleLabelLeading(metrics));
  return {
      content.x + leading,
      content.y,
      std::max(0.0F, content.width - leading),
      content.height,
  };
}

} // namespace detail

Checkbox::Checkbox(bool checked)
    : detail::TypedView<Checkbox>(MakeToggleSpec(detail::NodeKind::Checkbox, ToggleVisualKind::Checkbox, checked)) {}

Checkbox::Checkbox(StringVariant label, bool checked)
    : detail::TypedView<Checkbox>(MakeToggleSpec(
          detail::NodeKind::Checkbox,
          ToggleVisualKind::Checkbox,
          checked,
          std::move(label)
      )) {}

RadioButton::RadioButton(bool selected)
    : detail::TypedView<RadioButton>(
          MakeToggleSpec(detail::NodeKind::RadioButton, ToggleVisualKind::RadioButton, selected)
      ) {}

RadioButton::RadioButton(StringVariant label, bool selected)
    : detail::TypedView<RadioButton>(MakeToggleSpec(
          detail::NodeKind::RadioButton,
          ToggleVisualKind::RadioButton,
          selected,
          std::move(label)
      )) {}

Switch::Switch(bool checked)
    : detail::TypedView<Switch>(MakeToggleSpec(detail::NodeKind::Switch, ToggleVisualKind::Switch, checked)) {}

Switch::Switch(StringVariant label, bool checked)
    : detail::TypedView<Switch>(MakeToggleSpec(
          detail::NodeKind::Switch,
          ToggleVisualKind::Switch,
          checked,
          std::move(label)
      )) {}

} // namespace huxerui
