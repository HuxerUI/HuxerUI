#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/app.h>
#include <huxerui/event.h>
#include <huxerui/environment.h>
#include <huxerui/gesture.h>
#include <huxerui/lifecycle.h>
#include <huxerui/modifier.h>
#include <huxerui/platform_registry.h>
#include <huxerui/state.h>
#include <huxerui/task.h>
#include <huxerui/view.h>

#include "mounted_node_internal.h"
#include "semantics_internal.h"

namespace huxerui::detail {

class AppResources;
class TaskDelayScheduler;
struct WindowState;
class WindowService;
class FileDropReceiver;
class PointerInteraction;
class TextInteraction;

class EnvironmentTransaction {
public:
  EnvironmentTransaction(
      Environment& mounted,
      const Environment& declaration,
      std::shared_ptr<const Environment> parent
  );
  ~EnvironmentTransaction();

  EnvironmentTransaction(const EnvironmentTransaction&) = delete;
  EnvironmentTransaction& operator=(const EnvironmentTransaction&) = delete;

  void Commit();

private:
  struct Change {
    std::type_index key{typeid(void)};
    std::any previous_value;
    EnvironmentEquals previous_equals = nullptr;
  };

  void Rollback() noexcept;

  Environment* mounted_ = nullptr;
  std::shared_ptr<const Environment> previous_parent_;
  std::vector<Change> changes_;
  std::vector<std::shared_ptr<CompositionDependency>> dependencies_;
  bool parent_changed_ = false;
  bool committed_ = false;
};

struct DebugMetricsSnapshot {
  float fps = 0.0F;
  float average_commit_time_ms = 0.0F;
  float maximum_commit_time_ms = 0.0F;
  std::optional<float> cpu_percent;
  std::optional<std::uint64_t> memory_usage_bytes;
  float average_damage_ratio = 0.0F;
  Size viewport;
  std::size_t painted_frame_count = 0;

  bool operator==(const DebugMetricsSnapshot&) const = default;
};

class DebugMetricsState {
public:
  explicit DebugMetricsState(PlatformAdapter& platform) : platform_(&platform) {}

  void RecordCommit(double commit_time_seconds, const DamageRegion& damage, Size viewport) noexcept;
  void ResetSampling() noexcept;
  DebugMetricsSnapshot Sample(double timestamp) noexcept;

private:
  PlatformAdapter* platform_;
  bool window_initialized_ = false;
  double window_started_at_ = 0.0;
  std::size_t painted_frame_count_ = 0;
  double total_commit_time_seconds_ = 0.0;
  double maximum_commit_time_seconds_ = 0.0;
  double total_damage_ratio_ = 0.0;
  Size viewport_;
  std::optional<ProcessMetrics> previous_process_metrics_;
  double previous_process_timestamp_ = 0.0;
};

void InstallBuiltinPresentation(RootContext& root);
void InstallDebugOverlay(RootContext& root, std::shared_ptr<DebugMetricsState> metrics);

enum class LayerPlacementKind : std::uint8_t {
  Natural,
  Center,
  TopCenter,
  BottomCenter,
  Fill,
  Anchored,
};

enum class LayerAnchorSide : std::uint8_t {
  Below,
  Above,
  Right,
  Left,
};

enum class LayerAnchorAlignment : std::uint8_t {
  Start,
  Center,
  End,
};

enum class LayerSafeAreaPolicy : std::uint8_t {
  Constrain,
  ExtendBottom,
  Ignore,
};

struct LayerTransitionState {
  // LayerEntry owns this while retained modifiers observe it. The completion callback holds the controller weakly and
  // removes the entry only after the exit value settles.
  bool target_visible = true;
  // A transition attached to content that is already visible starts settled and is retained only for its later exit.
  bool enter_on_mount = true;
  float hidden_opacity = 0.0F;
  AnimationSpec enter = TweenSpec{.duration = 0.2};
  AnimationSpec exit = TweenSpec{.duration = 0.14};
  std::function<void()> on_exit_complete;
};

struct LayerPlacement {
  LayerPlacementKind kind = LayerPlacementKind::Natural;
  Rect anchor;
  LayerAnchorSide preferred_side = LayerAnchorSide::Below;
  LayerAnchorAlignment alignment = LayerAnchorAlignment::Start;
  float gap = 0.0F;
  float viewport_margin = 0.0F;
  Point offset;
  LayerSafeAreaPolicy safe_area_policy = LayerSafeAreaPolicy::Constrain;

  // BottomCenter uses these fields for surfaces that fill compact viewports but retain a desktop width limit.
  bool fill_cross_axis = false;
  float maximum_cross_axis_extent = std::numeric_limits<float>::infinity();

  bool operator==(const LayerPlacement&) const = default;
};

struct LayerPlacementValue {
  // Anchor geometry mutates this shared value so only the retained layer entry needs layout invalidation.
  using Value = std::shared_ptr<LayerPlacement>;
};

// This fieldless token provides only a strongly typed shared identity; Layer semantics must not attach state to it.
struct SemanticModalGroupToken final {};

struct LayerEntrySnapshot {
  // This metadata describes the controller declaration used for the mounted child in one FrameCommit. Later controller
  // mutations belong to the next frame and must not change semantics or geometry decisions for this mounted snapshot.
  LayerId id = 0;
  std::uint64_t revision = 0;
  bool exiting = false;
  std::shared_ptr<const SemanticModalGroupToken> semantic_modal_group;
  std::optional<std::uint64_t> retained_focus_identity;

  bool operator==(const LayerEntrySnapshot&) const = default;
};

struct LayerEntrySnapshotValue {
  // LayerStack retains entries by id and skips unchanged content factories by revision.
  using Value = LayerEntrySnapshot;
};

struct LayerEntry {
  LayerId id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t revision = 1;
  LayerOptions options;
  ViewFactory content;
  std::shared_ptr<const Environment> environment;
  // Active menu layers share this token so one logical menu chain remains one semantic modal region.
  std::shared_ptr<const SemanticModalGroupToken> semantic_modal_group;
  std::optional<std::uint64_t> retained_focus_identity;
  // Placement is non-null for every attached entry and may be updated without rebuilding its content scope.
  std::shared_ptr<LayerPlacement> placement;
  std::shared_ptr<LayerTransitionState> transition;
};

enum class SceneTransitionKind {
  Fade,
  CircularReveal,
};

struct SceneTransitionRequest {
  SceneTransitionKind kind = SceneTransitionKind::Fade;
  AnimationSpec animation = TweenSpec{};
  double delay = 0.0;
  Point origin;
};

class InteractionOriginScope final {
public:
  InteractionOriginScope(std::optional<Point>& current, Point origin, bool replace_existing)
      : current_(&current), previous_(current) {
    if (replace_existing || !current.has_value()) {
      current = origin;
    }
  }

  ~InteractionOriginScope() {
    *current_ = previous_;
  }

  InteractionOriginScope(const InteractionOriginScope&) = delete;
  InteractionOriginScope& operator=(const InteractionOriginScope&) = delete;

private:
  std::optional<Point>* current_;
  std::optional<Point> previous_;
};

struct SceneTransitionAnchorState {
  void Mount();
  void Unmount() noexcept;
  void UpdateBounds(Rect bounds) noexcept;
  [[nodiscard]] std::optional<Point> Center() const noexcept;

  std::optional<Rect> bounds;
  bool mounted = false;
};

class SceneTransitionService {
public:
  explicit SceneTransitionService(Runtime::State& runtime_state) : runtime_state_(&runtime_state) {}

  SceneTransitionService(const SceneTransitionService&) = delete;
  SceneTransitionService& operator=(const SceneTransitionService&) = delete;

  [[nodiscard]] std::shared_ptr<SceneTransitionAnchorState> CreateAnchor() const;
  [[nodiscard]] std::optional<Point> CurrentInteractionOrigin() const noexcept;
  void Run(SceneTransitionRequest request, std::function<void()> mutation, bool reduced_motion);
  [[nodiscard]] bool IsActive() const noexcept;
  MotionAdvanceResult Advance(const FrameInfo& frame);
  const RenderNode* Compose(const RenderNode* live_root);
  void Disconnect() noexcept;

private:
  struct ActiveTransition {
    SceneTransitionRequest request;
    std::shared_ptr<FrozenScene> frozen;
    MotionController progress{0.0F};
    Size viewport;
    RenderNode composite;
    RenderNode old_wrapper;
    RenderNode new_wrapper;
  };

  Runtime::State* runtime_state_;
  std::optional<ActiveTransition> active_;
};

class VirtualMeasureSession {
public:
  VirtualMeasureSession(Runtime& runtime, MountedNode& owner);
  ~VirtualMeasureSession();

  VirtualMeasureSession(const VirtualMeasureSession&) = delete;
  VirtualMeasureSession& operator=(const VirtualMeasureSession&) = delete;

  [[nodiscard]] std::size_t ItemCount() const noexcept;
  MountedNode& Item(std::size_t index);
  void CommitRealization(const std::vector<VirtualLayoutResult::Placement>& placements);

private:
  VirtualItemState CaptureItemState(MountedNode& mounted);
  void RestoreItemState(MountedNode& mounted, VirtualItemState& state);
  void SaveUnmounted(std::unique_ptr<MountedNode> node, std::size_t index);
  void RestoreOwner() noexcept;

  Runtime* runtime_;
  MountedNode* owner_;
  std::vector<std::unique_ptr<MountedNode>> previous_nodes_;
  std::vector<std::size_t> previous_realized_indices_;
  std::vector<std::uint64_t> previous_node_identities_;
  std::vector<std::unique_ptr<MountedNode>> requested_nodes_;
  std::vector<std::size_t> requested_item_indices_;
  std::unordered_map<std::size_t, std::size_t> requested_positions_by_index_;
  std::unordered_set<ViewKey> requested_item_keys_;
  bool committed_ = false;
};

class RecomposeScope final : public std::enable_shared_from_this<RecomposeScope> {
public:
  RecomposeScope(Runtime& runtime, std::uint64_t id, StateSlotStorage state_slots = {});
  ~RecomposeScope();

  void BeginComposition();
  void EndComposition();
  void AbortComposition() noexcept;
  void Observe(const std::shared_ptr<StateCellBase>& cell);
  void Observe(const std::shared_ptr<CompositionDependency>& dependency);
  void ObserveRetained(
      std::uint64_t owner_identity,
      const std::shared_ptr<CompositionDependency>& dependency
  );
  void ClearRetained(std::uint64_t owner_identity);
  void RegisterLifecycle(LifecycleSetup setup, std::vector<LifecycleDependency> dependencies);
  TaskScope Tasks();
  void Invalidate();
  void SetEventBindings(EventBindings bindings);

  [[nodiscard]] std::shared_ptr<EventHub> Events() const noexcept {
    return event_hub_;
  }

  std::shared_ptr<StateCellBase>
  UseState(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial);

  [[nodiscard]] std::uint64_t Id() const noexcept {
    return id_;
  }

  [[nodiscard]] bool IsDirty() const noexcept {
    return dirty_;
  }

  StateSlotStorage TakeStateSlots() noexcept {
    return std::move(state_slots_);
  }

private:
  [[nodiscard]] bool HasRetainedDependency(
      CompositionDependency* dependency,
      std::optional<std::uint64_t> excluding_owner = std::nullopt
  ) const;
  void PrepareLifecycleCommit();
  void CommitLifecycleCleanups() noexcept;
  void CommitLifecycleSetups();
  void DiscardLifecycleCommit() noexcept;

  Runtime* runtime_;
  std::uint64_t id_;
  bool dirty_ = true;
  bool composing_ = false;
  bool invalidated_during_composition_ = false;
  StateSlotStorage state_slots_;
  StateSlotStorage pending_state_slots_;
  std::unordered_set<CompositionSlotKey, CompositionSlotKeyHash> touched_state_slots_;
  std::unordered_map<CompositionSlotKey, std::uint32_t, CompositionSlotKeyHash> state_slot_occurrences_;
  std::unordered_map<CompositionSlotKey, LifecycleSlot, CompositionSlotKeyHash> lifecycle_slots_;
  std::vector<CompositionSlotKey> lifecycle_order_;
  std::vector<LifecycleDeclaration> pending_lifecycle_declarations_;
  std::unordered_map<CompositionSlotKey, std::size_t, CompositionSlotKeyHash> pending_lifecycle_indices_;
  std::unordered_map<CompositionSlotKey, std::uint32_t, CompositionSlotKeyHash> lifecycle_occurrences_;
  bool lifecycle_commit_pending_ = false;
  std::shared_ptr<TaskScopeState> task_scope_;
  std::unordered_map<CompositionDependency*, std::weak_ptr<CompositionDependency>> dependencies_;
  std::unordered_map<CompositionDependency*, std::weak_ptr<CompositionDependency>> pending_dependencies_;
  std::unordered_map<
      std::uint64_t,
      std::unordered_map<CompositionDependency*, std::weak_ptr<CompositionDependency>>
  > retained_dependencies_;
  std::unordered_map<StateCellBase*, std::uint64_t> observed_versions_;
  std::shared_ptr<EventHub> event_hub_ = std::make_shared<EventHub>();

  friend class huxerui::Runtime;
};

class Composer {
public:
  explicit Composer(std::shared_ptr<RecomposeScope> scope, std::shared_ptr<const Environment> environment = {});

  static Composer* Current() noexcept;
  static Composer& RequireCurrent();

  void Observe(const std::shared_ptr<StateCellBase>& cell);
  void Observe(const std::shared_ptr<CompositionDependency>& dependency);
  std::shared_ptr<StateCellBase>
  UseState(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial);
  void RegisterLifecycle(LifecycleSetup setup, std::vector<LifecycleDependency> dependencies);
  TaskScope Tasks();
  [[nodiscard]] std::shared_ptr<EventHub> Events() const noexcept;
  [[nodiscard]] std::uint64_t ScopeId() const noexcept {
    return scope_->Id();
  }
  [[nodiscard]] const std::shared_ptr<RecomposeScope>& Scope() const noexcept {
    return scope_;
  }
  [[nodiscard]] const std::shared_ptr<const Environment>& CurrentEnvironment() const noexcept {
    return environment_;
  }

  class Guard {
  public:
    explicit Guard(Composer& composer);
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

  private:
    Composer* previous_;
  };

private:
  static thread_local Composer* current_;
  std::shared_ptr<RecomposeScope> scope_;
  std::shared_ptr<const Environment> environment_;
};

struct FocusTrapFrame {
  std::uint64_t identity = 0;
  std::optional<std::uint64_t> restore_identity;
};

enum class BackTargetKind : std::uint8_t {
  SelectionOverlay,
  Layer,
  Event,
  Extension,
};

struct BackTarget {
  BackTargetKind kind = BackTargetKind::Event;
  LayerId layer_id = 0;
  std::uint64_t node_identity = 0;
  NodeExtensionHandle extension;
};

} // namespace huxerui::detail

namespace huxerui {

struct LayerController::State {
  Runtime* runtime = nullptr;
  std::vector<detail::LayerEntry> entries;
  LayerId next_id = 1;
  std::uint64_t next_sequence = 1;
};

struct Runtime::State {
  State(Runtime& owner, const Application& application, PlatformAdapter& platform);
  ~State();

  Runtime& owner_;
  RootFactory root_factory_;
  PlatformAdapter* platform_;
  UIThreadDispatcher ui_thread_dispatcher_;
  ViewportBreakpoints viewport_breakpoints_;
  std::shared_ptr<detail::WindowState> window_;
  ViewportClass viewport_class_ = ViewportClass::Compact;
  std::shared_ptr<detail::RecomposeScope> root_scope_;
  LayerController layer_controller_;
  std::vector<std::shared_ptr<void>> root_services_;
  std::unordered_set<std::type_index> root_service_types_;
  std::shared_ptr<Environment> root_environment_;
  std::shared_ptr<detail::AppResources> app_resources_;
  std::shared_ptr<detail::ApplicationService> application_service_;
  std::shared_ptr<detail::DebugMetricsState> debug_metrics_;
  std::shared_ptr<detail::WindowService> window_service_;
  std::shared_ptr<detail::SceneTransitionService> scene_transition_service_;
  std::unique_ptr<detail::TextInteraction> text_;
  std::unique_ptr<detail::PointerInteraction> pointer_;
  std::unique_ptr<detail::SemanticTree> semantics_;
  GestureSettings gesture_settings_;
  ScrollPhysics default_scroll_physics_;
  std::unique_ptr<detail::MountedNode> mounted_root_;
  std::vector<std::weak_ptr<detail::RecomposeScope>> lifecycle_commits_;
  std::vector<detail::LifecycleCleanup> retired_lifecycle_cleanups_;
  std::vector<std::shared_ptr<detail::TaskScopeState>> retired_task_scopes_;
  std::shared_ptr<detail::TaskDelayScheduler> task_delay_scheduler_;
  FrameCommit frame_commit_;
  detail::RenderDamageSnapshot committed_scene_snapshot_;
  Size committed_viewport_;
  bool has_committed_scene_snapshot_ = false;
  bool application_dirty_ = true;
  bool layers_dirty_ = true;
  bool extension_tree_dirty_ = true;
  bool scroll_motion_active_ = false;
  bool building_frame_ = false;
  bool frame_requested_ = false;
  double frame_request_deadline_ = 0.0;
  std::optional<double> previous_frame_timestamp_;
  std::uint64_t next_node_identity_ = 1;
  std::uint64_t next_scope_identity_ = 2;
  std::uint64_t next_press_id_ = 1;
  std::optional<Point> current_interaction_origin_;
  std::unique_ptr<detail::FileDropReceiver> file_drop_;
  std::optional<std::uint64_t> focused_node_identity_;
  bool focus_visible_ = false;
  std::optional<std::uint64_t> keyboard_activation_identity_;
  std::optional<std::uint64_t> keyboard_press_id_;
  std::vector<detail::FocusTrapFrame> focus_trap_stack_;
  std::optional<detail::BackTarget> back_target_;
};

} // namespace huxerui

namespace huxerui::detail {

struct RuntimeAccess {
  static void InvalidateRoot(Runtime& runtime) {
    runtime.InvalidateRoot();
  }

  static const MountedNode* RootNode(const Runtime& runtime) noexcept {
    return runtime.RootNode();
  }

  static const ScrollPhysics& DefaultScrollPhysics(const Runtime& runtime) noexcept {
    return runtime.state_->default_scroll_physics_;
  }

  static void NotifyScrollActivity(Runtime& runtime, MountedNode& node, const ScrollActivity& activity) {
    runtime.NotifyScrollActivity(node, activity);
  }

  static void RequestFrame(Runtime& runtime) {
    runtime.RequestFrame();
  }

  static void InvalidateLayout(Runtime& runtime, MountedNode& node) {
    runtime.InvalidateLayout(node);
  }

  static std::optional<std::uint64_t> HitTestPlatformView(const Runtime& runtime, Point position) {
    return runtime.HitTestPlatformView(position);
  }

  static std::optional<std::uint64_t> FocusedPlatformView(const Runtime& runtime) {
    return runtime.FocusedPlatformView();
  }

  static void
  SynchronizePlatformViewFocus(Runtime& runtime, std::optional<std::uint64_t> identity, bool focus_visible) {
    runtime.SynchronizePlatformViewFocus(identity, focus_visible);
  }

  static bool MoveFocusFromPlatformView(Runtime& runtime, std::uint64_t identity, bool reverse) {
    return runtime.MoveFocusFromPlatformView(identity, reverse);
  }

  static std::optional<PlatformPayload> DispatchPlatformViewEvent(
      Runtime& runtime, std::uint64_t identity, std::string_view name, const PlatformPayload& payload
  ) {
    return runtime.DispatchPlatformViewEvent(identity, name, payload);
  }

  static std::optional<PlatformValue> DispatchPlatformViewEvent(
      Runtime& runtime, std::uint64_t identity, std::type_index key, const PlatformValue& value
  ) {
    return runtime.DispatchPlatformViewEvent(identity, key, value);
  }
};


} // namespace huxerui::detail
