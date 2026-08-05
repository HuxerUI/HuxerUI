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

namespace detail {

template <class IconVariant> std::optional<ResolvedImageAsset> ResolveOptionalControlIcon(IconVariant& value) {
    return std::visit(
        [](auto& icon) -> std::optional<ResolvedImageAsset> {
          using Icon = std::decay_t<decltype(icon)>;
          if constexpr (std::same_as<Icon, std::monostate>) {
            return std::nullopt;
          } else if constexpr (std::same_as<Icon, ImageResource>) {
            return UseImageResource(std::move(icon));
          } else {
            if (!icon.HasValue()) {
              throw std::invalid_argument("HuxerUI control icon asset must not be empty");
            }
            return ResolvedImageAsset{std::move(icon)};
          }
        },
        value
  );
}

struct SegmentedButtonItemAccess {
  static std::optional<ResolvedImageAsset> ResolveIcon(SegmentedButtonItem& item) {
    return ResolveOptionalControlIcon(item.icon_
    );
  }

  static std::string ResolveLabel(SegmentedButtonItem& item) {
    return ResolveStringVariant(std::move(item.label_));
  }

  static bool ShowsLabel(const SegmentedButtonItem& item) noexcept {
    return item.show_label_;
  }
};

struct TabItemAccess {
  static std::optional<ResolvedImageAsset> ResolveIcon(TabItem& item) {
    return ResolveOptionalControlIcon(item.icon_);
  }

  static std::string ResolveLabel(TabItem& item) {
    return ResolveStringVariant(std::move(item.label_));
  }

  static bool ShowsLabel(const TabItem& item) noexcept {
    return item.show_label_;
  }

  static bool IsEnabled(const TabItem& item) noexcept {
    return item.enabled_;
  }
};

} // namespace detail

namespace {

detail::ResolvedImageAsset ResolveControlIcon(ImageResource icon) {
  return detail::UseImageResource(std::move(icon));
}

detail::ResolvedImageAsset ResolveControlIcon(ImageAsset icon) {
  if (!icon.HasValue()) {
    throw std::invalid_argument("HuxerUI control icon asset must not be empty");
  }
  return icon;
}

detail::ResolvedImageAsset ResolveControlIcon(VectorAsset icon) {
  if (!icon.HasValue()) {
    throw std::invalid_argument("HuxerUI control icon asset must not be empty");
  }
  return icon;
}

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

bool UsesDisabledVisualState(const MountedNode& node) {
  return static_cast<const detail::MountedNode&>(node).disabled_visual_state;
}

enum class ToggleVisualKind {
  Checkbox,
  RadioButton,
  Switch,
};

struct ResolvedCheckboxStyle {
  using Value = CheckboxStyle;
};

struct ResolvedRadioButtonStyle {
  using Value = RadioButtonStyle;
};

struct ResolvedSwitchStyle {
  using Value = SwitchStyle;
};

struct ResolvedProgressCircleStyle {
  using Value = ProgressCircleStyle;
};

struct ResolvedProgressBarStyle {
  using Value = ProgressBarStyle;
};

struct ResolvedSliderStyle {
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

  bool operator==(const ToggleVisual&) const = default;
};

class ToggleVisualExtension final : public NodeExtension {
public:
  ToggleVisualExtension(MountedNode& node, const ToggleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ToggleVisual& modifier) {
    kind_ = modifier.kind;
    if (kind_ == ToggleVisualKind::Checkbox) {
      checkbox_style_ = node.LayoutValueOr<ResolvedCheckboxStyle>(CheckboxStyle::Default());
    } else if (kind_ == ToggleVisualKind::RadioButton) {
      radio_button_style_ = node.LayoutValueOr<ResolvedRadioButtonStyle>(RadioButtonStyle::Default());
    } else {
      switch_style_ = node.LayoutValueOr<ResolvedSwitchStyle>(SwitchStyle::Default());
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
      progress_.Update(checked_ ? 1.0F : 0.0F, TweenSpec{duration});
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

  [[nodiscard]] bool PrepareGeometry(MountedNode& node) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    std::optional<Rect> indication_frame;
    if (kind_ == ToggleVisualKind::Switch) {
      const Rect track = detail::ResolveToggleControlBounds(mounted);
      const float state_layer_size =
          std::min(std::max(0.0F, switch_style_.state_layer_size), std::min(node.Bounds().width, node.Bounds().height));
      const float thumb_center_x =
          track.x + track.height * 0.5F + std::max(0.0F, track.width - track.height) * progress_.Value();
      indication_frame = Rect{
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
      indication_frame = Rect{
          control.x + control.width * 0.5F - size * 0.5F,
          control.y + control.height * 0.5F - size * 0.5F,
          size,
          size,
      };
    }
    if (mounted.indication_frame == indication_frame) {
      return false;
    }
    mounted.indication_frame = indication_frame;
    return true;
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
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
      context.DrawText(
          frame,
          "✓",
          TextStyle{Font::System(std::max(0.1F, checkbox_style_.size * 0.72F)), checkmark},
          TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
      );
      return;
    }
    context.DrawBorder(
        frame,
        disabled ? checkbox_style_.disabled_unchecked_border : checkbox_style_.unchecked_border,
        std::max(0.0F, checkbox_style_.border_width),
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
    const Color color = InterpolateColor(unselected, selected, progress);
    const float maximum_radius = std::max(0.0F, std::min(frame.width, frame.height) * 0.5F);
    const float border_width = std::clamp(radio_button_style_.border_width, 0.0F, maximum_radius);
    const Point center{
        frame.x + frame.width * 0.5F,
        frame.y + frame.height * 0.5F,
    };
    context.DrawArc(
        center, std::max(0.0F, maximum_radius - border_width * 0.5F), 0.0F, full_circle, color, border_width
    );
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
            ? InterpolateColor(switch_style_.disabled_unchecked_track, switch_style_.disabled_checked_track, progress)
            : InterpolateColor(switch_style_.unchecked_track, switch_style_.checked_track, progress);
    const Color border =
        disabled ? InterpolateColor(
                       switch_style_.disabled_unchecked_track_border,
                       switch_style_.disabled_checked_track_border,
                       progress
                   )
                 : InterpolateColor(switch_style_.unchecked_track_border, switch_style_.checked_track_border, progress);
    context.DrawRect(frame, track, std::max(0.0F, switch_style_.corner_radius));

    if (switch_style_.track_border_width > 0.0F && border.alpha > 0.0F) {
      context.DrawBorder(frame, border, switch_style_.track_border_width, std::max(0.0F, switch_style_.corner_radius));
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
            ? InterpolateColor(switch_style_.disabled_unchecked_thumb, switch_style_.disabled_checked_thumb, progress)
            : InterpolateColor(switch_style_.unchecked_thumb, switch_style_.checked_thumb, progress);
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
  detail::AnimatedValue<float> progress_;
  bool checked_ = false;
  bool initialized_ = false;
  bool target_pending_ = false;
};

const detail::ModifierDescriptor& ToggleVisual::Descriptor() {
  return detail::ModifierDescriptorFor<ToggleVisual, ToggleVisualExtension>();
}

struct SegmentedButtonBorderWidth {
  using Value = float;
};

struct SegmentedButtonInput {
  static const detail::ModifierDescriptor& Descriptor();

  std::size_t selected_index;
  std::size_t segment_count;

  bool operator==(const SegmentedButtonInput&) const = default;
};

class SegmentedButtonInputExtension final : public NodeExtension {
public:
  SegmentedButtonInputExtension(MountedNode& node, const SegmentedButtonInput& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const SegmentedButtonInput& modifier) {
    selected_index_ = modifier.selected_index;
    segment_count_ = modifier.segment_count;
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      pressed_index_.reset();
    }
  }

  [[nodiscard]] bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && SegmentIndexAt(node, position).has_value();
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      pressed_index_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      const std::optional<std::size_t> index = SegmentIndexAt(node, event.position);
      if (!index.has_value()) {
        return PointerResult::Ignored;
      }
      pointer_id_ = event.pointer_id;
      pressed_index_ = index;
      return PointerResult::Observe;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      const std::optional<std::size_t> released_index = SegmentIndexAt(node, event.position);
      if (released_index.has_value() && released_index == pressed_index_) {
        EmitSelection(node, *released_index);
      }
      pointer_id_.reset();
      pressed_index_.reset();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      pressed_index_.reset();
    }
    return PointerResult::Handled;
  }

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || segment_count_ == 0 || event.type != KeyEventType::Down || event.modifiers.alt ||
        event.modifiers.control || event.modifiers.meta) {
      return;
    }
    std::optional<std::size_t> requested;
    if (event.key == Key::ArrowLeft) {
      requested = selected_index_ == 0 ? segment_count_ - 1 : selected_index_ - 1;
    } else if (event.key == Key::ArrowRight) {
      requested = selected_index_ + 1 == segment_count_ ? 0 : selected_index_ + 1;
    } else if (event.key == Key::Home) {
      requested = 0;
    } else if (event.key == Key::End) {
      requested = segment_count_ - 1;
    }
    if (requested.has_value()) {
      EmitSelection(node, *requested);
    }
  }

private:
  static std::optional<std::size_t> SegmentIndexAt(MountedNode& node, Point position) {
    for (std::size_t index = node.ChildCount(); index > 0; --index) {
      const MountedNode& child = node.ChildAt(index - 1);
      const Point offset = child.LayoutOffset();
      const Size size = child.LayoutSize();
      if (Rect{offset.x, offset.y, size.width, size.height}.Contains(position)) {
        return index - 1;
      }
    }
    return std::nullopt;
  }

  void EmitSelection(MountedNode& node, std::size_t index) {
    if (index == selected_index_ || index >= segment_count_) {
      return;
    }
    detail::EmitEvent<SegmentedButtonEvents::Changed>(static_cast<detail::MountedNode&>(node).event_bindings, index);
  }

  std::optional<std::int64_t> pointer_id_;
  std::optional<std::size_t> pressed_index_;
  std::size_t selected_index_ = 0;
  std::size_t segment_count_ = 0;
};

const detail::ModifierDescriptor& SegmentedButtonInput::Descriptor() {
  return detail::ModifierDescriptorFor<SegmentedButtonInput, SegmentedButtonInputExtension>();
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
  TabLabel(const ResolvedTabItem& item, const TabsStyle& style, bool selected) : View(MakeSpec(item, style)) {
    TextStyle text_style = style.label_style;
    text_style.foreground = selected ? style.selected_label : style.label_style.foreground;
    SetTextStyle(std::move(text_style));
  }

private:
  static std::shared_ptr<detail::ViewSpec> MakeSpec(const ResolvedTabItem& item, const TabsStyle& style) {
    auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Text);
    spec->text = item.label;
    Size icon_size;
    if (item.icon.has_value()) {
      spec->image_properties.asset = *item.icon;
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
        .align = TextAlign::Center,
        .wrap = TextWrap::NoWrap,
    };
    spec->properties.indication_override = style.indication;
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
    static_cast<void>(indicator_x_.Advance(frame.timestamp, frame.delta_time));
    static_cast<void>(indicator_width_.Advance(frame.timestamp, frame.delta_time));
    if (indicator_x_.Value() != previous_x || indicator_width_.Value() != previous_width) {
      InvalidatePaint();
    }
    if (reveal_offset_.has_value()) {
      static_cast<void>(scroll_controller_.ScrollTo(*reveal_offset_));
      reveal_offset_.reset();
    }
    return {
        .needs_frame = geometry_update_pending_ || indicator_x_.IsRunning() || indicator_width_.IsRunning(),
        .wake_after = std::nullopt,
    };
  }

  [[nodiscard]] bool PrepareGeometry(MountedNode& node) override {
    if (selected_index_ >= node.ChildCount()) {
      return false;
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
        indicator_x_.Update(target_x, animation);
        indicator_width_.Update(target_width, animation);
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
    return changed;
  }

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control ||
        event.modifiers.meta || enabled_items_.empty()) {
      return;
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
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
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

  detail::AnimatedValue<float> indicator_x_;
  detail::AnimatedValue<float> indicator_width_;
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
    spec->properties.background = style.background;
    spec->layout_values.insert_or_assign(typeid(TabsExpandItems), detail::MakeErasedLayoutValue(style.expand_items));
    spec->retained_modifiers.push_back(
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

class ProgressCircleVisualExtension final : public NodeExtension {
public:
  ProgressCircleVisualExtension(MountedNode& node, const ProgressCircleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ProgressCircleVisual& modifier) {
    style_ = node.LayoutValueOr<ResolvedProgressCircleStyle>(ProgressCircleStyle::Default());
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
    const float minimum_arc = std::clamp(style_.minimum_indeterminate_arc_fraction, 0.0F, 1.0F);
    const float maximum_arc = std::clamp(style_.maximum_indeterminate_arc_fraction, minimum_arc, 1.0F);
    const bool pulsing_arc = style_.indeterminate_motion == ProgressCircleIndeterminateMotion::PulsingArc;
    const float indeterminate_progress =
        pulsing_arc
            ? PulsingArcProgress(phase_.Value(), minimum_arc, maximum_arc)
            : minimum_arc + (maximum_arc - minimum_arc) * (1.0F - std::cos(phase_.Value() * full_circle)) * 0.5F;
    const float progress = progress_.value_or(indeterminate_progress);
    const Color track_color = progress_.has_value() ? style_.track_color : style_.indeterminate_track_color;
    const bool separated_track = style_.track_gap > 0.0F;
    if (!separated_track && track_color.alpha > 0.0F) {
      context.DrawArc(center, radius, -pi * 0.5F, full_circle, track_color, stroke_width);
    }

    if (progress <= 0.0F) {
      if (separated_track && track_color.alpha > 0.0F) {
        context.DrawArc(center, radius, -pi * 0.5F, full_circle, track_color, stroke_width, StrokeCap::Round);
      }
      return;
    }
    float start = -pi * 0.5F;
    if (!progress_.has_value()) {
      start = pulsing_arc ? PulsingArcRotation(phase_.Value()) : start + phase_.Value() * full_circle * 2.0F;
    }
    const float sweep = std::clamp(progress, 0.0F, 1.0F) * full_circle;
    const float adjusted_gap = std::max(0.0F, style_.track_gap) + stroke_width;
    const float gap_angle = std::min(sweep, adjusted_gap / radius);
    const float track_sweep = std::max(0.0F, full_circle - sweep - gap_angle * 2.0F);
    if (separated_track && track_color.alpha > 0.0F && track_sweep > 0.0F) {
      context
          .DrawArc(center, radius, start + sweep + gap_angle, track_sweep, track_color, stroke_width, StrokeCap::Round);
    }
    context.DrawArc(center, radius, start, sweep, style_.indicator_color, stroke_width, StrokeCap::Round);
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
    style_ = node.LayoutValueOr<ResolvedProgressBarStyle>(ProgressBarStyle::Default());
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

  void Paint(const MountedNode& node, PaintContext& context) const override {
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
    style_ = node.LayoutValueOr<ResolvedSliderStyle>(SliderStyle::Default());
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
    UpdateThumbSize(node.IsEnabled());
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_width = thumb_width_.Value();
    const float previous_height = thumb_height_.Value();
    thumb_width_.Advance(frame.timestamp, frame.delta_time);
    thumb_height_.Advance(frame.timestamp, frame.delta_time);
    if (thumb_width_.Value() != previous_width || thumb_height_.Value() != previous_height) {
      InvalidatePaint();
    }
    return {
        .needs_frame = thumb_width_.IsRunning() || thumb_height_.IsRunning(),
        .wake_after = std::nullopt,
    };
  }

  [[nodiscard]] bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHoverChanged(MountedNode& node, bool hovered) override {
    static_cast<void>(node);
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

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control || event.modifiers.meta) {
      return;
    }
    const float increment = step_.value_or((maximum_ - minimum_) / 100.0F);
    switch (event.key) {
    case Key::ArrowLeft:
    case Key::ArrowDown:
      EmitValue(node, last_emitted_value_ - increment);
      break;
    case Key::ArrowRight:
    case Key::ArrowUp:
      EmitValue(node, last_emitted_value_ + increment);
      break;
    case Key::Home:
      EmitValue(node, minimum_);
      break;
    case Key::End:
      EmitValue(node, maximum_);
      break;
    default:
      break;
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

  void Paint(const MountedNode& node, PaintContext& context) const override {
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
    thumb_width_.Update(target_width, animation);
    thumb_height_.Update(target_height, animation);
  }

  SliderStyle style_;
  detail::AnimatedValue<float> thumb_width_;
  detail::AnimatedValue<float> thumb_height_;
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
  if (spec.text.empty()) {
    spec.properties.frame.width = metrics.interactive_size.width;
    spec.properties.frame.height = metrics.interactive_size.height;
    return;
  }

  spec.properties.frame.min_width = metrics.interactive_size.width;
  spec.properties.frame.min_height = metrics.interactive_size.height;
  spec.properties.text_style =
      ResolveStyleOverride<TextStyle>(spec.environment).value_or(detail::DefaultTextStyle(theme, TextRole::Body));
  spec.properties.text_layout_options = {.wrap = TextWrap::NoWrap};
  Color disabled_label = spec.properties.text_style.foreground;
  disabled_label.alpha *= spec.properties.disabled_opacity;
  spec.properties.disabled_foreground = disabled_label;
}

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
      spec->image_properties.asset = std::move(*icon);
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
    spec->properties.border = selected ? style.selected_border : style.border;
    spec->properties.border_width = std::max(0.0F, style.border_width);
    spec->properties.corner_radii = SegmentCornerRadii(index, count, style.corner_radius);
    spec->properties.frame.min_width = std::max(0.0F, style.minimum_segment_width);
    spec->properties.frame.min_height = std::max(0.0F, style.minimum_height);
    spec->properties.text_layout_options = {
        .align = TextAlign::Center,
        .wrap = TextWrap::NoWrap,
    };
    spec->properties.indication_override =
        selected && style.selected_indication.has_value() ? style.selected_indication : style.indication;
    spec->retained_modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
    return spec;
  }
};

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
    spec.properties.disabled_background = style.disabled_background;
    spec.properties.text_style = style.label_style;
    spec.properties.disabled_foreground = style.disabled_label;
    spec.properties.corner_radii = style.corner_radius;
    spec.properties.frame.min_width = std::max(0.0F, style.minimum_width);
    spec.properties.frame.min_height = std::max(0.0F, style.minimum_height);
    spec.properties.indication_override = style.indication;
    spec.properties.disabled_opacity = 1.0F;
    return;
  }
  if (spec.kind == detail::NodeKind::Chip) {
    const ChipStyle style =
        ResolveStyleOverride<ChipStyle>(spec.environment).value_or(detail::DefaultChipStyle(theme));
    const bool selected = spec.chip_selection.value_or(false);
    spec.properties.padding = style.padding;
    spec.properties.background = selected ? style.selected_background : style.background;
    spec.properties.disabled_background =
        selected ? style.disabled_selected_background : style.disabled_background;
    spec.properties.border = selected ? style.selected_border : style.border;
    spec.properties.disabled_border = selected ? style.disabled_selected_border : style.disabled_border;
    spec.properties.border_width = std::max(0.0F, style.border_width);
    spec.properties.text_style = style.label_style;
    spec.properties.text_style.foreground = selected ? style.selected_label : style.label_style.foreground;
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
    spec.properties.indication_override =
        selected && style.selected_indication.has_value() ? style.selected_indication : style.indication;
    spec.properties.disabled_opacity = 1.0F;
    return;
  }
  if (spec.kind == detail::NodeKind::Divider) {
    const DividerStyle style =
        ResolveStyleOverride<DividerStyle>(spec.environment).value_or(detail::DefaultDividerStyle(theme));
    spec.properties.background = style.color;
    spec.layout_values.insert_or_assign(
        typeid(detail::DividerThicknessBinding),
        detail::MakeErasedLayoutValue(std::max(0.0F, style.thickness))
    );
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
    spec.properties.corner_radii = style.corner_radius;
    spec.properties.frame.min_height = std::max(0.0F, style.minimum_height);
    spec.properties.disabled_opacity = 1.0F;
    return;
  }
  if (spec.kind == detail::NodeKind::Checkbox) {
    const CheckboxStyle style =
        ResolveStyleOverride<CheckboxStyle>(spec.environment).value_or(detail::DefaultCheckboxStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedCheckboxStyle), detail::MakeErasedLayoutValue(style));
    const float interactive_size = std::max(0.0F, std::max(style.size, style.minimum_interactive_size));
    const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), interactive_size);
    ApplyToggleLayoutDefaults(
        spec,
        theme,
        {{style.size, style.size}, {interactive_size, interactive_size}, theme.spacing.small}
    );
    spec.properties.corner_radii = state_layer_size * 0.5F;
    spec.properties.indication_size = Size{state_layer_size, state_layer_size};
    spec.properties.indication_corner_radius = state_layer_size * 0.5F;
    spec.properties.disabled_opacity = 1.0F;
    return;
  }
  if (spec.kind == detail::NodeKind::RadioButton) {
    const RadioButtonStyle style =
        ResolveStyleOverride<RadioButtonStyle>(spec.environment).value_or(detail::DefaultRadioButtonStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedRadioButtonStyle), detail::MakeErasedLayoutValue(style));
    const float interactive_size = std::max(0.0F, std::max(style.size, style.minimum_interactive_size));
    const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), interactive_size);
    ApplyToggleLayoutDefaults(
        spec,
        theme,
        {{style.size, style.size}, {interactive_size, interactive_size}, theme.spacing.small}
    );
    spec.properties.corner_radii = state_layer_size * 0.5F;
    spec.properties.indication_size = Size{state_layer_size, state_layer_size};
    spec.properties.indication_corner_radius = state_layer_size * 0.5F;
    spec.properties.disabled_opacity = 1.0F;
    return;
  }
  if (spec.kind == detail::NodeKind::Switch) {
    const SwitchStyle style =
        ResolveStyleOverride<SwitchStyle>(spec.environment).value_or(detail::DefaultSwitchStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedSwitchStyle), detail::MakeErasedLayoutValue(style));
    const float width = std::max(0.0F, style.width);
    const float height = std::max(0.0F, std::max(style.height, style.minimum_interactive_height));
    const float state_layer_size = std::min(std::max(0.0F, style.state_layer_size), std::min(width, height));
    ApplyToggleLayoutDefaults(
        spec,
        theme,
        {{style.width, style.height}, {width, height}, theme.spacing.small}
    );
    spec.properties.corner_radii = state_layer_size * 0.5F;
    spec.properties.indication_size = Size{state_layer_size, state_layer_size};
    spec.properties.indication_corner_radius = state_layer_size * 0.5F;
    spec.properties.disabled_opacity = 1.0F;
    return;
  }
  if (spec.kind == detail::NodeKind::ProgressCircle) {
    const ProgressCircleStyle style =
        ResolveStyleOverride<ProgressCircleStyle>(spec.environment).value_or(detail::DefaultProgressCircleStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedProgressCircleStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.frame.width = std::max(0.0F, style.size);
    spec.properties.frame.height = std::max(0.0F, style.size);
    return;
  }
  if (spec.kind == detail::NodeKind::ProgressBar) {
    const ProgressBarStyle style =
        ResolveStyleOverride<ProgressBarStyle>(spec.environment).value_or(detail::DefaultProgressBarStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedProgressBarStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.frame.width = std::max(0.0F, style.width);
    spec.properties.frame.height = std::max(0.0F, style.height);
    return;
  }
  if (spec.kind == detail::NodeKind::Slider) {
    const SliderStyle style =
        ResolveStyleOverride<SliderStyle>(spec.environment).value_or(detail::DefaultSliderStyle(theme));
    spec.layout_values.insert_or_assign(typeid(ResolvedSliderStyle), detail::MakeErasedLayoutValue(style));
    spec.properties.frame.width = std::max(0.0F, style.width);
    spec.properties.frame.height = std::max(0.0F, style.height);
    spec.properties.corner_radii = std::max(0.0F, style.height * 0.5F);
    if (style.focus_ring_width.has_value()) {
      spec.properties.focus_ring_width = std::max(0.0F, *style.focus_ring_width);
    }
    spec.properties.disabled_opacity = 1.0F;
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

std::shared_ptr<detail::ViewSpec> MakeChipSpec(
    std::string label,
    std::optional<bool> selection,
    std::optional<detail::ResolvedImageAsset> icon = std::nullopt
) {
  if (icon.has_value() && label.empty()) {
    throw std::invalid_argument("HuxerUI Chip with an icon requires a non-empty label");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Chip);
  spec->text = std::move(label);
  if (icon.has_value()) {
    spec->image_properties.asset = std::move(*icon);
  }
  spec->focusable = true;
  spec->chip_selection = selection;
  const bool selected = selection.value_or(false);
  if (selection.has_value()) {
    spec->activation = [selected](const detail::EventBindings& bindings) {
      detail::EmitEvent<ToggleEvents::Changed>(bindings, !selected);
    };
  }
  if (selection.has_value()) {
    spec->retained_modifiers.push_back(detail::MakeModifierSpec(detail::DefaultIndication{}));
  }
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeDividerSpec(Axis axis) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Divider);
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

  const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const SegmentedButtonStyle style =
      ResolveStyleOverride<SegmentedButtonStyle>(environment).value_or(detail::DefaultSegmentedButtonStyle(theme));

  std::vector<View> segments;
  segments.reserve(items.size());
  for (std::size_t index = 0; index < items.size(); ++index) {
    const bool show_label = detail::SegmentedButtonItemAccess::ShowsLabel(items[index]);
    std::string label = detail::SegmentedButtonItemAccess::ResolveLabel(items[index]);
    std::optional<detail::ResolvedImageAsset> icon =
        detail::SegmentedButtonItemAccess::ResolveIcon(items[index]);
    if (label.empty()) {
      throw std::invalid_argument("HuxerUI SegmentedButton item requires a non-empty semantic label");
    }
    if (!show_label && !icon.has_value()) {
      throw std::invalid_argument("HuxerUI icon-only SegmentedButton item requires an icon and semantic label");
    }
    segments.emplace_back(SegmentedButtonLabel(
        std::move(label),
        std::move(icon),
        show_label,
        style,
        index == selected_index,
        index,
        items.size()
    ));
  }

  auto spec = detail::MakeLayoutSpec(detail::LayoutDescriptorFor<SegmentedButtonLayoutPolicy>(), std::move(segments));
  spec->focusable = true;
  spec->properties.corner_radii = std::max(0.0F, style.corner_radius);
  spec->properties.clip_children = true;
  spec->layout_values.insert_or_assign(
      typeid(SegmentedButtonBorderWidth),
      detail::MakeErasedLayoutValue(std::max(0.0F, style.border_width))
  );
  spec->retained_modifiers.push_back(detail::MakeModifierSpec(SegmentedButtonInput{selected_index, items.size()}));
  return spec;
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
MakeToggleSpec(detail::NodeKind kind, ToggleVisualKind visual_kind, bool checked, std::string label = {}) {
  auto spec = std::make_shared<detail::ViewSpec>(kind);
  spec->text = std::move(label);
  spec->focusable = true;
  spec->activation = [visual_kind, checked](const detail::EventBindings& bindings) {
    if (visual_kind == ToggleVisualKind::RadioButton && checked) {
      return;
    }
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

std::shared_ptr<detail::ViewSpec> MakeProgressBarSpec(std::optional<float> progress) {
  if (progress.has_value()) {
    progress = NormalizeProgress(*progress);
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::ProgressBar);
  spec->retained_modifiers.push_back(detail::MakeModifierSpec(ProgressBarVisual{progress}));
  return spec;
}

std::shared_ptr<detail::ViewSpec> MakeSliderSpec(float value) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Slider);
  spec->focusable = true;
  spec->retained_modifiers.push_back(detail::MakeModifierSpec(SliderVisual{value, 0.0F, 1.0F, std::nullopt}));
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

std::shared_ptr<detail::ViewSpec> MakeTabsSpec(std::vector<TabItem> items, std::size_t selected_index) {
  if (items.empty()) {
    throw std::invalid_argument("HuxerUI Tabs requires at least one item");
  }
  if (selected_index >= items.size()) {
    throw std::invalid_argument("HuxerUI Tabs selected index is out of range");
  }

  std::vector<ResolvedTabItem> resolved_items;
  resolved_items.reserve(items.size());
  for (TabItem& item : items) {
    ResolvedTabItem resolved{
        detail::TabItemAccess::ResolveLabel(item),
        detail::TabItemAccess::ResolveIcon(item),
        detail::TabItemAccess::ShowsLabel(item),
        detail::TabItemAccess::IsEnabled(item),
    };
    if (resolved.label.empty()) {
      throw std::invalid_argument("HuxerUI Tabs item requires a non-empty semantic label");
    }
    if (!resolved.show_label && !resolved.icon.has_value()) {
      throw std::invalid_argument("HuxerUI icon-only Tabs item requires an icon and semantic label");
    }
    resolved_items.push_back(std::move(resolved));
  }

  return MakeScopeSpec([items = std::move(resolved_items), selected_index]() -> View {
    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const ThemeSpec theme = detail::ResolveThemeSpec(environment);
    const TabsStyle style = ResolveStyleOverride<TabsStyle>(environment).value_or(detail::DefaultTabsStyle(theme));
    const ScrollController scroll_controller = UseScrollController();
    const EventEmitter events = UseEvents();

    std::vector<View> labels;
    labels.reserve(items.size());
    std::vector<bool> enabled_items;
    enabled_items.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      const bool enabled = items[index].enabled;
      enabled_items.push_back(enabled);
      labels.push_back(
          std::move(TabLabel(items[index], style, index == selected_index))
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

Chip::Chip(StringResource resource) : Chip(UseString(std::move(resource))) {}

Chip::Chip(std::string label) : detail::TypedView<Chip>(MakeChipSpec(std::move(label), std::nullopt)) {}

Chip::Chip(std::string_view label) : Chip(std::string(label)) {}

Chip::Chip(const char* label) : Chip(label == nullptr ? std::string{} : std::string(label)) {}

Chip::Chip(StringResource resource, bool selected) : Chip(UseString(std::move(resource)), selected) {}

Chip::Chip(std::string label, bool selected)
    : detail::TypedView<Chip>(MakeChipSpec(std::move(label), selected)) {}

Chip::Chip(std::string_view label, bool selected) : Chip(std::string(label), selected) {}

Chip::Chip(const char* label, bool selected)
    : Chip(label == nullptr ? std::string{} : std::string(label), selected) {}

Chip::Chip(ImageResource icon, StringVariant label)
    : detail::TypedView<Chip>(MakeChipSpec(
          detail::ResolveStringVariant(std::move(label)),
          std::nullopt,
          ResolveControlIcon(std::move(icon))
      )) {}

Chip::Chip(ImageAsset icon, StringVariant label)
    : detail::TypedView<Chip>(MakeChipSpec(
          detail::ResolveStringVariant(std::move(label)),
          std::nullopt,
          ResolveControlIcon(std::move(icon))
      )) {}

Chip::Chip(VectorAsset icon, StringVariant label)
    : detail::TypedView<Chip>(MakeChipSpec(
          detail::ResolveStringVariant(std::move(label)),
          std::nullopt,
          ResolveControlIcon(std::move(icon))
      )) {}

Chip::Chip(ImageResource icon, StringVariant label, bool selected)
    : detail::TypedView<Chip>(MakeChipSpec(
          detail::ResolveStringVariant(std::move(label)),
          selected,
          ResolveControlIcon(std::move(icon))
      )) {}

Chip::Chip(ImageAsset icon, StringVariant label, bool selected)
    : detail::TypedView<Chip>(MakeChipSpec(
          detail::ResolveStringVariant(std::move(label)),
          selected,
          ResolveControlIcon(std::move(icon))
      )) {}

Chip::Chip(VectorAsset icon, StringVariant label, bool selected)
    : detail::TypedView<Chip>(MakeChipSpec(
          detail::ResolveStringVariant(std::move(label)),
          selected,
          ResolveControlIcon(std::move(icon))
      )) {}

Divider::Divider(Axis axis) : View(MakeDividerSpec(axis)) {}

SegmentedButtonItem::SegmentedButtonItem(StringVariant label)
    : SegmentedButtonItem(Icon{std::monostate{}}, std::move(label), true) {}

SegmentedButtonItem::SegmentedButtonItem(ImageResource icon, StringVariant label)
    : SegmentedButtonItem(Icon{std::move(icon)}, std::move(label), true) {}

SegmentedButtonItem::SegmentedButtonItem(ImageAsset icon, StringVariant label)
    : SegmentedButtonItem(Icon{std::move(icon)}, std::move(label), true) {}

SegmentedButtonItem::SegmentedButtonItem(VectorAsset icon, StringVariant label)
    : SegmentedButtonItem(Icon{std::move(icon)}, std::move(label), true) {}

SegmentedButtonItem SegmentedButtonItem::IconOnly(ImageResource icon, StringVariant semantic_label) {
  return SegmentedButtonItem(Icon{std::move(icon)}, std::move(semantic_label), false);
}

SegmentedButtonItem SegmentedButtonItem::IconOnly(ImageAsset icon, StringVariant semantic_label) {
  return SegmentedButtonItem(Icon{std::move(icon)}, std::move(semantic_label), false);
}

SegmentedButtonItem SegmentedButtonItem::IconOnly(VectorAsset icon, StringVariant semantic_label) {
  return SegmentedButtonItem(Icon{std::move(icon)}, std::move(semantic_label), false);
}

SegmentedButtonItem::SegmentedButtonItem(Icon icon, StringVariant label, bool show_label)
    : icon_(std::move(icon)), label_(std::move(label)), show_label_(show_label) {}

SegmentedButton::SegmentedButton(std::vector<StringVariant> labels, std::size_t selected_index)
    : detail::TypedView<SegmentedButton>(MakeSegmentedButtonSpec(std::move(labels), selected_index)) {}

SegmentedButton::SegmentedButton(std::vector<SegmentedButtonItem> items, std::size_t selected_index)
    : detail::TypedView<SegmentedButton>(MakeSegmentedButtonSpec(std::move(items), selected_index)) {}

TabItem::TabItem(StringVariant label) : TabItem(Icon{std::monostate{}}, std::move(label), true) {}

TabItem::TabItem(ImageResource icon, StringVariant label) : TabItem(Icon{std::move(icon)}, std::move(label), true) {}

TabItem::TabItem(ImageAsset icon, StringVariant label) : TabItem(Icon{std::move(icon)}, std::move(label), true) {}

TabItem::TabItem(VectorAsset icon, StringVariant label) : TabItem(Icon{std::move(icon)}, std::move(label), true) {}

TabItem TabItem::IconOnly(ImageResource icon, StringVariant semantic_label) {
  return TabItem(Icon{std::move(icon)}, std::move(semantic_label), false);
}

TabItem TabItem::IconOnly(ImageAsset icon, StringVariant semantic_label) {
  return TabItem(Icon{std::move(icon)}, std::move(semantic_label), false);
}

TabItem TabItem::IconOnly(VectorAsset icon, StringVariant semantic_label) {
  return TabItem(Icon{std::move(icon)}, std::move(semantic_label), false);
}

TabItem::TabItem(Icon icon, StringVariant label, bool show_label)
    : icon_(std::move(icon)), label_(std::move(label)), show_label_(show_label) {}

TabItem TabItem::Enabled(bool enabled) && {
  enabled_ = enabled;
  return std::move(*this);
}

Tabs::Tabs(std::vector<StringVariant> labels, std::size_t selected_index)
    : detail::TypedView<Tabs>(MakeTabsSpec(std::move(labels), selected_index)) {}

Tabs::Tabs(std::vector<TabItem> items, std::size_t selected_index)
    : detail::TypedView<Tabs>(MakeTabsSpec(std::move(items), selected_index)) {}

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

Checkbox::Checkbox(StringVariant label, bool checked)
    : detail::TypedView<Checkbox>(MakeToggleSpec(
          detail::NodeKind::Checkbox,
          ToggleVisualKind::Checkbox,
          checked,
          detail::ResolveStringVariant(std::move(label))
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
          detail::ResolveStringVariant(std::move(label))
      )) {}

Switch::Switch(bool checked)
    : detail::TypedView<Switch>(MakeToggleSpec(detail::NodeKind::Switch, ToggleVisualKind::Switch, checked)) {}

Switch::Switch(StringVariant label, bool checked)
    : detail::TypedView<Switch>(MakeToggleSpec(
          detail::NodeKind::Switch,
          ToggleVisualKind::Switch,
          checked,
          detail::ResolveStringVariant(std::move(label))
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
