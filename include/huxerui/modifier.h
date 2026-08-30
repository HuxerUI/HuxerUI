#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <huxerui/color.h>
#include <huxerui/event.h>
#include <huxerui/geometry.h>
#include <huxerui/layout.h>
#include <huxerui/paint.h>

namespace huxerui {

class Environment;
class PaintContext;
class Runtime;
class SemanticBuilder;
class TextInputClient;
class TextSelectionClient;
struct GestureSettings;
struct SemanticAction;

namespace detail {
struct DragSourceCapability;
struct DropTargetCapability;
class GestureRecognizer;
}

/// Describes the timing and accessibility policy for one extension frame callback.
struct FrameInfo {
  /// Monotonic frame timestamp in seconds.
  double timestamp = 0.0;

  /// Elapsed time since the preceding frame in seconds, clamped by Runtime to a safe animation interval.
  double delta_time = 0.0;

  /// Whether motion should resolve immediately for the current mounted node.
  bool reduced_motion = false;

  /// Compares all frame inputs.
  bool operator==(const FrameInfo&) const = default;
};

/// Adds retained lifecycle, input, semantics, or paint behavior to one mounted View node.
///
/// A custom retained modifier declares an `Extension` type derived from NodeExtension. Runtime preserves a compatible
/// extension across recomposition and calls its `Update()` method with the latest modifier value.
/// @code
/// struct Overlay {
///   class Extension;
///   Color color;
/// };
///
/// class Overlay::Extension final : public NodeExtension {
/// public:
///   Extension(MountedNode& node, const Overlay& value) { Update(node, value); }
///   void Update(MountedNode&, const Overlay& value) {
///     color_ = value.color;
///     InvalidatePaint();
///   }
///   void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
///     context.DrawRect(node.Bounds(), color_);
///   }
///
/// private:
///   Color color_;
/// };
/// @endcode
class NodeExtension {
public:
  /// Selects which retained paint sequence must be recorded again.
  enum class PaintInvalidation {
    /// Keeps both retained paint sequences unchanged.
    None,
    /// Invalidates the sequence behind node content.
    Content,
    /// Invalidates the sequence above node content.
    Foreground,
    /// Invalidates both retained paint sequences.
    Both,
  };

  /// Requests follow-up frame scheduling from OnFrame().
  struct FrameResult {
    /// Requests another frame as soon as the platform can produce one.
    bool needs_frame = false;

    /// Requests a frame after this many seconds when continuous frames are unnecessary.
    std::optional<double> wake_after;

    /// Compares both scheduling requests.
    bool operator==(const FrameResult&) const = default;
  };

  /// Reports how an extension participates in the current pointer sequence.
  enum class PointerResult {
    /// Rejects the sequence and performs no further observation.
    Ignored,
    /// Keeps observing while another target may continue to own the sequence.
    Observe,
    /// Accepts and consumes the sequence.
    Handled,
    /// Accepts the sequence and requests continued delivery outside the node bounds.
    Capture,
    /// Accepts after observation and cancels the active raw pointer target.
    CancelTarget,
  };

  /// Destroys the retained extension when its modifier is removed or its node is unmounted.
  virtual ~NodeExtension() = default;

  /// Advances retained time-based state for the current mounted node.
  ///
  /// Return a scheduling request when more work remains, and call InvalidatePaint() when retained visual state changes.
  virtual FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) {
    static_cast<void>(node);
    static_cast<void>(frame);
    return {};
  }

  /// Updates geometry-dependent retained state after final presentation geometry is resolved.
  ///
  /// Return every paint phase whose recorded inputs changed.
  [[nodiscard]] virtual PaintInvalidation PrepareGeometry(MountedNode& node) {
    static_cast<void>(node);
    return PaintInvalidation::None;
  }

  /// Observes mounted interaction state and the event that produced its latest transition, when available.
  virtual void OnInteraction(MountedNode& node, const InteractionState& state,
                             const std::optional<InteractionEvent>& event) {
    static_cast<void>(node);
    static_cast<void>(state);
    static_cast<void>(event);
  }

  /// Notifies the extension that the node's scroll offset changed.
  virtual void OnScrollActivity(MountedNode& node) {
    static_cast<void>(node);
  }

  /// Notifies the extension when direct scrolling begins or ends.
  virtual void OnScrollGesture(MountedNode& node, bool active) {
    static_cast<void>(node);
    static_cast<void>(active);
  }

  /// Returns whether this extension makes the node an input target at a node-local position.
  ///
  /// Runtime may call this more than once while routing an event, so the implementation must be deterministic and
  /// free of side effects.
  [[nodiscard]] virtual bool HitTest(MountedNode& node, Point position) const {
    static_cast<void>(node);
    static_cast<void>(position);
    return false;
  }

  /// Returns whether a node-local position participates in this extension's hover affordance.
  [[nodiscard]] virtual bool HoverHitTest(MountedNode& node, Point position) const {
    static_cast<void>(node);
    static_cast<void>(position);
    return false;
  }

  /// Returns whether hover callbacks remain available while the node is disabled.
  [[nodiscard]] virtual bool HoverWhenDisabled() const noexcept {
    return false;
  }

  /// Notifies the extension when its hover hit-test state changes.
  virtual void OnHoverChanged(MountedNode& node, bool hovered) {
    static_cast<void>(node);
    static_cast<void>(hovered);
  }

  /// Notifies the extension when keyboard focus enters or leaves its node.
  virtual void OnFocusChanged(MountedNode& node, bool focused) {
    static_cast<void>(node);
    static_cast<void>(focused);
  }

  /// Delivers a keyboard event routed to the focused node.
  virtual void OnKey(MountedNode& node, const KeyEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
  }

  /// Handles a platform back request and returns whether the extension consumed it.
  [[nodiscard]] virtual bool OnBack(MountedNode& node, const BackEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
    return false;
  }

  /// Returns the text-input client owned by this extension, or an empty pointer when none is active.
  [[nodiscard]] virtual std::shared_ptr<TextInputClient> GetTextInputClient() noexcept {
    return {};
  }

  /// Returns the active text-selection client owned by this extension, or nullptr when unavailable.
  [[nodiscard]] virtual TextSelectionClient* GetTextSelectionClient() noexcept {
    return nullptr;
  }

  /// Handles a node-local pointer event and reports recognition or ownership of its physical sequence.
  virtual PointerResult OnPointer(MountedNode& node, const PointerEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
    return PointerResult::Ignored;
  }

  /// Adds this extension's accessible semantics to the node declaration.
  virtual void BuildSemantics(SemanticBuilder& builder) const {
    static_cast<void>(builder);
  }

  /// Performs an accessibility action addressed to an extension-local semantics identifier.
  ///
  /// Returns true when the action was handled.
  [[nodiscard]] virtual bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) {
    static_cast<void>(local_id);
    static_cast<void>(action);
    return false;
  }

  /// Records node-local paint commands after the background and before the border, content, and descendants.
  virtual void PaintBehindContent(const MountedNode& node, PaintContext& context) const {
    static_cast<void>(node);
    static_cast<void>(context);
  }

  /// Records node-local paint commands after content and descendants and before the framework focus ring.
  virtual void PaintAboveContent(const MountedNode& node, PaintContext& context) const {
    static_cast<void>(node);
    static_cast<void>(context);
  }

protected:
  /// Invalidates retained paint output after extension-owned visual state changes.
  void InvalidatePaint(PaintInvalidation invalidation = PaintInvalidation::Foreground) {
    if (invalidation != PaintInvalidation::None && invalidate_paint_) {
      invalidate_paint_(invalidation);
    }
  }

  /// Invalidates semantics after extension-owned accessible state changes.
  void InvalidateSemantics() {
    if (invalidate_semantics_) {
      invalidate_semantics_();
    }
  }

private:
  virtual std::shared_ptr<detail::GestureRecognizer> CreateGestureRecognizer(
      MountedNode& node, const PointerEvent& event, double timestamp, const GestureSettings& settings,
      Transform2D frozen_node_to_window
  );

  [[nodiscard]] virtual const detail::DragSourceCapability* GetDragSourceCapability() const noexcept;

  [[nodiscard]] virtual const detail::DropTargetCapability* GetDropTargetCapability() const noexcept;

  void BindPaintInvalidation(std::function<void(PaintInvalidation)> callback) {
    invalidate_paint_ = std::move(callback);
  }

  void BindSemanticsInvalidation(std::function<void()> callback) {
    invalidate_semantics_ = std::move(callback);
  }

  std::function<void(PaintInvalidation)> invalidate_paint_;
  std::function<void()> invalidate_semantics_;

  friend class Runtime;
};

namespace detail {

class AppResources;
struct ViewSpec;
struct ModifierDescriptor;

struct ModifierSpec {
  const ModifierDescriptor* descriptor = nullptr;
  std::shared_ptr<const void> value;
};

struct ModifierDescriptor {
  // Compilation applies one ordered declaration and may replace its retained value before mounted state changes.
  void (*compile)(ViewSpec&, ModifierSpec&, const std::shared_ptr<const Environment>&, AppResources&) = nullptr;
  std::unique_ptr<NodeExtension> (*create_extension)(MountedNode&, const void*) = nullptr;
  void (*update_extension)(NodeExtension&, MountedNode&, const void*) = nullptr;
  // A changed retained value can affect this node's measured size.
  bool layout_affecting = false;
  bool (*equals)(const void*, const void*) = nullptr;
  bool (*layout_equals)(const void*, const void*) = nullptr;
};

template <class Value> constexpr auto ErasedEqualsFor() noexcept -> bool (*)(const void*, const void*) {
  if constexpr (std::equality_comparable<Value>) {
    return [](const void* left, const void* right) {
      return *static_cast<const Value*>(left) == *static_cast<const Value*>(right);
    };
  } else {
    return nullptr;
  }
}

template <
    class Spec,
    class Extension,
    bool LayoutAffecting = false,
    bool (*LayoutEquals)(const Spec&, const Spec&) = nullptr>
  requires std::derived_from<Extension, NodeExtension> &&
           std::constructible_from<Extension, MountedNode&, const Spec&> &&
           requires(Extension& extension, MountedNode& node, const Spec& spec) { extension.Update(node, spec); }
const ModifierDescriptor& ModifierDescriptorFor() {
  constexpr auto erased_layout_equals = []() -> bool (*)(const void*, const void*) {
    if constexpr (!LayoutAffecting) {
      return nullptr;
    } else if constexpr (LayoutEquals != nullptr) {
      return [](const void* left, const void* right) {
        return LayoutEquals(*static_cast<const Spec*>(left), *static_cast<const Spec*>(right));
      };
    } else {
      return ErasedEqualsFor<Spec>();
    }
  }();
  static const ModifierDescriptor descriptor{
      nullptr,
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<Extension>(node, *static_cast<const Spec*>(value));
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<Extension&>(extension).Update(node, *static_cast<const Spec*>(value));
      },
      LayoutAffecting,
      ErasedEqualsFor<Spec>(),
      erased_layout_equals,
  };
  return descriptor;
}

template <class Modifier>
concept ExplicitModifierDescriptor = requires {
  { Modifier::Descriptor() } -> std::same_as<const ModifierDescriptor&>;
};

template <class Modifier>
concept AutomaticModifierDescriptor =
    requires { typename Modifier::Extension; } && std::derived_from<typename Modifier::Extension, NodeExtension> &&
    std::constructible_from<typename Modifier::Extension, MountedNode&, const Modifier&> &&
    requires(typename Modifier::Extension& extension, MountedNode& node, const Modifier& modifier) {
      extension.Update(node, modifier);
    };

template <class Modifier>
  requires ExplicitModifierDescriptor<Modifier> || AutomaticModifierDescriptor<Modifier>
const ModifierDescriptor& ResolveModifierDescriptor() {
  if constexpr (ExplicitModifierDescriptor<Modifier>) {
    return Modifier::Descriptor();
  } else {
    return ModifierDescriptorFor<Modifier, typename Modifier::Extension>();
  }
}

} // namespace detail

/// Identifies a copyable value that View::With() can apply as a property or retained modifier.
template <class T>
concept ViewModifier =
    std::copy_constructible<std::remove_cvref_t<T>> && (detail::ExplicitModifierDescriptor<std::remove_cvref_t<T>> ||
                                                        detail::AutomaticModifierDescriptor<std::remove_cvref_t<T>>);

namespace detail {

template <ViewModifier Modifier> ModifierSpec MakeModifierSpec(Modifier&& modifier) {
  using Value = std::remove_cvref_t<Modifier>;
  return {
      &ResolveModifierDescriptor<Value>(),
      std::make_shared<Value>(std::forward<Modifier>(modifier)),
  };
}

} // namespace detail

/// Configures the geometry, motion, and colors of a ScrollBar.
///
/// Supplying no explicit style to ScrollBar resolves the current Theme style instead.
/// @code
/// ScrollView(content).With(ScrollBar{ScrollBarStyle{
///     .thickness = 8.0F,
///     .thumb_color = Color::Rgb(90, 90, 100),
/// }});
/// @endcode
struct ScrollBarStyle {
  /// Width of a vertical bar or height of a horizontal bar, in logical pixels.
  float thickness = 6.0F;

  /// Smallest thumb length along the scrolling axis, in logical pixels.
  float minimum_thumb_extent = 24.0F;

  /// Distance between the bar track and its nearest View edge, in logical pixels.
  float margin = 3.0F;

  /// Corner radius shared by the track and thumb, in logical pixels.
  float corner_radius = 3.0F;

  /// Duration of the fade-in animation in seconds.
  float fade_in_duration = 0.12F;

  /// Idle delay before fade-out begins, in seconds.
  float fade_out_delay = 0.7F;

  /// Duration of the fade-out animation in seconds.
  float fade_out_duration = 0.22F;

  /// Color painted behind the thumb for the complete scroll range.
  Color track_color = Color::Transparent();

  /// Color of the movable scroll thumb.
  Color thumb_color = Color::Rgb(137, 143, 152, 0.8F);

  /// Returns the framework baseline style before Theme-specific resolution.
  static ScrollBarStyle Default();

  /// Compares all geometry, motion, and color values.
  bool operator==(const ScrollBarStyle&) const = default;
};

/// Configures momentum for a scroll container without changing its controlled content or geometry.
///
/// @code
/// ScrollView(content).With(ScrollPhysics{
///     .fling_enabled = true,
///     .deceleration_rate = 4.0F,
/// });
/// @endcode
struct ScrollPhysics {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Whether release velocity may continue scrolling after direct input ends.
  bool fling_enabled = true;

  /// Exponential velocity decay rate per second; larger values stop sooner.
  float deceleration_rate = 3.0F;

  /// Smallest release velocity that starts a fling, in logical pixels per second.
  float minimum_fling_velocity = 40.0F;

  /// Largest release velocity accepted by the fling simulation, in logical pixels per second.
  float maximum_fling_velocity = 6000.0F;

  /// Compares all momentum settings.
  bool operator==(const ScrollPhysics&) const = default;
};

/// Enables or disables interaction for a View subtree.
///
/// @code
/// Button("Save").With(Enabled(can_save.Get()));
/// @endcode
struct Enabled {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Whether the modified View and its descendants may interact.
  bool value = true;

  /// Compares the enabled declaration.
  bool operator==(const Enabled&) const = default;
};

/// Makes a View eligible for keyboard focus independently of component defaults.
///
/// @code
/// Canvas([](PaintContext&, Size) {}).With(Focusable(true));
/// @endcode
struct Focusable {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Whether the modified View participates in focus traversal and focus requests.
  bool value = true;

  /// Compares the focusable declaration.
  bool operator==(const Focusable&) const = default;
};

/// Selects the pointer cursor while a mouse or pen points at a View.
///
/// The deepest explicit declaration in the hit route wins. PointerCursorKind::Default is an explicit declaration and
/// therefore prevents an ancestor cursor from applying to that View.
/// @code
/// Text("Open details").With(PointerCursor(PointerCursorKind::Hand));
/// @endcode
struct PointerCursor {
  /// Creates a pointer-cursor modifier with the requested portable kind.
  explicit PointerCursor(PointerCursorKind kind) : kind(kind) {}

  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Portable cursor requested by the modified View.
  PointerCursorKind kind = PointerCursorKind::Default;

  /// Compares the requested cursor kind.
  bool operator==(const PointerCursor&) const = default;
};

/// Insets a View's content while preserving the outer frame as the node's layout and paint bounds.
///
/// @code
/// Column {
///   Text("Profile"),
/// }.With(Padding(16.0F));
/// @endcode
struct Padding {
  /// Creates uniform padding on every edge.
  explicit Padding(float value) : insets(EdgeInsets::All(value)) {}

  /// Creates independently configured edge padding.
  explicit Padding(EdgeInsets value) : insets(value) {}

  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Insets applied to the content area.
  EdgeInsets insets;

  /// Compares all edge insets.
  bool operator==(const Padding&) const = default;
};

/// Paints a solid color, gradient, or other VisualFill behind a View's content.
///
/// @code
/// Text("Status").With(Background(Color::Rgb(240, 244, 255)));
/// @endcode
struct Background {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Fill painted inside the node's resolved corner shape.
  VisualFill fill;

  /// Compares the background fill.
  bool operator==(const Background&) const = default;
};

/// Paints a border inside a View's resolved bounds and corner shape.
///
/// @code
/// Text("Outlined").With(Border{.color = Color::Rgb(100, 110, 130), .width = 1.0F});
/// @endcode
struct Border {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Border color.
  Color color;

  /// Border width in logical pixels.
  float width = 1.0F;

  /// Compares the border color and width.
  bool operator==(const Border&) const = default;
};

/// Paints an outer shadow behind a View without changing measurement.
///
/// @code
/// Text("Elevated").With(Shadow{
///     .color = Color::Rgb(0, 0, 0, 0.2F),
///     .blur_radius = 12.0F,
/// });
/// @endcode
struct Shadow {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Shadow color including opacity.
  Color color;

  /// Translation of the shadow caster in logical pixels.
  Point offset;

  /// Conservative outer blur falloff in logical pixels.
  float blur_radius = 0.0F;

  /// Signed expansion of the shadow caster before blur, in logical pixels.
  float spread = 0.0F;

  /// Compares all shadow values.
  bool operator==(const Shadow&) const = default;
};

/// Sets the foreground color used by text content on the modified View.
///
/// @code
/// Text("Ready").With(Foreground(Color::Rgb(30, 120, 70)));
/// @endcode
struct Foreground {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Text foreground color.
  Color color;

  /// Compares the foreground color.
  bool operator==(const Foreground&) const = default;
};

/// Sets the text font size on the modified View.
///
/// @code
/// Text("Overview").With(FontSize(24.0F));
/// @endcode
struct FontSize {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Font size in logical pixels.
  float value;

  /// Compares the font size.
  bool operator==(const FontSize&) const = default;
};

/// Constrains the outer size of a View without introducing a wrapper node.
///
/// Each populated field updates that part of the current frame declaration, so separate Frame modifiers may constrain
/// independent axes. Preferred dimensions are clamped to the resolved minimum and maximum bounds.
/// @code
/// Image(icon).With(Frame{
///     .width = 48.0F,
///     .height = 48.0F,
///     .min_width = 32.0F,
/// });
/// @endcode
struct Frame {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Preferred outer width in logical pixels.
  std::optional<float> width;

  /// Preferred outer height in logical pixels.
  std::optional<float> height;

  /// Minimum outer width in logical pixels.
  std::optional<float> min_width;

  /// Maximum outer width in logical pixels.
  std::optional<float> max_width;

  /// Minimum outer height in logical pixels.
  std::optional<float> min_height;

  /// Maximum outer height in logical pixels.
  std::optional<float> max_height;

  /// Compares all preferred dimensions and bounds.
  bool operator==(const Frame&) const = default;
};

/// Rounds a View's background, border, descendant clip, and pointer containment shape.
///
/// @code
/// Text("Rounded").With(Background(Color::White()), CornerRadius(12.0F));
/// @endcode
struct CornerRadius {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Per-corner radii in logical pixels.
  CornerRadii value;

  /// Compares all corner radii.
  bool operator==(const CornerRadius&) const = default;
};

/// Clips descendants to the modified View's resolved bounds and corner shape.
///
/// @code
/// Stack {
///   oversized_content,
/// }.With(Frame{120.0F, 80.0F}, CornerRadius(12.0F), ClipChildren());
/// @endcode
struct ClipChildren {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Compares two stateless clipping declarations.
  bool operator==(const ClipChildren&) const = default;
};

/// Sets the gap between children of Row, Column, Flow, and compatible custom layouts.
///
/// @code
/// Row {
///   Button("Back"),
///   Button("Next"),
/// }.With(Spacing(8.0F));
/// @endcode
struct Spacing {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Gap between adjacent children in logical pixels.
  float value;

  /// Compares the spacing value.
  bool operator==(const Spacing&) const = default;
};

/// Aligns a layout's children and remaining space along its main axis.
///
/// @code
/// Row {
///   cancel,
///   confirm,
/// }.With(MainAlign(MainAxisAlignment::End));
/// @endcode
struct MainAlign {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Main-axis alignment consumed by a compatible layout.
  MainAxisAlignment alignment;

  /// Compares the main-axis alignment.
  bool operator==(const MainAlign&) const = default;
};

/// Aligns children across the main axis of Row, Column, Flow, and compatible custom layouts.
///
/// @code
/// Column {
///   title,
///   content,
/// }.With(CrossAlign(CrossAxisAlignment::Stretch));
/// @endcode
struct CrossAlign {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Cross-axis alignment consumed by a compatible layout.
  CrossAxisAlignment alignment;

  /// Compares the cross-axis alignment.
  bool operator==(const CrossAlign&) const = default;
};

/// Aligns children horizontally and vertically inside a Stack or compatible custom layout.
///
/// @code
/// Stack {
///   content,
/// }.With(Align(HorizontalAlignment::Center, VerticalAlignment::Center));
/// @endcode
struct Align {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Horizontal child alignment.
  HorizontalAlignment horizontal;

  /// Vertical child alignment.
  VerticalAlignment vertical;

  /// Compares both alignment axes.
  bool operator==(const Align&) const = default;
};

/// Assigns a child a proportional share of finite remaining main-axis space in a compatible parent layout.
///
/// Grow is parent-child layout metadata. It has no expansion effect when the parent's main axis is unbounded.
/// @code
/// Row {
///   Text("Name").With(Grow(1.0F)),
///   Button("Edit"),
/// };
/// @endcode
struct Grow {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Relative share of remaining main-axis space; zero opts out of growth.
  float factor = 1.0F;

  /// Compares the grow factor.
  bool operator==(const Grow&) const = default;
};

/// Adds a themed, interactive scroll indicator to a scroll container.
///
/// The bar appears during scrolling, supports direct thumb dragging, and fades when idle. Leave style empty to resolve
/// the active Theme value, or provide an explicit ScrollBarStyle for this View.
/// @code
/// ScrollView(content).With(Frame{.height = 240.0F}, ScrollBar());
/// @endcode
struct ScrollBar {
  /// Returns the modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Explicit style, or std::nullopt to resolve the current Theme style.
  std::optional<ScrollBarStyle> style;

  /// Compares the explicit style declaration.
  bool operator==(const ScrollBar&) const = default;
};

} // namespace huxerui
