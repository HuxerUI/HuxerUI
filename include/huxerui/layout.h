#pragma once

#include <any>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include <huxerui/event.h>
#include <huxerui/geometry.h>

namespace huxerui {

struct WindowTitleBarMetrics;

/// Distributes children and remaining space along a layout's main axis.
///
/// The main axis is horizontal for Row and vertical for Column. Space-distribution modes add to the configured
/// Spacing rather than replacing it.
enum class MainAxisAlignment {
  /// Places the child group at the beginning of the axis.
  Start,
  /// Centers the child group along the axis.
  Center,
  /// Places the child group at the end of the axis.
  End,
  /// Distributes remaining space between children, with no additional space at the outer edges.
  SpaceBetween,
  /// Distributes remaining space around children, with half a share at each outer edge.
  SpaceAround,
  /// Distributes remaining space equally before, between, and after children.
  SpaceEvenly,
};

/// Aligns children perpendicular to a layout's main axis.
///
/// The cross axis is horizontal for Column and vertical for Row. A custom layout chooses whether to honor this value.
enum class CrossAxisAlignment {
  /// Aligns children at the beginning of the cross axis.
  Start,
  /// Centers each child on the cross axis.
  Center,
  /// Aligns children at the end of the cross axis.
  End,
  /// Requests that children fill the available cross-axis extent.
  Stretch,
};

/// Selects horizontal placement or stretching within an available rectangle.
enum class HorizontalAlignment {
  /// Aligns at the start of the horizontal axis.
  Start,
  /// Centers horizontally.
  Center,
  /// Aligns at the end of the horizontal axis.
  End,
  /// Requests the available width when the consuming layout or component supports stretching.
  Stretch,
};

/// Selects vertical placement or stretching within an available rectangle.
enum class VerticalAlignment {
  /// Aligns at the top edge.
  Start,
  /// Centers vertically.
  Center,
  /// Aligns at the bottom edge.
  End,
  /// Requests the available height when the consuming layout or component supports stretching.
  Stretch,
};

namespace detail {
struct InternalAccess;

struct ErasedLayoutValue {
  std::any value;
  bool (*equals)(const std::any&, const std::any&) = nullptr;

  [[nodiscard]] bool EquivalentForReconciliation(const ErasedLayoutValue& other) const {
    return equals != nullptr && other.equals != nullptr && value.type() == other.value.type() &&
           equals(value, other.value);
  }
};

template <class T> ErasedLayoutValue MakeErasedLayoutValue(T&& value) {
  using Value = std::remove_cvref_t<T>;
  ErasedLayoutValue result{
      std::any(std::forward<T>(value)),
      nullptr,
  };
  if constexpr (std::equality_comparable<Value>) {
    result.equals = [](const std::any& left, const std::any& right) {
      const auto* typed_left = std::any_cast<Value>(&left);
      const auto* typed_right = std::any_cast<Value>(&right);
      return typed_left != nullptr && typed_right != nullptr && *typed_left == *typed_right;
    };
  }
  return result;
}
} // namespace detail

/// Public interface to a Runtime-owned node in the mounted View tree.
///
/// Custom layouts and NodeExtension callbacks receive this interface; application code does not create or own mounted
/// nodes. Child ranges, references, and metadata pointers are borrowed and must not be retained across reconciliation.
/// Geometry queries do not trigger measurement or layout. During measurement, use LayoutContext::Measure() and
/// LayoutSize(); Bounds() and LayoutOffset() describe the latest completed layout.
///
/// Local/window conversions use the resolved ancestor presentation transform. NodeExtension::PrepareGeometry() sees
/// the final transform for the current frame; earlier callbacks may still see the previous resolution. Paint commands
/// remain node-local even when geometry preparation publishes a window-space snapshot:
/// @code
/// const Rect local_content = node.ContentBounds();
/// const Rect window_content = node.LocalToWindowBounds(local_content);
/// @endcode
class ViewNode {
public:
  /// Forward iterator over the current direct children of a mounted node.
  class ChildIterator {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = ViewNode;
    using reference = ViewNode&;
    using pointer = ViewNode*;
    using iterator_category = std::forward_iterator_tag;

    ChildIterator() = default;

    reference operator*() const {
      return owner_->ChildAt(index_);
    }

    pointer operator->() const {
      return &owner_->ChildAt(index_);
    }

    ChildIterator& operator++() {
      ++index_;
      return *this;
    }

    ChildIterator operator++(int) {
      ChildIterator previous = *this;
      ++*this;
      return previous;
    }

    bool operator==(const ChildIterator&) const = default;

  private:
    ChildIterator(ViewNode& owner, std::size_t index) : owner_(&owner), index_(index) {}

    ViewNode* owner_ = nullptr;
    std::size_t index_ = 0;

    friend class ViewNode;
  };

  /// Non-owning range over a node's current direct children in mounted order.
  class ChildrenRange {
  public:
    /// Returns an iterator to the first child.
    ChildIterator begin() const {
      return ChildIterator{*owner_, 0};
    }

    /// Returns the past-the-end iterator.
    ChildIterator end() const {
      return ChildIterator{*owner_, owner_->ChildCount()};
    }

    /// Returns the current number of children, without copying them.
    [[nodiscard]] std::size_t Size() const noexcept {
      return owner_->ChildCount();
    }

    /// Returns whether the node currently has no direct children.
    [[nodiscard]] bool Empty() const noexcept {
      return Size() == 0;
    }

  private:
    explicit ChildrenRange(ViewNode& owner) : owner_(&owner) {}

    ViewNode* owner_;

    friend class ViewNode;
  };

  virtual ~ViewNode() = default;

  /// Returns a borrowed range for measuring or inspecting the current direct children.
  [[nodiscard]] ChildrenRange Children() noexcept {
    return ChildrenRange{*this};
  }

  /// Returns the number of current direct children.
  [[nodiscard]] virtual std::size_t ChildCount() const noexcept = 0;

  /// Returns a direct child by its position in the current mounted order.
  /// @param index Zero-based child index, less than ChildCount().
  /// @return A borrowed child reference valid for the current callback.
  /// @throws std::out_of_range if index is outside the current child range.
  [[nodiscard]] virtual ViewNode& ChildAt(std::size_t index) = 0;

  /// Returns a read-only direct child by its position in the current mounted order.
  /// @param index Zero-based child index, less than ChildCount().
  /// @return A borrowed child reference valid for the current callback.
  /// @throws std::out_of_range if index is outside the current child range.
  [[nodiscard]] virtual const ViewNode& ChildAt(std::size_t index) const = 0;

  /// Returns the latest measured outer size in DIPs, including this node's resolved Padding.
  ///
  /// LayoutContext::Measure() updates this value for a child before returning. It may differ from Bounds() until the
  /// corresponding layout pass completes.
  [[nodiscard]] virtual Size LayoutSize() const noexcept = 0;

  /// Returns the complete layout rectangle in node-local DIPs, with a zero origin and including Padding.
  ///
  /// Use this rectangle for a full-node background, border, or hit area. It does not clip painting or include overflow
  /// from shadows and descendants.
  [[nodiscard]] virtual Rect Bounds() const noexcept = 0;

  /// Returns Bounds() inset by the node's resolved Padding, with width and height clamped to at least zero.
  ///
  /// The rectangle stays in node-local DIPs; its origin is the left/top content inset, not necessarily zero. Resolved
  /// Padding includes safe-area edges consumed by this node's SafeAreaPadding. Use it for padding-aware content drawing
  /// or hit testing. It is neither a visible clip nor the union of child bounds, and does not subtract Border width.
  [[nodiscard]] virtual Rect ContentBounds() const noexcept = 0;

  /// Returns the node's layout origin in its parent's local DIPs, before presentation transforms.
  [[nodiscard]] virtual Point LayoutOffset() const noexcept = 0;

  /// Returns the axis-aligned window-space bound of Bounds() after the resolved presentation transform.
  ///
  /// This is a layout-geometry query, not the clipped visible region or the extent of all painted pixels.
  [[nodiscard]] virtual Rect PresentationBounds() const noexcept = 0;

  /// Maps a node-local point into window logical coordinates using the resolved presentation transform.
  /// @param point Position in this node's local DIPs; it may lie outside Bounds().
  /// @return The corresponding position in window DIPs, not screen coordinates or physical pixels.
  [[nodiscard]] virtual Point LocalToWindow(Point point) const noexcept = 0;

  /// Maps a window-logical point into node-local coordinates when the resolved transform is invertible.
  /// @param point Position in window DIPs, not screen coordinates or physical pixels.
  /// @return The node-local position, or std::nullopt when the resolved transform is non-invertible.
  [[nodiscard]] virtual std::optional<Point> WindowToLocal(Point point) const noexcept = 0;

  /// Maps a node-local rectangle to its axis-aligned bounds in window logical coordinates.
  ///
  /// All four corners are transformed, so rotation can make the returned rectangle larger than the transformed shape.
  /// @param bounds Rectangle in this node's local DIPs; it may extend outside Bounds().
  /// @return A conservative axis-aligned rectangle in window DIPs, without clipping.
  [[nodiscard]] virtual Rect LocalToWindowBounds(Rect bounds) const noexcept = 0;

  /// Returns the resolved effective opacity, including ancestor opacity, in the inclusive range from zero to one.
  [[nodiscard]] virtual float PresentationOpacity() const noexcept = 0;

  /// Returns whether this node and its interaction ancestors are enabled.
  [[nodiscard]] virtual bool IsEnabled() const noexcept = 0;

  /// Returns whether this node itself currently owns keyboard focus.
  [[nodiscard]] virtual bool IsFocused() const noexcept = 0;

  /// Returns the borrowed effective enabled, hover, focus, and press state for this node.
  [[nodiscard]] virtual const InteractionState& Interaction() const noexcept = 0;

  /// Returns the configured inter-child spacing in DIPs, or zero when no Spacing modifier was supplied.
  ///
  /// Custom layouts decide how to apply this value; querying it does not insert gaps automatically.
  [[nodiscard]] virtual float Spacing() const noexcept = 0;

  /// Returns this child's main-axis growth weight for a compatible parent layout, or zero when absent.
  ///
  /// Built-in Row and Column divide remaining bounded main-axis space in proportion to positive growth weights.
  [[nodiscard]] virtual float GrowFactor() const noexcept = 0;

  /// Returns the configured main-axis alignment for this node's children, defaulting to MainAxisAlignment::Start.
  [[nodiscard]] virtual MainAxisAlignment MainAlignment() const noexcept = 0;

  /// Returns the configured cross-axis alignment for this node's children, defaulting to CrossAxisAlignment::Start.
  [[nodiscard]] virtual CrossAxisAlignment CrossAlignment() const noexcept = 0;

  /// Returns the configured horizontal alignment, used by layouts such as Stack and defaulting to Start.
  [[nodiscard]] virtual HorizontalAlignment HorizontalAlignmentValue() const noexcept = 0;

  /// Returns the configured vertical alignment, used by layouts such as Stack and defaulting to Start.
  [[nodiscard]] virtual VerticalAlignment VerticalAlignmentValue() const noexcept = 0;

  /// Looks up typed parent-child layout metadata applied with View::LayoutValue().
  ///
  /// The key type identifies the meaning independently of its value type. Use different key types for independent
  /// values even when they share the same representation.
  /// @code
  /// struct ColumnSpan { using Value = std::size_t; };
  /// auto label = Text("Wide").LayoutValue<ColumnSpan>(std::size_t{2});
  ///
  /// // Inside a custom layout's Measure callback:
  /// const auto span = node.ChildAt(0).LayoutValueOr<ColumnSpan>(std::size_t{1});
  /// @endcode
  /// @tparam Key Semantic key type declaring a nested Value type.
  /// @return A borrowed value pointer, or nullptr when the key is absent. Do not retain it across reconciliation.
  template <class Key> [[nodiscard]] const typename Key::Value* LayoutValue() const noexcept {
    const std::any* value = FindLayoutValue(typeid(Key));
    return value == nullptr ? nullptr : std::any_cast<typename Key::Value>(value);
  }

  /// Returns typed layout metadata by value, or a caller-supplied default when the key is absent.
  /// @tparam Key Semantic key type declaring a nested Value type.
  /// @param fallback Value returned when this node has no metadata for Key.
  /// @return A copy of the stored value, or fallback.
  template <class Key> [[nodiscard]] typename Key::Value LayoutValueOr(typename Key::Value fallback) const {
    if (const auto* value = LayoutValue<Key>()) {
      return *value;
    }
    return fallback;
  }

  /// Returns one retained layout cache object identified by its type, constructing it on first use.
  ///
  /// The cache survives compatible recomposition and is destroyed with the mounted node. Constructor arguments are
  /// ignored after the object exists. The layout remains responsible for checking whether cached inputs are current;
  /// mutating the cache does not request measurement or invalidate paint.
  /// @tparam T Copy-constructible cache type; use a distinct type for each independent cache on the same node.
  /// @tparam Arguments Constructor argument types forwarded when the cache is first created.
  /// @param arguments Arguments used only to construct the first T instance.
  /// @return A reference to the node-owned cache object. Do not retain mounted-child references inside it.
  template <class T, class... Arguments>
    requires std::constructible_from<T, Arguments...>
  T& Cache(Arguments&&... arguments) {
    std::any& entry = EnsureCacheEntry(typeid(T));
    if (!entry.has_value()) {
      entry.emplace<T>(std::forward<Arguments>(arguments)...);
    }
    auto* value = std::any_cast<T>(&entry);
    if (value == nullptr) {
      throw std::logic_error("HuxerUI layout cache type mismatch");
    }
    return *value;
  }

protected:
  virtual const std::any* FindLayoutValue(std::type_index key) const noexcept = 0;
  virtual std::any& EnsureCacheEntry(std::type_index key) = 0;
};

/// Measures children and exposes window geometry for one custom layout measurement callback.
///
/// Measure children through this context rather than calling their layout policies directly. Runtime owns measurement
/// caching and child lifetimes. The context and its borrowed values must not escape the current Measure callback.
class LayoutContext {
public:
  /// Measures one current child and updates its ViewNode::LayoutSize().
  ///
  /// Measurement does not place the child; add it to the returned LayoutResult to participate in this layout.
  /// @param child A direct child obtained from the owning node's Children() or ChildAt().
  /// @param constraints Valid size limits in DIPs for the child's outer size, including its own Padding.
  /// @return The child's measured outer size under the supplied constraints and its declarative modifiers.
  [[nodiscard]] Size Measure(ViewNode& child, Constraints constraints) const {
    return measure_(state_, child, constraints);
  }

  /// Returns the remaining safe-area insets in DIPs after already-consumed SafeAreaPadding edges are removed.
  [[nodiscard]] EdgeInsets SafeAreaInsets() const noexcept {
    return safe_area_;
  }

  /// Returns borrowed native title-bar geometry when application content occupies title-bar space.
  ///
  /// Include `<huxerui/window.h>` to inspect the returned fields. This is frame-local measurement input, not persistent
  /// application state.
  /// @return The current title-bar metrics, or nullptr when title-bar content geometry is unavailable.
  [[nodiscard]] const WindowTitleBarMetrics* TitleBarMetrics() const noexcept {
    return title_bar_metrics_;
  }

private:
  using MeasureFunction = Size (*)(void*, ViewNode&, Constraints);

  LayoutContext(
      void* state, MeasureFunction measure, EdgeInsets safe_area, const WindowTitleBarMetrics* title_bar_metrics
  )
      : state_(state), measure_(measure), safe_area_(safe_area), title_bar_metrics_(title_bar_metrics) {}

  void* state_;
  MeasureFunction measure_;
  EdgeInsets safe_area_;
  const WindowTitleBarMetrics* title_bar_metrics_;

  friend struct detail::InternalAccess;
};

/// Describes a custom layout's measured content size and the children participating in that layout.
///
/// Runtime passes the layout constraints with the owning node's resolved Padding already removed. Return a content
/// size satisfying those constraints and place children relative to the content origin; Runtime adds the owning node's
/// Padding to both the outer size and child origins. Do not add it again in the layout policy.
///
/// This example requires `<huxerui/view.h>`, which declares Layout and the ordinary View types:
/// @code
/// class SimpleRow final : public Layout<SimpleRow> {
/// public:
///   using Layout::Layout;
///
///   static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
///     LayoutResult result;
///     float width = 0.0F;
///     float height = 0.0F;
///     for (ViewNode& child : node.Children()) {
///       const Size size = context.Measure(child, constraints.Loose());
///       result.Place(child, {width, 0.0F});
///       width += size.width;
///       height = std::max(height, size.height);
///     }
///     return result.SetSize(constraints.Constrain({width, height}));
///   }
/// };
/// @endcode
class LayoutResult {
public:
  /// Records the content-relative origin of one participating direct child.
  struct Placement {
    /// Borrowed child measured through the current LayoutContext.
    ViewNode* child;
    /// Child origin in DIPs relative to the owning layout's content origin, before presentation transforms.
    Point offset;
  };

  /// Appends a measured child placement without changing its measured size.
  ///
  /// Place each participating child at most once. Unplaced children remain mounted but do not participate in this
  /// layout. Placement offsets do not establish a clip or change the declaration's paint order.
  /// @param child A direct child measured through the current LayoutContext.
  /// @param offset Finite child origin in DIPs relative to the owning layout's content origin, excluding its Padding.
  /// @return This result for chaining additional placements or SetSize().
  LayoutResult& Place(ViewNode& child, Point offset) {
    placements_.push_back({&child, offset});
    return *this;
  }

  /// Sets the layout's measured content size without adding the owning node's Padding.
  /// @param size Finite nonnegative size in DIPs, normally produced by constraints.Constrain().
  /// @return This result for chaining.
  LayoutResult& SetSize(huxerui::Size size) noexcept {
    size_ = size;
    return *this;
  }

  /// Returns the recorded content size, or a zero size before SetSize() is called.
  [[nodiscard]] huxerui::Size MeasuredSize() const noexcept {
    return size_;
  }

  /// Returns the recorded placements in insertion order without copying them.
  [[nodiscard]] const std::vector<Placement>& Placements() const noexcept {
    return placements_;
  }

private:
  huxerui::Size size_;
  std::vector<Placement> placements_;
};

namespace detail {

struct LayoutDescriptor {
  std::type_index type;
  LayoutResult (*measure)(LayoutContext&, ViewNode&, Constraints);
};

template <class Derived> const LayoutDescriptor& LayoutDescriptorFor() {
  static const LayoutDescriptor descriptor{
      typeid(Derived),
      [](LayoutContext& context, ViewNode& node, Constraints constraints) -> LayoutResult {
        return Derived::Measure(context, node, constraints);
      },
  };
  return descriptor;
}

} // namespace detail

} // namespace huxerui
