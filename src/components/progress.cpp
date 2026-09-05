#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "runtime/mounted_node_internal.h"
#include "resources/resource_internal.h"

namespace huxerui {

namespace {

using detail::ResolveStyleOverride;
using detail::LoopingPhase;
using detail::PaintProgressCircle;

struct ProgressCircleStyleBinding {
  using Value = ProgressCircleStyle;
};

struct ProgressBarStyleBinding {
  using Value = ProgressBarStyle;
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

struct ProgressCircleVisual {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<float> progress;

  bool operator==(const ProgressCircleVisual&) const = default;
};

} // namespace

namespace detail {

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

} // namespace detail

namespace {

class ProgressCircleVisualExtension final : public NodeExtension {
public:
  ProgressCircleVisualExtension(ViewNode& node, const ProgressCircleVisual& modifier) {
    Update(node, modifier);
  }

  void Update(ViewNode& node, const ProgressCircleVisual& modifier) {
    style_ = node.LayoutValueOr<ProgressCircleStyleBinding>(ProgressCircleStyle::Default());
    if (progress_ != modifier.progress) {
      progress_ = modifier.progress;
      phase_.Reset();
    }
  }

  NodeExtension::FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
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

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
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
  ProgressBarVisualExtension(ViewNode& node, const ProgressBarVisual& modifier) {
    Update(node, modifier);
  }

  void Update(ViewNode& node, const ProgressBarVisual& modifier) {
    style_ = node.LayoutValueOr<ProgressBarStyleBinding>(ProgressBarStyle::Default());
    if (progress_ != modifier.progress) {
      progress_ = modifier.progress;
      phase_.Reset();
    }
  }

  NodeExtension::FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
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

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
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

} // namespace

ProgressCircle::ProgressCircle() : detail::TypedView<ProgressCircle>(MakeProgressCircleSpec(std::nullopt)) {}

ProgressCircle::ProgressCircle(float progress) : detail::TypedView<ProgressCircle>(MakeProgressCircleSpec(progress)) {}

ProgressBar::ProgressBar() : detail::TypedView<ProgressBar>(MakeProgressBarSpec(std::nullopt)) {}

ProgressBar::ProgressBar(float progress) : detail::TypedView<ProgressBar>(MakeProgressBarSpec(progress)) {}

} // namespace huxerui
