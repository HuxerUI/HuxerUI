#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/semantics.h>
#include <huxerui/animation.h>
#include <huxerui/theme.h>

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui {

namespace {

using detail::MakeContainerSpec;

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
          EmitEvent<PagerEvents::Changed>(proposal);
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
      detail::InternalAccess::RequestFrame(*mounted.runtime);
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
    EmitEvent<PagerEvents::Changed>(*target);
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
  }

  void UpdateAllowedSources(detail::MountedNode& node) const {
    const bool allows_drag = behavior_.drag_enabled && (mode_ == Mode::Stable || mode_ == Mode::Dragging);
    node.scroll_state->allowed_sources = allows_drag ? DragSourceMask() : 0U;
  }

  void InvalidateLayout(detail::MountedNode& node) {
    if (node.runtime) {
      detail::InternalAccess::InvalidateLayout(*node.runtime, node);
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

} // namespace huxerui
