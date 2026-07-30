#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include <huxerui/color.h>
#include <huxerui/display_list.h>
#include <huxerui/event.h>
#include <huxerui/geometry.h>
#include <huxerui/layout.h>

namespace huxerui {

class TextInputClient;

struct FrameInfo {
  double timestamp = 0.0;
  double delta_time = 0.0;
};

class NodeExtension {
public:
  struct FrameResult {
    bool needs_frame = false;
    std::optional<double> wake_after;
  };

  enum class PointerResult {
    Ignored,
    Observe,
    Handled,
    Capture,
  };

  virtual ~NodeExtension() = default;

  virtual FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) {
    static_cast<void>(node);
    static_cast<void>(frame);
    return {};
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

  [[nodiscard]] virtual std::shared_ptr<TextInputClient> GetTextInputClient() noexcept {
    return {};
  }

  virtual PointerResult OnPointer(MountedNode& node, const PointerEvent& event) {
    static_cast<void>(node);
    static_cast<void>(event);
    return PointerResult::Ignored;
  }

  virtual void Paint(const MountedNode& node, DisplayList& display_list) const {
    static_cast<void>(node);
    static_cast<void>(display_list);
  }
};

namespace detail {

struct ViewSpec;

struct ModifierDescriptor {
  void (*apply)(ViewSpec&, const void*) = nullptr;
  std::unique_ptr<NodeExtension> (*create_extension)(MountedNode&, const void*) = nullptr;
  void (*update_extension)(NodeExtension&, MountedNode&, const void*) = nullptr;
};

struct ModifierSpec {
  const ModifierDescriptor* descriptor = nullptr;
  std::shared_ptr<const void> value;
};

template <class Spec, class Extension>
  requires std::derived_from<Extension, NodeExtension> &&
           std::constructible_from<Extension, MountedNode&, const Spec&> &&
           requires(Extension& extension, MountedNode& node, const Spec& spec) { extension.Update(node, spec); }
const ModifierDescriptor& ModifierDescriptorFor() {
  static const ModifierDescriptor descriptor{
      nullptr,
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<Extension>(node, *static_cast<const Spec*>(value));
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<Extension&>(extension).Update(node, *static_cast<const Spec*>(value));
      },
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
};

struct ScrollPhysics {
  static const detail::ModifierDescriptor& Descriptor();

  bool fling_enabled = true;
  float deceleration_rate = 6.0F;
  float minimum_fling_velocity = 40.0F;
  float maximum_fling_velocity = 6000.0F;
};

struct Enabled {
  static const detail::ModifierDescriptor& Descriptor();

  bool value = true;
};

struct Focusable {
  static const detail::ModifierDescriptor& Descriptor();

  bool value = true;
};

struct Padding {
  explicit Padding(float value) : insets(EdgeInsets::All(value)) {}
  explicit Padding(EdgeInsets value) : insets(value) {}

  static const detail::ModifierDescriptor& Descriptor();

  EdgeInsets insets;
};

struct Background {
  static const detail::ModifierDescriptor& Descriptor();

  Color color;
};

struct Foreground {
  static const detail::ModifierDescriptor& Descriptor();

  Color color;
};

struct FontSize {
  static const detail::ModifierDescriptor& Descriptor();

  float value;
};

struct Frame {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<float> width;
  std::optional<float> height;
  std::optional<float> min_width;
  std::optional<float> max_width;
  std::optional<float> min_height;
  std::optional<float> max_height;
};

struct CornerRadius {
  static const detail::ModifierDescriptor& Descriptor();

  float value;
};

struct Spacing {
  static const detail::ModifierDescriptor& Descriptor();

  float value;
};

struct MainAlign {
  static const detail::ModifierDescriptor& Descriptor();

  MainAxisAlignment alignment;
};

struct CrossAlign {
  static const detail::ModifierDescriptor& Descriptor();

  CrossAxisAlignment alignment;
};

struct Align {
  static const detail::ModifierDescriptor& Descriptor();

  HorizontalAlignment horizontal;
  VerticalAlignment vertical;
};

struct Grow {
  static const detail::ModifierDescriptor& Descriptor();

  float factor = 1.0F;
};

struct ScrollBar {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<ScrollBarStyle> style;
};

} // namespace huxerui
