#pragma once

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
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
#include <huxerui/indication.h>
#include <huxerui/gesture.h>
#include <huxerui/lifecycle.h>
#include <huxerui/platform_registry.h>
#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/task.h>
#include <huxerui/view.h>

#include "geometry_internal.h"
#include "platform_registry_internal.h"

namespace huxerui::detail {

struct NodeExtensionHandle {
  std::uint64_t node_identity = 0;
  std::size_t extension_index = 0;
  const ModifierDescriptor* descriptor = nullptr;

  bool operator==(const NodeExtensionHandle&) const = default;
};

struct PointerHoverState {
  std::int64_t pointer_id = 0;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  Point window_position;
  std::vector<std::uint64_t> event_nodes;
  std::vector<NodeExtensionHandle> extensions;
};

} // namespace huxerui::detail

#include "gesture_internal.h"
#include "semantics_internal.h"

namespace huxerui::detail {

struct MountedNode;
class ScrollConnection;
class IndicationState;
class AppResources;
class TaskDelayScheduler;
struct WindowState;
class WindowService;

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

struct ScrollBarBinding {
  using Value = ScrollBarStyle;
};

struct GrowFactorBinding {
  using Value = float;
};

struct DividerAxisBinding {
  using Value = Axis;
};

struct DividerThicknessBinding {
  using Value = float;
};

struct ScrollAxisBinding {
  using Value = Axis;
};

struct ScrollFillViewport {
  using Value = bool;
};

struct IndexedPagesSelection {
  using Value = std::size_t;
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
  // Placement is non-null for every attached entry and may be updated without rebuilding its content scope.
  std::shared_ptr<LayerPlacement> placement;
  std::shared_ptr<LayerTransitionState> transition;
};

inline std::optional<double> EarliestWakeAfter(std::optional<double> first, std::optional<double> second) noexcept {
  if (!first.has_value()) {
    return second;
  }
  if (!second.has_value()) {
    return first;
  }
  return std::min(*first, *second);
}

inline Color InterpolateColor(Color from, Color to, float progress) noexcept {
  const float value = std::clamp(progress, 0.0F, 1.0F);
  return {
      from.red + (to.red - from.red) * value,
      from.green + (to.green - from.green) * value,
      from.blue + (to.blue - from.blue) * value,
      from.alpha + (to.alpha - from.alpha) * value,
  };
}

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
  IconButton,
  Chip,
  Divider,
  TextField,
  Checkbox,
  RadioButton,
  Switch,
  ProgressCircle,
  ProgressBar,
  Slider,
  Image,
  PlatformView,
  Canvas,
  Spacer,
  SelectionArea,
  Layout,
  ScrollView,
  VirtualLayout,
  Scope,
  Environment,
};

struct ToggleLayoutMetrics {
  using Value = ToggleLayoutMetrics;

  Size visual_size;
  Size interactive_size;
  float label_spacing = 0.0F;

  bool operator==(const ToggleLayoutMetrics&) const = default;
};

struct LabelContentMetrics {
  using Value = LabelContentMetrics;

  Size icon_size;
  float icon_spacing = 0.0F;
  bool show_label = true;

  bool operator==(const LabelContentMetrics&) const = default;
};

struct LabelLayoutCache {
  TextLayoutMetrics text;
};

using ViewKey = std::variant<std::int64_t, std::uint64_t, std::string>;

struct ViewProperties {
  // Declarative padding is copied unchanged from ViewSpec into MountedNode; dynamic safe-area insets are never folded
  // back into this value.
  EdgeInsets padding;
  // This declaration selects which inherited safe-area edges measurement adds to the node's resolved padding.
  std::optional<SafeAreaPadding> safe_area_padding;
  // This marker participates only in system window hit testing and has no layout, paint, or retained lifecycle state.
  bool window_drag_region = false;
  // Cursor resolution reads this declaration from the final pointer hit route without affecting layout or paint.
  std::optional<PointerCursorKind> pointer_cursor;
  Frame frame;
  std::optional<VisualFill> background;
  std::optional<VisualFill> disabled_background;
  std::optional<Border> border;
  std::optional<Border> disabled_border;
  std::optional<Shadow> shadow;
  TextStyle text_style;
  TextLayoutOptions text_layout_options;
  std::optional<Color> disabled_foreground;
  CornerRadii corner_radii;
  // The Theme supplies the ordinary value; specialized controls may override or suppress the generic focus ring.
  FocusRing focus_ring;
  bool clip_children = false;
  float spacing = 0.0F;
  MainAxisAlignment main_axis_alignment = MainAxisAlignment::Start;
  CrossAxisAlignment cross_axis_alignment = CrossAxisAlignment::Start;
  HorizontalAlignment horizontal_alignment = HorizontalAlignment::Start;
  VerticalAlignment vertical_alignment = VerticalAlignment::Start;
  float disabled_opacity = 0.42F;
  // Runtime resolves this declaration against final window-edge geometry after layout and presentation transforms.
  std::optional<SystemBarsAppearance> system_bars_appearance;

  bool operator==(const ViewProperties&) const = default;

  // Reconciliation compares the inputs consumed by layout, content paint, and foreground paint independently.
  // New property fields must participate in every projection whose stage reads them.
  [[nodiscard]] bool LayoutEquals(const ViewProperties& other) const {
    return padding == other.padding && safe_area_padding == other.safe_area_padding && frame == other.frame &&
           text_style.font == other.text_style.font && text_layout_options == other.text_layout_options &&
           spacing == other.spacing && main_axis_alignment == other.main_axis_alignment &&
           cross_axis_alignment == other.cross_axis_alignment && horizontal_alignment == other.horizontal_alignment &&
           vertical_alignment == other.vertical_alignment;
  }

  [[nodiscard]] bool ContentPaintEquals(const ViewProperties& other) const {
    return padding == other.padding && safe_area_padding == other.safe_area_padding && background == other.background &&
           disabled_background == other.disabled_background && border == other.border &&
           disabled_border == other.disabled_border && shadow == other.shadow && text_style == other.text_style &&
           text_layout_options == other.text_layout_options &&
           disabled_foreground == other.disabled_foreground && corner_radii == other.corner_radii;
  }

  [[nodiscard]] bool ForegroundPaintEquals(const ViewProperties& other) const {
    return corner_radii == other.corner_radii && focus_ring == other.focus_ring;
  }
};

struct ImageProperties {
  std::variant<ImageAsset, VectorAsset, ExternalTexture, ImageResource> source;
  ImageFit fit = ImageFit::Contain;
  HorizontalAlignment horizontal_alignment = HorizontalAlignment::Center;
  VerticalAlignment vertical_alignment = VerticalAlignment::Center;
  ImageSampling sampling = ImageSampling::Linear;
  std::optional<Color> tint;

  [[nodiscard]] Size IntrinsicSize() const noexcept {
    return std::visit(
        [](const auto& value) {
          using Image = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<Image, ImageResource>) {
            return Size{};
          } else {
            return value.IntrinsicSize();
          }
        },
        source
    );
  }

  [[nodiscard]] bool IsVector() const noexcept {
    return std::holds_alternative<VectorAsset>(source);
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return std::visit(
        [](const auto& value) {
          using Image = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<Image, ImageResource>) {
            return true;
          } else {
            return value.HasValue();
          }
        },
        source
    );
  }

  void SetResolvedAsset(std::variant<ImageAsset, VectorAsset> value) {
    std::visit([this](auto&& image) { source = std::forward<decltype(image)>(image); }, std::move(value));
  }

  void SetImage(ImageVariant value) {
    std::visit([this](auto&& image) { source = std::forward<decltype(image)>(image); }, std::move(value));
  }

  // Only intrinsic logical size affects measurement; image contents, fit, alignment, and sampling are paint inputs.
  [[nodiscard]] bool LayoutEquals(const ImageProperties& other) const noexcept {
    return IntrinsicSize() == other.IntrinsicSize();
  }

  bool operator==(const ImageProperties&) const = default;
};

struct PlatformViewDeclaration {
  std::string type;
  PlatformValue properties;
  PlatformValue controller;
  std::vector<PlatformEventDescriptor> events;
};

inline bool PlatformViewPropertiesEqual(
    const std::shared_ptr<const PlatformViewDeclaration>& left,
    const std::shared_ptr<const PlatformViewDeclaration>& right
) {
  return left == right || (left && right && left->type == right->type && left->properties == right->properties);
}

inline bool PlatformViewControllerEqual(const std::shared_ptr<const PlatformViewDeclaration>& left,
                                        const std::shared_ptr<const PlatformViewDeclaration>& right) {
  return left == right || (left && right && left->type == right->type && left->controller == right->controller);
}

// ViewSpec is View's transient copy-on-write declaration. NodeKind selects the component-specific payloads;
// fields unrelated to that kind stay at their defaults and are ignored by the corresponding Runtime stages.
using ViewDefaults = void (*)(ViewSpec&, const std::shared_ptr<const Environment>&);

struct ViewSpec {
  explicit ViewSpec(NodeKind kind_value) : kind(kind_value) {}

  NodeKind kind;
  TextRole text_role = TextRole::Body;
  std::optional<ViewKey> key;
  StringVariant text;
  ViewProperties properties;
  SemanticPatch component_semantics;
  std::optional<SemanticPatch> author_semantics;
  std::vector<View> children;
  std::function<View()> scope_factory;
  CanvasPainter canvas_painter;
  ImageProperties image_properties;
  std::shared_ptr<const PlatformViewDeclaration> platform_view;
  const LayoutDescriptor* layout_descriptor = nullptr;
  const VirtualLayoutDescriptor* virtual_layout_descriptor = nullptr;
  ViewItemSource virtual_items;
  std::unordered_map<std::type_index, ErasedLayoutValue> layout_values;
  EventBindings event_bindings;
  std::function<void(const EventBindings&)> activation;
  ViewDefaults defaults = nullptr;
  // Modifiers remain one ordered declaration sequence; each mounted phase selects the capabilities it consumes.
  std::vector<ModifierSpec> modifiers;
  // Component Theme resolution supplies this value to a retained DefaultIndication declaration.
  std::optional<Indication> default_indication;
  // Environment nodes retain only their local declaration values; Runtime attaches the inherited parent at mount.
  std::optional<Environment> local_environment;
  std::optional<bool> chip_selection;
  bool pointer_events_enabled = true;
  bool local_enabled = true;
  bool focusable = false;
  // The highest painted enabled trap confines keyboard and pointer focus to its subtree.
  bool trap_focus = false;
};

std::shared_ptr<ViewSpec> MakeScopeSpec(std::function<View()> factory);
ViewSpec CompileViewSpec(const ViewSpec& declaration, const std::shared_ptr<const Environment>& environment,
                         AppResources& resources);

struct CompositionSlotKey {
  std::string file;
  std::string function;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::uint32_t occurrence = 0;

  bool operator==(const CompositionSlotKey&) const = default;
};

struct CompositionSlotKeyHash {
  std::size_t operator()(const CompositionSlotKey& key) const noexcept;
};

struct StateSlotStorage {
  std::unordered_map<CompositionSlotKey, std::shared_ptr<StateCellBase>, CompositionSlotKeyHash> slots;
};

struct LifecycleSlot {
  std::vector<LifecycleDependency> dependencies;
  LifecycleCleanup cleanup;
  bool retained_for_commit = false;
};

struct LifecycleDeclaration {
  CompositionSlotKey key;
  LifecycleSetup setup;
  std::vector<LifecycleDependency> dependencies;
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

struct VirtualCollectionSemantics {
  SemanticRole role = SemanticRole::Generic;
  SemanticRole item_role = SemanticRole::Generic;
  SemanticCollection collection;

  bool operator==(const VirtualCollectionSemantics&) const = default;
};

class VirtualItemDependencyCapture {
public:
  VirtualItemDependencyCapture(std::shared_ptr<RecomposeScope> scope, std::uint64_t owner_identity);
  ~VirtualItemDependencyCapture();

  VirtualItemDependencyCapture(const VirtualItemDependencyCapture&) = delete;
  VirtualItemDependencyCapture& operator=(const VirtualItemDependencyCapture&) = delete;

  void Clear();
  void Observe(const std::shared_ptr<CompositionDependency>& dependency);

  class Guard {
  public:
    explicit Guard(VirtualItemDependencyCapture& capture);
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

  private:
    VirtualItemDependencyCapture* previous_;
  };

  static VirtualItemDependencyCapture* Current() noexcept;
  [[nodiscard]] std::shared_ptr<RecomposeScope> Scope() const noexcept;

private:
  static thread_local VirtualItemDependencyCapture* current_;
  std::weak_ptr<RecomposeScope> scope_;
  std::uint64_t owner_identity_ = 0;
};

struct VirtualNodeState {
  ViewItemSource source;
  std::unique_ptr<VirtualItemDependencyCapture> dependency_capture;
  std::unordered_map<std::size_t, View> item_declarations;
  std::vector<std::size_t> realized_indices;
  std::vector<VirtualLayoutResult::Placement> realized_placements;
  std::optional<VirtualCollectionSemantics> collection_semantics;
  std::unique_ptr<VirtualItemStateCache> item_state_cache;
  bool viewport_dirty = true;
  // A parent may measure the same child repeatedly; only the candidate selected for layout commits its correction.
  std::optional<float> pending_scroll_offset;
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
  std::optional<Rect> viewport_override;
  ScrollMotion motion;
  std::shared_ptr<ScrollConnection> connection;
};

struct NodeExtensionEntry {
  const ModifierDescriptor* descriptor = nullptr;
  std::unique_ptr<huxerui::NodeExtension> extension;
  std::shared_ptr<const void> value;
  bool interaction_sync_pending = true;
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
  SemanticPatch component_semantics;
  std::optional<SemanticPatch> author_semantics;
  std::function<View()> scope_factory;
  CanvasPainter canvas_painter;
  ImageProperties image_properties;
  std::shared_ptr<const PlatformViewDeclaration> platform_view;
  std::uint64_t platform_view_properties_revision = 0;
  std::uint64_t platform_view_controller_revision = 0;
  const LayoutDescriptor* layout_descriptor = nullptr;
  const VirtualLayoutDescriptor* virtual_layout_descriptor = nullptr;
  std::unordered_map<std::type_index, ErasedLayoutValue> layout_values;
  std::unordered_map<std::type_index, std::any> layout_cache;
  std::vector<LayoutResult::Placement> layout_placements;
  EventBindings event_bindings;
  std::function<void(const EventBindings&)> activation;
  std::shared_ptr<RecomposeScope> recompose_scope;
  // Constraints, unconsumed safe area, and native title-bar geometry jointly identify a reusable measurement result.
  std::optional<Constraints> measured_constraints;
  std::optional<EdgeInsets> measured_safe_area;
  std::optional<WindowTitleBarMetrics> measured_title_bar;
  // Measurement derives this from properties.padding plus the safe-area edges consumed by this node. Layout and paint
  // use the resolved value while the declarative properties remain stable across window-inset changes.
  EdgeInsets resolved_padding;
  Size measured_size;
  // Bounds stay at the node-local origin; layout_offset places the node in its parent's local coordinates.
  Rect bounds;
  Point layout_offset;
  NodePresentation presentation;
  RenderNode render_node;
  std::uint64_t measure_revision = 0;
  std::uint64_t layout_revision = 0;
  SemanticNodeId semantic_identity = 0;
  std::unordered_map<VirtualSemanticKey, SemanticNodeId, VirtualSemanticKeyHash> virtual_semantic_identities;
  // Measurement, descendant placement, content recording, and foreground recording are invalidated independently.
  bool measure_dirty = true;
  bool layout_dirty = true;
  bool content_paint_dirty = true;
  bool foreground_paint_dirty = true;
  // RenderNode children are retained raw pointers, so a mounted child-structure change must be synchronized even when
  // this subtree is not currently painted.
  bool render_structure_dirty = true;
  // This records whether the immediate parent committed this node in its LayoutResult placements. Effective
  // participation also requires every ancestor to participate and is intentionally distinct from visual visibility.
  bool participates_in_layout = true;
  std::unique_ptr<ScrollNodeState> scroll_state;
  std::unique_ptr<VirtualNodeState> virtual_state;
  std::vector<NodeExtensionEntry> extensions;
  std::shared_ptr<const Environment> environment;
  std::shared_ptr<Environment> owned_environment;
  // Cached when the compiled declaration is applied so animation frames do not repeatedly resolve ThemeSpec.
  bool reduced_motion = false;
  bool pointer_events_enabled = true;
  bool local_enabled = true;
  InteractionState interaction;
  std::uint32_t active_press_count = 0;
  // True only for the node that first disables an otherwise enabled subtree. Stateful controls use their disabled
  // colors at this boundary; inherited disabled descendants remain visually enabled under the boundary group opacity.
  bool applies_disabled_appearance = false;
  // Declarative border and radii remain stable targets; these fields hold the current animated surface presentation.
  std::optional<Border> resolved_border;
  CornerRadii resolved_corner_radii;
  // Re-resolved during geometry preparation from static indication geometry or a component's dynamic control bounds.
  std::optional<Rect> indication_bounds_override;
  bool focusable = false;
  bool trap_focus = false;
  bool subtree_has_extensions = true;
  std::vector<std::unique_ptr<MountedNode>> children;

  [[nodiscard]] Rect ContentBounds() const noexcept {
    return {
        bounds.x + resolved_padding.left,
        bounds.y + resolved_padding.top,
        std::max(0.0F, bounds.width - resolved_padding.Horizontal()),
        std::max(0.0F, bounds.height - resolved_padding.Vertical()),
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
    return interaction.enabled;
  }

  [[nodiscard]] bool IsFocusedImpl() const noexcept override {
    return interaction.focused;
  }

  [[nodiscard]] const InteractionState& InteractionImpl() const noexcept override {
    return interaction;
  }

  [[nodiscard]] float SpacingImpl() const noexcept override {
    return properties.spacing;
  }

  [[nodiscard]] float GrowFactorImpl() const noexcept override {
    const float* factor = LayoutValue<GrowFactorBinding>();
    return factor == nullptr ? 0.0F : *factor;
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
    if (found != layout_values.end()) {
      return &found->second.value;
    }
    if ((kind == NodeKind::Scope || kind == NodeKind::Environment) && children.size() == 1) {
      return children.front()->FindLayoutValue(key_value);
    }
    return nullptr;
  }

  std::any& EnsureCacheEntry(std::type_index key_value) override {
    return layout_cache[key_value];
  }
};

struct RenderSlice {
  std::optional<std::uint64_t> preceding_platform_view;
  std::optional<std::uint64_t> following_platform_view;
  std::size_t first_command = 0;
  std::size_t command_count = 0;

  bool operator==(const RenderSlice&) const = default;
};

struct PlatformViewPlacement {
  const PlacePlatformViewCommand* command = nullptr;
  Rect world_bounds;
  std::optional<Rect> clip;
  bool visible = false;

  bool operator==(const PlatformViewPlacement&) const = default;
};

struct PlatformViewPaintAccess {
  static void Paint(const MountedNode& node, PaintContext& context);
};

using RenderCompositionLayer = std::variant<RenderSlice, PlatformViewPlacement>;

struct RenderComposition {
  std::vector<RenderCompositionLayer> layers;
};

RenderComposition BuildRenderComposition(const RenderScene& scene);

struct ExternalTextureUseSnapshot {
  std::shared_ptr<ExternalTextureState> state;
  std::uint64_t revision = 0;
  Rect bounds;
};

struct RenderNodeSnapshot {
  std::uint64_t content_revision = 0;
  std::uint64_t foreground_revision = 0;
  Transform2D world_transform;
  Transform2D world_children_transform;
  std::optional<Rect> world_clip;
  std::optional<Rect> world_child_clip;
  std::vector<RenderClip> child_clips;
  Rect own_bounds;
  Rect subtree_bounds;
  std::vector<std::uint64_t> children;
  std::vector<ExternalTextureUseSnapshot> external_textures;
  float opacity = 1.0F;
  bool has_own_bounds = false;
  bool has_subtree_bounds = false;
  bool visible = false;
};

// Retains committed geometry and revisions for damage comparison, not visual scene content.
using RenderDamageSnapshot = std::unordered_map<std::uint64_t, RenderNodeSnapshot>;

struct FrozenScene {
  static PaintSequence CopyPaintSequence(const PaintSequence& source);

  const RenderNode* root = nullptr;
  std::vector<std::unique_ptr<RenderNode>> nodes;
};

std::shared_ptr<FrozenScene> FreezeRenderScene(const RenderNode* root);
bool RenderSceneHasPlatformViews(const RenderNode* root);

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
  explicit SceneTransitionService(Runtime& runtime) : runtime_(&runtime) {}

  [[nodiscard]] std::shared_ptr<SceneTransitionAnchorState> CreateAnchor() const;
  [[nodiscard]] std::optional<Point> CurrentInteractionOrigin() const noexcept;
  void Run(SceneTransitionRequest request, std::function<void()> mutation, bool reduced_motion) const;
  void Disconnect() noexcept;

private:
  Runtime* runtime_;
};

struct ActiveSceneTransition {
  SceneTransitionRequest request;
  std::shared_ptr<FrozenScene> frozen;
  MotionController progress{0.0F};
  Size viewport;
  RenderNode composite;
  RenderNode old_wrapper;
  RenderNode new_wrapper;
};

const RenderNode*
ComposeSceneTransition(ActiveSceneTransition& transition, const RenderNode* live_root, float progress);

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

struct ScrollBarGeometry {
  Axis axis = Axis::Vertical;
  Rect track;
  Rect thumb;
  ScrollBarStyle style;
  float scroll_offset = 0.0F;
  float maximum_offset = 0.0F;
  float thumb_travel = 0.0F;
};

struct SemanticExtensionRoute {
  NodeExtensionHandle extension;
  std::uint64_t local_id = 0;
};

inline constexpr std::size_t semantic_standard_action_count = static_cast<std::size_t>(SemanticActionKind::Custom);

struct SemanticActionRoute {
  std::uint64_t node_identity = 0;
  std::array<std::optional<SemanticExtensionRoute>, semantic_standard_action_count> extension_actions;
  std::unordered_map<std::uint64_t, SemanticExtensionRoute> custom_actions;
};

inline constexpr float touch_gesture_slop = 6.0F;

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
  std::optional<double> previous_tap_time;
  Point previous_tap_position;
  std::optional<std::uint64_t> previous_tap_node;
  PointerDeviceKind previous_tap_device = PointerDeviceKind::Mouse;
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
  std::optional<std::uint64_t> press_id;
  std::optional<std::size_t> pressed_action;
  std::optional<std::size_t> hovered_action;
  Rect start_handle_hit_rect;
  Rect end_handle_hit_rect;
  Rect painted_start;
  Rect painted_end;
  Rect toolbar_rect;
  Color toolbar_background;
  Color toolbar_separator;
  Shadow toolbar_shadow;
  EdgeInsets toolbar_separator_padding;
  float toolbar_corner_radius = 0.0F;
  float toolbar_separator_thickness = 0.0F;
  bool toolbar_separators = false;
  TextStyle toolbar_text_style;
  TextShapingOptions toolbar_text_shaping;
  std::vector<TextEditingAction> actions;
  std::vector<Rect> action_rects;
  std::vector<std::string> action_labels;
  std::vector<InteractionState> action_interactions;
  std::vector<std::shared_ptr<IndicationState>> action_indications;
};

struct TextSelectionOverlay {
  // The framework-owned selection overlay is rendered above application layers without becoming part of their tree.
  RenderNode render_node;
  TextSelectionOverlayState state;
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
  State(
      RootFactory root_factory,
      PlatformAdapter* platform,
      std::shared_ptr<detail::RecomposeScope> root_scope,
      LayerController layer_controller,
      ViewportBreakpoints viewport_breakpoints,
      std::shared_ptr<detail::WindowState> window
  )
      : root_factory_(root_factory), platform_(platform), viewport_breakpoints_(viewport_breakpoints),
        window_(std::move(window)), root_scope_(std::move(root_scope)),
        layer_controller_(std::move(layer_controller)) {}

  RootFactory root_factory_;
  PlatformAdapter* platform_;
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
  GestureSettings gesture_settings_;
  std::optional<detail::ActiveSceneTransition> scene_transition_;
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
  SemanticNodeId next_semantic_identity_ = 1;
  SemanticNodeId semantic_root_identity_ = 0;
  std::uint64_t semantic_revision_ = 0;
  std::uint64_t next_press_id_ = 1;
  std::optional<Point> current_interaction_origin_;
  std::optional<detail::PointerHoverState> pointer_hover_;
  PointerCursorKind pointer_cursor_kind_ = PointerCursorKind::Default;
  std::unordered_map<SemanticNodeId, detail::SemanticActionRoute> semantic_action_routes_;
  std::unordered_map<std::int64_t, detail::PointerSession> pointer_sessions_;
  std::optional<std::uint64_t> focused_node_identity_;
  bool focus_visible_ = false;
  std::optional<std::uint64_t> keyboard_activation_identity_;
  std::optional<std::uint64_t> keyboard_press_id_;
  std::optional<detail::ActiveTextInputSession> text_input_session_;
  detail::TextSelectionGestureState text_selection_gesture_;
  detail::TextSelectionOverlay text_selection_overlay_;
  TextInputSessionId next_text_input_session_id_ = 1;
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

Size MeasureNode(
    MountedNode& node,
    const Constraints& constraints,
    PlatformAdapter& platform,
    Runtime& runtime,
    EdgeInsets safe_area = {},
    const WindowTitleBarMetrics* title_bar_metrics = nullptr
);
void LayoutNode(MountedNode& node, Point offset);
float ToggleLabelLeading(const ToggleLayoutMetrics& metrics) noexcept;
Rect ResolveToggleControlBounds(const MountedNode& node) noexcept;
Rect ResolveToggleLabelBounds(const MountedNode& node) noexcept;
TextSelectionClient* FindTextSelectionClient(MountedNode& node);
void ResolvePresentationTree(MountedNode& node);
void ValidateBorder(const Border& border);
VisualFill ResolveVisualFill(const VisualFill& fill, AppResources& resources, const Locale& locale);
void UpdateRenderScene(MountedNode& node, Rect clip, const RenderNode* overlay = nullptr);
DamageRegion ComputeDamageRegion(
    const RenderNode* root,
    Size viewport,
    RenderDamageSnapshot& committed_scene,
    Size& committed_viewport,
    bool& has_committed_scene,
    const std::shared_ptr<ExternalTextureSurface>& texture_surface
);
void DeactivateExternalTextures(
    RenderDamageSnapshot& committed_scene, const std::shared_ptr<ExternalTextureSurface>& texture_surface
);
void UpdateInteraction(MountedNode& node, InteractionState state, std::optional<InteractionEvent> event = std::nullopt);
bool BuildPointerRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route);
bool BuildPointerCursorRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route);
bool BuildHoverRoute(MountedNode& node, Point position, std::vector<MountedNode*>& route);
MountedNode* HitTestPointer(MountedNode& node, Point position);
bool HitTestWindowDragRegion(MountedNode& node, Point position);
std::optional<ScrollBarGeometry> ResolveScrollBarGeometry(const MountedNode& node);
bool IsScrollContainer(const MountedNode& node) noexcept;
Axis ScrollAxis(const MountedNode& node) noexcept;
Rect ScrollViewport(const MountedNode& node) noexcept;
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
