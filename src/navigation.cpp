#include <huxerui/navigation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <huxerui/environment.h>
#include <huxerui/modifier.h>
#include <huxerui/state.h>
#include <huxerui/theme.h>

#include "geometry_internal.h"
#include "internal.h"

namespace huxerui::detail {

namespace {

enum class NavigationOperationKind {
  Push,
  Pop,
  Replace,
};

struct NavigationEntry {
  std::uint64_t id = 0;
  std::function<View()> factory;
};

struct NavigationOperation {
  std::uint64_t id = 0;
  NavigationOperationKind kind = NavigationOperationKind::Pop;
  std::function<View()> factory;
  bool ready_to_start = true;
};

struct NavigationTransition {
  NavigationOperationKind kind = NavigationOperationKind::Pop;
  std::uint64_t source_id = 0;
  std::uint64_t destination_id = 0;
  float progress = 0.0F;
  float target = 1.0F;
  bool interactive = false;
  bool complete_operation = true;
};

struct NavigationEnvironment {
  std::weak_ptr<NavigationState> state;

  static NavigationEnvironment Default() {
    return {};
  }
};

struct NavigationStateValue {
  using Value = std::shared_ptr<NavigationState>;
};

struct NavigationEntryIdValue {
  using Value = std::uint64_t;
};

float Interpolate(float from, float to, float progress) noexcept {
  return from + (to - from) * std::clamp(progress, 0.0F, 1.0F);
}

Point ResolveOffset(Point fraction, Rect bounds) noexcept {
  return {fraction.x * bounds.width, fraction.y * bounds.height};
}

Point Scale(Point value, float factor) noexcept {
  return {value.x * factor, value.y * factor};
}

AnimationSpec ContinuationAnimation(AnimationSpec animation, float from, float to) {
  const float remaining = std::clamp(std::abs(to - from), 0.0F, 1.0F);
  if (remaining <= 0.0F) {
    return SnapSpec{};
  }
  if (auto* tween = std::get_if<TweenSpec>(&animation)) {
    tween->duration *= remaining;
  } else if (const auto* keyframes = std::get_if<KeyframeSpec>(&animation)) {
    animation = KeyframeSpec(keyframes->Duration() * remaining, keyframes->Keyframes());
  }
  return animation;
}

} // namespace

class NavigationState {
public:
  explicit NavigationState(std::function<View()> root)
      : owner_thread_(std::this_thread::get_id()), entries_{{1, std::move(root)}} {}

  void Connect(std::function<void()> invalidate) {
    CheckThread();
    invalidate_ = std::move(invalidate);
  }

  void UpdateRoot(std::function<View()> root) {
    CheckThread();
    const auto found = std::ranges::find(entries_, initial_root_id_, &NavigationEntry::id);
    if (found != entries_.end()) {
      found->factory = std::move(root);
    }
  }

  void UpdateStyle(NavigationStyle style) {
    CheckThread();
    style_ = std::move(style);
  }

  void Push(std::function<View()> page) {
    CheckThread();
    ValidateFactory(page);
    ++logical_depth_;
    EnqueueOperation(NavigationOperationKind::Push, std::move(page));
    StartNextOperation();
    Invalidate();
  }

  bool Pop() {
    CheckThread();
    if (logical_depth_ <= 1) {
      return false;
    }
    // Logical history changes when the command is accepted; render entries leave after their queued transition.
    --logical_depth_;
    EnqueueOperation(NavigationOperationKind::Pop, {});
    StartNextOperation();
    Invalidate();
    return true;
  }

  void Replace(std::function<View()> page) {
    CheckThread();
    ValidateFactory(page);
    EnqueueOperation(NavigationOperationKind::Replace, std::move(page));
    StartNextOperation();
    Invalidate();
  }

  [[nodiscard]] bool CanPop() const {
    CheckThread();
    return logical_depth_ > 1;
  }

  [[nodiscard]] std::size_t Depth() const {
    CheckThread();
    return logical_depth_;
  }

  [[nodiscard]] const std::vector<NavigationEntry>& Entries() const noexcept {
    return entries_;
  }

  [[nodiscard]] const std::optional<NavigationTransition>& Transition() const noexcept {
    return transition_;
  }

  [[nodiscard]] const NavigationStyle& Style() const noexcept {
    return style_;
  }

  [[nodiscard]] std::uint64_t Revision() const noexcept {
    return revision_;
  }

  [[nodiscard]] std::uint64_t ActiveEntryId() const noexcept {
    if (!transition_.has_value()) {
      return entries_.empty() ? 0 : entries_.back().id;
    }
    if (transition_->interactive || !transition_->complete_operation) {
      return transition_->source_id;
    }
    return transition_->destination_id;
  }

  [[nodiscard]] bool BeginPredictivePop() {
    CheckThread();
    if (logical_depth_ <= 1) {
      return false;
    }
    // Reserve the logical Pop at Begin so later commands keep their order even before the gesture commits.
    --logical_depth_;
    if (transition_.has_value() || !pending_.empty()) {
      queued_predictive_pop_id_ = EnqueueOperation(NavigationOperationKind::Pop, {}, false);
      Invalidate();
      return true;
    }
    if (entries_.size() <= 1) {
      ++logical_depth_;
      return false;
    }
    transition_ = NavigationTransition{
        .kind = NavigationOperationKind::Pop,
        .source_id = entries_.back().id,
        .destination_id = entries_[entries_.size() - 2].id,
        .progress = 0.0F,
        .target = 0.0F,
        .interactive = true,
        .complete_operation = false,
    };
    ++revision_;
    Invalidate();
    return true;
  }

  [[nodiscard]] bool UpdatePredictivePop(float progress) {
    CheckThread();
    if (queued_predictive_pop_id_.has_value()) {
      return true;
    }
    if (!transition_.has_value() || !transition_->interactive) {
      return false;
    }
    transition_->progress = std::clamp(progress, 0.0F, 1.0F);
    return true;
  }

  [[nodiscard]] bool CancelPredictivePop() {
    CheckThread();
    if (queued_predictive_pop_id_.has_value()) {
      const auto operation = std::ranges::find(pending_, *queued_predictive_pop_id_, &NavigationOperation::id);
      if (operation == pending_.end()) {
        queued_predictive_pop_id_.reset();
        return false;
      }
      pending_.erase(operation);
      queued_predictive_pop_id_.reset();
      ++logical_depth_;
      StartNextOperation();
      Invalidate();
      return true;
    }
    if (!transition_.has_value() || !transition_->interactive) {
      return false;
    }
    ++logical_depth_;
    transition_->interactive = false;
    transition_->target = 0.0F;
    transition_->complete_operation = false;
    ++revision_;
    return true;
  }

  [[nodiscard]] bool CommitPredictivePop() {
    CheckThread();
    if (queued_predictive_pop_id_.has_value()) {
      const auto operation = std::ranges::find(pending_, *queued_predictive_pop_id_, &NavigationOperation::id);
      if (operation == pending_.end()) {
        queued_predictive_pop_id_.reset();
        return false;
      }
      operation->ready_to_start = true;
      queued_predictive_pop_id_.reset();
      StartNextOperation();
      Invalidate();
      return true;
    }
    if (!transition_.has_value() || !transition_->interactive) {
      return Pop();
    }
    transition_->interactive = false;
    transition_->target = 1.0F;
    transition_->complete_operation = true;
    ++revision_;
    Invalidate();
    return true;
  }

  void SetTransitionProgress(float progress) noexcept {
    if (transition_.has_value()) {
      transition_->progress = std::clamp(progress, 0.0F, 1.0F);
    }
  }

  void FinishTransition() {
    CheckThread();
    if (!transition_.has_value()) {
      return;
    }
    const NavigationTransition completed = *transition_;
    transition_.reset();
    if (completed.complete_operation) {
      if (completed.kind == NavigationOperationKind::Pop) {
        EraseEntry(completed.source_id);
      } else if (completed.kind == NavigationOperationKind::Replace) {
        EraseEntry(completed.source_id);
      }
    }
    ++revision_;
    StartNextOperation();
    Invalidate();
  }

private:
  static void ValidateFactory(const std::function<View()>& factory) {
    if (!factory) {
      throw std::invalid_argument("HuxerUI navigation page factory must not be empty");
    }
  }

  void CheckThread() const {
    if (std::this_thread::get_id() != owner_thread_) {
      throw std::logic_error("HuxerUI navigation must be accessed on its UI thread");
    }
  }

  void Invalidate() {
    if (invalidate_) {
      invalidate_();
    }
  }

  std::uint64_t
  EnqueueOperation(NavigationOperationKind kind, std::function<View()> factory, bool ready_to_start = true) {
    const std::uint64_t id = next_operation_id_++;
    pending_.push_back({id, kind, std::move(factory), ready_to_start});
    return id;
  }

  void StartNextOperation() {
    if (transition_.has_value() || pending_.empty() || !pending_.front().ready_to_start) {
      return;
    }
    NavigationOperation operation = std::move(pending_.front());
    pending_.pop_front();
    const std::uint64_t source_id = entries_.back().id;
    std::uint64_t destination_id = 0;
    if (operation.kind == NavigationOperationKind::Push || operation.kind == NavigationOperationKind::Replace) {
      destination_id = next_entry_id_++;
      entries_.push_back({destination_id, std::move(operation.factory)});
    } else {
      destination_id = entries_[entries_.size() - 2].id;
    }
    transition_ = NavigationTransition{
        .kind = operation.kind,
        .source_id = source_id,
        .destination_id = destination_id,
        .progress = 0.0F,
        .target = 1.0F,
        .interactive = false,
        .complete_operation = true,
    };
    ++revision_;
  }

  void EraseEntry(std::uint64_t id) {
    const auto found = std::ranges::find(entries_, id, &NavigationEntry::id);
    if (found != entries_.end()) {
      entries_.erase(found);
    }
  }

  const std::thread::id owner_thread_;
  const std::uint64_t initial_root_id_ = 1;
  std::vector<NavigationEntry> entries_;
  std::deque<NavigationOperation> pending_;
  std::optional<NavigationTransition> transition_;
  NavigationStyle style_;
  std::function<void()> invalidate_;
  std::size_t logical_depth_ = 1;
  std::uint64_t next_entry_id_ = 2;
  std::uint64_t next_operation_id_ = 1;
  std::uint64_t revision_ = 1;
  std::optional<std::uint64_t> queued_predictive_pop_id_;
};

namespace {

class NavigationStackLayout final : public Layout<NavigationStackLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    LayoutResult result;
    const auto* state = node.LayoutValue<NavigationStateValue>();
    if (state == nullptr || !*state) {
      return result.SetSize(constraints.Constrain({}));
    }

    std::uint64_t source_id = 0;
    std::uint64_t destination_id = (*state)->ActiveEntryId();
    if (const auto& transition = (*state)->Transition()) {
      source_id = transition->source_id;
      destination_id = transition->destination_id;
    }

    Constraints child_constraints = constraints;
    if (constraints.HasBoundedWidth()) {
      child_constraints = child_constraints.TightWidth(constraints.max_width);
    }
    if (constraints.HasBoundedHeight()) {
      child_constraints = child_constraints.TightHeight(constraints.max_height);
    }

    Size measured;
    for (huxerui::MountedNode& child : node.Children()) {
      const std::uint64_t id = child.LayoutValueOr<NavigationEntryIdValue>(0);
      if (id != source_id && id != destination_id) {
        continue;
      }
      const Size child_size = context.Measure(child, child_constraints);
      measured.width = std::max(measured.width, child_size.width);
      measured.height = std::max(measured.height, child_size.height);
      result.Place(child, {});
    }
    if (constraints.HasBoundedWidth()) {
      measured.width = constraints.max_width;
    }
    if (constraints.HasBoundedHeight()) {
      measured.height = constraints.max_height;
    }
    return result.SetSize(constraints.Constrain(measured));
  }
};

struct NavigationPageModifier {
  std::shared_ptr<NavigationState> state;
  std::uint64_t entry_id = 0;

  static const ModifierDescriptor& Descriptor();

  bool operator==(const NavigationPageModifier&) const = default;
};

class NavigationPageExtension final : public NodeExtension {
public:
  NavigationPageExtension(huxerui::MountedNode& node, const NavigationPageModifier& modifier) {
    Update(node, modifier);
  }

  void Update(huxerui::MountedNode& node, const NavigationPageModifier& modifier) {
    static_cast<void>(node);
    state_ = modifier.state;
    entry_id_ = modifier.entry_id;
  }

  FrameResult OnFrame(huxerui::MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(frame);
    auto& mounted = static_cast<detail::MountedNode&>(node);
    float opacity = 0.0F;
    Point offset;
    float scale = 1.0F;
    const Rect bounds = node.Bounds();

    if (!state_) {
      mounted.presentation.local_opacity = 0.0F;
      return {};
    }
    const auto& transition = state_->Transition();
    if (!transition.has_value()) {
      opacity = entry_id_ == state_->ActiveEntryId() ? 1.0F : 0.0F;
    } else {
      const float progress = transition->progress;
      const NavigationMotion motion = state_->Style().motion.value_or(NavigationMotion{});
      const Point entering_offset = ResolveOffset(motion.entering_offset_fraction, bounds);
      const Point covered_offset = ResolveOffset(motion.covered_offset_fraction, bounds);
      if (transition->kind == NavigationOperationKind::Push || transition->kind == NavigationOperationKind::Replace) {
        if (entry_id_ == transition->source_id) {
          offset = Scale(covered_offset, progress);
          scale = Interpolate(1.0F, motion.covered_scale, progress);
          opacity = Interpolate(1.0F, motion.covered_opacity, progress);
        } else if (entry_id_ == transition->destination_id) {
          offset = Scale(entering_offset, 1.0F - progress);
          scale = Interpolate(motion.entering_scale, 1.0F, progress);
          opacity = Interpolate(motion.entering_opacity, 1.0F, progress);
        }
      } else {
        if (entry_id_ == transition->source_id) {
          offset = Scale(entering_offset, progress);
          scale = Interpolate(1.0F, motion.entering_scale, progress);
          opacity = Interpolate(1.0F, motion.entering_opacity, progress);
        } else if (entry_id_ == transition->destination_id) {
          offset = Scale(covered_offset, 1.0F - progress);
          scale = Interpolate(motion.covered_scale, 1.0F, progress);
          opacity = Interpolate(motion.covered_opacity, 1.0F, progress);
        }
      }
    }

    if (scale != 1.0F) {
      const Point origin{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
      const Transform2D scale_transform{scale, 0.0F, 0.0F, scale};
      mounted.presentation.local_transform =
          ComposeTransform(AroundOriginTransform(scale_transform, origin), mounted.presentation.local_transform);
    }
    mounted.presentation.local_transform =
        ComposeTransform(TranslationTransform(offset), mounted.presentation.local_transform);
    mounted.presentation.local_opacity *= opacity;
    return {};
  }

private:
  std::shared_ptr<NavigationState> state_;
  std::uint64_t entry_id_ = 0;
};

const ModifierDescriptor& NavigationPageModifier::Descriptor() {
  static const ModifierDescriptor descriptor{
      [](ViewSpec& spec, const void* value) {
        const auto& modifier = *static_cast<const NavigationPageModifier*>(value);
        spec.local_enabled = modifier.state && modifier.state->ActiveEntryId() == modifier.entry_id;
        // Covered pages are disabled for interaction, not visually styled as disabled controls.
        spec.properties.disabled_opacity = 1.0F;
      },
      [](huxerui::MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<NavigationPageExtension>(node, *static_cast<const NavigationPageModifier*>(value));
      },
      [](NodeExtension& extension, huxerui::MountedNode& node, const void* value) {
        static_cast<NavigationPageExtension&>(extension).Update(
            node,
            *static_cast<const NavigationPageModifier*>(value)
        );
      },
      false,
      ErasedEqualsFor<NavigationPageModifier>(),
      nullptr,
  };
  return descriptor;
}

struct NavigationContainerModifier {
  std::shared_ptr<NavigationState> state;
  NavigationStyle style;
  std::uint64_t revision = 0;

  static const ModifierDescriptor& Descriptor();

  bool operator==(const NavigationContainerModifier&) const = default;
};

class NavigationContainerExtension final : public NodeExtension {
public:
  NavigationContainerExtension(huxerui::MountedNode& node, const NavigationContainerModifier& modifier) {
    Update(node, modifier);
  }

  void Update(huxerui::MountedNode& node, const NavigationContainerModifier& modifier) {
    static_cast<void>(node);
    state_ = modifier.state;
    style_ = modifier.style;
    if (revision_ != modifier.revision) {
      revision_ = modifier.revision;
      animation_initialized_ = false;
    }
  }

  FrameResult OnFrame(huxerui::MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    if (!state_ || !state_->Transition().has_value()) {
      animation_initialized_ = false;
      return {};
    }
    const NavigationTransition& transition = *state_->Transition();
    if (transition.interactive) {
      progress_.Set(transition.progress);
      animation_initialized_ = false;
      return {};
    }
    if (!animation_initialized_) {
      progress_.Set(transition.progress);
      const NavigationMotion motion = style_.motion.value_or(NavigationMotion{});
      AnimationSpec animation = transition.kind == NavigationOperationKind::Pop ? motion.pop : motion.push;
      animation = ContinuationAnimation(std::move(animation), transition.progress, transition.target);
      progress_.AnimateTo(transition.target, style_.motion.has_value() ? animation : AnimationSpec{SnapSpec{}});
      animation_initialized_ = true;
    }

    const MotionAdvanceResult result = progress_.Advance(frame);
    state_->SetTransitionProgress(progress_.Value());
    if (!result.needs_frame && !result.wake_after.has_value() && progress_.Value() == transition.target) {
      state_->FinishTransition();
      animation_initialized_ = false;
      return {true, std::nullopt};
    }
    return {result.needs_frame, result.wake_after};
  }

  bool OnBack(huxerui::MountedNode& node, const BackEvent& event) override {
    static_cast<void>(node);
    if (!state_) {
      return false;
    }
    switch (event.phase) {
    case BackPhase::Begin:
      animation_initialized_ = false;
      return state_->BeginPredictivePop();
    case BackPhase::Update:
      return state_->UpdatePredictivePop(event.progress);
    case BackPhase::Cancel:
      animation_initialized_ = false;
      return state_->CancelPredictivePop();
    case BackPhase::Commit:
      animation_initialized_ = false;
      return state_->CommitPredictivePop();
    }
    return false;
  }

private:
  std::shared_ptr<NavigationState> state_;
  NavigationStyle style_;
  MotionController progress_;
  std::uint64_t revision_ = 0;
  bool animation_initialized_ = false;
};

const ModifierDescriptor& NavigationContainerModifier::Descriptor() {
  return ModifierDescriptorFor<NavigationContainerModifier, NavigationContainerExtension, true>();
}

} // namespace

} // namespace huxerui::detail

namespace huxerui {

NavigationStyle NavigationStyle::Default() {
  return {};
}

void NavigationController::Push(std::function<View()> page) const {
  if (auto state = state_.lock()) {
    state->Push(std::move(page));
    return;
  }
  throw std::logic_error("HuxerUI navigation controller is disconnected");
}

bool NavigationController::Pop() const {
  if (auto state = state_.lock()) {
    return state->Pop();
  }
  return false;
}

void NavigationController::Replace(std::function<View()> page) const {
  if (auto state = state_.lock()) {
    state->Replace(std::move(page));
    return;
  }
  throw std::logic_error("HuxerUI navigation controller is disconnected");
}

bool NavigationController::CanPop() const {
  if (auto state = state_.lock()) {
    return state->CanPop();
  }
  return false;
}

std::size_t NavigationController::Depth() const {
  if (auto state = state_.lock()) {
    return state->Depth();
  }
  return 0;
}

View NavigationStack(std::function<View()> root) {
  if (!root) {
    throw std::invalid_argument("HuxerUI navigation root factory must not be empty");
  }
  return Scope([root = std::move(root)]() mutable -> View {
    auto state_holder = UseState(std::make_shared<detail::NavigationState>(root));
    auto revision = UseState<std::uint64_t>(0);
    const std::shared_ptr<detail::NavigationState> state = state_holder.Get();
    static_cast<void>(revision.Get());
    state->Connect([revision] { revision.Update([](std::uint64_t& value) { ++value; }); });
    state->UpdateRoot(root);
    const NavigationStyle style = UseEnvironment<NavigationStyle>();
    state->UpdateStyle(style);

    std::vector<View> pages;
    pages.reserve(state->Entries().size());
    for (const detail::NavigationEntry& entry : state->Entries()) {
      View page =
          ProvideEnvironment(detail::NavigationEnvironment{state}, [factory = entry.factory] { return factory(); });
      pages.push_back(
          Stack {
            std::move(page),
          }.LayoutValue<detail::NavigationEntryIdValue>(entry.id)
              .With(detail::NavigationPageModifier{state, entry.id})
              .Key(entry.id)
      );
    }

    detail::NavigationContainerModifier container{state, style, state->Revision()};
    View stack = detail::NavigationStackLayout {std::move(pages)}
                     .LayoutValue<detail::NavigationStateValue>(state)
                     .With(ClipChildren{}, std::move(container));
    return stack;
  });
}

NavigationController UseNavigation() {
  const std::weak_ptr<detail::NavigationState> state = UseEnvironment<detail::NavigationEnvironment>().state;
  if (state.expired()) {
    throw std::logic_error("HuxerUI UseNavigation() requires an enclosing NavigationStack");
  }
  return NavigationController{state};
}

} // namespace huxerui
