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

#include "graphics/geometry_internal.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui::detail {

namespace {

enum class NavigationOperationKind {
  Push,
  Pop,
  Replace,
  SetPath,
};

struct NavigationEntry {
  std::uint64_t id = 0;
  std::function<View()> factory;
};

struct RealizedNavigationRoute {
  NavigationRouteDescriptor descriptor;
  std::uint64_t entry_id = 0;
};

struct NavigationOperation {
  std::uint64_t id = 0;
  NavigationOperationKind kind = NavigationOperationKind::Pop;
  std::function<View()> factory;
  std::vector<NavigationRouteDescriptor> route_path;
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
  std::vector<std::uint64_t> retire_ids;
};

struct NavigationEnvironmentEntry {
  std::weak_ptr<NavigationState> state;
  std::optional<std::type_index> route_type;
  std::shared_ptr<void> path_state;
  std::shared_ptr<void> history_commit;
};

struct NavigationEnvironmentNode {
  NavigationEnvironmentEntry entry;
  std::shared_ptr<const NavigationEnvironmentNode> parent;
};

struct NavigationEnvironment {
  std::shared_ptr<const NavigationEnvironmentNode> current;

  static NavigationEnvironment Default() {
    return {};
  }

  bool operator==(const NavigationEnvironment&) const = default;
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

  NavigationState(
      std::function<View()> root, std::vector<NavigationRouteDescriptor> routes, NavigationRouteBinding binding
  )
      : owner_thread_(std::this_thread::get_id()), entries_{{1, std::move(root)}}, requested_routes_(std::move(routes)),
        request_route_pop_(std::move(binding.request_pop)), route_type_(binding.route_type) {
    ValidateRoutePath(requested_routes_);
    realized_routes_.reserve(requested_routes_.size());
    for (const NavigationRouteDescriptor& route : requested_routes_) {
      const std::uint64_t id = next_entry_id_++;
      entries_.push_back({id, route.factory});
      realized_routes_.push_back({route, id});
    }
  }

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

  void UpdateRouteBinding(NavigationRouteBinding binding) {
    CheckThread();
    if (!route_type_.has_value()) {
      throw std::logic_error("HuxerUI factory navigation cannot accept a route binding");
    }
    if (binding.route_type != *route_type_) {
      throw std::logic_error("HuxerUI routed NavigationStack route type changed without replacing the stack");
    }
    request_route_pop_ = std::move(binding.request_pop);
  }

  void SynchronizeRoutePath(std::vector<NavigationRouteDescriptor> routes) {
    CheckThread();
    if (!route_type_.has_value()) {
      throw std::logic_error("HuxerUI factory navigation cannot synchronize a route path");
    }
    ValidateRoutePath(routes);

    if (expected_route_pop_) {
      if (IsSinglePop(requested_routes_, routes) && transition_.has_value() &&
          transition_->kind == NavigationOperationKind::Pop) {
        requested_routes_ = std::move(routes);
        if (!realized_routes_.empty()) {
          realized_routes_.pop_back();
        }
        expected_route_pop_ = false;
        RefreshRealizedRouteFactories(requested_routes_);
        return;
      }
      expected_route_pop_ = false;
    }

    if (SameRoutePath(requested_routes_, routes)) {
      requested_routes_ = std::move(routes);
      RefreshRealizedRouteFactories(requested_routes_);
      return;
    }

    requested_routes_ = std::move(routes);
    std::erase_if(pending_, [](const NavigationOperation& operation) {
      return operation.kind == NavigationOperationKind::SetPath;
    });
    if (!SameRealizedRoutePath(realized_routes_, requested_routes_)) {
      EnqueueOperation(NavigationOperationKind::SetPath, {}, requested_routes_);
    }
    RefreshRealizedRouteFactories(requested_routes_);
    StartNextOperation();
  }

  void Push(std::function<View()> page) {
    CheckThread();
    EnsureFactoryMode();
    ValidateFactory(page);
    ++factory_depth_;
    EnqueueOperation(NavigationOperationKind::Push, std::move(page), {});
    StartNextOperation();
    Invalidate();
  }

  bool Pop() {
    CheckThread();
    EnsureFactoryMode();
    if (factory_depth_ <= 1) {
      return false;
    }
    // Logical history changes when the command is accepted; render entries leave after their queued transition.
    --factory_depth_;
    EnqueueOperation(NavigationOperationKind::Pop, {}, {});
    StartNextOperation();
    Invalidate();
    return true;
  }

  void Replace(std::function<View()> page) {
    CheckThread();
    EnsureFactoryMode();
    ValidateFactory(page);
    EnqueueOperation(NavigationOperationKind::Replace, std::move(page), {});
    StartNextOperation();
    Invalidate();
  }

  [[nodiscard]] bool CanPop() const {
    CheckThread();
    return factory_depth_ > 1;
  }

  [[nodiscard]] std::size_t Depth() const {
    CheckThread();
    return factory_depth_;
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
    if (route_type_.has_value()) {
      return BeginRoutedPredictivePop();
    }
    if (factory_depth_ <= 1) {
      return false;
    }
    // Reserve the logical Pop at Begin so later commands keep their order even before the gesture commits.
    --factory_depth_;
    if (transition_.has_value() || !pending_.empty()) {
      queued_predictive_pop_id_ = EnqueueOperation(NavigationOperationKind::Pop, {}, {}, false);
      Invalidate();
      return true;
    }
    if (entries_.size() <= 1) {
      ++factory_depth_;
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
        .retire_ids = {entries_.back().id},
    };
    ++revision_;
    Invalidate();
    return true;
  }

  [[nodiscard]] bool UpdatePredictivePop(float progress) {
    CheckThread();
    if (route_type_.has_value() && queued_routed_predictive_pop_) {
      return true;
    }
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
    if (route_type_.has_value()) {
      if (queued_routed_predictive_pop_) {
        queued_routed_predictive_pop_ = false;
        return true;
      }
      if (!transition_.has_value() || !transition_->interactive) {
        return false;
      }
      transition_->interactive = false;
      transition_->target = 0.0F;
      transition_->complete_operation = false;
      ++revision_;
      return true;
    }
    if (queued_predictive_pop_id_.has_value()) {
      const auto operation = std::ranges::find(pending_, *queued_predictive_pop_id_, &NavigationOperation::id);
      if (operation == pending_.end()) {
        queued_predictive_pop_id_.reset();
        return false;
      }
      pending_.erase(operation);
      queued_predictive_pop_id_.reset();
      ++factory_depth_;
      StartNextOperation();
      Invalidate();
      return true;
    }
    if (!transition_.has_value() || !transition_->interactive) {
      return false;
    }
    ++factory_depth_;
    transition_->interactive = false;
    transition_->target = 0.0F;
    transition_->complete_operation = false;
    ++revision_;
    return true;
  }

  [[nodiscard]] bool CommitPredictivePop() {
    CheckThread();
    if (route_type_.has_value()) {
      if (queued_routed_predictive_pop_) {
        queued_routed_predictive_pop_ = false;
        return RequestRoutedPop();
      }
      if (!transition_.has_value() || !transition_->interactive) {
        return RequestRoutedPop();
      }
      expected_route_pop_ = true;
      if (!RequestRoutedPop()) {
        expected_route_pop_ = false;
        return false;
      }
      transition_->interactive = false;
      transition_->target = 1.0F;
      transition_->complete_operation = true;
      ++revision_;
      Invalidate();
      return true;
    }
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
      for (const std::uint64_t id : completed.retire_ids) {
        EraseEntry(id);
      }
    }
    ++revision_;
    StartNextOperation();
    Invalidate();
  }

  void CheckAccess() const {
    CheckThread();
  }

private:
  static void ValidateFactory(const std::function<View()>& factory) {
    if (!factory) {
      throw std::invalid_argument("HuxerUI navigation page factory must not be empty");
    }
  }

  static void ValidateRoutePath(const std::vector<NavigationRouteDescriptor>& routes) {
    for (const NavigationRouteDescriptor& route : routes) {
      if (!route.value || route.equals == nullptr || !route.factory) {
        throw std::invalid_argument("HuxerUI navigation route descriptor is incomplete");
      }
    }
  }

  static bool SameRoute(const NavigationRouteDescriptor& first, const NavigationRouteDescriptor& second) {
    return first.equals != nullptr && second.equals != nullptr && first.equals(first.value.get(), second.value.get());
  }

  static bool SameRoutePath(
      const std::vector<NavigationRouteDescriptor>& first, const std::vector<NavigationRouteDescriptor>& second
  ) {
    return first.size() == second.size() && std::ranges::equal(first, second, [](const auto& left, const auto& right) {
             return SameRoute(left, right);
           });
  }

  static bool SameRealizedRoutePath(
      const std::vector<RealizedNavigationRoute>& first, const std::vector<NavigationRouteDescriptor>& second
  ) {
    return first.size() == second.size() &&
           std::ranges::equal(first, second, [](const RealizedNavigationRoute& left, const auto& right) {
             return SameRoute(left.descriptor, right);
           });
  }

  static bool IsSinglePop(
      const std::vector<NavigationRouteDescriptor>& first, const std::vector<NavigationRouteDescriptor>& second
  ) {
    return first.size() == second.size() + 1 &&
           std::ranges::equal(
               first.begin(),
               first.begin() + static_cast<std::ptrdiff_t>(second.size()),
               second.begin(),
               second.end(),
               [](const auto& left, const auto& right) { return SameRoute(left, right); }
           );
  }

  void EnsureFactoryMode() const {
    if (route_type_.has_value()) {
      throw std::logic_error("HuxerUI routed navigation cannot accept a page factory operation");
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

  bool BeginRoutedPredictivePop() {
    if (requested_routes_.empty()) {
      return false;
    }
    if (transition_.has_value() || !pending_.empty()) {
      queued_routed_predictive_pop_ = true;
      return true;
    }
    if (realized_routes_.empty()) {
      return false;
    }
    const std::uint64_t source_id = realized_routes_.back().entry_id;
    const std::uint64_t destination_id =
        realized_routes_.size() > 1 ? realized_routes_[realized_routes_.size() - 2].entry_id : initial_root_id_;
    transition_ = NavigationTransition{
        .kind = NavigationOperationKind::Pop,
        .source_id = source_id,
        .destination_id = destination_id,
        .progress = 0.0F,
        .target = 0.0F,
        .interactive = true,
        .complete_operation = false,
        .retire_ids = {source_id},
    };
    ++revision_;
    Invalidate();
    return true;
  }

  bool RequestRoutedPop() {
    if (!request_route_pop_ || requested_routes_.empty()) {
      return false;
    }
    return request_route_pop_();
  }

  void RefreshRealizedRouteFactories(const std::vector<NavigationRouteDescriptor>& routes) {
    const std::size_t shared = std::min(realized_routes_.size(), routes.size());
    for (std::size_t index = 0; index < shared; ++index) {
      if (!SameRoute(realized_routes_[index].descriptor, routes[index])) {
        break;
      }
      realized_routes_[index].descriptor = routes[index];
      const auto entry = std::ranges::find(entries_, realized_routes_[index].entry_id, &NavigationEntry::id);
      if (entry != entries_.end()) {
        entry->factory = routes[index].factory;
      }
    }
  }

  std::uint64_t EnqueueOperation(
      NavigationOperationKind kind,
      std::function<View()> factory,
      std::vector<NavigationRouteDescriptor> route_path,
      bool ready_to_start = true
  ) {
    const std::uint64_t id = next_operation_id_++;
    pending_.push_back({id, kind, std::move(factory), std::move(route_path), ready_to_start});
    return id;
  }

  void StartNextOperation() {
    while (!transition_.has_value() && !pending_.empty() && pending_.front().ready_to_start) {
      NavigationOperation operation = std::move(pending_.front());
      pending_.pop_front();
      if (operation.kind == NavigationOperationKind::SetPath) {
        StartRoutePath(std::move(operation.route_path));
      } else {
        StartFactoryOperation(std::move(operation));
      }
    }
  }

  void StartFactoryOperation(NavigationOperation operation) {
    const std::uint64_t source_id = entries_.back().id;
    std::uint64_t destination_id = 0;
    std::vector<std::uint64_t> retire_ids;
    if (operation.kind == NavigationOperationKind::Push || operation.kind == NavigationOperationKind::Replace) {
      destination_id = next_entry_id_++;
      entries_.push_back({destination_id, std::move(operation.factory)});
      if (operation.kind == NavigationOperationKind::Replace) {
        retire_ids.push_back(source_id);
      }
    } else {
      destination_id = entries_[entries_.size() - 2].id;
      retire_ids.push_back(source_id);
    }
    transition_ = NavigationTransition{
        .kind = operation.kind,
        .source_id = source_id,
        .destination_id = destination_id,
        .progress = 0.0F,
        .target = 1.0F,
        .interactive = false,
        .complete_operation = true,
        .retire_ids = std::move(retire_ids),
    };
    ++revision_;
  }

  void StartRoutePath(std::vector<NavigationRouteDescriptor> routes) {
    if (SameRealizedRoutePath(realized_routes_, routes)) {
      RefreshRealizedRouteFactories(routes);
      return;
    }

    std::size_t shared = 0;
    const std::size_t shared_limit = std::min(realized_routes_.size(), routes.size());
    while (shared < shared_limit && SameRoute(realized_routes_[shared].descriptor, routes[shared])) {
      ++shared;
    }

    RefreshRealizedRouteFactories(routes);
    std::vector<RealizedNavigationRoute> next_routes(
        realized_routes_.begin(),
        realized_routes_.begin() + static_cast<std::ptrdiff_t>(shared)
    );
    std::vector<std::uint64_t> retire_ids;
    retire_ids.reserve(realized_routes_.size() - shared);
    for (auto route = realized_routes_.begin() + static_cast<std::ptrdiff_t>(shared); route != realized_routes_.end();
         ++route) {
      retire_ids.push_back(route->entry_id);
    }
    const std::uint64_t source_id = realized_routes_.empty() ? initial_root_id_ : realized_routes_.back().entry_id;

    for (std::size_t index = shared; index < routes.size(); ++index) {
      const std::uint64_t id = next_entry_id_++;
      entries_.push_back({id, routes[index].factory});
      next_routes.push_back({routes[index], id});
    }

    const std::uint64_t destination_id = next_routes.empty() ? initial_root_id_ : next_routes.back().entry_id;
    NavigationOperationKind kind = NavigationOperationKind::Replace;
    if (shared == realized_routes_.size() && routes.size() > realized_routes_.size()) {
      kind = NavigationOperationKind::Push;
    } else if (shared == routes.size() && routes.size() < realized_routes_.size()) {
      kind = NavigationOperationKind::Pop;
    }

    realized_routes_ = std::move(next_routes);
    transition_ = NavigationTransition{
        .kind = kind,
        .source_id = source_id,
        .destination_id = destination_id,
        .progress = 0.0F,
        .target = 1.0F,
        .interactive = false,
        .complete_operation = true,
        .retire_ids = std::move(retire_ids),
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
  std::size_t factory_depth_ = 1;
  std::uint64_t next_entry_id_ = 2;
  std::uint64_t next_operation_id_ = 1;
  std::uint64_t revision_ = 1;
  std::optional<std::uint64_t> queued_predictive_pop_id_;
  // The controlled NavigationPath remains authoritative; these snapshots only reconcile retained visual entries.
  std::vector<NavigationRouteDescriptor> requested_routes_;
  std::vector<RealizedNavigationRoute> realized_routes_;
  std::function<bool()> request_route_pop_;
  std::optional<std::type_index> route_type_;
  bool expected_route_pop_ = false;
  bool queued_routed_predictive_pop_ = false;
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
      [](ViewSpec& spec,
         ModifierSpec& compiled_modifier,
         const std::shared_ptr<const Environment>&,
         AppResources&) {
        const auto& modifier = *static_cast<const NavigationPageModifier*>(compiled_modifier.value.get());
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

View BuildNavigationView(
    const std::shared_ptr<NavigationState>& state, NavigationStyle style, NavigationEnvironmentEntry environment_entry
) {
  NavigationEnvironment environment = UseEnvironment<NavigationEnvironment>();
  environment.current = std::make_shared<NavigationEnvironmentNode>(NavigationEnvironmentNode{
      .entry = std::move(environment_entry),
      .parent = std::move(environment.current),
  });

  std::vector<View> pages;
  pages.reserve(state->Entries().size());
  for (const NavigationEntry& entry : state->Entries()) {
    View page = ProvideEnvironment(environment, Scope(entry.factory));
    pages.push_back(
        Stack{
            std::move(page),
        }
            .LayoutValue<NavigationEntryIdValue>(entry.id)
            .With(NavigationPageModifier{state, entry.id})
            .Key(entry.id)
    );
  }

  NavigationContainerModifier container{state, style, state->Revision()};
  return NavigationStackLayout {std::move(pages)}.LayoutValue<NavigationStateValue>(state).With(
      ClipChildren{},
      std::move(container)
  );
}

} // namespace

View BuildRoutedNavigationStack(
    std::function<View()> root, std::vector<NavigationRouteDescriptor> routes, NavigationRouteBinding binding
) {
  auto state_holder = UseState(std::make_shared<NavigationState>(root, routes, binding));
  auto revision = UseState<std::uint64_t>(0);
  const std::shared_ptr<NavigationState> state = state_holder.Get();
  static_cast<void>(revision.Get());
  state->Connect([revision] { revision.Update([](std::uint64_t& value) { ++value; }); });
  state->UpdateRoot(std::move(root));
  state->UpdateRouteBinding(binding);
  state->SynchronizeRoutePath(std::move(routes));
  NavigationStyle style = UseEnvironment<NavigationStyle>();
  state->UpdateStyle(style);
  return BuildNavigationView(
      state,
      std::move(style),
      NavigationEnvironmentEntry{
          .state = state,
          .route_type = binding.route_type,
          .path_state = std::move(binding.path_state),
          .history_commit = std::move(binding.history_commit),
      }
  );
}

NavigationAccess FindNavigationAccess(std::optional<std::type_index> route_type, bool outermost) {
  const NavigationEnvironment environment = UseEnvironment<NavigationEnvironment>();
  const auto matches = [route_type](const NavigationEnvironmentEntry& entry) {
    return entry.route_type == route_type && !entry.state.expired();
  };

  NavigationAccess found;
  for (auto node = environment.current; node; node = node->parent) {
    if (matches(node->entry)) {
      found = {node->entry.state, node->entry.path_state, node->entry.history_commit};
      if (!outermost) {
        break;
      }
    }
  }
  return found;
}

bool CheckNavigationAccess(const std::weak_ptr<NavigationState>& state) {
  const std::shared_ptr<NavigationState> shared = state.lock();
  if (!shared) {
    return false;
  }
  shared->CheckAccess();
  return true;
}

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
    NavigationStyle style = UseEnvironment<NavigationStyle>();
    state->UpdateStyle(style);
    return detail::BuildNavigationView(
        state,
        std::move(style),
        detail::NavigationEnvironmentEntry{
            .state = state,
            .route_type = std::nullopt,
            .path_state = nullptr,
            .history_commit = nullptr,
        }
    );
  });
}

NavigationController UseNavigation() {
  detail::NavigationAccess access = detail::FindNavigationAccess(std::nullopt, false);
  if (access.state.expired()) {
    throw std::logic_error("HuxerUI UseNavigation() requires an enclosing NavigationStack");
  }
  return NavigationController{std::move(access.state)};
}

NavigationController UseRootNavigation() {
  detail::NavigationAccess access = detail::FindNavigationAccess(std::nullopt, true);
  if (access.state.expired()) {
    throw std::logic_error("HuxerUI UseRootNavigation() requires an enclosing NavigationStack");
  }
  return NavigationController{std::move(access.state)};
}

} // namespace huxerui
