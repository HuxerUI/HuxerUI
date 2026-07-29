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
#include <huxerui/display_list.h>
#include <huxerui/event.h>
#include <huxerui/environment.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace huxerui::detail {

struct MountedNode;
class ScrollConnection;

struct PresentationTransform {
  float m11 = 1.0F;
  float m12 = 0.0F;
  float m21 = 0.0F;
  float m22 = 1.0F;
  float translate_x = 0.0F;
  float translate_y = 0.0F;

  [[nodiscard]] bool IsIdentity() const noexcept {
    return m11 == 1.0F && m12 == 0.0F && m21 == 0.0F && m22 == 1.0F && translate_x == 0.0F && translate_y == 0.0F;
  }

  [[nodiscard]] Point Apply(Point point) const noexcept {
    return {
        m11 * point.x + m21 * point.y + translate_x,
        m12 * point.x + m22 * point.y + translate_y,
    };
  }

  [[nodiscard]] std::optional<Point> Inverse(Point point) const noexcept {
    const float determinant = m11 * m22 - m12 * m21;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001F) {
      return std::nullopt;
    }
    const float x = point.x - translate_x;
    const float y = point.y - translate_y;
    return Point{
        (m22 * x - m21 * y) / determinant,
        (-m12 * x + m11 * y) / determinant,
    };
  }
};

inline PresentationTransform
ComposeTransform(const PresentationTransform& outer, const PresentationTransform& inner) noexcept {
  return {
      outer.m11 * inner.m11 + outer.m21 * inner.m12,
      outer.m12 * inner.m11 + outer.m22 * inner.m12,
      outer.m11 * inner.m21 + outer.m21 * inner.m22,
      outer.m12 * inner.m21 + outer.m22 * inner.m22,
      outer.m11 * inner.translate_x + outer.m21 * inner.translate_y + outer.translate_x,
      outer.m12 * inner.translate_x + outer.m22 * inner.translate_y + outer.translate_y,
  };
}

inline PresentationTransform TranslationTransform(Point offset) noexcept {
  return {
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      offset.x,
      offset.y,
  };
}

inline PresentationTransform AroundOriginTransform(const PresentationTransform& linear, Point origin) noexcept {
  return ComposeTransform(
      TranslationTransform(origin),
      ComposeTransform(linear, TranslationTransform({-origin.x, -origin.y}))
  );
}

inline Rect TransformBounds(const PresentationTransform& transform, Rect rect) noexcept {
  const Point top_left = transform.Apply({rect.x, rect.y});
  const Point top_right = transform.Apply({rect.x + rect.width, rect.y});
  const Point bottom_left = transform.Apply({rect.x, rect.y + rect.height});
  const Point bottom_right = transform.Apply({rect.x + rect.width, rect.y + rect.height});
  const float left = std::min({top_left.x, top_right.x, bottom_left.x, bottom_right.x});
  const float right = std::max({top_left.x, top_right.x, bottom_left.x, bottom_right.x});
  const float top = std::min({top_left.y, top_right.y, bottom_left.y, bottom_right.y});
  const float bottom = std::max({top_left.y, top_right.y, bottom_left.y, bottom_right.y});
  return {
      left,
      top,
      right - left,
      bottom - top,
  };
}

struct EnvironmentFrame {
  std::shared_ptr<const EnvironmentFrame> parent;
  EnvironmentValues overrides;
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

class ScrollStateData {
public:
  explicit ScrollStateData(float initial_offset);

  std::shared_ptr<StateCell<ScrollMetrics>> metrics;
  std::weak_ptr<ScrollConnection> connection;
  std::optional<float> pending_offset;
  std::optional<ScrollItemRequest> pending_item;
  bool was_connected = false;
};

enum class NodeKind {
  Text,
  Button,
  Checkbox,
  Switch,
  ProgressCircle,
  Spacer,
  Scope,
  Layout,
  ScrollView,
  VirtualLayout,
};

using ViewKey = std::variant<std::int64_t, std::uint64_t, std::string>;

struct ViewStyle {
  struct FrameConstraints {
    std::optional<float> width;
    std::optional<float> height;
    std::optional<float> min_width;
    std::optional<float> max_width;
    std::optional<float> min_height;
    std::optional<float> max_height;
  };

  EdgeInsets padding;
  FrameConstraints frame;
  std::optional<Color> background;
  std::optional<Color> foreground;
  std::optional<float> font_size;
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
};

struct ViewSpec {
  explicit ViewSpec(NodeKind kind_value) : kind(kind_value) {}

  NodeKind kind;
  TextRole text_role = TextRole::Body;
  std::optional<ViewKey> key;
  std::string text;
  ViewStyle style;
  std::vector<View> children;
  std::function<View()> scope_factory;
  const LayoutDescriptor* layout = nullptr;
  const VirtualLayoutDescriptor* virtual_layout = nullptr;
  VirtualItemSource virtual_items;
  std::unordered_map<std::type_index, std::any> layout_values;
  EventBindings event_bindings;
  std::function<void(const EventBindings&)> activation;
  std::vector<ModifierSpec> modifiers;
  std::shared_ptr<const EnvironmentFrame> environment;
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

struct SavedNodeState {
  NodeKind kind = NodeKind::Layout;
  std::optional<ViewKey> key;
  const LayoutDescriptor* layout = nullptr;
  const VirtualLayoutDescriptor* virtual_layout = nullptr;
  std::optional<StateSlotStorage> state_slots;
  std::vector<SavedNodeState> children;
};

struct VirtualStateCache {
  std::unordered_map<ViewKey, SavedNodeState> keyed;
  std::unordered_map<std::size_t, SavedNodeState> indexed;
};

struct VirtualNodeState {
  VirtualItemSource source;
  std::unordered_map<std::size_t, View> item_views;
  std::vector<std::size_t> child_indices;
  std::vector<VirtualLayoutResult::Placement> placements;
  std::unique_ptr<VirtualStateCache> saved_state;
  bool source_dirty = true;
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
};

struct NodePresentation {
  PresentationTransform local_transform;
  float local_opacity = 1.0F;
  PresentationTransform resolved_transform;
  float resolved_opacity = 1.0F;
};

struct MountedNode final : public huxerui::MountedNode {
  NodeKind kind = NodeKind::Layout;
  std::uint64_t identity = 0;
  std::optional<ViewKey> key;
  std::string text;
  ViewStyle style;
  std::function<View()> scope_factory;
  const LayoutDescriptor* layout = nullptr;
  const VirtualLayoutDescriptor* virtual_layout = nullptr;
  std::unordered_map<std::type_index, std::any> layout_values;
  std::unordered_map<std::type_index, std::any> layout_cache;
  std::vector<LayoutResult::Placement> layout_placements;
  EventBindings event_bindings;
  std::function<void(const EventBindings&)> activation;
  std::shared_ptr<RecomposeScope> recompose_scope;
  Size measured_size;
  Rect frame;
  NodePresentation presentation;
  std::unique_ptr<ScrollNodeState> scroll;
  std::unique_ptr<VirtualNodeState> virtual_state;
  std::vector<NodeExtensionEntry> extensions;
  std::shared_ptr<const EnvironmentFrame> environment;
  bool pointer_events_enabled = true;
  bool local_enabled = true;
  bool enabled = true;
  bool focusable = false;
  bool focused = false;
  bool focus_visible = false;
  bool subtree_has_extensions = true;
  std::vector<std::unique_ptr<MountedNode>> children;

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

  [[nodiscard]] Size MeasuredSizeImpl() const noexcept override {
    return measured_size;
  }

  [[nodiscard]] Rect FrameImpl() const noexcept override {
    return frame;
  }

  [[nodiscard]] Rect PresentationFrameImpl() const noexcept override {
    return TransformBounds(presentation.resolved_transform, frame);
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
    return style.spacing;
  }

  [[nodiscard]] float GrowFactorImpl() const noexcept override {
    return style.grow;
  }

  [[nodiscard]] MainAxisAlignment MainAlignmentImpl() const noexcept override {
    return style.main_axis_alignment;
  }

  [[nodiscard]] CrossAxisAlignment CrossAlignmentImpl() const noexcept override {
    return style.cross_axis_alignment;
  }

  [[nodiscard]] HorizontalAlignment HorizontalAlignmentImpl() const noexcept override {
    return style.horizontal_alignment;
  }

  [[nodiscard]] VerticalAlignment VerticalAlignmentImpl() const noexcept override {
    return style.vertical_alignment;
  }

  [[nodiscard]] const std::any* FindLayoutValue(std::type_index key_value) const noexcept override {
    const auto found = layout_values.find(key_value);
    return found == layout_values.end() ? nullptr : &found->second;
  }

  std::any& EnsureCacheEntry(std::type_index key_value) override {
    return layout_cache[key_value];
  }
};

class ScrollConnection : public std::enable_shared_from_this<ScrollConnection> {
public:
  ScrollConnection(Runtime& runtime, MountedNode& node, std::shared_ptr<ScrollStateData> data);

  [[nodiscard]] const std::shared_ptr<ScrollStateData>& Data() const noexcept {
    return data_;
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
  std::shared_ptr<ScrollStateData> data_;
};

void PrepareScrollState(MountedNode& node, Runtime& runtime);
void CompleteScrollState(MountedNode& node);

class VirtualMeasureSession {
public:
  VirtualMeasureSession(Runtime& runtime, MountedNode& owner);
  ~VirtualMeasureSession();

  VirtualMeasureSession(const VirtualMeasureSession&) = delete;
  VirtualMeasureSession& operator=(const VirtualMeasureSession&) = delete;

  [[nodiscard]] std::size_t ItemCount() const noexcept;
  MountedNode& Item(std::size_t index);
  void Commit(const std::vector<VirtualLayoutResult::Placement>& placements);

private:
  void SaveUnmounted(std::unique_ptr<MountedNode> node, std::size_t index);
  void RestoreOwner() noexcept;

  Runtime* runtime_;
  MountedNode* owner_;
  std::vector<std::unique_ptr<MountedNode>> previous_;
  std::vector<std::size_t> previous_indices_;
  std::vector<std::uint64_t> previous_identities_;
  std::vector<std::unique_ptr<MountedNode>> requested_;
  std::vector<std::size_t> requested_indices_;
  std::unordered_map<std::size_t, std::size_t> requested_positions_;
  std::unordered_set<ViewKey> requested_keys_;
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
  explicit Composer(std::shared_ptr<RecomposeScope> scope, std::shared_ptr<const EnvironmentFrame> environment = {});

  static Composer* Current() noexcept;
  static Composer& RequireCurrent();

  void Observe(const std::shared_ptr<StateCellBase>& cell);
  std::shared_ptr<StateCellBase>
  UseState(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial);
  [[nodiscard]] std::shared_ptr<EventHub> Events() const noexcept;
  [[nodiscard]] const std::shared_ptr<const EnvironmentFrame>& Environment() const noexcept {
    return environment_;
  }

  class EnvironmentGuard {
  public:
    explicit EnvironmentGuard(std::shared_ptr<const EnvironmentFrame> environment);
    ~EnvironmentGuard();

    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

  private:
    Composer* composer_;
    std::shared_ptr<const EnvironmentFrame> previous_;
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
  std::shared_ptr<const EnvironmentFrame> environment_;
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

struct PointerSession {
  std::optional<std::uint64_t> target_identity;
  std::vector<std::uint64_t> scroll_chain;
  Point down_position;
  Point last_position;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  double velocity_sample_timestamp = 0.0;
  float scroll_velocity = 0.0F;
  bool has_velocity_sample = false;
  std::optional<Axis> drag_axis;
  std::size_t active_scroll = 0;
  std::optional<std::uint64_t> active_scroll_node;
  std::optional<NodeExtensionHandle> extension_capture;
  std::vector<NodeExtensionHandle> extension_observers;
};

struct LayerEntry {
  LayerId id = 0;
  LayerOptions options;
  ViewFactory content;
  std::shared_ptr<const EnvironmentFrame> environment;
};

} // namespace huxerui::detail

namespace huxerui {

struct Runtime::State {
  State(
      RootFactory root_factory,
      PlatformHost* platform,
      std::shared_ptr<detail::RecomposeScope> root_scope,
      LayerController layer_controller
  )
      : root_factory_(root_factory), platform_(platform), root_scope_(std::move(root_scope)),
        layer_controller_(std::move(layer_controller)) {}

  RootFactory root_factory_;
  PlatformHost* platform_;
  Size viewport_;
  std::shared_ptr<detail::RecomposeScope> root_scope_;
  LayerController layer_controller_;
  std::vector<std::shared_ptr<void>> root_services_;
  EnvironmentValues root_environment_values_;
  std::unordered_set<std::type_index> root_service_types_;
  std::shared_ptr<const detail::EnvironmentFrame> root_environment_;
  std::vector<detail::LayerEntry> layers_;
  std::unique_ptr<detail::MountedNode> mounted_root_;
  DisplayList display_list_;
  bool composition_dirty_ = true;
  bool composing_root_ = false;
  bool layer_snapshot_taken_ = false;
  bool extension_tree_dirty_ = true;
  bool scroll_motion_active_ = false;
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

Size MeasureNode(MountedNode& node, const Constraints& constraints, PlatformHost& platform, Runtime& runtime);
void LayoutNode(MountedNode& node, Point origin);
void PaintNode(MountedNode& node, DisplayList& display_list);
bool BuildPointerRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route);
MountedNode* HitTestPointer(MountedNode& node, Point position);
std::optional<ScrollBarGeometry> ResolveScrollBarGeometry(const MountedNode& node);
bool IsScrollContainer(const MountedNode& node) noexcept;
Axis ScrollAxis(const MountedNode& node) noexcept;
bool CanScrollNode(const MountedNode& node, float delta);
float ScrollNodeBy(MountedNode& node, float delta);

struct ScrollEventResult {
  std::vector<MountedNode*> scroll_chain;
  std::vector<MountedNode*> scrolled_nodes;
};

ScrollEventResult ApplyScrollEvent(MountedNode& node, const ScrollEvent& event);
bool AdvanceMountedNodeFrame(MountedNode& node, const FrameInfo& frame);

bool IsVirtualLayoutNode(const MountedNode& node) noexcept;

#if !defined(__ANDROID__)
int RunPlatformApp(AppDefinition definition);
#endif

} // namespace huxerui::detail
