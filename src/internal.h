#pragma once

#include <algorithm>
#include <any>
#include <atomic>
#include <cmath>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/display_list.h>
#include <huxerui/event.h>
#include <huxerui/environment.h>
#include <huxerui/platform.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace huxerui::detail {

class Runtime;
struct MountedNode;
class ScrollConnection;

enum class AnimationEasing {
  Linear,
  EaseOut,
};

template <std::floating_point T> class AnimatedValue {
public:
  explicit AnimatedValue(T value = {}) noexcept
      : value_(value), start_(value), target_(value) {}

  [[nodiscard]] T Value() const noexcept { return value_; }
  [[nodiscard]] T Target() const noexcept { return target_; }
  [[nodiscard]] bool IsRunning() const noexcept { return running_; }

  void Set(T value) noexcept {
    value_ = value;
    start_ = value;
    target_ = value;
    running_ = false;
  }

  void AnimateTo(T target, double timestamp, double duration,
                 AnimationEasing easing = AnimationEasing::EaseOut) noexcept {
    Advance(timestamp);
    if (target == target_ && (running_ || value_ == target)) {
      return;
    }
    if (!std::isfinite(duration) || duration <= 0.0) {
      Set(target);
      return;
    }
    start_ = value_;
    target_ = target;
    start_time_ = timestamp;
    duration_ = duration;
    easing_ = easing;
    running_ = true;
  }

  bool Advance(double timestamp) noexcept {
    if (!running_) {
      return false;
    }
    const double progress =
        std::clamp((timestamp - start_time_) / duration_, 0.0, 1.0);
    double eased = progress;
    if (easing_ == AnimationEasing::EaseOut) {
      const double inverse = 1.0 - progress;
      eased = 1.0 - inverse * inverse * inverse;
    }
    value_ = static_cast<T>(
        start_ + (target_ - start_) * static_cast<T>(eased));
    if (progress >= 1.0) {
      value_ = target_;
      running_ = false;
    }
    return running_;
  }

private:
  T value_;
  T start_;
  T target_;
  double start_time_ = 0.0;
  double duration_ = 0.0;
  AnimationEasing easing_ = AnimationEasing::EaseOut;
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
  EdgeInsets padding;
  std::optional<float> width;
  std::optional<float> height;
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
  const LayoutDescriptor *layout = nullptr;
  const VirtualLayoutDescriptor *virtual_layout = nullptr;
  VirtualItemSource virtual_items;
  std::unordered_map<std::type_index, std::any> layout_values;
  EventBindings event_bindings;
  std::function<void(const EventBindings &)> activation;
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

  bool operator==(const StateSlotKey &) const = default;
};

struct StateSlotKeyHash {
  std::size_t operator()(const StateSlotKey &key) const noexcept;
};

struct StateSlotStorage {
  std::unordered_map<StateSlotKey, std::shared_ptr<StateCellBase>,
                     StateSlotKeyHash>
      slots;
};

struct SavedNodeState {
  NodeKind kind = NodeKind::Layout;
  std::optional<ViewKey> key;
  const LayoutDescriptor *layout = nullptr;
  const VirtualLayoutDescriptor *virtual_layout = nullptr;
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
  Axis axis = Axis::Vertical;
  Size content_size;
  bool source_dirty = true;
};

struct MountedModifierEntry {
  const ModifierDescriptor *descriptor = nullptr;
  std::unique_ptr<huxerui::MountedModifier> mounted;
};

struct MountedNode final : public huxerui::MountedNode {
  NodeKind kind = NodeKind::Layout;
  std::uint64_t identity = 0;
  std::optional<ViewKey> key;
  std::string text;
  ViewStyle style;
  std::function<View()> scope_factory;
  const LayoutDescriptor *layout = nullptr;
  const VirtualLayoutDescriptor *virtual_layout = nullptr;
  std::unordered_map<std::type_index, std::any> layout_values;
  std::unordered_map<std::type_index, std::any> layout_cache;
  std::vector<LayoutResult::Placement> layout_placements;
  EventBindings event_bindings;
  std::function<void(const EventBindings &)> activation;
  std::shared_ptr<RecomposeScope> recompose_scope;
  Size measured_size;
  Rect frame;
  Point presentation_offset;
  float presentation_opacity = 1.0F;
  Point resolved_presentation_offset;
  float resolved_presentation_opacity = 1.0F;
  float scroll_offset_y = 0.0F;
  float scroll_offset_x = 0.0F;
  float scroll_content_height = 0.0F;
  float scroll_content_width = 0.0F;
  std::shared_ptr<ScrollConnection> scroll_connection;
  std::unique_ptr<VirtualNodeState> virtual_state;
  std::vector<MountedModifierEntry> modifiers;
  std::shared_ptr<const EnvironmentFrame> environment;
  bool pointer_events_enabled = true;
  bool local_enabled = true;
  bool enabled = true;
  bool focusable = false;
  bool focused = false;
  bool subtree_has_mounted_modifiers = true;
  std::vector<std::unique_ptr<MountedNode>> children;

protected:
  [[nodiscard]] std::size_t ChildCountImpl() const noexcept override {
    return children.size();
  }

  MountedNode &ChildAtImpl(std::size_t index) override {
    return *children[index];
  }

  const MountedNode &ChildAtImpl(std::size_t index) const override {
    return *children[index];
  }

  [[nodiscard]] Size MeasuredSizeImpl() const noexcept override {
    return measured_size;
  }

  [[nodiscard]] Rect FrameImpl() const noexcept override {
    return frame;
  }

  [[nodiscard]] Rect PresentationFrameImpl() const noexcept override {
    return {
        frame.x + resolved_presentation_offset.x,
        frame.y + resolved_presentation_offset.y,
        frame.width,
        frame.height,
    };
  }

  [[nodiscard]] float PresentationOpacityImpl() const noexcept override {
    return resolved_presentation_opacity;
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

  [[nodiscard]] CrossAxisAlignment
  CrossAlignmentImpl() const noexcept override {
    return style.cross_axis_alignment;
  }

  [[nodiscard]] HorizontalAlignment
  HorizontalAlignmentImpl() const noexcept override {
    return style.horizontal_alignment;
  }

  [[nodiscard]] VerticalAlignment
  VerticalAlignmentImpl() const noexcept override {
    return style.vertical_alignment;
  }

  [[nodiscard]] const std::any *
  FindLayoutValue(std::type_index key_value) const noexcept override {
    const auto found = layout_values.find(key_value);
    return found == layout_values.end() ? nullptr : &found->second;
  }

  std::any &EnsureCacheEntry(std::type_index key_value) override {
    return layout_cache[key_value];
  }
};

class ScrollConnection : public std::enable_shared_from_this<ScrollConnection> {
public:
  ScrollConnection(Runtime &runtime, MountedNode &node,
                   std::shared_ptr<ScrollStateData> data);

  [[nodiscard]] const std::shared_ptr<ScrollStateData> &Data() const noexcept {
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

  Runtime *runtime_;
  MountedNode *node_;
  std::shared_ptr<ScrollStateData> data_;
};

void PrepareScrollState(MountedNode &node, Runtime &runtime);
void CompleteScrollState(MountedNode &node);

class VirtualMeasureSession {
public:
  VirtualMeasureSession(Runtime &runtime, MountedNode &owner);
  ~VirtualMeasureSession();

  VirtualMeasureSession(const VirtualMeasureSession &) = delete;
  VirtualMeasureSession &operator=(const VirtualMeasureSession &) = delete;

  [[nodiscard]] std::size_t ItemCount() const noexcept;
  MountedNode &Item(std::size_t index);
  void Commit(const std::vector<VirtualLayoutResult::Placement> &placements);

private:
  void SaveUnmounted(std::unique_ptr<MountedNode> node, std::size_t index);
  void RestoreOwner() noexcept;

  Runtime *runtime_;
  MountedNode *owner_;
  std::vector<std::unique_ptr<MountedNode>> previous_;
  std::vector<std::size_t> previous_indices_;
  std::vector<std::uint64_t> previous_identities_;
  std::vector<std::unique_ptr<MountedNode>> requested_;
  std::vector<std::size_t> requested_indices_;
  std::unordered_map<std::size_t, std::size_t> requested_positions_;
  std::unordered_set<ViewKey> requested_keys_;
  bool committed_ = false;
};

class RecomposeScope final
    : public std::enable_shared_from_this<RecomposeScope> {
public:
  RecomposeScope(Runtime &runtime, std::uint64_t id,
                 StateSlotStorage state_slots = {});
  ~RecomposeScope();

  void BeginComposition();
  void EndComposition();
  void Observe(const std::shared_ptr<StateCellBase> &cell);
  void Invalidate();
  void SetEventBindings(EventBindings bindings);

  [[nodiscard]] std::shared_ptr<EventHub> Events() const noexcept {
    return event_hub_;
  }

  std::shared_ptr<StateCellBase>
  UseState(std::type_index type, const std::source_location &location,
           std::shared_ptr<StateCellBase> initial);

  [[nodiscard]] std::uint64_t Id() const noexcept { return id_; }

  [[nodiscard]] bool IsDirty() const noexcept { return dirty_; }

  StateSlotStorage TakeStateSlots() noexcept { return std::move(state_slots_); }

private:
  Runtime *runtime_;
  std::uint64_t id_;
  bool dirty_ = true;
  StateSlotStorage state_slots_;
  std::unordered_set<StateSlotKey, StateSlotKeyHash> touched_state_slots_;
  std::unordered_map<StateSlotKey, std::uint32_t, StateSlotKeyHash>
      state_slot_occurrences_;
  std::unordered_map<StateCellBase *, std::weak_ptr<StateCellBase>>
      dependencies_;
  std::shared_ptr<EventHub> event_hub_ = std::make_shared<EventHub>();
};

class Composer {
public:
  explicit Composer(
      std::shared_ptr<RecomposeScope> scope,
      std::shared_ptr<const EnvironmentFrame> environment = {});

  static Composer *Current() noexcept;
  static Composer &RequireCurrent();

  void Observe(const std::shared_ptr<StateCellBase> &cell);
  std::shared_ptr<StateCellBase>
  UseState(std::type_index type, const std::source_location &location,
           std::shared_ptr<StateCellBase> initial);
  [[nodiscard]] std::shared_ptr<EventHub> Events() const noexcept;
  [[nodiscard]] const std::shared_ptr<const EnvironmentFrame> &
  Environment() const noexcept {
    return environment_;
  }

  class EnvironmentGuard {
  public:
    explicit EnvironmentGuard(
        std::shared_ptr<const EnvironmentFrame> environment);
    ~EnvironmentGuard();

    EnvironmentGuard(const EnvironmentGuard &) = delete;
    EnvironmentGuard &operator=(const EnvironmentGuard &) = delete;

  private:
    Composer *composer_;
    std::shared_ptr<const EnvironmentFrame> previous_;
  };

  class Guard {
  public:
    explicit Guard(Composer &composer);
    ~Guard();

    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

  private:
    Composer *previous_;
  };

private:
  static thread_local Composer *current_;
  std::shared_ptr<RecomposeScope> scope_;
  std::shared_ptr<const EnvironmentFrame> environment_;
};

class PlatformHost {
public:
  virtual ~PlatformHost() = default;

  virtual int Run(Runtime &runtime, const AppOptions &options) = 0;
  virtual void RequestFrame(double delay_seconds) = 0;
  virtual double Now() const noexcept = 0;
  virtual TextService &Text() = 0;
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

struct ModifierPointerCapture {
  std::uint64_t node_identity = 0;
  std::size_t modifier_index = 0;
  const ModifierDescriptor *descriptor = nullptr;

  bool operator==(const ModifierPointerCapture &) const = default;
};

struct PointerSession {
  std::optional<std::uint64_t> target_identity;
  std::vector<std::uint64_t> scroll_chain;
  Point down_position;
  Point last_position;
  std::optional<Axis> drag_axis;
  std::size_t active_scroll = 0;
  std::optional<std::uint64_t> active_scroll_node;
  std::optional<ModifierPointerCapture> modifier_capture;
  std::vector<ModifierPointerCapture> modifier_observers;
};

struct LayerEntry {
  LayerId id = 0;
  LayerOptions options;
  ViewFactory content;
  std::shared_ptr<const EnvironmentFrame> environment;
};

class Runtime {
public:
  Runtime(RootFactory root_factory, PlatformHost &platform,
          AppOptions options = {});
  ~Runtime();

  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  void SetViewport(Size viewport);
  const DisplayList &BuildFrame();
  const DisplayList &BuildFrame(FrameInfo frame);
  void HandlePointerEvent(const PointerEvent &event);
  void HandleScrollEvent(const ScrollEvent &event);
  void HandleKeyEvent(const KeyEvent &event);
  void InvalidateRoot();
  void InvalidateScope(std::uint64_t scope_id);

  [[nodiscard]] const MountedNode *RootNode() const noexcept {
    return has_application_root_ && mounted_root_ &&
                   !mounted_root_->children.empty()
               ? mounted_root_->children.front().get()
               : nullptr;
  }

private:
  LayerId AttachLayer(LayerOptions options, ViewFactory content,
                      std::shared_ptr<const EnvironmentFrame> environment);
  bool UpdateLayer(LayerId id, ViewFactory content);
  bool UpdateLayer(
      LayerId id, LayerOptions options, ViewFactory content);
  bool DismissLayer(LayerId id);
  void RequestFrame();
  void RequestFrameAfter(double delay_seconds);
  void NotifyScrollActivity(MountedNode &node);
  void UpdateHoveredModifier(Point position);
  void RefreshInteractionTree();
  [[nodiscard]] std::optional<LayerId>
  ActiveModalLayerId() const;
  MountedNode *ActiveModalFocusRoot();
  void SetFocusedNode(
      std::optional<std::uint64_t> identity);
  void MoveFocus(bool reverse);
  bool UpdateMountedModifiers(
      MountedNode &node, const FrameInfo &frame,
      bool &needs_frame, std::optional<double> &next_wakeup,
      bool rebuild_cache);
  void ComposeRoot();
  void ComposeScope(MountedNode &mounted);
  void RecomposeDirtyScopes(MountedNode &mounted);
  void Reconcile(std::unique_ptr<MountedNode> &mounted,
                 const std::shared_ptr<ViewSpec> &incoming);
  std::unique_ptr<MountedNode> Mount(const std::shared_ptr<ViewSpec> &incoming);
  void ReconcileChildren(MountedNode &mounted,
                         const std::vector<View> &incoming_children);
  SavedNodeState SaveNodeState(MountedNode &mounted);
  void RestoreNodeState(MountedNode &mounted, SavedNodeState &saved);

  RootFactory root_factory_;
  PlatformHost *platform_;
  Size viewport_;
  std::shared_ptr<RecomposeScope> root_scope_;
  LayerController layer_controller_;
  std::vector<std::shared_ptr<void>> root_services_;
  EnvironmentValues root_environment_values_;
  std::unordered_set<std::type_index> root_service_types_;
  std::shared_ptr<const EnvironmentFrame> root_environment_;
  std::vector<LayerEntry> layers_;
  std::unique_ptr<MountedNode> mounted_root_;
  DisplayList display_list_;
  bool composition_dirty_ = true;
  bool composing_root_ = false;
  bool modifier_tree_dirty_ = true;
  bool frame_requested_ = false;
  double frame_request_deadline_ = 0.0;
  std::optional<double> previous_frame_timestamp_;
  std::uint64_t next_node_identity_ = 1;
  std::uint64_t next_scope_identity_ = 2;
  LayerId next_layer_id_ = 1;
  bool has_application_root_ = false;
  std::optional<ModifierPointerCapture> hovered_modifier_;
  std::unordered_map<std::int64_t, PointerSession> pointer_sessions_;
  std::optional<std::uint64_t> focused_node_identity_;
  std::optional<std::uint64_t> keyboard_activation_identity_;
  std::optional<LayerId> active_modal_focus_layer_;
  std::unordered_map<
      LayerId, std::optional<std::uint64_t>>
      modal_focus_restore_;

  friend class VirtualMeasureSession;
  friend class ScrollConnection;
  friend class huxerui::LayerController;
};

Size MeasureNode(MountedNode &node, const Constraints &constraints,
                 TextService &text_service, Runtime &runtime);
void LayoutNode(MountedNode &node, Point origin);
void PaintNode(MountedNode &node, DisplayList &display_list);
bool BuildPointerRoute(MountedNode &node, Point position,
                       std::vector<MountedNode *> &route);
MountedNode *HitTestPointer(MountedNode &node, Point position);
std::optional<ScrollBarGeometry>
ResolveScrollBarGeometry(const MountedNode &node);
bool CanScrollNode(const MountedNode &node, float delta);
float ScrollNodeBy(MountedNode &node, float delta);
MountedNode *ScrollNode(MountedNode &node, const ScrollEvent &event);

bool IsVirtualLayoutNode(const MountedNode &node) noexcept;

std::unique_ptr<PlatformHost> CreateDefaultPlatformHost();

} // namespace huxerui::detail
