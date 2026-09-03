#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <huxerui/animation.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "internal.h"
#include "paint_internal.h"
#include "resource_internal.h"
#include "indication_internal.h"
#include "text_field_internal.h"

namespace huxerui {

namespace detail {

std::optional<ResolvedImageAsset> ResolveOptionalControlIcon(const std::optional<ImageVariant>& value) {
  return value.has_value() ? std::optional<ResolvedImageAsset>{UseImageVariant(*value)} : std::nullopt;
}

struct SegmentedButtonItemAccess {
  static std::optional<ResolvedImageAsset> ResolveIcon(SegmentedButtonItem& item) {
    return ResolveOptionalControlIcon(item.icon_);
  }

  static std::string ResolveLabel(SegmentedButtonItem& item) {
    return UseString(std::move(item.label_));
  }

  static bool ShowsLabel(const SegmentedButtonItem& item) noexcept {
    return item.show_label_;
  }

  static bool HasIcon(const SegmentedButtonItem& item) noexcept {
    return item.icon_.has_value();
  }

  static bool HasBlankLiteralLabel(const SegmentedButtonItem& item) noexcept {
    return IsBlankStringVariantLiteral(item.label_);
  }

  static void ValidateIcon(const SegmentedButtonItem& item) {
    if (item.icon_.has_value()) {
      ValidateImageVariant(*item.icon_);
    }
  }
};

struct TabItemAccess {
  static std::optional<ResolvedImageAsset> ResolveIcon(TabItem& item) {
    return ResolveOptionalControlIcon(item.icon_);
  }

  static std::string ResolveLabel(TabItem& item) {
    return UseString(std::move(item.label_));
  }

  static bool ShowsLabel(const TabItem& item) noexcept {
    return item.show_label_;
  }

  static bool IsEnabled(const TabItem& item) noexcept {
    return item.enabled_;
  }

  static bool HasIcon(const TabItem& item) noexcept {
    return item.icon_.has_value();
  }

  static bool HasBlankLiteralLabel(const TabItem& item) noexcept {
    return IsBlankStringVariantLiteral(item.label_);
  }

  static void ValidateIcon(const TabItem& item) {
    if (item.icon_.has_value()) {
      ValidateImageVariant(*item.icon_);
    }
  }
};

} // namespace detail

namespace {

template <class Modifier, void (*Apply)(detail::ViewSpec&, const Modifier&)>
const detail::ModifierDescriptor& ApplyOnlyModifierDescriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        Apply(spec, *static_cast<const Modifier*>(modifier.value.get()));
      },
      nullptr,
      nullptr,
  };
  return descriptor;
}

struct TextStyleProperty {
  static const detail::ModifierDescriptor& Descriptor();

  TextStyle value;
};

void ApplyTextStyleProperty(detail::ViewSpec& spec, const TextStyleProperty& property) {
  spec.properties.text_style = property.value;
}

const detail::ModifierDescriptor& TextStyleProperty::Descriptor() {
  return ApplyOnlyModifierDescriptor<TextStyleProperty, ApplyTextStyleProperty>();
}

} // namespace

namespace detail {

void ValidateBorder(const Border& border) {
  ValidateColor(border.color, "HuxerUI border color must be finite");
  if (!std::isfinite(border.width) || border.width < 0.0F) {
    throw std::invalid_argument("HuxerUI border width must be finite and non-negative");
  }
}

} // namespace detail

namespace {

void ValidateVisualFill(const VisualFill& fill) {
  std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Value, Brush>) {
          detail::ValidateBrush(value);
        } else {
          if (!std::isfinite(value.opacity) || value.opacity < 0.0F || value.opacity > 1.0F) {
            throw std::invalid_argument("HuxerUI visual fill image opacity must be finite within [0, 1]");
          }
          if (value.tint.has_value()) {
            detail::ValidateColor(*value.tint, "HuxerUI visual fill image tint must be finite");
          }
          const bool has_source = std::visit(
              [](const auto& source) {
                using Source = std::decay_t<decltype(source)>;
                if constexpr (std::same_as<Source, ImageResource>) {
                  return true;
                } else {
                  return source.HasValue();
                }
              },
              value.source
          );
          if (!has_source) {
            throw std::invalid_argument("HuxerUI visual fill image asset must not be empty");
          }
        }
      },
      fill.Get()
  );
}

} // namespace

namespace detail {

VisualFill ResolveVisualFill(const VisualFill& fill, AppResources& resources, const Locale& locale) {
  const auto* image = std::get_if<ImageFill>(&fill.Get());
  if (image == nullptr) {
    ValidateVisualFill(fill);
    return fill;
  }
  ImageFill resolved = *image;
  std::visit(
      [&resolved](auto asset) { resolved.source = std::move(asset); },
      ResolveImage(resolved.source, resources, locale)
  );
  VisualFill fill_result{std::move(resolved)};
  ValidateVisualFill(fill_result);
  return fill_result;
}

} // namespace detail

namespace {

void ValidateFocusRing(const FocusRing& focus_ring) {
  detail::ValidateColor(focus_ring.color, "HuxerUI focus ring color must be finite");
  if (!std::isfinite(focus_ring.width) || focus_ring.width < 0.0F || !std::isfinite(focus_ring.offset) ||
      focus_ring.offset < 0.0F) {
    throw std::invalid_argument("HuxerUI focus ring width and offset must be finite and non-negative");
  }
}

void ApplyPadding(detail::ViewSpec& spec, const Padding& modifier) {
  spec.properties.padding = modifier.insets;
}

void ApplyBackground(detail::ViewSpec& spec, const Background& modifier) {
  ValidateVisualFill(modifier.fill);
  spec.properties.background = modifier.fill;
}

void ApplyBorder(detail::ViewSpec& spec, const Border& modifier) {
  detail::ValidateBorder(modifier);
  spec.properties.border = modifier;
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
  spec.properties.corner_radii = modifier.value;
}

void ApplyClipChildren(detail::ViewSpec& spec, const ClipChildren&) {
  spec.properties.clip_children = true;
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
  spec.layout_values.insert_or_assign(
      typeid(detail::GrowFactorBinding), detail::MakeErasedLayoutValue(modifier.factor)
  );
}

void ApplyEnabled(detail::ViewSpec& spec, const Enabled& modifier) {
  spec.local_enabled = modifier.value;
}

void ApplyFocusable(detail::ViewSpec& spec, const Focusable& modifier) {
  spec.focusable = modifier.value;
}

void ApplyPointerCursor(detail::ViewSpec& spec, const PointerCursor& modifier) {
  spec.properties.pointer_cursor = modifier.kind;
}

bool UsesDisabledVisualState(const MountedNode& node) {
  return static_cast<const detail::MountedNode&>(node).applies_disabled_appearance;
}

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

struct ProgressCircleStyleBinding {
  using Value = ProgressCircleStyle;
};

struct ProgressBarStyleBinding {
  using Value = ProgressBarStyle;
};

struct SliderStyleBinding {
  using Value = SliderStyle;
};

class LoopingPhase {
public:
  bool Reset() {
    previous_timestamp_.reset();
    const bool changed = value_ != 0.0F;
    value_ = 0.0F;
    return changed;
  }

  bool Advance(const FrameInfo& frame, double duration) {
    if (!previous_timestamp_.has_value()) {
      previous_timestamp_ = frame.timestamp;
      return false;
    }
    const double elapsed = std::max(0.0, frame.timestamp - *previous_timestamp_);
    previous_timestamp_ = frame.timestamp;
    if (elapsed <= 0.0) {
      return false;
    }
    const float previous = value_;
    const double increment = std::fmod(elapsed, duration) / duration;
    value_ = static_cast<float>(std::fmod(static_cast<double>(value_) + increment, 1.0));
    return value_ != previous;
  }

  [[nodiscard]] float Value() const noexcept {
    return value_;
  }

private:
  std::optional<double> previous_timestamp_;
  float value_ = 0.0F;
};

float CubicBezierCoordinate(float time, float first_control, float second_control) {
  const float inverse = 1.0F - time;
  return 3.0F * inverse * inverse * time * first_control + 3.0F * inverse * time * time * second_control +
         time * time * time;
}

float CubicBezierProgress(float progress, float x1, float y1, float x2, float y2) {
  const float target = std::clamp(progress, 0.0F, 1.0F);
  if (target <= 0.0F || target >= 1.0F) {
    return target;
  }
  float lower = 0.0F;
  float upper = 1.0F;
  for (int iteration = 0; iteration < 16; ++iteration) {
    const float parameter = (lower + upper) * 0.5F;
    if (CubicBezierCoordinate(parameter, x1, x2) < target) {
      lower = parameter;
    } else {
      upper = parameter;
    }
  }
  return CubicBezierCoordinate((lower + upper) * 0.5F, y1, y2);
}

float SegmentedProgressPosition(float phase, float delay, float duration) {
  if (phase <= delay) {
    return 0.0F;
  }
  if (phase >= delay + duration) {
    return 1.0F;
  }
  return CubicBezierProgress((phase - delay) / duration, 0.3F, 0.0F, 0.8F, 0.15F);
}

constexpr float segmented_progress_cycle = 1750.0F;

float PulsingArcProgress(float phase, float minimum, float maximum) {
  if (phase < 0.5F) {
    return minimum + (maximum - minimum) * phase * 2.0F;
  }
  const float eased = CubicBezierProgress((phase - 0.5F) * 2.0F, 0.2F, 0.0F, 0.0F, 1.0F);
  return maximum + (minimum - maximum) * eased;
}

float PulsingArcRotation(float phase) {
  constexpr float pi = 3.14159265358979323846F;
  constexpr float stage_duration = 0.25F;
  constexpr float rotation_duration = 0.05F;
  const float stage = std::floor(phase / stage_duration);
  const float stage_progress = phase - stage * stage_duration;
  const float eased_rotation =
      CubicBezierProgress(std::min(stage_progress / rotation_duration, 1.0F), 0.05F, 0.7F, 0.1F, 1.0F);
  const float global_rotation = phase * pi * 6.0F;
  const float additional_rotation = (stage + eased_rotation) * pi * 0.5F;
  return global_rotation + additional_rotation;
}

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
  ToggleVisualExtension(MountedNode& node, const ToggleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ToggleVisual& modifier) {
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

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
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

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
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

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    if (kind_ == ToggleVisualKind::Checkbox) {
      PaintCheckbox(node, context);
    } else if (kind_ == ToggleVisualKind::RadioButton) {
      PaintRadioButton(node, context);
    } else {
      PaintSwitch(node, context);
    }
  }

private:
  void PaintCheckbox(const MountedNode& node, PaintContext& context) const {
    const Rect frame = detail::ResolveToggleControlBounds(static_cast<const detail::MountedNode&>(node));
    const bool disabled = UsesDisabledVisualState(node);
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

  void PaintRadioButton(const MountedNode& node, PaintContext& context) const {
    constexpr float full_circle = 6.28318530717958647692F;
    const Rect frame = detail::ResolveToggleControlBounds(static_cast<const detail::MountedNode&>(node));
    const float progress = progress_.Value();
    const bool disabled = UsesDisabledVisualState(node);
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

  void PaintSwitch(const MountedNode& node, PaintContext& context) const {
    const Rect frame = detail::ResolveToggleControlBounds(static_cast<const detail::MountedNode&>(node));
    const float progress = progress_.Value();
    const bool disabled = UsesDisabledVisualState(node);
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
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
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
    for (MountedNode& child : node.Children()) {
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
      MountedNode& child = node.ChildAt(index);
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
  TabsBehaviorExtension(MountedNode& node, const TabsBehavior& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const TabsBehavior& modifier) {
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

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
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

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer&) override {
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

  bool OnKey(MountedNode& node, const KeyEvent& event) override {
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

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
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
    if (!metrics.show_label || node.text.empty()) {
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

struct ProgressCircleVisual {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<float> progress;

  bool operator==(const ProgressCircleVisual&) const = default;
};

void PaintProgressCircle(PaintContext& context, Rect frame, const ProgressCircleStyle& style,
                         std::optional<float> progress, float phase) {
  constexpr float pi = 3.14159265358979323846F;
  constexpr float full_circle = pi * 2.0F;

  const float stroke_width = std::max(0.0F, style.stroke_width);
  const float radius = std::max(0.0F, std::min(frame.width, frame.height) * 0.5F - stroke_width * 0.5F);
  if (radius <= 0.0F || stroke_width <= 0.0F) {
    return;
  }

  const Point center{
      frame.x + frame.width * 0.5F,
      frame.y + frame.height * 0.5F,
  };
  const float minimum_arc = std::clamp(style.minimum_indeterminate_arc_fraction, 0.0F, 1.0F);
  const float maximum_arc = std::clamp(style.maximum_indeterminate_arc_fraction, minimum_arc, 1.0F);
  const bool pulsing_arc = style.indeterminate_motion == ProgressCircleIndeterminateMotion::PulsingArc;
  const float indeterminate_progress =
      pulsing_arc ? PulsingArcProgress(phase, minimum_arc, maximum_arc)
                  : minimum_arc + (maximum_arc - minimum_arc) * (1.0F - std::cos(phase * full_circle)) * 0.5F;
  const float resolved_progress = progress.value_or(indeterminate_progress);
  const Color track_color = progress.has_value() ? style.track_color : style.indeterminate_track_color;
  const bool separated_track = style.track_gap > 0.0F;
  if (!separated_track && track_color.alpha > 0.0F) {
    context.DrawArc(center, radius, -pi * 0.5F, full_circle, track_color, StrokeStyle{.width = stroke_width});
  }

  if (resolved_progress <= 0.0F) {
    if (separated_track && track_color.alpha > 0.0F) {
      context.DrawArc(center, radius, -pi * 0.5F, full_circle, track_color,
                      StrokeStyle{.width = stroke_width, .cap = StrokeCap::Round});
    }
    return;
  }
  float start = -pi * 0.5F;
  if (!progress.has_value()) {
    start = pulsing_arc ? PulsingArcRotation(phase) : start + phase * full_circle * 2.0F;
  }
  const float sweep = std::clamp(resolved_progress, 0.0F, 1.0F) * full_circle;
  const float adjusted_gap = std::max(0.0F, style.track_gap) + stroke_width;
  const float gap_angle = std::min(sweep, adjusted_gap / radius);
  const float track_sweep = std::max(0.0F, full_circle - sweep - gap_angle * 2.0F);
  if (separated_track && track_color.alpha > 0.0F && track_sweep > 0.0F) {
    context.DrawArc(center, radius, start + sweep + gap_angle, track_sweep, track_color,
                    StrokeStyle{.width = stroke_width, .cap = StrokeCap::Round});
  }
  context.DrawArc(center, radius, start, sweep, style.indicator_color,
                  StrokeStyle{.width = stroke_width, .cap = StrokeCap::Round});
}

class ProgressCircleVisualExtension final : public NodeExtension {
public:
  ProgressCircleVisualExtension(MountedNode& node, const ProgressCircleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ProgressCircleVisual& modifier) {
    style_ = node.LayoutValueOr<ProgressCircleStyleBinding>(ProgressCircleStyle::Default());
    if (progress_ != modifier.progress) {
      progress_ = modifier.progress;
      phase_.Reset();
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    if (progress_.has_value() || !std::isfinite(style_.animation_duration) || style_.animation_duration <= 0.0) {
      if (phase_.Reset()) {
        InvalidatePaint();
      }
      return {};
    }
    if (phase_.Advance(frame, style_.animation_duration)) {
      InvalidatePaint();
    }
    return {
        .needs_frame = true,
        .wake_after = std::nullopt,
    };
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    PaintProgressCircle(context, node.Bounds(), style_, progress_, phase_.Value());
  }

private:
  ProgressCircleStyle style_;
  std::optional<float> progress_;
  LoopingPhase phase_;
};

const detail::ModifierDescriptor& ProgressCircleVisual::Descriptor() {
  return detail::ModifierDescriptorFor<ProgressCircleVisual, ProgressCircleVisualExtension>();
}

struct ProgressBarVisual {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<float> progress;

  bool operator==(const ProgressBarVisual&) const = default;
};

class ProgressBarVisualExtension final : public NodeExtension {
public:
  ProgressBarVisualExtension(MountedNode& node, const ProgressBarVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ProgressBarVisual& modifier) {
    style_ = node.LayoutValueOr<ProgressBarStyleBinding>(ProgressBarStyle::Default());
    if (progress_ != modifier.progress) {
      progress_ = modifier.progress;
      phase_.Reset();
    }
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    if (progress_.has_value() || !std::isfinite(style_.animation_duration) || style_.animation_duration <= 0.0) {
      if (phase_.Reset()) {
        InvalidatePaint();
      }
      return {};
    }
    if (phase_.Advance(frame, style_.animation_duration)) {
      InvalidatePaint();
    }
    return {
        .needs_frame = true,
        .wake_after = std::nullopt,
    };
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const Rect frame = node.Bounds();
    if (frame.width <= 0.0F || frame.height <= 0.0F) {
      return;
    }

    const float track_radius = std::clamp(style_.corner_radius, 0.0F, frame.height * 0.5F);
    const auto draw_segment = [&](float start, float end, Color color) {
      start = std::clamp(start, 0.0F, 1.0F);
      end = std::clamp(end, 0.0F, 1.0F);
      if (end <= start || color.alpha <= 0.0F) {
        return;
      }
      const float x = frame.x + frame.width * start;
      const float width = frame.width * (end - start);
      context.DrawRect(
          {
              x,
              frame.y,
              width,
              frame.height,
          },
          color,
          std::min(track_radius, width * 0.5F)
      );
    };

    if (progress_.has_value()) {
      const float progress = std::clamp(*progress_, 0.0F, 1.0F);
      const bool separated_track = style_.track_gap > 0.0F || style_.stop_indicator_size > 0.0F;
      if (separated_track) {
        const float gap = std::max(0.0F, style_.track_gap) / frame.width;
        draw_segment(progress + std::min(progress, gap), 1.0F, style_.track_color);
      } else {
        draw_segment(0.0F, 1.0F, style_.track_color);
      }
      draw_segment(0.0F, progress, style_.indicator_color);
      const float stop_size = std::clamp(style_.stop_indicator_size, 0.0F, std::min(frame.width, frame.height));
      if (stop_size > 0.0F && style_.indicator_color.alpha > 0.0F) {
        context.DrawCircle(
            {frame.x + frame.width - stop_size * 0.5F, frame.y + frame.height * 0.5F},
            stop_size * 0.5F,
            style_.indicator_color
        );
      }
      return;
    }

    if (style_.indeterminate_motion == ProgressBarIndeterminateMotion::Segmented) {
      const float phase = style_.animation_duration > 0.0 ? phase_.Value() : 0.5F;
      // One normalized cycle keeps the four coupled timelines intact when a style changes the loop duration.
      const float first_head = SegmentedProgressPosition(phase, 0.0F, 1000.0F / segmented_progress_cycle);
      const float first_tail =
          SegmentedProgressPosition(phase, 250.0F / segmented_progress_cycle, 1000.0F / segmented_progress_cycle);
      const float second_head =
          SegmentedProgressPosition(phase, 650.0F / segmented_progress_cycle, 850.0F / segmented_progress_cycle);
      const float second_tail =
          SegmentedProgressPosition(phase, 900.0F / segmented_progress_cycle, 850.0F / segmented_progress_cycle);
      const float gap = std::max(0.0F, style_.track_gap) / frame.width;

      draw_segment(first_head > 0.0F ? first_head + gap : 0.0F, 1.0F, style_.track_color);
      draw_segment(second_head > 0.0F ? second_head + gap : 0.0F, first_tail - gap, style_.track_color);
      draw_segment(0.0F, second_tail - gap, style_.track_color);
      draw_segment(first_tail, first_head, style_.indicator_color);
      draw_segment(second_tail, second_head, style_.indicator_color);
      return;
    }

    draw_segment(0.0F, 1.0F, style_.track_color);
    const float indicator_width = frame.width * std::clamp(style_.indeterminate_fraction, 0.0F, 1.0F);
    if (indicator_width <= 0.0F || style_.indicator_color.alpha <= 0.0F) {
      return;
    }
    const float indicator_x = frame.x + frame.width * phase_.Value();
    context.PushClip(frame, track_radius);
    context.DrawRect(
        {indicator_x, frame.y, indicator_width, frame.height},
        style_.indicator_color,
        std::min(track_radius, indicator_width * 0.5F)
    );
    if (indicator_x + indicator_width > frame.x + frame.width) {
      context.DrawRect(
          {indicator_x - frame.width, frame.y, indicator_width, frame.height},
          style_.indicator_color,
          std::min(track_radius, indicator_width * 0.5F)
      );
    }
    context.PopClip();
  }

private:
  ProgressBarStyle style_;
  std::optional<float> progress_;
  LoopingPhase phase_;
};

const detail::ModifierDescriptor& ProgressBarVisual::Descriptor() {
  return detail::ModifierDescriptorFor<ProgressBarVisual, ProgressBarVisualExtension>();
}

struct SliderVisual {
  static const detail::ModifierDescriptor& Descriptor();

  float value;
  float minimum;
  float maximum;
  std::optional<float> step;

  bool operator==(const SliderVisual&) const = default;
};

class SliderVisualExtension final : public NodeExtension {
public:
  SliderVisualExtension(MountedNode& node, const SliderVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const SliderVisual& modifier) {
    style_ = node.LayoutValueOr<SliderStyleBinding>(SliderStyle::Default());
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      hovered_ = false;
      pressed_ = false;
    }
    value_ = std::clamp(modifier.value, modifier.minimum, modifier.maximum);
    minimum_ = modifier.minimum;
    maximum_ = modifier.maximum;
    step_ = modifier.step;
    last_emitted_value_ = value_;
    event_bindings_ = static_cast<detail::MountedNode&>(node).event_bindings;
    UpdateThumbSize(node.IsEnabled());
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.role = SemanticRole::Slider;
    semantics.range = SemanticRange{
        minimum_,
        maximum_,
        value_,
        step_.has_value() ? std::optional<double>{*step_} : std::nullopt,
    };
    builder.SetOwner(std::move(semantics));
    builder.AddAction(0, SemanticActionKind::SetValue);
    builder.AddAction(0, SemanticActionKind::Increment);
    builder.AddAction(0, SemanticActionKind::Decrement);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0) {
      return false;
    }
    float requested = value_;
    if (action.kind == SemanticActionKind::SetValue) {
      const auto* value = std::get_if<double>(&action.value);
      if (value == nullptr || !std::isfinite(*value)) {
        return false;
      }
      requested = static_cast<float>(*value);
    } else if (action.kind == SemanticActionKind::Increment) {
      requested += step_.value_or((maximum_ - minimum_) / 100.0F);
    } else if (action.kind == SemanticActionKind::Decrement) {
      requested -= step_.value_or((maximum_ - minimum_) / 100.0F);
    } else {
      return false;
    }
    const float snapped = Snap(requested);
    if (snapped != last_emitted_value_) {
      last_emitted_value_ = snapped;
      detail::EmitEvent<SliderEvents::Changed>(event_bindings_, snapped);
    }
    return true;
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_width = thumb_width_.Value();
    const float previous_height = thumb_height_.Value();
    const MotionAdvanceResult width_result = thumb_width_.Advance(frame);
    const MotionAdvanceResult height_result = thumb_height_.Advance(frame);
    if (thumb_width_.Value() != previous_width || thumb_height_.Value() != previous_height) {
      InvalidatePaint();
    }
    return {
        .needs_frame = width_result.needs_frame || height_result.needs_frame,
        .wake_after = detail::EarliestWakeAfter(width_result.wake_after, height_result.wake_after),
    };
  }

  [[nodiscard]] bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    const bool hovered = event.type != HoverEventType::Leave;
    if (hovered_ == hovered) {
      return;
    }
    hovered_ = hovered;
    UpdateThumbSize(node.IsEnabled());
  }

  void OnFocusChanged(MountedNode& node, bool focused) override {
    static_cast<void>(node);
    if (focused_ == focused) {
      return;
    }
    focused_ = focused;
    UpdateThumbSize(node.IsEnabled());
  }

  bool OnKey(MountedNode& node, const KeyEvent& event) override {
    if (event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control || event.modifiers.meta) {
      return false;
    }
    const float increment = step_.value_or((maximum_ - minimum_) / 100.0F);
    switch (event.key) {
    case Key::ArrowLeft:
    case Key::ArrowDown:
      EmitValue(node, last_emitted_value_ - increment);
      return true;
    case Key::ArrowRight:
    case Key::ArrowUp:
      EmitValue(node, last_emitted_value_ + increment);
      return true;
    case Key::Home:
      EmitValue(node, minimum_);
      return true;
    case Key::End:
      EmitValue(node, maximum_);
      return true;
    default:
      return false;
    }
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      pressed_ = false;
      UpdateThumbSize(false);
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      pointer_id_ = event.pointer_id;
      pressed_ = true;
      UpdateThumbSize(true);
      EmitPointerValue(node, event.position.x);
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Move) {
      EmitPointerValue(node, event.position.x);
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      EmitPointerValue(node, event.position.x);
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      pressed_ = false;
      UpdateThumbSize(true);
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const Rect frame = node.Bounds();
    if (frame.width <= 0.0F || frame.height <= 0.0F) {
      return;
    }
    const Rect track = ResolveTrackBounds(node);
    const float progress = (value_ - minimum_) / (maximum_ - minimum_);
    const float thumb_x = track.x + track.width * progress;
    const float thumb_width = std::clamp(thumb_width_.Value(), 0.0F, frame.width);
    const float thumb_height = std::clamp(thumb_height_.Value(), 0.0F, frame.height);
    const float thumb_half_width = thumb_width * 0.5F;
    const float gap = style_.thumb_track_gap > 0.0F ? thumb_half_width + style_.thumb_track_gap : 0.0F;
    const float active_end = std::clamp(thumb_x - gap, track.x, track.x + track.width);
    const float inactive_start = std::clamp(thumb_x + gap, track.x, track.x + track.width);
    const bool disabled = UsesDisabledVisualState(node);
    const Color active_track = disabled ? style_.disabled_active_track : style_.active_track;
    const Color inactive_track = disabled ? style_.disabled_inactive_track : style_.inactive_track;
    const Color active_tick = disabled ? style_.disabled_active_tick : style_.active_tick;
    const Color inactive_tick = disabled ? style_.disabled_inactive_tick : style_.inactive_tick;
    const Color stop_indicator = disabled ? style_.disabled_stop_indicator : style_.stop_indicator;
    const Color thumb = disabled ? style_.disabled_thumb : style_.thumb;

    DrawTrackSegment(context, {track.x, track.y, active_end - track.x, track.height}, active_track, true, false);
    DrawTrackSegment(
        context,
        {inactive_start, track.y, track.x + track.width - inactive_start, track.height},
        inactive_track,
        false,
        true
    );
    DrawTicks(context, track, thumb_x, progress, gap, active_tick, inactive_tick);
    DrawStopIndicator(context, track, thumb_x, gap, stop_indicator);

    if (thumb_width > 0.0F && thumb_height > 0.0F && thumb.alpha > 0.0F) {
      context.DrawRect(
          {
              thumb_x - thumb_half_width,
              frame.y + (frame.height - thumb_height) * 0.5F,
              thumb_width,
              thumb_height,
          },
          thumb,
          std::min(thumb_width, thumb_height) * 0.5F
      );
    }
  }

private:
  void DrawTrackSegment(PaintContext& context, Rect segment, Color color, bool rounded_start, bool rounded_end) const {
    if (segment.width <= 0.0F || segment.height <= 0.0F || color.alpha <= 0.0F) {
      return;
    }
    const float outer_radius = segment.height * 0.5F;
    const float inside_radius = std::clamp(style_.track_inside_corner_radius, 0.0F, outer_radius);
    float start_radius = rounded_start ? outer_radius : inside_radius;
    float end_radius = rounded_end ? outer_radius : inside_radius;
    const float combined_radius = start_radius + end_radius;
    if (combined_radius > segment.width) {
      const float scale = segment.width / combined_radius;
      start_radius *= scale;
      end_radius *= scale;
    }
    const float right = segment.x + segment.width;
    const float bottom = segment.y + segment.height;
    Path path;
    path.MoveTo({segment.x + start_radius, segment.y})
        .LineTo({right - end_radius, segment.y})
        .QuadraticTo({right, segment.y}, {right, segment.y + end_radius})
        .LineTo({right, bottom - end_radius})
        .QuadraticTo({right, bottom}, {right - end_radius, bottom})
        .LineTo({segment.x + start_radius, bottom})
        .QuadraticTo({segment.x, bottom}, {segment.x, bottom - start_radius})
        .LineTo({segment.x, segment.y + start_radius})
        .QuadraticTo({segment.x, segment.y}, {segment.x + start_radius, segment.y})
        .Close();
    context.FillPath(std::move(path), color);
  }

  void DrawTicks(
      PaintContext& context,
      const Rect& track,
      float thumb_x,
      float progress,
      float gap,
      Color active_color,
      Color inactive_color
  ) const {
    const float tick_size = std::max(0.0F, style_.tick_size);
    if (!step_.has_value() || tick_size <= 0.0F || track.width <= 0.0F || track.height <= 0.0F) {
      return;
    }
    const double interval_count = std::ceil(static_cast<double>(maximum_ - minimum_) / *step_);
    if (!std::isfinite(interval_count) || interval_count <= 1.0 || interval_count > 512.0 ||
        track.width / static_cast<float>(interval_count) < tick_size * 1.5F) {
      return;
    }
    const float radius = tick_size * 0.5F;
    const float center_y = track.y + track.height * 0.5F;
    for (int interval = 1; interval < static_cast<int>(interval_count); ++interval) {
      const float tick_value = std::min(maximum_, minimum_ + static_cast<float>(interval) * *step_);
      const float tick_progress = (tick_value - minimum_) / (maximum_ - minimum_);
      const float tick_x = track.x + track.width * tick_progress;
      if (std::abs(tick_x - thumb_x) <= gap + radius) {
        continue;
      }
      const Color color = tick_progress < progress ? active_color : inactive_color;
      if (color.alpha > 0.0F) {
        context.DrawCircle({tick_x, center_y}, radius, color);
      }
    }
  }

  void DrawStopIndicator(PaintContext& context, const Rect& track, float thumb_x, float gap, Color color) const {
    const float size = std::max(0.0F, style_.stop_indicator_size);
    if (size <= 0.0F || track.width <= 0.0F || track.height <= 0.0F || color.alpha <= 0.0F) {
      return;
    }
    const float radius = size * 0.5F;
    const float stop_x = track.x + track.width - track.height * 0.5F;
    if (std::abs(stop_x - thumb_x) <= gap + radius) {
      return;
    }
    context.DrawCircle({stop_x, track.y + track.height * 0.5F}, radius, color);
  }

  [[nodiscard]] float Snap(float value) const {
    const float clamped = std::clamp(value, minimum_, maximum_);
    if (!step_.has_value() || clamped == minimum_ || clamped == maximum_) {
      return clamped;
    }
    const float steps = std::round((clamped - minimum_) / *step_);
    return std::clamp(minimum_ + steps * *step_, minimum_, maximum_);
  }

  void EmitPointerValue(MountedNode& node, float pointer_x) {
    const Rect track = ResolveTrackBounds(node);
    const float progress = track.width > 0.0F ? std::clamp((pointer_x - track.x) / track.width, 0.0F, 1.0F) : 0.0F;
    EmitValue(node, minimum_ + (maximum_ - minimum_) * progress);
  }

  [[nodiscard]] Rect ResolveTrackBounds(const MountedNode& node) const {
    const Rect frame = node.Bounds();
    const float maximum_thumb_width =
        std::max({style_.thumb_width, style_.hovered_thumb_width, style_.pressed_thumb_width, 0.0F});
    const float inset = std::min(frame.width * 0.5F, maximum_thumb_width * 0.5F);
    const float height = std::clamp(style_.track_height, 0.0F, frame.height);
    return {
        frame.x + inset,
        frame.y + (frame.height - height) * 0.5F,
        std::max(0.0F, frame.width - inset * 2.0F),
        height,
    };
  }

  void EmitValue(MountedNode& node, float value) {
    const float snapped = Snap(value);
    if (snapped == last_emitted_value_) {
      return;
    }
    last_emitted_value_ = snapped;
    detail::EmitEvent<SliderEvents::Changed>(static_cast<detail::MountedNode&>(node).event_bindings, snapped);
  }

  void UpdateThumbSize(bool enabled) {
    float target_width = style_.thumb_width;
    float target_height = style_.thumb_height;
    if (enabled && (pressed_ || focused_)) {
      target_width = style_.pressed_thumb_width;
      target_height = style_.pressed_thumb_height;
    } else if (enabled && hovered_) {
      target_width = style_.hovered_thumb_width;
      target_height = style_.hovered_thumb_height;
    }
    target_width = std::max(0.0F, target_width);
    target_height = std::max(0.0F, target_height);
    if (!thumb_size_initialized_) {
      thumb_width_.Set(target_width);
      thumb_height_.Set(target_height);
      thumb_size_initialized_ = true;
      return;
    }
    const TweenSpec animation{style_.animation_duration};
    thumb_width_.AnimateTo(target_width, animation);
    thumb_height_.AnimateTo(target_height, animation);
  }

  SliderStyle style_;
  detail::EventBindings event_bindings_;
  MotionController thumb_width_;
  MotionController thumb_height_;
  std::optional<std::int64_t> pointer_id_;
  std::optional<float> step_;
  float value_ = 0.0F;
  float minimum_ = 0.0F;
  float maximum_ = 1.0F;
  float last_emitted_value_ = 0.0F;
  bool hovered_ = false;
  bool pressed_ = false;
  bool focused_ = false;
  bool thumb_size_initialized_ = false;
};

const detail::ModifierDescriptor& SliderVisual::Descriptor() {
  return detail::ModifierDescriptorFor<SliderVisual, SliderVisualExtension>();
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
  spec.layout_values.insert_or_assign(
      typeid(detail::ToggleLayoutMetrics),
      detail::MakeErasedLayoutValue(metrics)
  );
  if (detail::IsEmptyStringVariantLiteral(spec.text)) {
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
        .align = TextAlign::Leading,
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

void ApplyInteractionDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  ValidateFocusRing(theme.interactions.focus_ring);
  spec.properties.focus_ring = theme.interactions.focus_ring;
  spec.properties.disabled_opacity = std::clamp(theme.interactions.disabled_opacity, 0.0F, 1.0F);
}

void ResolveComponentLabel(detail::ViewSpec& spec) {
  if (!spec.component_semantics.label.has_value()) {
    spec.component_semantics.label = detail::StringLiteral(spec.text);
  }
}

void ApplyTextDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  spec.properties.text_style =
      ResolveStyleOverride<TextStyle>(environment).value_or(detail::DefaultTextStyle(theme, spec.text_role));
  ResolveComponentLabel(spec);
}

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

void ResolveProgressStateDescription(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  if (spec.component_semantics.busy.value_or(false) && !spec.component_semantics.state_description.has_value()) {
    std::shared_ptr<detail::AppResources> resources = detail::RequireAppResources(environment);
    const Locale locale = detail::ResolveResourceLocale(environment, *resources);
    spec.component_semantics.state_description =
        detail::ResolveString(StringVariant(strings::progress_in_progress), *resources, locale);
  }
}

void ApplyProgressCircleDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const ProgressCircleStyle style =
      ResolveStyleOverride<ProgressCircleStyle>(environment).value_or(detail::DefaultProgressCircleStyle(theme));
  spec.layout_values.insert_or_assign(typeid(ProgressCircleStyleBinding), detail::MakeErasedLayoutValue(style));
  spec.properties.frame.width = std::max(0.0F, style.size);
  spec.properties.frame.height = std::max(0.0F, style.size);
  ResolveProgressStateDescription(spec, environment);
}

void ApplyProgressBarDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const ProgressBarStyle style =
      ResolveStyleOverride<ProgressBarStyle>(environment).value_or(detail::DefaultProgressBarStyle(theme));
  spec.layout_values.insert_or_assign(typeid(ProgressBarStyleBinding), detail::MakeErasedLayoutValue(style));
  spec.properties.frame.width = std::max(0.0F, style.width);
  spec.properties.frame.height = std::max(0.0F, style.height);
  ResolveProgressStateDescription(spec, environment);
}

void ApplySliderDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const SliderStyle style = ResolveStyleOverride<SliderStyle>(environment).value_or(detail::DefaultSliderStyle(theme));
  spec.layout_values.insert_or_assign(typeid(SliderStyleBinding), detail::MakeErasedLayoutValue(style));
  spec.properties.frame.width = std::max(0.0F, style.width);
  spec.properties.frame.height = std::max(0.0F, style.height);
  spec.properties.corner_radii = std::max(0.0F, style.height * 0.5F);
  if (style.focus_ring.has_value()) {
    ValidateFocusRing(*style.focus_ring);
    spec.properties.focus_ring = *style.focus_ring;
  }
  spec.properties.disabled_opacity = 1.0F;
}

std::shared_ptr<detail::ViewSpec> MakeTextSpec(StringVariant value, TextRole role) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
  spec->defaults = ApplyTextDefaults;
  spec->text = std::move(value);
  spec->text_role = role;
  spec->component_semantics.role = SemanticRole::Text;
  return spec;
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

std::shared_ptr<detail::ViewSpec>
MakeSegmentedButtonSpec(std::vector<SegmentedButtonItem> items, std::size_t selected_index) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI SegmentedButton requires at least one item");
  }
  if (selected_index >= items.size()) {
    throw std::invalid_argument("HuxerUI SegmentedButton selected index is out of range");
  }

  for (const SegmentedButtonItem& item : items) {
    if (detail::SegmentedButtonItemAccess::HasBlankLiteralLabel(item)) {
      throw std::invalid_argument("HuxerUI SegmentedButton item requires a non-empty semantic label");
    }
    if (!detail::SegmentedButtonItemAccess::ShowsLabel(item) &&
        !detail::SegmentedButtonItemAccess::HasIcon(item)) {
      throw std::invalid_argument("HuxerUI icon-only SegmentedButton item requires an icon and semantic label");
    }
    detail::SegmentedButtonItemAccess::ValidateIcon(item);
  }

  return detail::MakeScopeSpec([items = std::move(items), selected_index]() -> View {
    std::vector<ResolvedSegmentedButtonItem> resolved_items;
    resolved_items.reserve(items.size());
    for (SegmentedButtonItem item : items) {
      ResolvedSegmentedButtonItem resolved{
          detail::SegmentedButtonItemAccess::ResolveLabel(item),
          detail::SegmentedButtonItemAccess::ResolveIcon(item),
          detail::SegmentedButtonItemAccess::ShowsLabel(item),
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
  spec->defaults = ApplyProgressCircleDefaults;
  spec->component_semantics.role = SemanticRole::ProgressIndicator;
  spec->component_semantics.busy = !progress.has_value();
  if (progress.has_value()) {
    spec->component_semantics.range = SemanticRange{0.0, 1.0, *progress, std::nullopt};
  }
  spec->modifiers.push_back(detail::MakeModifierSpec(ProgressCircleVisual{progress}));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeProgressBarSpec(std::optional<float> progress) {
  if (progress.has_value()) {
    progress = NormalizeProgress(*progress);
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::ProgressBar);
  spec->defaults = ApplyProgressBarDefaults;
  spec->component_semantics.role = SemanticRole::ProgressIndicator;
  spec->component_semantics.busy = !progress.has_value();
  if (progress.has_value()) {
    spec->component_semantics.range = SemanticRange{0.0, 1.0, *progress, std::nullopt};
  }
  spec->modifiers.push_back(detail::MakeModifierSpec(ProgressBarVisual{progress}));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeSliderSpec(float value) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Slider);
  spec->defaults = ApplySliderDefaults;
  spec->focusable = true;
  spec->component_semantics.role = SemanticRole::Slider;
  spec->modifiers.push_back(detail::MakeModifierSpec(SliderVisual{value, 0.0F, 1.0F, std::nullopt}));
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
  spec->layout_values.emplace(typeid(detail::GrowFactorBinding), detail::MakeErasedLayoutValue(1.0F));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeTabsSpec(std::vector<TabItem> items, std::size_t selected_index) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI Tabs requires at least one item");
  }
  if (selected_index >= items.size()) {
    throw std::invalid_argument("HuxerUI Tabs selected index is out of range");
  }

  for (const TabItem& item : items) {
    if (detail::TabItemAccess::HasBlankLiteralLabel(item)) {
      throw std::invalid_argument("HuxerUI Tabs item requires a non-empty semantic label");
    }
    if (!detail::TabItemAccess::ShowsLabel(item) && !detail::TabItemAccess::HasIcon(item)) {
      throw std::invalid_argument("HuxerUI icon-only Tabs item requires an icon and semantic label");
    }
    detail::TabItemAccess::ValidateIcon(item);
  }

  return detail::MakeScopeSpec([items = std::move(items), selected_index]() -> View {
    std::vector<ResolvedTabItem> resolved_items;
    resolved_items.reserve(items.size());
    for (TabItem item : items) {
      ResolvedTabItem resolved{
          detail::TabItemAccess::ResolveLabel(item),
          detail::TabItemAccess::ResolveIcon(item),
          detail::TabItemAccess::ShowsLabel(item),
          detail::TabItemAccess::IsEnabled(item),
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

std::shared_ptr<detail::ViewSpec> MakeContainerSpec(detail::NodeKind kind, std::vector<View> children) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  spec->children = std::move(children);
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeScrollViewSpec(View content) {
  auto spec = MakeContainerSpec(detail::NodeKind::ScrollView, std::vector<View>{std::move(content)});
  spec->component_semantics.role = SemanticRole::ScrollView;
  return spec;
}

struct PagerPageState {
  static const detail::ModifierDescriptor& Descriptor();

  std::size_t index = 0;
  bool selected = false;
};

const detail::ModifierDescriptor& PagerPageState::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& value = *static_cast<const PagerPageState*>(modifier.value.get());
        if (spec.local_enabled && !value.selected) {
          // Transition peers remain visually live while the authoritative page alone owns interaction.
          spec.properties.disabled_opacity = 1.0F;
        }
        spec.local_enabled = spec.local_enabled && value.selected;
        spec.component_semantics.selected = value.selected;
        spec.component_semantics.collection_item = SemanticCollectionItem{.index = value.index};
        spec.component_semantics.hidden = !value.selected;
      },
      nullptr,
      nullptr,
  };
  return descriptor;
}

struct RefreshBoxBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  bool refreshing = false;
  RefreshBoxStyle style;

  bool operator==(const RefreshBoxBehavior&) const = default;
};

void ValidateRefreshBoxStyle(const RefreshBoxStyle& style) {
  if (!std::isfinite(style.container_size) || style.container_size <= 0.0F ||
      !std::isfinite(style.pull_resistance) || style.pull_resistance <= 0.0F || style.pull_resistance > 1.0F ||
      !std::isfinite(style.maximum_pull_distance) || style.maximum_pull_distance <= 0.0F ||
      !std::isfinite(style.trigger_distance) || style.trigger_distance <= 0.0F ||
      style.trigger_distance > style.maximum_pull_distance ||
      !std::isfinite(style.refresh_distance) || style.refresh_distance <= 0.0F ||
      style.refresh_distance > style.trigger_distance || !std::isfinite(style.settle_motion.duration) ||
      style.settle_motion.duration < 0.0) {
    throw std::invalid_argument("HuxerUI RefreshBox style geometry and motion must be finite and valid");
  }
  if (const auto* easing = std::get_if<Easing>(&style.settle_motion.easing);
      easing && *easing != Easing::Linear && *easing != Easing::EaseIn && *easing != Easing::EaseOut &&
      *easing != Easing::EaseInOut) {
    throw std::invalid_argument("HuxerUI RefreshBox settle easing is invalid");
  }
}

class RefreshBoxBehaviorExtension final : public NodeExtension {
public:
  RefreshBoxBehaviorExtension(MountedNode& node, const RefreshBoxBehavior& behavior) {
    Update(node, behavior);
  }

  void Update(MountedNode& node, const RefreshBoxBehavior& behavior) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool was_initialized = initialized_;
    const bool previous_refreshing = behavior_.refreshing;
    const bool style_changed = was_initialized && behavior_.style != behavior.style;
    behavior_ = behavior;
    event_bindings_ = mounted.event_bindings;
    ConfigureScrollState(mounted);

    if (!was_initialized) {
      initialized_ = true;
      mode_ = behavior_.refreshing ? Mode::Refreshing : Mode::Idle;
      displacement_.Set(behavior_.refreshing ? behavior_.style.refresh_distance : 0.0F);
      return;
    }

    if (behavior_.refreshing != previous_refreshing) {
      const float current_displacement =
          drag_active_ ? LeadingDisplacement(mounted) : std::max(0.0F, displacement_.Value());
      drag_active_ = false;
      mounted.scroll_state->overscroll_offset = 0.0F;
      displacement_.Set(current_displacement);
      mode_ = behavior_.refreshing ? Mode::Refreshing : Mode::Settling;
      displacement_.AnimateTo(
          behavior_.refreshing ? behavior_.style.refresh_distance : 0.0F,
          behavior_.style.settle_motion
      );
      UpdateAllowedSources(mounted);
      InvalidatePaint();
      InvalidateSemantics();
      return;
    }

    if (style_changed) {
      if (mode_ == Mode::Refreshing) {
        displacement_.AnimateTo(behavior_.style.refresh_distance, behavior_.style.settle_motion);
      }
      InvalidatePaint();
    }
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (mode_ == Mode::AwaitingCommit && !behavior_.refreshing) {
      BeginSettlement(mounted);
    }

    MotionAdvanceResult motion_result;
    if (!drag_active_) {
      motion_result = displacement_.Advance(frame);
      const float displacement = std::max(0.0F, displacement_.Value());
      mounted.presentation.children_transform = detail::ComposeTransform(
          detail::TranslationTransform({0.0F, displacement}),
          mounted.presentation.children_transform
      );
      if (motion_result.changed) {
        InvalidatePaint();
      }
      if (mode_ == Mode::Settling && !displacement_.IsRunning()) {
        mode_ = Mode::Idle;
        UpdateAllowedSources(mounted);
        InvalidateSemantics();
      }
    }

    bool indicator_changed = false;
    const bool animate_indicator = mode_ == Mode::Refreshing && !frame.reduced_motion &&
                                   behavior_.style.indicator.animation_duration > 0.0 &&
                                   std::isfinite(behavior_.style.indicator.animation_duration);
    if (animate_indicator) {
      indicator_changed = indicator_phase_.Advance(frame, behavior_.style.indicator.animation_duration);
    } else {
      indicator_changed = indicator_phase_.Reset();
    }
    if (indicator_changed) {
      InvalidatePaint();
    }
    return {
        motion_result.needs_frame || animate_indicator,
        motion_result.wake_after,
    };
  }

  void OnScrollActivity(MountedNode& node, const ScrollActivity& activity) override {
    if (activity.source != ScrollSource::Drag || activity.axis != Axis::Vertical) {
      return;
    }
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (activity.phase == ScrollPhase::Begin) {
      drag_active_ = true;
      displacement_.Set(0.0F);
      InvalidateSemantics();
      return;
    }
    if (activity.phase == ScrollPhase::Update && drag_active_) {
      drag_displacement_ = LeadingDisplacement(mounted);
      InvalidatePaint();
      return;
    }
    if (activity.phase == ScrollPhase::End && drag_active_) {
      const float released_displacement = LeadingDisplacement(mounted);
      TransferDragDisplacement(mounted, released_displacement);
      if (released_displacement >= behavior_.style.trigger_distance &&
          detail::HasEventBinding<RefreshEvents::Requested>(event_bindings_)) {
        mode_ = Mode::AwaitingCommit;
        UpdateAllowedSources(mounted);
        InvalidateSemantics();
        detail::EmitEvent<RefreshEvents::Requested>(event_bindings_);
      } else {
        BeginSettlement(mounted);
      }
      return;
    }
    if (activity.phase == ScrollPhase::Cancel && drag_active_) {
      TransferDragDisplacement(mounted, LeadingDisplacement(mounted));
      BeginSettlement(mounted);
    }
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    builder.SetOwner(Semantics{.busy = behavior_.refreshing});
    if (mode_ == Mode::Idle && !behavior_.refreshing &&
        detail::HasEventBinding<RefreshEvents::Requested>(event_bindings_)) {
      builder.AddCustomAction(0, refresh_action_id, strings::refresh);
    }
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    const auto* action_id = std::get_if<std::uint64_t>(&action.value);
    if (local_id != 0 || action.kind != SemanticActionKind::Custom || action_id == nullptr ||
        *action_id != refresh_action_id || mode_ != Mode::Idle || behavior_.refreshing ||
        !detail::HasEventBinding<RefreshEvents::Requested>(event_bindings_)) {
      return false;
    }
    detail::EmitEvent<RefreshEvents::Requested>(event_bindings_);
    return true;
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const float displacement = drag_active_ ? drag_displacement_ : displacement_.Value();
    if (displacement <= 0.0F) {
      return;
    }
    const float container_size = behavior_.style.container_size;
    const float reveal = std::clamp(displacement / container_size, 0.0F, 1.0F);
    const Point center{
        node.Bounds().x + node.Bounds().width * 0.5F,
        node.Bounds().y + displacement - container_size * 0.5F,
    };
    Color container = behavior_.style.container_color;
    container.alpha *= reveal;
    context.PushClip(node.Bounds());
    context.DrawCircle(center, container_size * 0.5F, container);

    ProgressCircleStyle indicator = behavior_.style.indicator;
    indicator.size = std::min(std::max(0.0F, indicator.size), container_size);
    indicator.indicator_color.alpha *= reveal;
    indicator.track_color.alpha *= reveal;
    indicator.indeterminate_track_color.alpha *= reveal;
    const Rect indicator_frame{
        center.x - indicator.size * 0.5F,
        center.y - indicator.size * 0.5F,
        indicator.size,
        indicator.size,
    };
    const std::optional<float> progress = mode_ == Mode::Refreshing
                                              ? std::nullopt
                                              : std::optional{std::clamp(
                                                    displacement / behavior_.style.trigger_distance,
                                                    0.0F,
                                                    1.0F
                                                )};
    PaintProgressCircle(context, indicator_frame, indicator, progress, indicator_phase_.Value());
    context.PopClip();
  }

private:
  enum class Mode {
    Idle,
    AwaitingCommit,
    Refreshing,
    Settling,
  };

  static constexpr std::uint64_t refresh_action_id = 1;

  static constexpr std::uint32_t DragSourceMask() {
    return 1U << static_cast<std::uint32_t>(ScrollSource::Drag);
  }

  void ConfigureScrollState(detail::MountedNode& node) {
    if (!node.scroll_state) {
      throw std::logic_error("HuxerUI mounted RefreshBox has no scroll state");
    }
    node.scroll_state->axis = Axis::Vertical;
    node.scroll_state->touch_drag_only = true;
    node.scroll_state->allows_automatic_reveal = false;
    node.scroll_state->allows_leading_overscroll = true;
    node.scroll_state->allows_trailing_overscroll = false;
    UpdateAllowedSources(node);
  }

  void UpdateAllowedSources(detail::MountedNode& node) const {
    node.scroll_state->allowed_sources = (mode_ == Mode::Idle || drag_active_) ? DragSourceMask() : 0U;
  }

  static float LeadingDisplacement(const detail::MountedNode& node) {
    return std::max(0.0F, -node.scroll_state->overscroll_offset);
  }

  void TransferDragDisplacement(detail::MountedNode& node, float displacement) {
    node.scroll_state->overscroll_offset = 0.0F;
    drag_active_ = false;
    drag_displacement_ = 0.0F;
    displacement_.Set(displacement);
    InvalidatePaint();
  }

  void BeginSettlement(detail::MountedNode& node) {
    mode_ = Mode::Settling;
    displacement_.AnimateTo(0.0F, behavior_.style.settle_motion);
    UpdateAllowedSources(node);
    InvalidateSemantics();
    if (node.runtime) {
      detail::RuntimeAccess::RequestFrame(*node.runtime);
    }
  }

  RefreshBoxBehavior behavior_;
  detail::EventBindings event_bindings_;
  MotionController displacement_;
  LoopingPhase indicator_phase_;
  float drag_displacement_ = 0.0F;
  Mode mode_ = Mode::Idle;
  bool initialized_ = false;
  bool drag_active_ = false;
};

struct RefreshBoxLayout {
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    if (node.ChildCount() != 1) {
      throw std::logic_error("HuxerUI RefreshBox requires exactly one mounted child");
    }
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const Size child_size = context.Measure(node.ChildAt(0), constraints);
    const Size measured = constraints.Constrain(child_size);
    mounted.scroll_state->content_width = measured.width;
    mounted.scroll_state->content_height = measured.height;
    return LayoutResult{}.Place(node.ChildAt(0), {}).SetSize(measured);
  }
};

const detail::ModifierDescriptor& RefreshBoxBehavior::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>& environment,
         detail::AppResources&) {
        RefreshBoxBehavior behavior = *static_cast<const RefreshBoxBehavior*>(modifier.value.get());
        const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
        behavior.style =
            ResolveStyleOverride<RefreshBoxStyle>(environment).value_or(detail::DefaultRefreshBoxStyle(theme));
        ValidateRefreshBoxStyle(behavior.style);
        spec.layout_values.insert_or_assign(
            typeid(ScrollPhysics),
            detail::MakeErasedLayoutValue(ScrollPhysics{
                .fling_enabled = false,
                .overscroll_resistance = behavior.style.pull_resistance,
                .maximum_overscroll = behavior.style.maximum_pull_distance,
            })
        );
        modifier.value = std::make_shared<RefreshBoxBehavior>(std::move(behavior));
      },
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<RefreshBoxBehaviorExtension>(
            node,
            *static_cast<const RefreshBoxBehavior*>(value)
        );
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<RefreshBoxBehaviorExtension&>(extension).Update(
            node,
            *static_cast<const RefreshBoxBehavior*>(value)
        );
      },
      false,
      detail::ErasedEqualsFor<RefreshBoxBehavior>(),
      nullptr,
  };
  return descriptor;
}

std::shared_ptr<detail::ViewSpec> MakeRefreshBoxSpec(View content, bool refreshing) {
  if (!content) {
    throw std::invalid_argument("HuxerUI RefreshBox content must not be an empty View");
  }
  auto spec = MakeContainerSpec(detail::NodeKind::ScrollView, std::vector<View>{std::move(content)});
  spec->layout_descriptor = &detail::LayoutDescriptorFor<RefreshBoxLayout>();
  spec->component_semantics.role = SemanticRole::ScrollView;
  spec->layout_values.insert_or_assign(
      typeid(detail::ScrollAxisBinding),
      detail::MakeErasedLayoutValue(Axis::Vertical)
  );
  spec->modifiers.push_back(detail::MakeModifierSpec(RefreshBoxBehavior{refreshing}));
  return spec;
}

struct PagerBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  static bool LayoutEquals(const PagerBehavior& left, const PagerBehavior& right) {
    return left.selected_index == right.selected_index && left.page_count == right.page_count &&
           left.axis == right.axis && left.reverse == right.reverse;
  }

  std::size_t selected_index = 0;
  std::size_t page_count = 0;
  Axis axis = Axis::Horizontal;
  bool reverse = false;
  bool drag_enabled = true;
  TweenSpec animation{0.2, Easing::EaseOut};

  bool operator==(const PagerBehavior&) const = default;
};

class PagerBehaviorExtension final : public NodeExtension {
public:
  PagerBehaviorExtension(MountedNode& node, const PagerBehavior& behavior) {
    Update(node, behavior);
  }

  void Update(MountedNode& node, const PagerBehavior& behavior) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool geometry_changed = initialized_ &&
                                  (behavior.page_count != behavior_.page_count || behavior.axis != behavior_.axis ||
                                   behavior.reverse != behavior_.reverse);
    const std::size_t previous_selected = behavior_.selected_index;
    behavior_ = behavior;

    if (!initialized_ || geometry_changed) {
      initialized_ = true;
      displayed_index_ = behavior_.selected_index;
      mode_ = Mode::Stable;
      needs_rebase_ = true;
      ConfigureScrollState(mounted);
      InvalidateLayout(mounted);
      return;
    }
    ConfigureScrollState(mounted);
    if (!behavior_.drag_enabled && (mode_ == Mode::Dragging || mode_ == Mode::AwaitingCommit)) {
      BeginAnimation(mounted, displayed_index_);
      return;
    }
    if (behavior_.selected_index != previous_selected) {
      BeginAnimation(mounted, behavior_.selected_index);
    }
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (mode_ == Mode::AwaitingCommit) {
      if (!proposal_emitted_) {
        const std::size_t proposal = ResolveReleaseProposal(mounted);
        proposal_emitted_ = true;
        UpdateAllowedSources(mounted);
        if (proposal != displayed_index_) {
          detail::EmitEvent<PagerEvents::Changed>(mounted.event_bindings, proposal);
        }
        return {.needs_frame = true};
      }
      BeginAnimation(mounted, displayed_index_);
      return {.needs_frame = true};
    }
    if (mode_ != Mode::Animating || !layout_ready_) {
      return {.needs_frame = mode_ == Mode::Animating};
    }

    const MotionAdvanceResult result = progress_.Advance(frame);
    const float next = animation_start_offset_ +
                       (animation_target_offset_ - animation_start_offset_) * progress_.Value();
    SetOffset(mounted, next);
    if (!result.needs_frame && !result.wake_after.has_value()) {
      displayed_index_ = behavior_.selected_index;
      mode_ = Mode::Stable;
      needs_rebase_ = true;
      layout_ready_ = false;
      UpdateAllowedSources(mounted);
      InvalidateLayout(mounted);
      return {.needs_frame = true};
    }
    return {result.needs_frame, result.wake_after};
  }

  float OnPreFling(MountedNode& node, Axis axis, float available_velocity) override {
    static_cast<void>(node);
    if (axis != behavior_.axis || (mode_ != Mode::Dragging && mode_ != Mode::AwaitingCommit)) {
      return 0.0F;
    }
    release_velocity_ = available_velocity;
    return available_velocity;
  }

  void OnScrollActivity(MountedNode& node, const ScrollActivity& activity) override {
    if (activity.source != ScrollSource::Drag || activity.axis != behavior_.axis) {
      return;
    }
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (activity.phase == ScrollPhase::Begin) {
      mode_ = Mode::Dragging;
      drag_target_.reset();
      release_velocity_ = 0.0F;
      InvalidateLayout(mounted);
      return;
    }
    if (activity.phase == ScrollPhase::Update && mode_ == Mode::Dragging) {
      const std::optional<std::size_t> target = ResolveDragTarget(mounted);
      if (target != drag_target_) {
        drag_target_ = target;
        InvalidateLayout(mounted);
      }
      return;
    }
    if (activity.phase == ScrollPhase::End && mode_ == Mode::Dragging) {
      mode_ = Mode::AwaitingCommit;
      proposal_emitted_ = false;
      detail::RuntimeAccess::RequestFrame(*mounted.runtime);
      return;
    }
    if (activity.phase == ScrollPhase::Cancel && mode_ == Mode::Dragging) {
      BeginAnimation(mounted, displayed_index_);
    }
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    if (behavior_.page_count <= 1) {
      return;
    }
    builder.SetOwner({});
    builder.AddAction(0, SemanticActionKind::Scroll);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0 || action.kind != SemanticActionKind::Scroll || mode_ != Mode::Stable) {
      return false;
    }
    const auto* delta = std::get_if<Point>(&action.value);
    if (!delta) {
      return false;
    }
    const float axis_delta = behavior_.axis == Axis::Vertical ? delta->y : delta->x;
    if (axis_delta == 0.0F) {
      return false;
    }
    const int physical_direction = axis_delta > 0.0F ? 1 : -1;
    const int logical_direction = behavior_.reverse ? -physical_direction : physical_direction;
    const std::optional<std::size_t> target = Adjacent(displayed_index_, logical_direction);
    if (!target.has_value()) {
      return false;
    }
    detail::EmitEvent<PagerEvents::Changed>(event_bindings_, *target);
    return true;
  }

  struct LayoutPlan {
    std::vector<std::size_t> measured_indices;
    std::vector<std::pair<std::size_t, std::size_t>> slots;
    std::size_t slot_count = 1;
    std::size_t anchor_slot = 0;
    std::size_t target_slot = 0;
  };

  LayoutPlan Plan() const {
    std::vector<std::pair<std::size_t, int>> relative;
    const auto add = [&](std::size_t index, int logical_direction) {
      const int physical_direction = behavior_.reverse ? -logical_direction : logical_direction;
      relative.emplace_back(index, physical_direction);
    };

    if (mode_ == Mode::Animating && animation_target_index_ != displayed_index_) {
      add(displayed_index_, 0);
      add(animation_target_index_, animation_target_index_ > displayed_index_ ? 1 : -1);
    } else {
      add(displayed_index_, 0);
      if (displayed_index_ > 0) {
        add(displayed_index_ - 1, -1);
      }
      if (displayed_index_ + 1 < behavior_.page_count) {
        add(displayed_index_ + 1, 1);
      }
    }
    std::ranges::sort(relative, {}, &std::pair<std::size_t, int>::second);

    LayoutPlan plan;
    plan.slot_count = relative.size();
    for (std::size_t slot = 0; slot < relative.size(); ++slot) {
      plan.slots.emplace_back(relative[slot].first, slot);
      if (relative[slot].first == displayed_index_) {
        plan.anchor_slot = slot;
      }
      if (mode_ == Mode::Animating && relative[slot].first == animation_target_index_) {
        plan.target_slot = slot;
      }
    }
    plan.target_slot = mode_ == Mode::Animating ? plan.target_slot : plan.anchor_slot;
    plan.measured_indices.push_back(displayed_index_);
    if ((mode_ == Mode::Dragging || mode_ == Mode::AwaitingCommit) && drag_target_.has_value()) {
      plan.measured_indices.push_back(*drag_target_);
    } else if (mode_ == Mode::Animating) {
      if (animation_target_index_ != displayed_index_) {
        plan.measured_indices.push_back(animation_target_index_);
      } else if (drag_target_.has_value()) {
        plan.measured_indices.push_back(*drag_target_);
      }
    }
    return plan;
  }

  void PrepareLayout(detail::MountedNode& node, float extent, const LayoutPlan& plan) {
    const float normalized_displacement = extent_ > 0.0F ? (Offset(node) - anchor_offset_) / extent_ : 0.0F;
    const bool extent_changed = extent_ > 0.0F && extent != extent_;
    extent_ = extent;
    anchor_offset_ = static_cast<float>(plan.anchor_slot) * extent;
    if (needs_rebase_ || mode_ == Mode::Stable) {
      SetOffset(node, anchor_offset_);
      needs_rebase_ = false;
    } else if (extent_changed && (mode_ == Mode::Dragging || mode_ == Mode::AwaitingCommit)) {
      SetOffset(node, anchor_offset_ + normalized_displacement * extent);
    }
    if (mode_ == Mode::Animating && !layout_ready_) {
      animation_start_offset_ = static_cast<float>(plan.anchor_slot) * extent +
                                animation_initial_displacement_ * extent;
      animation_target_offset_ = static_cast<float>(plan.target_slot) * extent;
      SetOffset(node, animation_start_offset_);
      progress_.Set(0.0F);
      progress_.AnimateTo(1.0F, behavior_.animation);
      layout_ready_ = true;
    } else if (mode_ == Mode::Animating && extent_changed) {
      animation_start_offset_ = static_cast<float>(plan.anchor_slot) * extent +
                                animation_initial_displacement_ * extent;
      animation_target_offset_ = static_cast<float>(plan.target_slot) * extent;
      const float offset = animation_start_offset_ +
                           (animation_target_offset_ - animation_start_offset_) * progress_.Value();
      SetOffset(node, offset);
    }
  }

private:
  enum class Mode {
    Stable,
    Dragging,
    AwaitingCommit,
    Animating,
  };

  static constexpr std::uint32_t DragSourceMask() {
    return 1U << static_cast<std::uint32_t>(ScrollSource::Drag);
  }

  void ConfigureScrollState(detail::MountedNode& node) {
    if (!node.scroll_state) {
      throw std::logic_error("HuxerUI mounted Pager has no scroll state");
    }
    node.scroll_state->axis = behavior_.axis;
    UpdateAllowedSources(node);
    node.scroll_state->allows_automatic_reveal = false;
    node.scroll_state->allows_leading_overscroll = false;
    node.scroll_state->allows_trailing_overscroll = false;
    node.scroll_state->overscroll_offset = 0.0F;
    event_bindings_ = node.event_bindings;
  }

  void UpdateAllowedSources(detail::MountedNode& node) const {
    const bool allows_drag = behavior_.drag_enabled && (mode_ == Mode::Stable || mode_ == Mode::Dragging);
    node.scroll_state->allowed_sources = allows_drag ? DragSourceMask() : 0U;
  }

  void InvalidateLayout(detail::MountedNode& node) {
    if (node.runtime) {
      detail::RuntimeAccess::InvalidateLayout(*node.runtime, node);
    }
  }

  float Offset(const detail::MountedNode& node) const {
    return behavior_.axis == Axis::Vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x;
  }

  void SetOffset(detail::MountedNode& node, float value) const {
    if (behavior_.axis == Axis::Vertical) {
      node.scroll_state->offset_y = value;
    } else {
      node.scroll_state->offset_x = value;
    }
  }

  std::optional<std::size_t> Adjacent(std::size_t index, int logical_direction) const {
    if (logical_direction < 0 && index > 0) {
      return index - 1;
    }
    if (logical_direction > 0 && index + 1 < behavior_.page_count) {
      return index + 1;
    }
    return std::nullopt;
  }

  std::optional<std::size_t> ResolveDragTarget(const detail::MountedNode& node) const {
    const float displacement = Offset(node) - anchor_offset_;
    if (displacement == 0.0F) {
      return std::nullopt;
    }
    const int physical_direction = displacement > 0.0F ? 1 : -1;
    return Adjacent(displayed_index_, behavior_.reverse ? -physical_direction : physical_direction);
  }

  std::size_t ResolveReleaseProposal(const detail::MountedNode& node) const {
    const float displacement = Offset(node) - anchor_offset_;
    const float decisive_velocity = std::abs(release_velocity_) >= 600.0F ? release_velocity_ : 0.0F;
    const float direction_value = decisive_velocity != 0.0F ? decisive_velocity : displacement;
    if (direction_value == 0.0F ||
        (decisive_velocity == 0.0F && std::abs(displacement) < extent_ * 0.35F)) {
      return displayed_index_;
    }
    const int physical_direction = direction_value > 0.0F ? 1 : -1;
    return Adjacent(displayed_index_, behavior_.reverse ? -physical_direction : physical_direction)
        .value_or(displayed_index_);
  }

  void BeginAnimation(detail::MountedNode& node, std::size_t target) {
    animation_initial_displacement_ = extent_ > 0.0F ? (Offset(node) - anchor_offset_) / extent_ : 0.0F;
    animation_target_index_ = target;
    mode_ = Mode::Animating;
    UpdateAllowedSources(node);
    layout_ready_ = false;
    proposal_emitted_ = false;
    InvalidateLayout(node);
  }

  PagerBehavior behavior_;
  detail::EventBindings event_bindings_;
  MotionController progress_;
  std::size_t displayed_index_ = 0;
  std::size_t animation_target_index_ = 0;
  std::optional<std::size_t> drag_target_;
  float extent_ = 0.0F;
  float anchor_offset_ = 0.0F;
  float animation_start_offset_ = 0.0F;
  float animation_target_offset_ = 0.0F;
  float animation_initial_displacement_ = 0.0F;
  float release_velocity_ = 0.0F;
  Mode mode_ = Mode::Stable;
  bool initialized_ = false;
  bool needs_rebase_ = true;
  bool layout_ready_ = false;
  bool proposal_emitted_ = false;
};

struct PagerLayout {
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const detail::ModifierDescriptor& behavior_descriptor = PagerBehavior::Descriptor();
    auto extension = std::ranges::find(
        mounted.extensions, &behavior_descriptor, &detail::NodeExtensionEntry::descriptor
    );
    if (extension == mounted.extensions.end()) {
      throw std::logic_error("HuxerUI mounted Pager has no behavior extension");
    }
    auto& behavior = static_cast<PagerBehaviorExtension&>(*extension->extension);
    const bool vertical = detail::ScrollAxis(mounted) == Axis::Vertical;
    if ((vertical && !constraints.HasBoundedHeight()) || (!vertical && !constraints.HasBoundedWidth())) {
      throw std::logic_error("HuxerUI Pager requires bounded constraints along its paging axis");
    }

    const float extent = vertical ? constraints.max_height : constraints.max_width;
    const Constraints page_constraints = vertical
                                             ? Constraints{
                                                   constraints.min_width,
                                                   constraints.max_width,
                                                   extent,
                                                   extent,
                                               }
                                             : Constraints{
                                                   extent,
                                                   extent,
                                                   constraints.min_height,
                                                   constraints.max_height,
                                               };
    const PagerBehaviorExtension::LayoutPlan plan = behavior.Plan();
    LayoutResult result;
    Size measured;
    for (std::size_t index : plan.measured_indices) {
      MountedNode& page = node.ChildAt(index);
      const Size page_size = context.Measure(page, page_constraints);
      measured.width = std::max(measured.width, page_size.width);
      measured.height = std::max(measured.height, page_size.height);
      const auto slot = std::ranges::find(plan.slots, index, &std::pair<std::size_t, std::size_t>::first);
      if (slot == plan.slots.end()) {
        throw std::logic_error("HuxerUI Pager page has no layout slot");
      }
      result.Place(page, vertical ? Point{0.0F, static_cast<float>(slot->second) * extent}
                                  : Point{static_cast<float>(slot->second) * extent, 0.0F});
    }
    measured = constraints.Constrain(vertical ? Size{measured.width, extent} : Size{extent, measured.height});
    mounted.scroll_state->content_width = vertical ? measured.width : extent * static_cast<float>(plan.slot_count);
    mounted.scroll_state->content_height = vertical ? extent * static_cast<float>(plan.slot_count) : measured.height;
    behavior.PrepareLayout(mounted, extent, plan);
    return result.SetSize(measured);
  }
};

const detail::ModifierDescriptor& PagerBehavior::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec&,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>& environment,
         detail::AppResources&) {
        PagerBehavior behavior = *static_cast<const PagerBehavior*>(modifier.value.get());
        behavior.animation.duration = detail::ResolveThemeSpec(environment).motion.normal;
        modifier.value = std::make_shared<PagerBehavior>(std::move(behavior));
      },
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<PagerBehaviorExtension>(node, *static_cast<const PagerBehavior*>(value));
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<PagerBehaviorExtension&>(extension).Update(node, *static_cast<const PagerBehavior*>(value));
      },
      true,
      detail::ErasedEqualsFor<PagerBehavior>(),
      [](const void* left, const void* right) {
        return PagerBehavior::LayoutEquals(
            *static_cast<const PagerBehavior*>(left), *static_cast<const PagerBehavior*>(right)
        );
      },
  };
  return descriptor;
}

std::shared_ptr<detail::ViewSpec> MakePagerSpec(std::vector<View> pages, const PagerBehavior& behavior) {
  for (std::size_t index = 0; index < pages.size(); ++index) {
    pages[index] = std::move(pages[index]).With(PagerPageState{
        index,
        index == behavior.selected_index,
    });
  }
  auto spec = MakeContainerSpec(detail::NodeKind::ScrollView, std::move(pages));
  spec->layout_descriptor = &detail::LayoutDescriptorFor<PagerLayout>();
  spec->component_semantics.role = SemanticRole::ScrollView;
  spec->component_semantics.collection = SemanticCollection{.item_count = behavior.page_count};
  spec->layout_values.insert_or_assign(
      typeid(detail::ScrollAxisBinding), detail::MakeErasedLayoutValue(behavior.axis)
  );
  spec->modifiers.push_back(detail::MakeModifierSpec(behavior));
  return spec;
}

std::vector<View> ValidateIndexedPages(std::vector<View> pages, std::size_t selected_index) {
  if (pages.empty()) {
    throw std::invalid_argument("HuxerUI IndexedPages requires at least one page");
  }
  if (selected_index >= pages.size()) {
    throw std::invalid_argument("HuxerUI IndexedPages selected index is out of range");
  }
  if (std::ranges::any_of(pages, [](const View& page) { return !page; })) {
    throw std::invalid_argument("HuxerUI IndexedPages pages must not be empty Views");
  }
  return pages;
}

std::vector<View> ValidatePagerPages(std::vector<View> pages, std::size_t selected_index) {
  if (pages.empty()) {
    throw std::invalid_argument("HuxerUI Pager requires at least one page");
  }
  if (selected_index >= pages.size()) {
    throw std::invalid_argument("HuxerUI Pager selected index is out of range");
  }
  if (std::ranges::any_of(pages, [](const View& page) { return !page; })) {
    throw std::invalid_argument("HuxerUI Pager pages must not be empty Views");
  }
  return pages;
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

const detail::ModifierDescriptor& PointerCursor::Descriptor() {
  return ApplyOnlyModifierDescriptor<PointerCursor, ApplyPointerCursor>();
}

const detail::ModifierDescriptor& Background::Descriptor() {
  return ApplyOnlyModifierDescriptor<Background, ApplyBackground>();
}

const detail::ModifierDescriptor& Border::Descriptor() {
  return ApplyOnlyModifierDescriptor<Border, ApplyBorder>();
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

const detail::ModifierDescriptor& ClipChildren::Descriptor() {
  return ApplyOnlyModifierDescriptor<ClipChildren, ApplyClipChildren>();
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
namespace {

bool NeedsDefaultShapingLocale(const ViewSpec& spec) {
  if (spec.kind == NodeKind::Canvas) {
    return true;
  }
  switch (spec.kind) {
  case NodeKind::Text:
  case NodeKind::Button:
  case NodeKind::Chip:
  case NodeKind::Checkbox:
  case NodeKind::RadioButton:
  case NodeKind::Switch:
    return !StringLiteral(spec.text).empty();
  default:
    return false;
  }
}

} // namespace

ViewSpec CompileViewSpec(
    const ViewSpec& declaration,
    const std::shared_ptr<const Environment>& environment,
    AppResources& resources
) {
  std::optional<Locale> locale;
  const auto resource_locale = [&]() -> const Locale& {
    if (!locale.has_value()) {
      locale = ResolveResourceLocale(environment, resources);
    }
    return *locale;
  };
  const auto compile_fill = [&resources, &resource_locale](const VisualFill& fill) {
    return ResolveVisualFill(
        fill, resources, NeedsResourceResolution(fill) ? resource_locale() : Locale::Default()
    );
  };
  ViewSpec compiled(declaration.kind);
  compiled.key = declaration.key;
  compiled.text = NeedsResourceResolution(declaration.text)
                      ? StringVariant{ResolveString(declaration.text, resources, resource_locale())}
                      : declaration.text;
  compiled.text_role = declaration.text_role;
  compiled.properties = declaration.properties;
  compiled.component_semantics = declaration.component_semantics;
  compiled.author_semantics = declaration.author_semantics;
  compiled.scope_factory = declaration.scope_factory;
  compiled.canvas_painter = declaration.canvas_painter;
  compiled.image_properties = declaration.image_properties;
  if (const auto* resource = std::get_if<ImageResource>(&compiled.image_properties.source)) {
    compiled.image_properties.SetResolvedAsset(ResolveImage(ImageVariant{*resource}, resources, resource_locale()));
  }
  if (compiled.image_properties.tint.has_value() && !compiled.image_properties.IsVector()) {
    throw std::invalid_argument("HuxerUI image Tint is supported for vector images only");
  }
  compiled.platform_view = declaration.platform_view;
  compiled.layout_descriptor = declaration.layout_descriptor;
  compiled.virtual_layout_descriptor = declaration.virtual_layout_descriptor;
  compiled.event_bindings = declaration.event_bindings;
  compiled.activation = declaration.activation;
  compiled.defaults = declaration.defaults;
  compiled.modifiers.reserve(declaration.modifiers.size());
  compiled.default_indication = declaration.default_indication;
  compiled.chip_selection = declaration.chip_selection;
  compiled.pointer_events_enabled = declaration.pointer_events_enabled;
  compiled.local_enabled = declaration.local_enabled;
  compiled.focusable = declaration.focusable;
  compiled.trap_focus = declaration.trap_focus;
  compiled.layout_values = declaration.layout_values;
  if (compiled.kind != NodeKind::Environment) {
    ApplyInteractionDefaults(compiled, environment);
  }
  if (compiled.defaults != nullptr) {
    compiled.defaults(compiled, environment);
  }
  for (const ModifierSpec& declaration_modifier : declaration.modifiers) {
    if (declaration_modifier.descriptor == nullptr || !declaration_modifier.value) {
      throw std::logic_error("HuxerUI modifier descriptor and value must not be empty");
    }
    ModifierSpec compiled_modifier = declaration_modifier;
    if (declaration_modifier.descriptor->compile != nullptr) {
      declaration_modifier.descriptor->compile(compiled, compiled_modifier, environment, resources);
    }
    if (!compiled_modifier.value) {
      throw std::logic_error("HuxerUI compiled modifier value must not be empty");
    }
    if (compiled_modifier.descriptor->create_extension != nullptr) {
      compiled.modifiers.push_back(std::move(compiled_modifier));
    }
  }

  if (compiled.properties.background.has_value()) {
    compiled.properties.background = compile_fill(*compiled.properties.background);
  }
  if (compiled.properties.disabled_background.has_value()) {
    compiled.properties.disabled_background = compile_fill(*compiled.properties.disabled_background);
  }
  TextShapingOptions& shaping = compiled.properties.text_layout_options.shaping;
  if (shaping.locale.empty() && NeedsDefaultShapingLocale(compiled)) {
    shaping.locale = resource_locale().LanguageTag();
  }

  return compiled;
}

std::shared_ptr<ViewSpec> MakeScopeSpec(std::function<View()> factory) {
  if (!factory) {
    throw std::invalid_argument("HuxerUI scope factory must not be empty");
  }
  auto spec = std::make_shared<ViewSpec>(NodeKind::Scope);
  spec->scope_factory = std::move(factory);
  return spec;
}

float ToggleLabelLeading(const ToggleLayoutMetrics& metrics) noexcept {
  return metrics.visual_size.width + metrics.label_spacing;
}

Rect ResolveToggleControlBounds(const MountedNode& node) noexcept {
  const ToggleLayoutMetrics metrics = node.LayoutValueOr<ToggleLayoutMetrics>({});
  const Rect content = node.ContentBounds();
  const float width = std::min(metrics.visual_size.width, content.width);
  const float height = std::min(metrics.visual_size.height, content.height);
  const float requested_horizontal_offset =
      node.text.empty() ? (content.width - metrics.visual_size.width) * 0.5F : 0.0F;
  return {
      content.x + std::clamp(requested_horizontal_offset, 0.0F, content.width - width),
      content.y + std::max(0.0F, (content.height - height) * 0.5F),
      width,
      height,
  };
}

Rect ResolveToggleLabelBounds(const MountedNode& node) noexcept {
  if (node.text.empty()) {
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

std::shared_ptr<ViewSpec> MakeLayoutSpec(const LayoutDescriptor& layout, std::vector<View> children) {
  auto spec = std::make_shared<ViewSpec>(NodeKind::Layout);
  spec->layout_descriptor = &layout;
  spec->children = std::move(children);
  return spec;
}

std::shared_ptr<ViewSpec> MakeVirtualLayoutSpec(const VirtualLayoutDescriptor& layout, ViewItemSource source) {
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
  if (modifier.descriptor->compile == nullptr && modifier.descriptor->create_extension == nullptr) {
    throw std::invalid_argument("HuxerUI modifier descriptor must compile or create a node extension");
  }
  EnsureUniqueSpec();
  if (detail::IsExplicitIndicationDescriptor(modifier.descriptor)) {
    std::erase_if(spec_->modifiers, [](const detail::ModifierSpec& existing) {
      return detail::IsDefaultIndicationDescriptor(existing.descriptor);
    });
  } else if (detail::IsDefaultIndicationDescriptor(modifier.descriptor)) {
    const bool already_has_indication =
        std::ranges::any_of(spec_->modifiers, [](const detail::ModifierSpec& existing) {
          return detail::IsDefaultIndicationDescriptor(existing.descriptor) ||
                 detail::IsExplicitIndicationDescriptor(existing.descriptor);
        });
    if (already_has_indication) {
      return;
    }
  }
  spec_->modifiers.push_back(std::move(modifier));
}

void View::SetModifier(detail::ModifierSpec modifier) {
  if (modifier.descriptor == nullptr || !modifier.value) {
    throw std::invalid_argument("HuxerUI modifier descriptor and value must not be empty");
  }
  if (modifier.descriptor->create_extension == nullptr && modifier.descriptor->update_extension != nullptr) {
    throw std::invalid_argument("HuxerUI modifier extension update requires extension creation");
  }
  if (modifier.descriptor->compile == nullptr && modifier.descriptor->create_extension == nullptr) {
    throw std::invalid_argument("HuxerUI modifier descriptor must compile or create a node extension");
  }
  EnsureUniqueSpec();
  const auto found = std::ranges::find_if(spec_->modifiers, [&modifier](const detail::ModifierSpec& existing) {
    return existing.descriptor == modifier.descriptor;
  });
  if (found == spec_->modifiers.end()) {
    spec_->modifiers.push_back(std::move(modifier));
  } else {
    *found = std::move(modifier);
  }
}

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

void View::SetTextStyle(TextStyle style) {
  AddModifier(detail::MakeModifierSpec(TextStyleProperty{std::move(style)}));
}

void View::SetTextShaping(TextShapingOptions shaping) {
  EnsureUniqueSpec();
  spec_->properties.text_layout_options.shaping = std::move(shaping);
}

void View::SetTextAlign(TextAlign align) {
  EnsureUniqueSpec();
  spec_->properties.text_layout_options.align = align;
}

void View::SetTextVerticalAlign(TextVerticalAlign align) {
  EnsureUniqueSpec();
  spec_->properties.text_layout_options.vertical_align = align;
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
  if (!spec_->image_properties.IsVector() &&
      !std::holds_alternative<ImageResource>(spec_->image_properties.source)) {
    throw std::invalid_argument("HuxerUI image Tint is supported for vector images only");
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

Text::Text(StringVariant value, TextRole role) : View(MakeTextSpec(std::move(value), role)) {}

Text Text::Style(TextStyle style) && {
  SetTextStyle(std::move(style));
  return std::move(*this);
}

Text Text::Shaping(TextShapingOptions shaping) && {
  SetTextShaping(std::move(shaping));
  return std::move(*this);
}

Text Text::Align(TextAlign align) && {
  SetTextAlign(align);
  return std::move(*this);
}

Text Text::VerticalAlign(TextVerticalAlign align) && {
  SetTextVerticalAlign(align);
  return std::move(*this);
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

ProgressCircle::ProgressCircle() : detail::TypedView<ProgressCircle>(MakeProgressCircleSpec(std::nullopt)) {}

ProgressCircle::ProgressCircle(float progress) : detail::TypedView<ProgressCircle>(MakeProgressCircleSpec(progress)) {}

ProgressBar::ProgressBar() : detail::TypedView<ProgressBar>(MakeProgressBarSpec(std::nullopt)) {}

ProgressBar::ProgressBar(float progress) : detail::TypedView<ProgressBar>(MakeProgressBarSpec(progress)) {}

Slider::Slider(float value) : detail::TypedView<Slider>(MakeSliderSpec(value)), value_(value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("HuxerUI Slider value must be finite");
  }
}

Slider Slider::Range(float minimum, float maximum) && {
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) {
    throw std::invalid_argument("HuxerUI Slider range must be finite and increasing");
  }
  minimum_ = minimum;
  maximum_ = maximum;
  UpdateModifier();
  return std::move(*this);
}

Slider Slider::Step(float step) && {
  if (!std::isfinite(step) || step <= 0.0F) {
    throw std::invalid_argument("HuxerUI Slider step must be finite and greater than zero");
  }
  step_ = step;
  UpdateModifier();
  return std::move(*this);
}

void Slider::UpdateModifier() {
  SetModifier(detail::MakeModifierSpec(SliderVisual{value_, minimum_, maximum_, step_}));
}

Canvas::Canvas(CanvasPainter painter) : View(MakeCanvasSpec(std::move(painter))) {}

Scope::Scope(std::function<View()> factory) : View(detail::MakeScopeSpec(std::move(factory))) {}

Spacer::Spacer() : View(MakeSpacerSpec()) {}

IndexedPages::IndexedPages(std::vector<View> pages, std::size_t selected_index)
    : Layout<IndexedPages>(ValidateIndexedPages(std::move(pages), selected_index)) {
  SetLayoutValue(typeid(detail::IndexedPagesSelection), selected_index);
}

Pager::Pager(std::vector<View> pages, std::size_t selected_index)
    : detail::TypedView<Pager>(MakePagerSpec(
          ValidatePagerPages(pages, selected_index),
          PagerBehavior{selected_index, pages.size()}
      )),
      selected_index_(selected_index),
      page_count_(pages.size()) {}

Pager Pager::ScrollAxis(Axis axis) && {
  axis_ = axis;
  SetLayoutValue(typeid(detail::ScrollAxisBinding), axis);
  UpdateBehavior();
  return std::move(*this);
}

Pager Pager::Reverse(bool reverse) && {
  reverse_ = reverse;
  UpdateBehavior();
  return std::move(*this);
}

Pager Pager::DragEnabled(bool enabled) && {
  drag_enabled_ = enabled;
  UpdateBehavior();
  return std::move(*this);
}

void Pager::UpdateBehavior() {
  SetModifier(detail::MakeModifierSpec(PagerBehavior{
      selected_index_,
      page_count_,
      axis_,
      reverse_,
      drag_enabled_,
  }));
}

RefreshBox::RefreshBox(View content, bool refreshing)
    : detail::TypedView<RefreshBox>(MakeRefreshBoxSpec(std::move(content), refreshing)) {}

ScrollView::ScrollView(View content) : detail::TypedView<ScrollView>(MakeScrollViewSpec(std::move(content))) {}

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
