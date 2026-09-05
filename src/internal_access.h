#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <variant>

#include <huxerui/layer.h>
#include <huxerui/layout.h>
#include <huxerui/modifier.h>
#include <huxerui/paint.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/text.h>
#include <huxerui/vector.h>
#include <huxerui/virtual_layout.h>

namespace huxerui {
class NavigationItem;
class Runtime;
}

namespace huxerui::detail {

struct MountedNode;
struct PathElement;
struct VirtualCollectionSemantics;

// Public headers only declare this friend. Implementations stay with the subsystem owning each operation.
// Existing ownership, internal cooperation, and direct field sharing do not need a forwarding function here.
struct InternalAccess {
#pragma region Runtime

  static void InvalidateRoot(Runtime& runtime);
  static const MountedNode* RootNode(const Runtime& runtime) noexcept;
  static const ScrollPhysics& DefaultScrollPhysics(const Runtime& runtime) noexcept;
  static void NotifyScrollActivity(Runtime& runtime, MountedNode& node, const ScrollActivity& activity);
  static void RequestFrame(Runtime& runtime);
  static void InvalidateLayout(Runtime& runtime, MountedNode& node);
  static std::optional<std::uint64_t> HitTestPlatformView(const Runtime& runtime, Point position);
  static std::optional<std::uint64_t> FocusedPlatformView(const Runtime& runtime);
  static void SynchronizePlatformViewFocus(Runtime& runtime, std::optional<std::uint64_t> identity, bool focus_visible);
  static bool MoveFocusFromPlatformView(Runtime& runtime, std::uint64_t identity, bool reverse);
  static std::optional<PlatformPayload> DispatchPlatformViewEvent(
      Runtime& runtime, std::uint64_t identity, std::string_view name, const PlatformPayload& payload
  );
  static std::optional<PlatformValue>
  DispatchPlatformViewEvent(Runtime& runtime, std::uint64_t identity, std::type_index key, const PlatformValue& value);

#pragma endregion

#pragma region LayerController

  static LayerId AttachCaptured(
      const LayerController& layers,
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      LayerPlacement placement,
      std::shared_ptr<LayerTransitionState> transition = {},
      std::shared_ptr<const SemanticModalGroupToken> semantic_modal_group = {},
      std::optional<std::uint64_t> retained_focus_identity = std::nullopt
  );
  static LayerId AttachCapturedReplacing(
      const LayerController& layers,
      std::optional<LayerId> replaced,
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      LayerPlacement placement,
      std::shared_ptr<LayerTransitionState> transition,
      std::shared_ptr<const SemanticModalGroupToken> semantic_modal_group = {},
      std::optional<std::uint64_t> retained_focus_identity = std::nullopt
  );
  static bool UpdateCaptured(
      const LayerController& layers,
      LayerId id,
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      LayerPlacement placement,
      std::shared_ptr<LayerTransitionState> transition
  );
  static bool UpdateEntry(
      const LayerController& layers,
      LayerId id,
      std::optional<LayerOptions> options,
      ViewFactory content,
      std::optional<std::shared_ptr<const Environment>> environment
  );
  static bool UpdatePlacement(const LayerController& layers, LayerId id, LayerPlacement placement);
  static std::optional<LayerOptions> EntryOptions(const LayerController& layers, LayerId id);
  static std::shared_ptr<LayerTransitionState> Transition(const LayerController& layers, LayerId id);
  static LayerController::DismissRequestResult RequestDismiss(const LayerController& layers, LayerId id);

#pragma endregion

#pragma region NodeExtension

  static std::shared_ptr<GestureRecognizer> CreateGestureRecognizer(
      NodeExtension& extension,
      huxerui::ViewNode& node,
      const PointerEvent& event,
      double timestamp,
      const GestureSettings& settings,
      Transform2D frozen_node_to_window
  );
  static const DragSourceCapability* GetDragSourceCapability(const NodeExtension& extension) noexcept;
  static const DropTargetCapability* GetDropTargetCapability(const NodeExtension& extension) noexcept;
  static const FileDropTargetCapability* GetFileDropTargetCapability(const NodeExtension& extension) noexcept;

#pragma endregion

#pragma region LayoutContext

  static LayoutContext CreateLayoutContext(
      void* state,
      LayoutContext::MeasureFunction measure,
      EdgeInsets safe_area,
      const WindowTitleBarMetrics* title_bar_metrics
  );

#pragma endregion

#pragma region VirtualLayoutContext

  static VirtualLayoutContext CreateVirtualLayoutContext(
      void* state,
      VirtualLayoutContext::ItemCountFunction item_count,
      VirtualLayoutContext::ViewportFunction viewport,
      VirtualLayoutContext::ItemFunction item,
      VirtualLayoutContext::MeasureFunction measure
  );

#pragma endregion

#pragma region VirtualLayoutResult

  static std::optional<VirtualCollectionSemantics> CollectionSemantics(const VirtualLayoutResult& result);

#pragma endregion

#pragma region PlatformView

  static void PaintPlatformView(const MountedNode& node, PaintContext& context);

#pragma endregion

#pragma region SegmentedButtonItem

  static std::optional<std::variant<ImageAsset, VectorAsset>> ResolveIcon(SegmentedButtonItem& item);
  static std::string ResolveLabel(SegmentedButtonItem& item);
  static bool ShowsLabel(const SegmentedButtonItem& item) noexcept;
  static bool HasIcon(const SegmentedButtonItem& item) noexcept;
  static bool HasBlankLiteralLabel(const SegmentedButtonItem& item) noexcept;
  static void ValidateIcon(const SegmentedButtonItem& item);

#pragma endregion

#pragma region TabItem

  static std::optional<std::variant<ImageAsset, VectorAsset>> ResolveIcon(TabItem& item);
  static std::string ResolveLabel(TabItem& item);
  static bool ShowsLabel(const TabItem& item) noexcept;
  static bool IsEnabled(const TabItem& item) noexcept;
  static bool HasIcon(const TabItem& item) noexcept;
  static bool HasBlankLiteralLabel(const TabItem& item) noexcept;
  static void ValidateIcon(const TabItem& item);

#pragma endregion

#pragma region NavigationItem

  static std::string ResolveLabel(NavigationItem& item);
  static std::optional<std::variant<ImageAsset, VectorAsset>> ResolveIcon(NavigationItem& item);
  static std::optional<std::variant<ImageAsset, VectorAsset>> ResolveSelectedIcon(NavigationItem& item);
  static bool IsEnabled(const NavigationItem& item) noexcept;
  static bool HasIcon(const NavigationItem& item) noexcept;
  static bool HasSelectedIcon(const NavigationItem& item) noexcept;
  static bool HasBlankLiteralLabel(const NavigationItem& item) noexcept;
  static void ValidateImages(const NavigationItem& item);

#pragma endregion

#pragma region AttributedText

  static bool SameBody(const AttributedText& left, const AttributedText& right) noexcept;
  static std::size_t BodyHash(const AttributedText& text) noexcept;

#pragma endregion

#pragma region StringVariant

  static const std::variant<std::string, StringResource>& StringValue(const StringVariant& value) noexcept;
  static std::variant<std::string, StringResource>& StringValue(StringVariant& value) noexcept;
  static std::span<const std::string> StringArguments(const StringVariant& value) noexcept;

#pragma endregion

#pragma region RawAsset

  static RawAsset WithMimeType(RawAsset asset, std::string mime_type);

#pragma endregion

#pragma region ImageAsset

  static ImageAsset ImageFromRaw(RawAsset asset, float scale);
  static std::uint64_t ImageIdentity(const ImageAsset& image) noexcept;

#pragma endregion

#pragma region VectorAsset

  static VectorAsset VectorFromRaw(RawAsset asset);
  static bool IsVectorPayload(const RawAsset& asset) noexcept;
  [[nodiscard]] static const PaintSequence& Sequence(const VectorAsset& asset) noexcept;

#pragma endregion

#pragma region Path

  [[nodiscard]] static std::span<const PathElement> Elements(const Path& path) noexcept;

#pragma endregion
};

} // namespace huxerui::detail
