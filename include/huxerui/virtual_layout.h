#pragma once

#include <cstddef>
#include <optional>
#include <typeindex>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/layout.h>
#include <huxerui/scroll_state.h>

namespace huxerui {

enum class Axis {
  Horizontal,
  Vertical,
};

class GridColumns {
public:
  static GridColumns Fixed(std::size_t count);
  static GridColumns Adaptive(float minimum_width);

  [[nodiscard]] std::size_t Resolve(float available_width,
                                    float spacing) const noexcept;

  bool operator==(const GridColumns &) const = default;

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
struct VirtualLayoutContextAccess;
}

class VirtualLayoutContext {
public:
  [[nodiscard]] std::size_t ItemCount() const noexcept {
    return item_count_(state_);
  }

  [[nodiscard]] VirtualViewport Viewport() const noexcept {
    return viewport_(state_);
  }

  MountedNode &Item(std::size_t index) const { return item_(state_, index); }

  [[nodiscard]] Size Measure(MountedNode &item, Constraints constraints) const {
    return measure_(state_, item, constraints);
  }

private:
  using ItemCountFunction = std::size_t (*)(void *);
  using ViewportFunction = VirtualViewport (*)(void *);
  using ItemFunction = MountedNode &(*)(void *, std::size_t);
  using MeasureFunction = Size (*)(void *, MountedNode &, Constraints);

  VirtualLayoutContext(void *state, ItemCountFunction item_count,
                       ViewportFunction viewport, ItemFunction item,
                       MeasureFunction measure)
      : state_(state), item_count_(item_count), viewport_(viewport),
        item_(item), measure_(measure) {}

  void *state_;
  ItemCountFunction item_count_;
  ViewportFunction viewport_;
  ItemFunction item_;
  MeasureFunction measure_;

  friend struct detail::VirtualLayoutContextAccess;
};

class VirtualLayoutResult {
public:
  struct Placement {
    MountedNode *item;
    Point offset;
  };

  VirtualLayoutResult &Place(MountedNode &item, Point offset) {
    placements_.push_back({&item, offset});
    return *this;
  }

  VirtualLayoutResult &SetSize(Size size) noexcept {
    size_ = size;
    return *this;
  }

  VirtualLayoutResult &SetContentSize(Size size) noexcept {
    content_size_ = size;
    return *this;
  }

  VirtualLayoutResult &SetAxis(Axis axis) noexcept {
    axis_ = axis;
    return *this;
  }

  VirtualLayoutResult &SetScrollOffset(float offset) noexcept {
    scroll_offset_ = offset;
    return *this;
  }

  [[nodiscard]] Size MeasuredSize() const noexcept { return size_; }

  [[nodiscard]] Size ContentSize() const noexcept { return content_size_; }

  [[nodiscard]] Axis ScrollAxis() const noexcept { return axis_; }

  [[nodiscard]] std::optional<float> CorrectedScrollOffset() const noexcept {
    return scroll_offset_;
  }

  [[nodiscard]] const std::vector<Placement> &Placements() const noexcept {
    return placements_;
  }

private:
  Size size_;
  Size content_size_;
  Axis axis_ = Axis::Vertical;
  std::optional<float> scroll_offset_;
  std::vector<Placement> placements_;
};

namespace detail {

struct VirtualLayoutDescriptor {
  std::type_index type;
  VirtualLayoutResult (*measure)(VirtualLayoutContext &, MountedNode &,
                                 Constraints);
  std::optional<float> (*scroll_offset_for_item)(MountedNode &, std::size_t,
                                                 ScrollAlignment, float);
};

template <class Derived>
const VirtualLayoutDescriptor &VirtualLayoutDescriptorFor() {
  static const VirtualLayoutDescriptor descriptor{
      typeid(Derived),
      [](VirtualLayoutContext &context, MountedNode &node,
         Constraints constraints) -> VirtualLayoutResult {
        return Derived::Measure(context, node, constraints);
      },
      [](MountedNode &node, std::size_t index, ScrollAlignment alignment,
         float viewport_extent) -> std::optional<float> {
        if constexpr (requires {
                        Derived::ScrollOffsetForItem(node, index, alignment,
                                                     viewport_extent);
                      }) {
          return Derived::ScrollOffsetForItem(node, index, alignment,
                                              viewport_extent);
        } else {
          return std::nullopt;
        }
      },
  };
  return descriptor;
}

struct VirtualListAxis {
  using Value = Axis;
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

} // namespace detail

} // namespace huxerui
