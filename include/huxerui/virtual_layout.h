#pragma once

#include <cstddef>
#include <optional>
#include <typeindex>
#include <utility>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/layout.h>
#include <huxerui/scroll.h>
#include <huxerui/semantics.h>

namespace huxerui {

class GridColumns {
public:
  static GridColumns Fixed(std::size_t count);
  static GridColumns Adaptive(float minimum_width);

  [[nodiscard]] std::size_t Resolve(float available_width, float spacing) const noexcept;

  bool operator==(const GridColumns&) const = default;

private:
  enum class Mode {
    Fixed,
    Adaptive,
  };

  GridColumns(Mode mode, std::size_t count, float minimum_width) noexcept
      : mode_(mode), count_(count), minimum_width_(minimum_width) {}

  Mode mode_;
  std::size_t count_;
  float minimum_width_;
};

struct VirtualViewport {
  Point offset;
  Size size;

  bool operator==(const VirtualViewport&) const = default;

  [[nodiscard]] Rect Bounds() const noexcept {
    return {
        offset.x,
        offset.y,
        size.width,
        size.height,
    };
  }
};

namespace detail {
struct InternalAccess;
}

class VirtualLayoutContext {
public:
  [[nodiscard]] std::size_t ItemCount() const noexcept {
    return item_count_(state_);
  }

  [[nodiscard]] VirtualViewport Viewport() const noexcept {
    return viewport_(state_);
  }

  ViewNode& Item(std::size_t index) const {
    return item_(state_, index);
  }

  [[nodiscard]] Size Measure(ViewNode& item, Constraints constraints) const {
    return measure_(state_, item, constraints);
  }

private:
  using ItemCountFunction = std::size_t (*)(void*);
  using ViewportFunction = VirtualViewport (*)(void*);
  using ItemFunction = ViewNode& (*)(void*, std::size_t);
  using MeasureFunction = Size (*)(void*, ViewNode&, Constraints);

  VirtualLayoutContext(
      void* state, ItemCountFunction item_count, ViewportFunction viewport, ItemFunction item, MeasureFunction measure
  )
      : state_(state), item_count_(item_count), viewport_(viewport), item_(item), measure_(measure) {}

  void* state_;
  ItemCountFunction item_count_;
  ViewportFunction viewport_;
  ItemFunction item_;
  MeasureFunction measure_;

  friend struct detail::InternalAccess;
};

class VirtualLayoutResult {
public:
  struct Placement {
    ViewNode* item;
    Point offset;
    std::optional<SemanticCollectionItem> collection_item;
  };

  VirtualLayoutResult& Place(ViewNode& item, Point offset) {
    placements_.push_back({&item, offset, std::nullopt});
    return *this;
  }

  // Collection metadata follows the same realized placement commit, so Runtime never materializes semantic-only items.
  VirtualLayoutResult& Place(ViewNode& item, Point offset, SemanticCollectionItem collection_item) {
    placements_.push_back({&item, offset, std::move(collection_item)});
    return *this;
  }

  VirtualLayoutResult& SetCollectionSemantics(
      SemanticRole role, SemanticRole item_role, SemanticCollection collection
  ) noexcept {
    collection_role_ = role;
    collection_item_role_ = item_role;
    collection_ = std::move(collection);
    return *this;
  }

  VirtualLayoutResult& SetSize(Size size) noexcept {
    size_ = size;
    return *this;
  }

  VirtualLayoutResult& SetContentSize(Size size) noexcept {
    content_size_ = size;
    return *this;
  }

  VirtualLayoutResult& SetAxis(Axis axis) noexcept {
    axis_ = axis;
    return *this;
  }

  VirtualLayoutResult& SetScrollOffset(float offset) noexcept {
    scroll_offset_ = offset;
    return *this;
  }

  [[nodiscard]] Size MeasuredSize() const noexcept {
    return size_;
  }

  [[nodiscard]] Size ContentSize() const noexcept {
    return content_size_;
  }

  [[nodiscard]] Axis ScrollAxis() const noexcept {
    return axis_;
  }

  [[nodiscard]] std::optional<float> CorrectedScrollOffset() const noexcept {
    return scroll_offset_;
  }

  [[nodiscard]] const std::vector<Placement>& Placements() const noexcept {
    return placements_;
  }

private:
  Size size_;
  Size content_size_;
  Axis axis_ = Axis::Vertical;
  std::optional<float> scroll_offset_;
  std::vector<Placement> placements_;
  SemanticRole collection_role_ = SemanticRole::Generic;
  SemanticRole collection_item_role_ = SemanticRole::Generic;
  std::optional<SemanticCollection> collection_;

  friend struct detail::InternalAccess;
};

namespace detail {

struct VirtualLayoutDescriptor {
  std::type_index type;
  VirtualLayoutResult (*measure)(VirtualLayoutContext&, ViewNode&, Constraints);
  std::optional<float> (*scroll_offset_for_item)(ViewNode&, std::size_t, ScrollAlignment, float);
};

template <class Derived> const VirtualLayoutDescriptor& VirtualLayoutDescriptorFor() {
  static const VirtualLayoutDescriptor descriptor{
      typeid(Derived),
      [](VirtualLayoutContext& context, ViewNode& node, Constraints constraints) -> VirtualLayoutResult {
        return Derived::Measure(context, node, constraints);
      },
      [](ViewNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent)
          -> std::optional<float> {
        if constexpr (requires { Derived::ScrollOffsetForItem(node, index, alignment, viewport_extent); }) {
          return Derived::ScrollOffsetForItem(node, index, alignment, viewport_extent);
        } else {
          return std::nullopt;
        }
      },
  };
  return descriptor;
}

} // namespace detail

} // namespace huxerui
