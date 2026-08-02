#pragma once

#include <algorithm>
#include <any>
#include <atomic>
#include <cmath>
#include <concepts>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/app.h>
#include <huxerui/event.h>
#include <huxerui/environment.h>
#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

#include "geometry_internal.h"

namespace huxerui::detail {

struct MountedNode;
class ScrollConnection;
class IndicationState;
class AppResources;

struct ScrollBarBinding {
  using Value = ScrollBarStyle;
};

struct ScrollAxisBinding {
  using Value = Axis;
};

struct VirtualListItemExtent {
  using Value = float;
};

struct VirtualListEstimatedItemExtent {
  using Value = float;
};

struct VirtualListCacheExtent {
  using Value = float;
};

struct VirtualGridColumns {
  using Value = GridColumns;
};

struct VirtualGridRowExtent {
  using Value = float;
};

struct VirtualGridEstimatedRowExtent {
  using Value = float;
};

struct VirtualGridRowSpacing {
  using Value = float;
};

struct VirtualGridColumnSpacing {
  using Value = float;
};

struct VirtualGridCacheExtent {
  using Value = float;
};

struct VirtualGridItemSpans {
  using Value = std::vector<std::size_t>;
};

struct TextMeasurerService {
  TextMeasurer* measurer = nullptr;
};

void InstallBuiltinPresentation(RootContext& root);

struct LayerControllerState {
  Runtime* runtime = nullptr;
};

template <std::floating_point T> class AnimatedValue {
public:
  AnimatedValue() noexcept = default;

  explicit AnimatedValue(T value) noexcept {
    Set(value);
  }

  [[nodiscard]] T Value() const noexcept {
    return value_;
  }
  [[nodiscard]] T Target() const noexcept {
    return target_;
  }
  [[nodiscard]] bool IsRunning() const noexcept {
    return running_;
  }

  void Set(T value) noexcept {
    value_ = value;
    start_ = value;
    target_ = value;
    velocity_ = {};
    initialized_ = true;
    pending_ = false;
    running_ = false;
  }

  void Update(T target, AnimationSpec animation) {
    if (!initialized_) {
      Set(target);
      animation_ = std::move(animation);
      return;
    }
    animation_ = std::move(animation);
    if (target == target_ && !pending_) {
      return;
    }
    target_ = target;
    pending_ = true;
  }

  bool Advance(double timestamp, double delta_time, bool reduced_motion = false) noexcept {
    if (pending_) {
      pending_ = false;
      if (reduced_motion || std::holds_alternative<SnapSpec>(animation_)) {
        Set(target_);
        return false;
      }
      start_ = value_;
      start_time_ = timestamp;
      running_ = true;
    }
    if (!running_) {
      return false;
    }

    if (const auto* tween = std::get_if<TweenSpec>(&animation_)) {
      if (!std::isfinite(tween->duration) || tween->duration <= 0.0) {
        Set(target_);
        return false;
      }
      const double progress = std::clamp((timestamp - start_time_) / tween->duration, 0.0, 1.0);
      double eased = progress;
      if (tween->easing == Easing::EaseOut) {
        const double inverse = 1.0 - progress;
        eased = 1.0 - inverse * inverse * inverse;
      }
      value_ = static_cast<T>(start_ + (target_ - start_) * static_cast<T>(eased));
      if (progress >= 1.0) {
        Set(target_);
      }
      return running_;
    }

    const auto& spring = std::get<SpringSpec>(animation_);
    if (!std::isfinite(spring.stiffness) || spring.stiffness <= 0.0F || !std::isfinite(spring.damping_ratio) ||
        spring.damping_ratio < 0.0F) {
      Set(target_);
      return false;
    }
    const T step = static_cast<T>(std::clamp(delta_time, 0.0, 1.0 / 30.0));
    const T stiffness = static_cast<T>(spring.stiffness);
    const T damping = static_cast<T>(2.0F * std::sqrt(spring.stiffness) * spring.damping_ratio);
    const T acceleration = stiffness * (target_ - value_) - damping * velocity_;
    velocity_ += acceleration * step;
    value_ += velocity_ * step;
    if (std::abs(target_ - value_) < static_cast<T>(0.001) && std::abs(velocity_) < static_cast<T>(0.001)) {
      Set(target_);
    }
    return running_;
  }

private:
  AnimationSpec animation_ = SnapSpec{};
  T value_{};
  T start_{};
  T target_{};
  T velocity_{};
  double start_time_ = 0.0;
  bool initialized_ = false;
  bool pending_ = false;
  bool running_ = false;
};

struct ScrollItemRequest {
  std::size_t index;
  ScrollAlignment alignment;
};

class ScrollControllerState {
public:
  explicit ScrollControllerState(float initial_offset);

  std::shared_ptr<StateCell<ScrollMetrics>> metrics;
  std::weak_ptr<ScrollConnection> connection;
  std::optional<float> pending_offset;
  std::optional<ScrollItemRequest> pending_item;
  bool was_connected = false;
};

enum class NodeKind {
  Text,
  Button,
  TextField,
  Checkbox,
  Switch,
  ProgressCircle,
  Image,
  Canvas,
  Spacer,
  Scope,
  SelectionArea,
  Layout,
  ScrollView,
  VirtualLayout,
};

using ViewKey = std::variant<std::int64_t, std::uint64_t, std::string>;

struct ViewProperties {
  EdgeInsets padding;
  Frame frame;
  std::optional<Color> background;
  std::optional<Shadow> shadow;
  TextStyle text_style;
  float corner_radius = 0.0F;
  float spacing = 0.0F;
  float grow = 0.0F;
  MainAxisAlignment main_axis_alignment = MainAxisAlignment::Start;
  CrossAxisAlignment cross_axis_alignment = CrossAxisAlignment::Start;
  HorizontalAlignment horizontal_alignment = HorizontalAlignment::Start;
  VerticalAlignment vertical_alignment = VerticalAlignment::Start;
  Color focus_ring = Color::Rgb(31, 111, 235);
  float focus_ring_width = 2.0F;
  float disabled_opacity = 0.42F;

  bool operator==(const ViewProperties&) const = default;

  // Reconciliation compares the inputs consumed by layout, content paint, and foreground paint independently.
  // New property fields must participate in every projection whose stage reads them.
  [[nodiscard]] bool LayoutEquals(const ViewProperties& other) const {
    return padding == other.padding && frame == other.frame && text_style.font == other.text_style.font &&
           spacing == other.spacing && grow == other.grow && main_axis_alignment == other.main_axis_alignment &&
           cross_axis_alignment == other.cross_axis_alignment && horizontal_alignment == other.horizontal_alignment &&
           vertical_alignment == other.vertical_alignment;
  }

  [[nodiscard]] bool ContentPaintEquals(const ViewProperties& other) const {
    return padding == other.padding && background == other.background && shadow == other.shadow &&
           text_style == other.text_style && corner_radius == other.corner_radius;
  }

  [[nodiscard]] bool ForegroundPaintEquals(const ViewProperties& other) const {
    return corner_radius == other.corner_radius && focus_ring == other.focus_ring &&
           focus_ring_width == other.focus_ring_width;
  }
};

struct ImageProperties {
  ImageAsset asset;
  ImageFit fit = ImageFit::Contain;
  HorizontalAlignment horizontal_alignment = HorizontalAlignment::Center;
  VerticalAlignment vertical_alignment = VerticalAlignment::Center;
  ImageSampling sampling = ImageSampling::Linear;

  // Only intrinsic logical size affects measurement; image contents, fit, alignment, and sampling are paint inputs.
  [[nodiscard]] bool LayoutEquals(const ImageProperties& other) const noexcept {
    return asset.IntrinsicSize() == other.asset.IntrinsicSize();
  }

  bool operator==(const ImageProperties&) const = default;
};

// ViewSpec is View's transient copy-on-write declaration. NodeKind selects the component-specific payloads;
// fields unrelated to that kind stay at their defaults and are ignored by the corresponding Runtime stages.
struct ViewSpec {
  explicit ViewSpec(NodeKind kind_value) : kind(kind_value) {}

  NodeKind kind;
  TextRole text_role = TextRole::Body;
  std::optional<ViewKey> key;
  std::string text;
  ViewProperties properties;
  std::vector<View> children;
  std::function<View()> scope_factory;
  CanvasPainter canvas_painter;
  ImageProperties image_properties;
  const LayoutDescriptor* layout_descriptor = nullptr;
  const VirtualLayoutDescriptor* virtual_layout_descriptor = nullptr;
  VirtualItemSource virtual_items;
  std::unordered_map<std::type_index, ErasedLayoutValue> layout_values;
  EventBindings event_bindings;
  std::function<void(const EventBindings&)> activation;
  std::vector<ModifierSpec> retained_modifiers;
  std::shared_ptr<const Environment> environment;
  bool pointer_events_enabled = true;
  bool local_enabled = true;
  bool focusable = false;
};

struct StateSlotKey {
  std::string file;
  std::string function;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::uint32_t occurrence = 0;

  bool operator==(const StateSlotKey&) const = default;
};

struct StateSlotKeyHash {
  std::size_t operator()(const StateSlotKey& key) const noexcept;
};

struct StateSlotStorage {
  std::unordered_map<StateSlotKey, std::shared_ptr<StateCellBase>, StateSlotKeyHash> slots;
};

struct VirtualItemState {
  NodeKind kind = NodeKind::Layout;
  std::optional<ViewKey> key;
  const LayoutDescriptor* layout_descriptor = nullptr;
  const VirtualLayoutDescriptor* virtual_layout_descriptor = nullptr;
  std::optional<StateSlotStorage> state_slots;
  std::vector<VirtualItemState> children;
};

struct VirtualItemStateCache {
  std::unordered_map<ViewKey, VirtualItemState> keyed;
  std::unordered_map<std::size_t, VirtualItemState> indexed;
};

struct VirtualNodeState {
  VirtualItemSource source;
  std::unordered_map<std::size_t, View> item_declarations;
  std::vector<std::size_t> realized_indices;
  std::vector<VirtualLayoutResult::Placement> realized_placements;
  std::unique_ptr<VirtualItemStateCache> item_state_cache;
  bool source_dirty = true;
  bool viewport_dirty = true;
};

struct ScrollMotionFrameResult {
  bool needs_frame = false;
  std::optional<float> transfer_velocity;
};

class ScrollMotion {
public:
  void Stop() noexcept;
  bool StartMomentum(MountedNode& node, float velocity);
  ScrollMotionFrameResult Advance(MountedNode& node, const FrameInfo& frame);

private:
  float velocity_ = 0.0F;
  std::optional<double> previous_timestamp_;
  bool momentum_active_ = false;
};

struct ScrollNodeState {
  Axis axis = Axis::Vertical;
  bool touch_drag_only = false;
  bool allows_automatic_reveal = true;
  float offset_y = 0.0F;
  float offset_x = 0.0F;
  float content_height = 0.0F;
  float content_width = 0.0F;
  ScrollMotion motion;
  std::shared_ptr<ScrollConnection> connection;
};

struct NodeExtensionEntry {
  const ModifierDescriptor* descriptor = nullptr;
  std::unique_ptr<huxerui::NodeExtension> extension;
  std::shared_ptr<const void> value;
};

struct NodePresentation {
  Transform2D local_transform;
  float local_opacity = 1.0F;
  float render_opacity = 1.0F;
  Transform2D resolved_transform;
  float resolved_opacity = 1.0F;
};

// MountedNode is the retained counterpart of ViewSpec. Runtime reads copied component payloads only from matching
// NodeKind branches, while layout, paint, interaction, and extension state persist across compatible declarations.
struct MountedNode final : public huxerui::MountedNode {
  NodeKind kind = NodeKind::Layout;
  std::uint64_t identity = 0;
  std::optional<ViewKey> key;
  std::string text;
  ViewProperties properties;
  std::function<View()> scope_factory;
  CanvasPainter canvas_painter;
  ImageProperties image_properties;
  const LayoutDescriptor* layout_descriptor = nullptr;
  const VirtualLayoutDescriptor* virtual_layout_descriptor = nullptr;
  std::unordered_map<std::type_index, ErasedLayoutValue> layout_values;
  std::unordered_map<std::type_index, std::any> layout_cache;
  std::vector<LayoutResult::Placement> layout_placements;
  EventBindings event_bindings;
  std::function<void(const EventBindings&)> activation;
  std::shared_ptr<RecomposeScope> recompose_scope;
  std::optional<Constraints> measured_constraints;
  Size measured_size;
  // Bounds stay at the node-local origin; layout_offset places the node in its parent's local coordinates.
  Rect bounds;
  Point layout_offset;
  NodePresentation presentation;
  RenderNode render_node;
  std::uint64_t measure_revision = 0;
  std::uint64_t layout_revision = 0;
  // Measurement, descendant placement, content recording, and foreground recording are invalidated independently.
  bool measure_dirty = true;
  bool layout_dirty = true;
  bool content_paint_dirty = true;
  bool foreground_paint_dirty = true;
  std::unique_ptr<ScrollNodeState> scroll_state;
  std::unique_ptr<VirtualNodeState> virtual_state;
  std::vector<NodeExtensionEntry> extensions;
  std::shared_ptr<const Environment> environment;
  bool pointer_events_enabled = true;
  bool local_enabled = true;
  bool enabled = true;
  bool focusable = false;
  bool focused = false;
  bool focus_visible = false;
  bool subtree_has_extensions = true;
  std::vector<std::unique_ptr<MountedNode>> children;

  [[nodiscard]] Rect ContentBounds() const noexcept {
    return {
        bounds.x + properties.padding.left,
        bounds.y + properties.padding.top,
        std::max(0.0F, bounds.width - properties.padding.Horizontal()),
        std::max(0.0F, bounds.height - properties.padding.Vertical()),
    };
  }

protected:
  [[nodiscard]] std::size_t ChildCountImpl() const noexcept override {
    return children.size();
  }

  MountedNode& ChildAtImpl(std::size_t index) override {
    return *children[index];
  }

  const MountedNode& ChildAtImpl(std::size_t index) const override {
    return *children[index];
  }

  [[nodiscard]] Size LayoutSizeImpl() const noexcept override {
    return measured_size;
  }

  [[nodiscard]] Rect BoundsImpl() const noexcept override {
    return bounds;
  }

  [[nodiscard]] Point LayoutOffsetImpl() const noexcept override {
    return layout_offset;
  }

  [[nodiscard]] Rect PresentationBoundsImpl() const noexcept override {
    return TransformBounds(presentation.resolved_transform, bounds);
  }

  [[nodiscard]] float PresentationOpacityImpl() const noexcept override {
    return presentation.resolved_opacity;
  }

  [[nodiscard]] bool IsEnabledImpl() const noexcept override {
    return enabled;
  }

  [[nodiscard]] bool IsFocusedImpl() const noexcept override {
    return focused;
  }

  [[nodiscard]] float SpacingImpl() const noexcept override {
    return properties.spacing;
  }

  [[nodiscard]] float GrowFactorImpl() const noexcept override {
    return properties.grow;
  }

  [[nodiscard]] MainAxisAlignment MainAlignmentImpl() const noexcept override {
    return properties.main_axis_alignment;
  }

  [[nodiscard]] CrossAxisAlignment CrossAlignmentImpl() const noexcept override {
    return properties.cross_axis_alignment;
  }

  [[nodiscard]] HorizontalAlignment HorizontalAlignmentImpl() const noexcept override {
    return properties.horizontal_alignment;
  }

  [[nodiscard]] VerticalAlignment VerticalAlignmentImpl() const noexcept override {
    return properties.vertical_alignment;
  }

  [[nodiscard]] const std::any* FindLayoutValue(std::type_index key_value) const noexcept override {
    const auto found = layout_values.find(key_value);
    return found == layout_values.end() ? nullptr : &found->second.value;
  }

  std::any& EnsureCacheEntry(std::type_index key_value) override {
    return layout_cache[key_value];
  }
};

struct RenderNodeSnapshot {
  std::uint64_t content_revision = 0;
  std::uint64_t foreground_revision = 0;
  Transform2D world_transform;
  Transform2D world_children_transform;
  std::optional<Rect> world_clip;
  std::optional<Rect> world_child_clip;
  std::optional<float> child_clip_corner_radius;
  Rect own_bounds;
  Rect subtree_bounds;
  std::vector<std::uint64_t> children;
  float opacity = 1.0F;
  bool has_own_bounds = false;
  bool has_subtree_bounds = false;
  bool visible = false;
};

using RenderSceneSnapshot = std::unordered_map<std::uint64_t, RenderNodeSnapshot>;

class ScrollConnection : public std::enable_shared_from_this<ScrollConnection> {
public:
  ScrollConnection(Runtime& runtime, MountedNode& node, std::shared_ptr<ScrollControllerState> state);

  [[nodiscard]] const std::shared_ptr<ScrollControllerState>& State() const noexcept {
    return state_;
  }

  [[nodiscard]] bool IsCurrent() const noexcept;
  bool ScrollTo(float offset);
  bool ScrollBy(float delta);
  bool ScrollToItem(std::size_t index, ScrollAlignment alignment);
  void ApplyPending();
  void PublishMetrics();

private:
  [[nodiscard]] bool IsVertical() const noexcept;
  [[nodiscard]] float ViewportExtent() const noexcept;
  [[nodiscard]] float ContentExtent() const noexcept;
  [[nodiscard]] float CurrentOffset() const noexcept;
  void SetCurrentOffset(float offset) noexcept;

  Runtime* runtime_;
  MountedNode* node_;
  std::shared_ptr<ScrollControllerState> state_;
};

void PrepareScrollController(MountedNode& node, Runtime& runtime);
void CompleteScrollController(MountedNode& node);

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
  Runtime* runtime_;
  std::uint64_t id_;
  bool dirty_ = true;
  bool composing_ = false;
  bool invalidated_during_composition_ = false;
  StateSlotStorage state_slots_;
  StateSlotStorage pending_state_slots_;
  std::unordered_set<StateSlotKey, StateSlotKeyHash> touched_state_slots_;
  std::unordered_map<StateSlotKey, std::uint32_t, StateSlotKeyHash> state_slot_occurrences_;
  std::unordered_map<StateCellBase*, std::weak_ptr<StateCellBase>> dependencies_;
  std::unordered_map<StateCellBase*, std::weak_ptr<StateCellBase>> pending_dependencies_;
  std::unordered_map<StateCellBase*, std::uint64_t> observed_versions_;
  std::shared_ptr<EventHub> event_hub_ = std::make_shared<EventHub>();
};

class Composer {
public:
  explicit Composer(std::shared_ptr<RecomposeScope> scope, std::shared_ptr<const Environment> environment = {});

  static Composer* Current() noexcept;
  static Composer& RequireCurrent();

  void Observe(const std::shared_ptr<StateCellBase>& cell);
  std::shared_ptr<StateCellBase>
  UseState(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial);
  [[nodiscard]] std::shared_ptr<EventHub> Events() const noexcept;
  [[nodiscard]] const std::shared_ptr<const Environment>& CurrentEnvironment() const noexcept {
    return environment_;
  }

  class EnvironmentGuard {
  public:
    explicit EnvironmentGuard(std::shared_ptr<const Environment> environment);
    ~EnvironmentGuard();

    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

  private:
    Composer* composer_;
    std::shared_ptr<const Environment> previous_;
  };

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

struct ScrollBarGeometry {
  Axis axis = Axis::Vertical;
  Rect track;
  Rect thumb;
  ScrollBarStyle style;
  float scroll_offset = 0.0F;
  float maximum_offset = 0.0F;
  float thumb_travel = 0.0F;
};

struct NodeExtensionHandle {
  std::uint64_t node_identity = 0;
  std::size_t extension_index = 0;
  const ModifierDescriptor* descriptor = nullptr;

  bool operator==(const NodeExtensionHandle&) const = default;
};

inline constexpr float touch_gesture_slop = 6.0F;

struct PointerSession {
  std::optional<std::uint64_t> target_identity;
  std::optional<std::uint64_t> pending_focus_identity;
  std::vector<std::uint64_t> scroll_chain;
  Point down_position;
  Point last_position;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  double velocity_sample_timestamp = 0.0;
  float scroll_velocity = 0.0F;
  bool has_velocity_sample = false;
  bool focus_pending = false;
  std::optional<Axis> drag_axis;
  std::size_t active_scroll = 0;
  std::optional<std::uint64_t> active_scroll_node;
  std::optional<NodeExtensionHandle> extension_capture;
  std::vector<NodeExtensionHandle> extension_observers;
};

struct ActiveTextInputSession {
  struct GeometrySnapshot {
    std::uint64_t client_revision = 0;
    std::uint64_t layout_revision = 0;
    Transform2D node_to_host;
    TextInputGeometry geometry;
  };

  std::uint64_t node_identity = 0;
  TextInputSessionId session_id = 0;
  std::shared_ptr<TextInputClient> client;
  TextInputConfiguration configuration;
  TextInputState state;
  std::optional<GeometrySnapshot> published_geometry;
  std::optional<GeometrySnapshot> prepared_geometry;
};

struct TextSelectionGestureState {
  bool long_press_pending = false;
  std::int64_t long_press_pointer_id = 0;
  Point long_press_position;
  double long_press_deadline = 0.0;
  bool tap_pending = false;
  std::int64_t tap_pointer_id = 0;
  Point tap_position;
  bool double_tap_pending = false;
  std::int64_t double_tap_pointer_id = 0;
  std::optional<std::uint64_t> double_tap_node;
  std::optional<double> previous_tap_time;
  Point previous_tap_position;
  std::optional<std::uint64_t> previous_tap_node;
};

struct TextSelectionOverlayState {
  bool visible = false;
  bool paint_dirty = true;
  bool indication_frame_active = false;
  bool has_painted_geometry = false;
  bool dragging = false;
  bool dragging_start_handle = false;
  bool show_handles = false;
  bool dismissing = false;
  std::optional<std::int64_t> pointer_id;
  std::optional<std::size_t> pressed_action;
  std::optional<std::size_t> hovered_action;
  Rect start_handle_hit_rect;
  Rect end_handle_hit_rect;
  Rect painted_start;
  Rect painted_end;
  Rect toolbar_rect;
  Color toolbar_background;
  Color toolbar_border;
  float toolbar_corner_radius = 0.0F;
  TextStyle toolbar_text_style;
  std::vector<TextEditingAction> actions;
  std::vector<Rect> action_rects;
  std::vector<std::string> action_labels;
  std::vector<std::shared_ptr<IndicationState>> action_indications;
};

struct TextSelectionOverlay {
  // The framework-owned selection overlay is rendered above application layers without becoming part of their tree.
  RenderNode render_node;
  TextSelectionOverlayState state;
};

struct LayerEntry {
  LayerId id = 0;
  LayerOptions options;
  ViewFactory content;
  std::shared_ptr<const Environment> environment;
};

} // namespace huxerui::detail

namespace huxerui {

struct Runtime::State {
  State(
      RootFactory root_factory,
      PlatformAdapter* platform,
      std::shared_ptr<detail::RecomposeScope> root_scope,
      LayerController layer_controller
  )
      : root_factory_(root_factory), platform_(platform), root_scope_(std::move(root_scope)),
        layer_controller_(std::move(layer_controller)) {}

  RootFactory root_factory_;
  PlatformAdapter* platform_;
  Size viewport_;
  std::shared_ptr<detail::RecomposeScope> root_scope_;
  LayerController layer_controller_;
  std::vector<std::shared_ptr<void>> root_services_;
  std::unordered_set<std::type_index> root_service_types_;
  std::shared_ptr<Environment> root_environment_;
  std::shared_ptr<detail::AppResources> app_resources_;
  std::vector<detail::LayerEntry> layers_;
  std::unique_ptr<detail::MountedNode> mounted_root_;
  FrameCommit frame_commit_;
  detail::RenderSceneSnapshot committed_scene_snapshot_;
  Size committed_viewport_;
  bool has_committed_scene_snapshot_ = false;
  bool composition_dirty_ = true;
  bool composing_root_ = false;
  bool layer_snapshot_taken_ = false;
  bool extension_tree_dirty_ = true;
  bool scroll_motion_active_ = false;
  bool building_frame_ = false;
  bool frame_requested_ = false;
  double frame_request_deadline_ = 0.0;
  std::optional<double> previous_frame_timestamp_;
  std::uint64_t next_node_identity_ = 1;
  std::uint64_t next_scope_identity_ = 2;
  LayerId next_layer_id_ = 1;
  bool has_application_root_ = false;
  std::optional<detail::NodeExtensionHandle> hovered_extension_;
  std::unordered_map<std::int64_t, detail::PointerSession> pointer_sessions_;
  std::optional<std::uint64_t> focused_node_identity_;
  bool focus_visible_ = false;
  std::optional<std::uint64_t> keyboard_activation_identity_;
  std::optional<detail::ActiveTextInputSession> text_input_session_;
  detail::TextSelectionGestureState text_selection_gesture_;
  detail::TextSelectionOverlay text_selection_overlay_;
  TextInputSessionId next_text_input_session_id_ = 1;
  std::optional<LayerId> active_modal_focus_layer_;
  std::unordered_map<LayerId, std::optional<std::uint64_t>> modal_focus_restore_;
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
};

Size MeasureNode(MountedNode& node, const Constraints& constraints, PlatformAdapter& platform, Runtime& runtime);
void LayoutNode(MountedNode& node, Point offset);
TextSelectionClient* FindTextSelectionClient(MountedNode& node);
void ResolvePresentationTree(MountedNode& node);
void UpdateRenderScene(MountedNode& node, Rect clip, const RenderNode* overlay = nullptr);
DamageRegion ComputeDamageRegion(
    const RenderNode* root,
    Size viewport,
    RenderSceneSnapshot& committed_scene,
    Size& committed_viewport,
    bool& has_committed_scene
);
bool BuildPointerRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route);
MountedNode* HitTestPointer(MountedNode& node, Point position);
std::optional<ScrollBarGeometry> ResolveScrollBarGeometry(const MountedNode& node);
bool IsScrollContainer(const MountedNode& node) noexcept;
Axis ScrollAxis(const MountedNode& node) noexcept;
bool CanScrollNode(const MountedNode& node, float delta);
float ScrollNodeBy(MountedNode& node, float delta);
bool ScrollNodeRectIntoView(MountedNode& node, Rect& rect);

struct ScrollEventResult {
  std::vector<MountedNode*> scroll_chain;
};

ScrollEventResult ApplyScrollEvent(MountedNode& node, const ScrollEvent& event);
bool AdvanceMountedNodeFrame(MountedNode& node, const FrameInfo& frame);

bool IsVirtualLayoutNode(const MountedNode& node) noexcept;

} // namespace huxerui::detail
