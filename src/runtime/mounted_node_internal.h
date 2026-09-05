#pragma once

#include <algorithm>
#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/event.h>
#include <huxerui/environment.h>
#include <huxerui/lifecycle.h>
#include <huxerui/modifier.h>
#include <huxerui/platform_registry.h>
#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

#include "graphics/geometry_internal.h"
#include "semantics_internal.h"
#include "view_internal.h"

namespace huxerui::detail {

class AppResources;
class RecomposeScope;
struct ParagraphLayout;
class ScrollConnection;

inline std::optional<double> EarliestWakeAfter(std::optional<double> first, std::optional<double> second) noexcept {
  if (!first.has_value()) {
    return second;
  }
  if (!second.has_value()) {
    return first;
  }
  return std::min(*first, *second);
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
  std::optional<float> transfer_velocity{};
};

class ScrollMotion {
public:
  void Stop(MountedNode& node, ScrollPhase phase = ScrollPhase::Cancel);
  bool StartMomentum(MountedNode& node, float velocity);
  bool StartOverscrollSettlement(MountedNode& node);
  ScrollMotionFrameResult Advance(MountedNode& node, const FrameInfo& frame);

private:
  enum class Mode {
    Idle,
    Momentum,
    OverscrollSettlement,
  };

  void Reset() noexcept;

  float velocity_ = 0.0F;
  std::optional<double> previous_timestamp_;
  Mode mode_ = Mode::Idle;
};

struct ScrollNodeState {
  Axis axis = Axis::Vertical;
  std::uint32_t allowed_sources = std::numeric_limits<std::uint32_t>::max();
  bool touch_drag_only = false;
  bool allows_automatic_reveal = true;
  bool allows_leading_overscroll = true;
  bool allows_trailing_overscroll = true;
  float offset_y = 0.0F;
  float offset_x = 0.0F;
  float overscroll_offset = 0.0F;
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
  // A retained container can move its descendants without changing its own layout or foreground presentation.
  Transform2D children_transform;
  float local_opacity = 1.0F;
  float render_opacity = 1.0F;
  Transform2D resolved_transform;
  float resolved_opacity = 1.0F;
};

// MountedNode is the retained counterpart of ViewSpec. Runtime reads copied component payloads only from matching
// NodeKind branches, while layout, paint, interaction, and extension state persist across compatible declarations.
struct MountedNode final : public huxerui::MountedNode {
  Runtime* runtime = nullptr;
  NodeKind kind = NodeKind::Layout;
  std::uint64_t identity = 0;
  std::optional<ViewKey> key;
  AttributedText text;
  std::shared_ptr<ParagraphLayout> paragraph_layout;
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

  [[nodiscard]] Rect ContentBoundsImpl() const noexcept override {
    return {
        bounds.x + resolved_padding.left,
        bounds.y + resolved_padding.top,
        std::max(0.0F, bounds.width - resolved_padding.Horizontal()),
        std::max(0.0F, bounds.height - resolved_padding.Vertical()),
    };
  }

  [[nodiscard]] Point LayoutOffsetImpl() const noexcept override {
    return layout_offset;
  }

  [[nodiscard]] Rect PresentationBoundsImpl() const noexcept override {
    return TransformBounds(presentation.resolved_transform, bounds);
  }

  [[nodiscard]] Point LocalToWindowImpl(Point point) const noexcept override {
    return presentation.resolved_transform.Apply(point);
  }

  [[nodiscard]] std::optional<Point> WindowToLocalImpl(Point point) const noexcept override {
    return presentation.resolved_transform.Inverse(point);
  }

  [[nodiscard]] Rect LocalToWindowBoundsImpl(Rect local_bounds) const noexcept override {
    return TransformBounds(presentation.resolved_transform, local_bounds);
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


inline Color InterpolateColor(Color from, Color to, float progress) noexcept {
  const float value = std::clamp(progress, 0.0F, 1.0F);
  return {
      from.red + (to.red - from.red) * value,
      from.green + (to.green - from.green) * value,
      from.blue + (to.blue - from.blue) * value,
      from.alpha + (to.alpha - from.alpha) * value,
  };
}

class ScrollConnection : public std::enable_shared_from_this<ScrollConnection> {
public:
  ScrollConnection(MountedNode& node, const ScrollController& controller);

  [[nodiscard]] bool Matches(const ScrollController& controller) const noexcept;
  [[nodiscard]] bool IsCurrent() const noexcept;
  void Connect();
  bool ScrollTo(float offset);
  bool ScrollBy(float delta);
  bool ScrollToItem(std::size_t index, ScrollAlignment alignment);
  void CancelPending();
  void ApplyPending(bool after_layout = false);
  void PublishMetrics();

private:
  [[nodiscard]] bool IsVertical() const noexcept;
  [[nodiscard]] float ViewportExtent() const noexcept;
  [[nodiscard]] float ContentExtent() const noexcept;
  [[nodiscard]] float CurrentOffset() const noexcept;
  bool ScrollToOffset(float offset);
  void SetCurrentOffset(float offset) noexcept;

  MountedNode* node_;
  std::shared_ptr<ScrollControllerState> state_;
};

void PrepareScrollController(MountedNode& node);
void CompleteScrollController(MountedNode& node);

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

using RenderCompositionLayer = std::variant<RenderSlice, PlatformViewPlacement>;

struct RenderComposition {
  std::vector<RenderCompositionLayer> layers;
};

RenderComposition BuildRenderComposition(const RenderScene& scene);

struct ExternalTextureUseSnapshot {
  std::shared_ptr<ExternalTexture> texture;
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

struct ScrollBarGeometry {
  Axis axis = Axis::Vertical;
  Rect track;
  Rect thumb;
  ScrollBarStyle style;
  float scroll_offset = 0.0F;
  float maximum_offset = 0.0F;
  float thumb_travel = 0.0F;
};

inline constexpr float touch_gesture_slop = 6.0F;

MountedNode* FindNode(MountedNode& root, std::uint64_t identity);
NodeExtension* FindExtension(MountedNode& root, const NodeExtensionHandle& handle);
void ActivateNode(MountedNode& node);
bool ContainsNodeIdentity(const MountedNode& root, std::uint64_t identity);
std::optional<bool> DeclaredEnabled(const MountedNode& node, std::uint64_t identity, bool parent_enabled = true);
void AutoScrollDropTarget(Runtime& runtime, const std::unique_ptr<MountedNode>& root,
                          std::uint64_t target_identity, Point window_position, const FrameInfo& frame);

Size MeasureNode(
    MountedNode& node,
    const Constraints& constraints,
    PlatformAdapter& platform,
    Runtime& runtime,
    EdgeInsets safe_area = {},
    const WindowTitleBarMetrics* title_bar_metrics = nullptr
);
void LayoutNode(MountedNode& node, Point offset);

inline bool AppliesDisabledAppearance(const huxerui::MountedNode& node) {
  return static_cast<const MountedNode&>(node).applies_disabled_appearance;
}

float ToggleLabelLeading(const ToggleLayoutMetrics& metrics) noexcept;
Rect ResolveToggleControlBounds(const MountedNode& node) noexcept;
Rect ResolveToggleLabelBounds(const MountedNode& node) noexcept;
TextSelectionClient* FindTextSelectionClient(MountedNode& node);
MountedNode* FindTextSelectionOwner(MountedNode& root, std::uint64_t identity);
void ResolvePresentationTree(MountedNode& node);
void ValidateBorder(const Border& border);
void UpdateRenderScene(MountedNode& node, Rect clip, const RenderNode* overlay = nullptr);
DamageRegion ComputeDamageRegion(
    const RenderNode* root,
    Size viewport,
    RenderDamageSnapshot& committed_scene,
    Size& committed_viewport,
    bool& has_committed_scene,
    const std::shared_ptr<ExternalTextureFrameRequester>& texture_frame_requester
);
void DeactivateExternalTextures(
    RenderDamageSnapshot& committed_scene,
    const std::shared_ptr<ExternalTextureFrameRequester>& texture_frame_requester
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
bool AllowsScrollSource(const MountedNode& node, ScrollSource source) noexcept;
bool CanScrollNode(const MountedNode& node, float delta);
bool CanOverscrollNode(const MountedNode& node, float delta);
float ScrollNodeBy(MountedNode& node, float delta, ScrollSource source);
bool ScrollNodeRectIntoView(MountedNode& node, Rect& rect);

ScrollMetrics ResolveScrollMetrics(const MountedNode& node) noexcept;
const ScrollPhysics& ResolveScrollPhysics(const MountedNode& node);
void ValidateScrollPhysics(const ScrollPhysics& physics);
void NotifyScrollNodeActivity(MountedNode& node, ScrollSource source, ScrollPhase phase, float delta);
void StopScrollNodeMotion(MountedNode& node, ScrollPhase phase = ScrollPhase::Cancel);
float ApplyScrollTransaction(const std::vector<MountedNode*>& route, Axis axis, float delta, ScrollSource source,
                             std::vector<std::uint64_t>* direct_activity_nodes = nullptr,
                             bool allow_overscroll = false);
float ApplyPreFling(const std::vector<MountedNode*>& route, Axis axis, float velocity);
bool AdvanceMountedNodeFrame(MountedNode& node, const FrameInfo& frame);

bool IsVirtualLayoutNode(const MountedNode& node) noexcept;

} // namespace huxerui::detail
