#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <huxerui/theme.h>

#include "internal.h"
#include "resource_internal.h"
#include "indication_internal.h"
#include "text_field_internal.h"

namespace huxerui {

namespace {

template <class Modifier, void (*Apply)(detail::ViewSpec&, const Modifier&)>
const detail::ModifierDescriptor& ApplyOnlyModifierDescriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, const void* value) { Apply(spec, *static_cast<const Modifier*>(value)); },
      nullptr,
      nullptr,
  };
  return descriptor;
}

void ApplyPadding(detail::ViewSpec& spec, const Padding& modifier) {
  spec.properties.padding = modifier.insets;
}

void ApplyBackground(detail::ViewSpec& spec, const Background& modifier) {
  spec.properties.background = modifier.color;
}

void ApplyShadow(detail::ViewSpec& spec, const Shadow& modifier) {
  const bool color_finite = std::isfinite(modifier.color.red) && std::isfinite(modifier.color.green) &&
                            std::isfinite(modifier.color.blue) && std::isfinite(modifier.color.alpha);
  if (!color_finite || !std::isfinite(modifier.offset.x) || !std::isfinite(modifier.offset.y) ||
      !std::isfinite(modifier.blur_radius) || modifier.blur_radius < 0.0F || !std::isfinite(modifier.spread)) {
    throw std::invalid_argument("HuxerUI shadow values must be finite with non-negative blur");
  }
  spec.properties.shadow = modifier;
}

void ApplyForeground(detail::ViewSpec& spec, const Foreground& modifier) {
  spec.properties.text_style.foreground = modifier.color;
}

void ApplyFontSize(detail::ViewSpec& spec, const FontSize& modifier) {
  if (!std::isfinite(modifier.value) || modifier.value <= 0.0F) {
    throw std::invalid_argument("HuxerUI font size must be finite and greater than zero");
  }
  spec.properties.text_style.font = spec.properties.text_style.font.WithSize(modifier.value);
}

void ValidateFrameValue(const std::optional<float>& value, const char* name) {
  if (value.has_value() && (!std::isfinite(*value) || *value < 0.0F)) {
    throw std::invalid_argument(std::string("HuxerUI frame ") + name + " must be finite and non-negative");
  }
}

void ValidateFrameConstraints(const Frame& frame) {
  ValidateFrameValue(frame.width, "width");
  ValidateFrameValue(frame.height, "height");
  ValidateFrameValue(frame.min_width, "minimum width");
  ValidateFrameValue(frame.max_width, "maximum width");
  ValidateFrameValue(frame.min_height, "minimum height");
  ValidateFrameValue(frame.max_height, "maximum height");
  if (frame.min_width.has_value() && frame.max_width.has_value() && *frame.min_width > *frame.max_width) {
    throw std::invalid_argument("HuxerUI frame minimum width must not exceed maximum width");
  }
  if (frame.min_height.has_value() && frame.max_height.has_value() && *frame.min_height > *frame.max_height) {
    throw std::invalid_argument("HuxerUI frame minimum height must not exceed maximum height");
  }
}

void ApplyFrame(detail::ViewSpec& spec, const Frame& modifier) {
  Frame frame = spec.properties.frame;
  if (modifier.width.has_value()) {
    frame.width = modifier.width;
  }
  if (modifier.height.has_value()) {
    frame.height = modifier.height;
  }
  if (modifier.min_width.has_value()) {
    frame.min_width = modifier.min_width;
  }
  if (modifier.max_width.has_value()) {
    frame.max_width = modifier.max_width;
  }
  if (modifier.min_height.has_value()) {
    frame.min_height = modifier.min_height;
  }
  if (modifier.max_height.has_value()) {
    frame.max_height = modifier.max_height;
  }
  ValidateFrameConstraints(frame);
  spec.properties.frame = std::move(frame);
}

void ApplyCornerRadius(detail::ViewSpec& spec, const CornerRadius& modifier) {
  spec.properties.corner_radius = modifier.value;
}

void ApplySpacing(detail::ViewSpec& spec, const Spacing& modifier) {
  spec.properties.spacing = modifier.value;
}

void ApplyMainAlign(detail::ViewSpec& spec, const MainAlign& modifier) {
  spec.properties.main_axis_alignment = modifier.alignment;
}

void ApplyCrossAlign(detail::ViewSpec& spec, const CrossAlign& modifier) {
  spec.properties.cross_axis_alignment = modifier.alignment;
}

void ApplyAlign(detail::ViewSpec& spec, const Align& modifier) {
  spec.properties.horizontal_alignment = modifier.horizontal;
  spec.properties.vertical_alignment = modifier.vertical;
}

void ApplyGrow(detail::ViewSpec& spec, const Grow& modifier) {
  if (!std::isfinite(modifier.factor) || modifier.factor < 0.0F) {
    throw std::invalid_argument("HuxerUI grow factor must be finite and non-negative");
  }
  spec.properties.grow = modifier.factor;
}

void ApplyEnabled(detail::ViewSpec& spec, const Enabled& modifier) {
  spec.local_enabled = modifier.value;
}

void ApplyFocusable(detail::ViewSpec& spec, const Focusable& modifier) {
  spec.focusable = modifier.value;
}

Color InterpolateColor(Color from, Color to, float progress) {
  const float value = std::clamp(progress, 0.0F, 1.0F);
  return {
      from.red + (to.red - from.red) * value,
      from.green + (to.green - from.green) * value,
      from.blue + (to.blue - from.blue) * value,
      from.alpha + (to.alpha - from.alpha) * value,
  };
}

enum class ToggleVisualKind {
  Checkbox,
  Switch,
};

struct ResolvedCheckboxStyle {
  using Value = CheckboxStyle;
};

struct ResolvedSwitchStyle {
  using Value = SwitchStyle;
};

struct ResolvedProgressCircleStyle {
  using Value = ProgressCircleStyle;
};

struct ToggleVisual {
  static const detail::ModifierDescriptor& Descriptor();

  ToggleVisualKind kind;
  bool checked;

  bool operator==(const ToggleVisual&) const = default;
};

class ToggleVisualExtension final : public NodeExtension {
public:
  ToggleVisualExtension(MountedNode& node, const ToggleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ToggleVisual& modifier) {
    kind_ = modifier.kind;
    checkbox_style_ = node.LayoutValueOr<ResolvedCheckboxStyle>(CheckboxStyle::Default());
    switch_style_ = node.LayoutValueOr<ResolvedSwitchStyle>(SwitchStyle::Default());
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

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_progress = progress_.Value();
    if (kind_ != ToggleVisualKind::Switch) {
      progress_.Set(checked_ ? 1.0F : 0.0F);
      target_pending_ = false;
      if (progress_.Value() != previous_progress) {
        InvalidatePaint();
      }
      return {};
    }
    if (target_pending_) {
      progress_.Update(checked_ ? 1.0F : 0.0F, TweenSpec{switch_style_.animation_duration});
      target_pending_ = false;
    }
    progress_.Advance(frame.timestamp, frame.delta_time);
    if (progress_.Value() != previous_progress) {
      InvalidatePaint();
    }
    return {
        .needs_frame = progress_.IsRunning(),
        .wake_after = std::nullopt,
    };
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
    if (kind_ == ToggleVisualKind::Checkbox) {
      PaintCheckbox(node, context);
    } else {
      PaintSwitch(node, context);
    }
  }

private:
  void PaintCheckbox(const MountedNode& node, PaintContext& context) const {
    const Rect frame = node.Bounds();
    if (checked_) {
      context.DrawRect(frame, checkbox_style_.checked_background, std::max(0.0F, checkbox_style_.corner_radius));
      context.DrawText(
          frame,
          "✓",
          TextStyle{Font::System(std::max(0.1F, checkbox_style_.size * 0.72F)), checkbox_style_.checkmark},
          TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
      );
      return;
    }
    context.DrawBorder(
        frame,
        checkbox_style_.unchecked_border,
        std::max(0.0F, checkbox_style_.border_width),
        std::max(0.0F, checkbox_style_.corner_radius)
    );
  }

  void PaintSwitch(const MountedNode& node, PaintContext& context) const {
    const Rect frame = node.Bounds();
    const float progress = progress_.Value();
    const Color track = InterpolateColor(switch_style_.unchecked_track, switch_style_.checked_track, progress);
    context.DrawRect(frame, track, std::max(0.0F, switch_style_.corner_radius));

    const float padding = std::max(0.0F, switch_style_.track_padding);
    const float maximum_radius = std::max(0.0F, std::min(frame.height * 0.5F - padding, frame.width * 0.5F - padding));
    const float radius = std::clamp(switch_style_.thumb_radius, 0.0F, maximum_radius);
    const float start_x = frame.x + padding + radius;
    const float travel = std::max(0.0F, frame.width - 2.0F * (padding + radius));
    context.DrawCircle(
        {
            start_x + travel * progress,
            frame.y + frame.height * 0.5F,
        },
        radius,
        switch_style_.thumb
    );
  }

  ToggleVisualKind kind_ = ToggleVisualKind::Checkbox;
  CheckboxStyle checkbox_style_;
  SwitchStyle switch_style_;
  detail::AnimatedValue<float> progress_;
  bool checked_ = false;
  bool initialized_ = false;
  bool target_pending_ = false;
};

const detail::ModifierDescriptor& ToggleVisual::Descriptor() {
  return detail::ModifierDescriptorFor<ToggleVisual, ToggleVisualExtension>();
}

struct ProgressCircleVisual {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<float> progress;

  bool operator==(const ProgressCircleVisual&) const = default;
};

class ProgressCircleVisualExtension final : public NodeExtension {
public:
  ProgressCircleVisualExtension(MountedNode& node, const ProgressCircleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ProgressCircleVisual& modifier) {
    style_ = node.LayoutValueOr<ResolvedProgressCircleStyle>(ProgressCircleStyle::Default());
    if (progress_ != modifier.progress) {
      progress_ = modifier.progress;
      animation_start_.reset();
      phase_ = 0.0F;
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_phase = phase_;
    if (progress_.has_value() || !std::isfinite(style_.animation_duration) || style_.animation_duration <= 0.0) {
      animation_start_.reset();
      phase_ = 0.0F;
      if (phase_ != previous_phase) {
        InvalidatePaint();
      }
      return {};
    }
    if (!animation_start_.has_value()) {
      animation_start_ = frame.timestamp;
    }
    const double elapsed = std::max(0.0, frame.timestamp - *animation_start_);
    phase_ = static_cast<float>(std::fmod(elapsed, style_.animation_duration) / style_.animation_duration);
    if (phase_ != previous_phase) {
      InvalidatePaint();
    }
    return {
        .needs_frame = true,
        .wake_after = std::nullopt,
    };
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
    constexpr float pi = 3.14159265358979323846F;
    constexpr float full_circle = pi * 2.0F;

    const Rect frame = node.Bounds();
    const float stroke_width = std::max(0.0F, style_.stroke_width);
    const float radius = std::max(0.0F, std::min(frame.width, frame.height) * 0.5F - stroke_width * 0.5F);
    if (radius <= 0.0F || stroke_width <= 0.0F) {
      return;
    }

    const Point center{
        frame.x + frame.width * 0.5F,
        frame.y + frame.height * 0.5F,
    };
    if (style_.track_color.alpha > 0.0F) {
      context.DrawArc(center, radius, -pi * 0.5F, full_circle, style_.track_color, stroke_width);
    }

    const float progress = progress_.value_or(std::clamp(style_.indeterminate_arc_fraction, 0.0F, 1.0F));
    if (progress <= 0.0F) {
      return;
    }
    const float start = -pi * 0.5F + (progress_.has_value() ? 0.0F : phase_ * full_circle);
    context
        .DrawArc(center, radius, start, progress * full_circle, style_.indicator_color, stroke_width, StrokeCap::Round);
  }

private:
  ProgressCircleStyle style_;
  std::optional<float> progress_;
  std::optional<double> animation_start_;
  float phase_ = 0.0F;
};

const detail::ModifierDescriptor& ProgressCircleVisual::Descriptor() {
  return detail::ModifierDescriptorFor<ProgressCircleVisual, ProgressCircleVisualExtension>();
}

template <class Style>
std::optional<Style> ResolveStyleOverride(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(Style))) {
    if (const auto* style = std::any_cast<Style>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI component style environment value has an invalid type");
  }
  return std::nullopt;
}

void ApplyThemeDefaults(detail::ViewSpec& spec) {
  const ThemeSpec theme = detail::ResolveThemeSpec(spec.environment);
  spec.properties.focus_ring = theme.interactions.focus_ring.value_or(theme.colors.primary);
  spec.properties.focus_ring_width = std::max(0.0F, theme.interactions.focus_ring_width);
  spec.properties.disabled_opacity = std::clamp(theme.interactions.disabled_opacity, 0.0F, 1.0F);
  if (spec.kind == detail::NodeKind::Text) {
    spec.properties.text_style =
        ResolveStyleOverride<TextStyle>(spec.environment).value_or(detail::DefaultTextStyle(theme, spec.text_role));
    return;
  }
  if (spec.kind == detail::NodeKind::Button) {
    const ButtonStyle style =
        ResolveStyleOverride<ButtonStyle>(spec.environment).value_or(detail::DefaultButtonStyle(theme));
    spec.properties.padding = style.padding;
    spec.properties.background = style.background;
    spec.properties.text_style = style.label_style;
    spec.properties.corner_radius = style.corner_radius;
    return;
  }
  if (spec.kind == detail::NodeKind::TextField) {
    const TextFieldStyle style =
        ResolveStyleOverride<TextFieldStyle>(spec.environment).value_or(detail::DefaultTextFieldStyle(theme));
    spec.layout_values.insert_or_assign(typeid(detail::ResolvedTextFieldStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.focus_ring_width = 0.0F;
    spec.properties.padding = style.padding;
    spec.properties.background = style.background;
    spec.properties.text_style = style.text_style;
    spec.properties.corner_radius = style.corner_radius;
    spec.properties.frame.min_height = std::max(0.0F, style.minimum_height);
    return;
  }
  if (spec.kind == detail::NodeKind::Checkbox) {
    const CheckboxStyle style =
        ResolveStyleOverride<CheckboxStyle>(spec.environment).value_or(detail::DefaultCheckboxStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedCheckboxStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.frame.width = std::max(0.0F, style.size);
    spec.properties.frame.height = std::max(0.0F, style.size);
    spec.properties.corner_radius = std::max(0.0F, style.corner_radius);
    return;
  }
  if (spec.kind == detail::NodeKind::Switch) {
    const SwitchStyle style =
        ResolveStyleOverride<SwitchStyle>(spec.environment).value_or(detail::DefaultSwitchStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedSwitchStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.frame.width = std::max(0.0F, style.width);
    spec.properties.frame.height = std::max(0.0F, style.height);
    spec.properties.corner_radius = std::max(0.0F, style.corner_radius);
    return;
  }
  if (spec.kind == detail::NodeKind::ProgressCircle) {
    const ProgressCircleStyle style =
        ResolveStyleOverride<ProgressCircleStyle>(spec.environment).value_or(detail::DefaultProgressCircleStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedProgressCircleStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.frame.width = std::max(0.0F, style.size);
    spec.properties.frame.height = std::max(0.0F, style.size);
  }
}

std::shared_ptr<detail::ViewSpec> MakeTextSpec(std::string value, TextRole role) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
  spec->text = std::move(value);
  spec->text_role = role;
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeButtonSpec(std::string label) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Button);
  spec->text = std::move(label);
  spec->focusable = true;
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeToggleSpec(detail::NodeKind kind, ToggleVisualKind visual_kind, bool checked) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  spec->focusable = true;
  spec->activation = [checked](const detail::EventBindings& bindings) {
    detail::EmitEvent<ToggleEvents::Changed>(bindings, !checked);
  };
  spec->retained_modifiers.push_back(detail::MakeModifierSpec(ToggleVisual{visual_kind, checked}));
  spec->retained_modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
  return spec;
}

float NormalizeProgress(float progress) {
  if (std::isnan(progress) || progress <= 0.0F) {
    return 0.0F;
  }
  if (progress >= 1.0F) {
    return 1.0F;
  }
  return progress;
}

std::shared_ptr<detail::ViewSpec> MakeProgressCircleSpec(std::optional<float> progress) {
  if (progress.has_value()) {
    progress = NormalizeProgress(*progress);
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::ProgressCircle);
  spec->retained_modifiers.push_back(detail::MakeModifierSpec(ProgressCircleVisual{progress}));
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

std::shared_ptr<detail::ViewSpec> MakeSpacerSpec() {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Spacer);
  spec->properties.grow = 1.0F;
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeScopeSpec(std::function<View()> factory) {
  if (!factory) {
    throw std::invalid_argument("HuxerUI scope factory must not be empty");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Scope);
  spec->scope_factory = std::move(factory);
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeContainerSpec(detail::NodeKind kind, std::vector<View> children) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  spec->children = std::move(children);
  return spec;
}

} // namespace

const detail::ModifierDescriptor& Padding::Descriptor() {
  return ApplyOnlyModifierDescriptor<Padding, ApplyPadding>();
}

const detail::ModifierDescriptor& Enabled::Descriptor() {
  return ApplyOnlyModifierDescriptor<Enabled, ApplyEnabled>();
}

const detail::ModifierDescriptor& Focusable::Descriptor() {
  return ApplyOnlyModifierDescriptor<Focusable, ApplyFocusable>();
}

const detail::ModifierDescriptor& Background::Descriptor() {
  return ApplyOnlyModifierDescriptor<Background, ApplyBackground>();
}

const detail::ModifierDescriptor& Shadow::Descriptor() {
  return ApplyOnlyModifierDescriptor<Shadow, ApplyShadow>();
}

const detail::ModifierDescriptor& Foreground::Descriptor() {
  return ApplyOnlyModifierDescriptor<Foreground, ApplyForeground>();
}

const detail::ModifierDescriptor& FontSize::Descriptor() {
  return ApplyOnlyModifierDescriptor<FontSize, ApplyFontSize>();
}

const detail::ModifierDescriptor& Frame::Descriptor() {
  return ApplyOnlyModifierDescriptor<Frame, ApplyFrame>();
}

const detail::ModifierDescriptor& CornerRadius::Descriptor() {
  return ApplyOnlyModifierDescriptor<CornerRadius, ApplyCornerRadius>();
}

const detail::ModifierDescriptor& Spacing::Descriptor() {
  return ApplyOnlyModifierDescriptor<Spacing, ApplySpacing>();
}

const detail::ModifierDescriptor& MainAlign::Descriptor() {
  return ApplyOnlyModifierDescriptor<MainAlign, ApplyMainAlign>();
}

const detail::ModifierDescriptor& CrossAlign::Descriptor() {
  return ApplyOnlyModifierDescriptor<CrossAlign, ApplyCrossAlign>();
}

const detail::ModifierDescriptor& Align::Descriptor() {
  return ApplyOnlyModifierDescriptor<Align, ApplyAlign>();
}

const detail::ModifierDescriptor& Grow::Descriptor() {
  return ApplyOnlyModifierDescriptor<Grow, ApplyGrow>();
}

namespace detail {

std::shared_ptr<ViewSpec> MakeLayoutSpec(const LayoutDescriptor& layout, std::vector<View> children) {
  auto spec = std::make_shared<ViewSpec>(NodeKind::Layout);
  spec->layout_descriptor = &layout;
  spec->children = std::move(children);
  return spec;
}

std::shared_ptr<ViewSpec> MakeVirtualLayoutSpec(const VirtualLayoutDescriptor& layout, VirtualItemSource source) {
  if (source.size > 0 && !source.factory) {
    throw std::invalid_argument("HuxerUI virtual item factory must not be empty");
  }
  auto spec = std::make_shared<ViewSpec>(NodeKind::VirtualLayout);
  spec->virtual_layout_descriptor = &layout;
  spec->virtual_items = std::move(source);
  return spec;
}

} // namespace detail

View::View(std::shared_ptr<detail::ViewSpec> spec) : spec_(std::move(spec)) {
  if (spec_) {
    spec_->environment = detail::CurrentEnvironment();
    ApplyThemeDefaults(*spec_);
  }
}

void View::SetEventBinding(std::type_index key, std::shared_ptr<detail::EventHandlerBase> handler) {
  EnsureUniqueSpec();
  spec_->event_bindings.insert_or_assign(key, std::move(handler));
}

void View::SetErasedLayoutValue(std::type_index key, detail::ErasedLayoutValue value) {
  EnsureUniqueSpec();
  spec_->layout_values.insert_or_assign(key, std::move(value));
}

void View::AddDefaultIndication() {
  AddModifier(detail::MakeModifierSpec(detail::DefaultIndication{}));
}

void View::AddModifier(detail::ModifierSpec modifier) {
  if (modifier.descriptor == nullptr || !modifier.value) {
    throw std::invalid_argument("HuxerUI modifier descriptor and value must not be empty");
  }
  if (modifier.descriptor->create_extension == nullptr && modifier.descriptor->update_extension != nullptr) {
    throw std::invalid_argument("HuxerUI modifier extension update requires extension creation");
  }
  if (modifier.descriptor->apply == nullptr && modifier.descriptor->create_extension == nullptr) {
    throw std::invalid_argument("HuxerUI modifier descriptor must apply or create a node extension");
  }
  EnsureUniqueSpec();
  if (detail::IsExplicitIndicationDescriptor(modifier.descriptor)) {
    std::erase_if(spec_->retained_modifiers, [](const detail::ModifierSpec& existing) {
      return detail::IsDefaultIndicationDescriptor(existing.descriptor);
    });
  } else if (detail::IsDefaultIndicationDescriptor(modifier.descriptor)) {
    const bool already_has_indication =
        std::ranges::any_of(spec_->retained_modifiers, [](const detail::ModifierSpec& existing) {
          return detail::IsDefaultIndicationDescriptor(existing.descriptor) ||
                 detail::IsExplicitIndicationDescriptor(existing.descriptor);
        });
    if (already_has_indication) {
      return;
    }
  }
  if (modifier.descriptor->apply != nullptr) {
    modifier.descriptor->apply(*spec_, modifier.value.get());
  }
  if (modifier.descriptor->create_extension == nullptr) {
    return;
  }
  spec_->retained_modifiers.push_back(std::move(modifier));
}

void View::SetModifier(detail::ModifierSpec modifier) {
  if (modifier.descriptor == nullptr || !modifier.value || modifier.descriptor->create_extension == nullptr) {
    throw std::invalid_argument("HuxerUI retained modifier descriptor and value must not be empty");
  }
  EnsureUniqueSpec();
  const auto found = std::ranges::find_if(spec_->retained_modifiers, [&modifier](const detail::ModifierSpec& existing) {
    return existing.descriptor == modifier.descriptor;
  });
  if (found == spec_->retained_modifiers.end()) {
    spec_->retained_modifiers.push_back(std::move(modifier));
  } else {
    *found = std::move(modifier);
  }
}

std::shared_ptr<detail::ViewSpec> MakeImageSpec(detail::ResolvedImageAsset image) {
  const bool has_value = std::visit([](const auto& asset) { return asset.HasValue(); }, image);
  if (!has_value) {
    throw std::invalid_argument("HuxerUI image view asset must not be empty");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Image);
  spec->image_properties.asset = std::move(image);
  return spec;
}

void View::SetTextStyle(TextStyle style) {
  EnsureUniqueSpec();
  spec_->properties.text_style = std::move(style);
}

void View::SetImageFit(ImageFit fit) {
  EnsureUniqueSpec();
  spec_->image_properties.fit = fit;
}

void View::SetImageAlignment(HorizontalAlignment horizontal, VerticalAlignment vertical) {
  if (horizontal == HorizontalAlignment::Stretch || vertical == VerticalAlignment::Stretch) {
    throw std::invalid_argument("HuxerUI image content alignment must not use Stretch");
  }
  EnsureUniqueSpec();
  spec_->image_properties.horizontal_alignment = horizontal;
  spec_->image_properties.vertical_alignment = vertical;
}

void View::SetImageSampling(ImageSampling sampling) {
  EnsureUniqueSpec();
  if (spec_->image_properties.IsVector()) {
    throw std::invalid_argument("HuxerUI vector images do not support raster sampling configuration");
  }
  spec_->image_properties.sampling = sampling;
}

void View::SetImageTint(std::optional<Color> tint) {
  EnsureUniqueSpec();
  if (!spec_->image_properties.IsVector()) {
    throw std::invalid_argument("HuxerUI raster images do not support Tint");
  }
  spec_->image_properties.tint = tint;
}

void View::SetKey(std::int64_t value) {
  EnsureUniqueSpec();
  spec_->key = value;
}

void View::SetKey(std::uint64_t value) {
  EnsureUniqueSpec();
  spec_->key = value;
}

void View::SetKey(std::string value) {
  EnsureUniqueSpec();
  spec_->key = std::move(value);
}

View View::Key(std::int64_t value) && {
  SetKey(value);
  return std::move(*this);
}

View View::Key(std::uint64_t value) && {
  SetKey(value);
  return std::move(*this);
}

View View::Key(std::string value) && {
  SetKey(std::move(value));
  return std::move(*this);
}

View View::Key(std::string_view value) && {
  return std::move(*this).Key(std::string(value));
}

View View::Key(const char* value) && {
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI key string must not be null");
  }
  return std::move(*this).Key(std::string(value));
}

void View::EnsureUniqueSpec() {
  if (!spec_) {
    throw std::logic_error("Cannot modify an empty HuxerUI view");
  }
  if (spec_.use_count() != 1) {
    spec_ = std::make_shared<detail::ViewSpec>(*spec_);
  }
}

Text::Text(StringResource resource, TextRole role) : Text(UseString(std::move(resource)), role) {}

Text::Text(std::string value, TextRole role) : View(MakeTextSpec(std::move(value), role)) {}

Text::Text(std::string_view value, TextRole role) : Text(std::string(value), role) {}

Text::Text(const char* value, TextRole role) : Text(value == nullptr ? std::string{} : std::string(value), role) {}

Text Text::Style(TextStyle style) && {
  SetTextStyle(std::move(style));
  return std::move(*this);
}

Button::Button(StringResource resource) : Button(UseString(std::move(resource))) {}

Button::Button(std::string label) : View(MakeButtonSpec(std::move(label))) {}

Button::Button(std::string_view label) : Button(std::string(label)) {}

Button::Button(const char* label) : Button(label == nullptr ? std::string{} : std::string(label)) {}

Image::Image(ImageResource resource) : View(MakeImageSpec(detail::UseImageResource(std::move(resource)))) {}

Image::Image(ImageAsset asset) : View(MakeImageSpec(std::move(asset))) {}

Image::Image(VectorAsset asset) : View(MakeImageSpec(std::move(asset))) {}

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

Checkbox::Checkbox(bool checked)
    : detail::TypedView<Checkbox>(MakeToggleSpec(detail::NodeKind::Checkbox, ToggleVisualKind::Checkbox, checked)) {}

Switch::Switch(bool checked)
    : detail::TypedView<Switch>(MakeToggleSpec(detail::NodeKind::Switch, ToggleVisualKind::Switch, checked)) {}

ProgressCircle::ProgressCircle() : detail::TypedView<ProgressCircle>(MakeProgressCircleSpec(std::nullopt)) {}

ProgressCircle::ProgressCircle(float progress) : detail::TypedView<ProgressCircle>(MakeProgressCircleSpec(progress)) {}

Canvas::Canvas(CanvasPainter painter) : View(MakeCanvasSpec(std::move(painter))) {}

Scope::Scope(std::function<View()> factory) : View(MakeScopeSpec(std::move(factory))) {}

Spacer::Spacer() : View(MakeSpacerSpec()) {}

ScrollView::ScrollView(View content)
    : detail::TypedView<ScrollView>(
          MakeContainerSpec(detail::NodeKind::ScrollView, std::vector<View>{std::move(content)})
      ) {}

ScrollView ScrollView::ScrollAxis(Axis axis) && {
  SetLayoutValue(typeid(detail::ScrollAxisBinding), axis);
  return std::move(*this);
}

ScrollView ScrollView::Controller(huxerui::ScrollController controller) && {
  SetLayoutValue(typeid(detail::ScrollControllerBinding), std::move(controller));
  return std::move(*this);
}

VirtualList VirtualList::ScrollAxis(Axis axis) && {
  SetLayoutValue(typeid(detail::ScrollAxisBinding), axis);
  return std::move(*this);
}

VirtualList VirtualList::ItemExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument("HuxerUI virtual item extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualListItemExtent), extent);
  return std::move(*this);
}

VirtualList VirtualList::EstimatedItemExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument("HuxerUI estimated virtual item extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualListEstimatedItemExtent), extent);
  return std::move(*this);
}

VirtualList VirtualList::CacheExtent(float extent) && {
  if (!std::isfinite(extent) || extent < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual cache extent must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualListCacheExtent), extent);
  return std::move(*this);
}

GridColumns GridColumns::Fixed(std::size_t count) {
  if (count == 0) {
    throw std::invalid_argument("HuxerUI fixed grid column count must be positive");
  }
  return GridColumns{Mode::Fixed, count, 0.0F};
}

GridColumns GridColumns::Adaptive(float minimum_width) {
  if (!std::isfinite(minimum_width) || minimum_width <= 0.0F) {
    throw std::invalid_argument("HuxerUI adaptive grid column width must be finite and positive");
  }
  return GridColumns{Mode::Adaptive, 0, minimum_width};
}

std::size_t GridColumns::Resolve(float available_width, float spacing) const noexcept {
  if (mode_ == Mode::Fixed) {
    return count_;
  }
  const float stride = minimum_width_ + std::max(0.0F, spacing);
  return std::max(
      std::size_t{1},
      static_cast<std::size_t>(std::floor((std::max(0.0F, available_width) + std::max(0.0F, spacing)) / stride))
  );
}

VirtualGrid VirtualGrid::Columns(GridColumns columns) && {
  SetLayoutValue(typeid(detail::VirtualGridColumns), columns);
  return std::move(*this);
}

VirtualGrid VirtualGrid::RowExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid row extent must be finite and positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridRowExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::EstimatedRowExtent(float extent) && {
  if (!std::isfinite(extent) || extent <= 0.0F) {
    throw std::invalid_argument(
        "HuxerUI estimated virtual grid row extent must be finite and "
        "positive"
    );
  }
  SetLayoutValue(typeid(detail::VirtualGridEstimatedRowExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::RowSpacing(float spacing) && {
  if (!std::isfinite(spacing) || spacing < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid row spacing must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridRowSpacing), spacing);
  return std::move(*this);
}

VirtualGrid VirtualGrid::ColumnSpacing(float spacing) && {
  if (!std::isfinite(spacing) || spacing < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid column spacing must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridColumnSpacing), spacing);
  return std::move(*this);
}

VirtualGrid VirtualGrid::CacheExtent(float extent) && {
  if (!std::isfinite(extent) || extent < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual grid cache extent must be finite and non-negative");
  }
  SetLayoutValue(typeid(detail::VirtualGridCacheExtent), extent);
  return std::move(*this);
}

VirtualGrid VirtualGrid::ItemSpans(std::vector<std::size_t> spans) && {
  if (std::ranges::any_of(spans, [](std::size_t span) { return span == 0; })) {
    throw std::invalid_argument("HuxerUI virtual grid item spans must be positive");
  }
  SetLayoutValue(typeid(detail::VirtualGridItemSpans), std::move(spans));
  return std::move(*this);
}

} // namespace huxerui
