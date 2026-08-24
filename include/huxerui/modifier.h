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
struct SemanticAction;

struct FrameInfo {
  double timestamp = 0.0;
  double delta_time = 0.0;
  // Resolved for the current mounted node before its extensions run.
  bool reduced_motion = false;

  bool operator==(const FrameInfo&) const = default;
};

class NodeExtension {
public:
  enum class PaintInvalidation {
    None,
    Content,
    Foreground,
    Both,
  };

  struct FrameResult {
    bool needs_frame = false;
    std::optional<double> wake_after;

    bool operator==(const FrameResult&) const = default;
  };

  enum class PointerResult {
    Ignored,
    Observe,
    Handled,
    Capture,
    // An observing extension may suppress the active target after it recognizes a competing gesture.
    CancelTarget,
  };

  virtual ~NodeExtension() = default;

  virtual FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) {
    static_cast<void>(node);
    static_cast<void>(frame);
    return {};
  }

  // Called after final presentation geometry is resolved. Report every retained paint phase whose inputs changed.
  [[nodiscard]] virtual PaintInvalidation PrepareGeometry(MountedNode& node) {
    static_cast<void>(node);
    return PaintInvalidation::None;
  }

  virtual void OnInteraction(MountedNode& node, const InteractionState& state,
                             const std::optional<InteractionEvent>& event) {
    static_cast<void>(node);
    static_cast<void>(state);
    static_cast<void>(event);
  }

  virtual void OnScrollActivity(MountedNode& node) {
    static_cast<void>(node);
  }

  virtual void OnScrollGesture(MountedNode& node, bool active) {
    static_cast<void>(node);
    static_cast<void>(active);
  }

  [[nodiscard]] virtual bool HitTest(MountedNode& node, Point position) const {
    static_cast<void>(node);
    static_cast<void>(position);
    return false;
  }

  [[nodiscard]] virtual bool HoverHitTest(MountedNode& node, Point position) const {
    static_cast<void>(node);
    static_cast<void>(position);
    return false;
  }

  // Hover-only affordances can opt into disabled targets without enabling other input.
  [[nodiscard]] virtual bool HoverWhenDisabled() const noexcept {
    return false;
  }

  virtual void OnHoverChanged(MountedNode& node, bool hovered) {
    static_cast<void>(node);
    static_cast<void>(hovered);
  }

  virtual void OnFocusChanged(MountedNode& node, bool focused) {
    static_cast<void>(node);
    static_cast<void>(focused);
  }

  virtual void OnKey(MountedNode& node, const KeyEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
  }

  [[nodiscard]] virtual bool OnBack(MountedNode& node, const BackEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
    return false;
  }

  [[nodiscard]] virtual std::shared_ptr<TextInputClient> GetTextInputClient() noexcept {
    return {};
  }

  [[nodiscard]] virtual TextSelectionClient* GetTextSelectionClient() noexcept {
    return nullptr;
  }

  virtual PointerResult OnPointer(MountedNode& node, const PointerEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
    return PointerResult::Ignored;
  }

  virtual void BuildSemantics(SemanticBuilder& builder) const {
    static_cast<void>(builder);
  }

  [[nodiscard]] virtual bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) {
    static_cast<void>(local_id);
    static_cast<void>(action);
    return false;
  }

  virtual void PaintBehindContent(const MountedNode& node, PaintContext& context) const {
    static_cast<void>(node);
    static_cast<void>(context);
  }

  virtual void PaintAboveContent(const MountedNode& node, PaintContext& context) const {
    static_cast<void>(node);
    static_cast<void>(context);
  }

protected:
  void InvalidatePaint(PaintInvalidation invalidation = PaintInvalidation::Foreground) {
    if (invalidation != PaintInvalidation::None && invalidate_paint_) {
      invalidate_paint_(invalidation);
    }
  }

  void InvalidateSemantics() {
    if (invalidate_semantics_) {
      invalidate_semantics_();
    }
  }

private:
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

struct ScrollBarStyle {
  float thickness = 6.0F;
  float minimum_thumb_extent = 24.0F;
  float margin = 3.0F;
  float corner_radius = 3.0F;
  float fade_in_duration = 0.12F;
  float fade_out_delay = 0.7F;
  float fade_out_duration = 0.22F;
  Color track_color = Color::Transparent();
  Color thumb_color = Color::Rgb(137, 143, 152, 0.8F);

  static ScrollBarStyle Default();

  bool operator==(const ScrollBarStyle&) const = default;
};

struct ScrollPhysics {
  static const detail::ModifierDescriptor& Descriptor();

  bool fling_enabled = true;
  float deceleration_rate = 3.0F;
  float minimum_fling_velocity = 40.0F;
  float maximum_fling_velocity = 6000.0F;

  bool operator==(const ScrollPhysics&) const = default;
};

struct Enabled {
  static const detail::ModifierDescriptor& Descriptor();

  bool value = true;

  bool operator==(const Enabled&) const = default;
};

struct Focusable {
  static const detail::ModifierDescriptor& Descriptor();

  bool value = true;

  bool operator==(const Focusable&) const = default;
};

struct Padding {
  explicit Padding(float value) : insets(EdgeInsets::All(value)) {}
  explicit Padding(EdgeInsets value) : insets(value) {}

  static const detail::ModifierDescriptor& Descriptor();

  EdgeInsets insets;

  bool operator==(const Padding&) const = default;
};

struct Background {
  static const detail::ModifierDescriptor& Descriptor();

  VisualFill fill;

  bool operator==(const Background&) const = default;
};

struct Border {
  static const detail::ModifierDescriptor& Descriptor();

  Color color;
  float width = 1.0F;

  bool operator==(const Border&) const = default;
};

struct Shadow {
  static const detail::ModifierDescriptor& Descriptor();

  Color color;
  Point offset;
  // blur_radius is the conservative outer falloff; signed spread adjusts the caster before blurring.
  float blur_radius = 0.0F;
  float spread = 0.0F;

  bool operator==(const Shadow&) const = default;
};

struct Foreground {
  static const detail::ModifierDescriptor& Descriptor();

  Color color;

  bool operator==(const Foreground&) const = default;
};

struct FontSize {
  static const detail::ModifierDescriptor& Descriptor();

  float value;

  bool operator==(const FontSize&) const = default;
};

struct Frame {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<float> width;
  std::optional<float> height;
  std::optional<float> min_width;
  std::optional<float> max_width;
  std::optional<float> min_height;
  std::optional<float> max_height;

  bool operator==(const Frame&) const = default;
};

struct CornerRadius {
  static const detail::ModifierDescriptor& Descriptor();

  CornerRadii value;

  bool operator==(const CornerRadius&) const = default;
};

struct ClipChildren {
  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const ClipChildren&) const = default;
};

struct Spacing {
  static const detail::ModifierDescriptor& Descriptor();

  float value;

  bool operator==(const Spacing&) const = default;
};

struct MainAlign {
  static const detail::ModifierDescriptor& Descriptor();

  MainAxisAlignment alignment;

  bool operator==(const MainAlign&) const = default;
};

struct CrossAlign {
  static const detail::ModifierDescriptor& Descriptor();

  CrossAxisAlignment alignment;

  bool operator==(const CrossAlign&) const = default;
};

struct Align {
  static const detail::ModifierDescriptor& Descriptor();

  HorizontalAlignment horizontal;
  VerticalAlignment vertical;

  bool operator==(const Align&) const = default;
};

struct Grow {
  static const detail::ModifierDescriptor& Descriptor();

  float factor = 1.0F;

  bool operator==(const Grow&) const = default;
};

struct ScrollBar {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<ScrollBarStyle> style;

  bool operator==(const ScrollBar&) const = default;
};

} // namespace huxerui
