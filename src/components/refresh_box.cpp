#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/semantics.h>
#include <huxerui/animation.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "internal_access.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui {

namespace {

using detail::ResolveStyleOverride;
using detail::LoopingPhase;
using detail::PaintProgressCircle;

struct RefreshBoxBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  bool refreshing = false;
  RefreshBoxStyle style{};

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
  RefreshBoxBehaviorExtension(ViewNode& node, const RefreshBoxBehavior& behavior) {
    Update(node, behavior);
  }

  void Update(ViewNode& node, const RefreshBoxBehavior& behavior) {
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

  FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
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

  void OnScrollActivity(ViewNode& node, const ScrollActivity& activity) override {
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

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
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
      detail::InternalAccess::RequestFrame(*node.runtime);
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
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
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
      [](ViewNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<RefreshBoxBehaviorExtension>(
            node,
            *static_cast<const RefreshBoxBehavior*>(value)
        );
      },
      [](NodeExtension& extension, ViewNode& node, const void* value) {
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

} // namespace

RefreshBox::RefreshBox(View content, bool refreshing)
    : detail::TypedView<RefreshBox>(MakeRefreshBoxSpec(std::move(content), refreshing)) {}

} // namespace huxerui
