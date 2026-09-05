#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/event.h>
#include <huxerui/environment.h>
#include <huxerui/indication.h>
#include <huxerui/modifier.h>
#include <huxerui/platform_registry.h>
#include <huxerui/resource.h>
#include <huxerui/view.h>

#include "semantics_internal.h"

namespace huxerui::detail {

class AppResources;
struct ViewSpec;

struct TextMeasurerService {
  TextMeasurer* measurer = nullptr;
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
  std::variant<ImageAsset, VectorAsset, std::shared_ptr<ExternalTexture>, ImageResource> source;
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
          } else if constexpr (std::same_as<Image, std::shared_ptr<ExternalTexture>>) {
            return value ? value->IntrinsicSize() : Size{};
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
          } else if constexpr (std::same_as<Image, std::shared_ptr<ExternalTexture>>) {
            return static_cast<bool>(value);
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

using ViewText = std::variant<StringVariant, AttributedText>;
const std::string& StringLiteral(const ViewText& text);

struct ViewSpec {
  explicit ViewSpec(NodeKind kind_value) : kind(kind_value) {}

  NodeKind kind;
  TextRole text_role = TextRole::Body;
  std::optional<ViewKey> key;
  ViewText text;
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


} // namespace huxerui::detail
