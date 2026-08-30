#pragma once

#include <array>
#include <span>

#include <huxerui/vector.h>

namespace huxerui::detail {

enum class PathVerb {
  MoveTo,
  LineTo,
  QuadraticTo,
  CubicTo,
  Close,
};

struct PathElement {
  PathVerb verb = PathVerb::MoveTo;
  // MoveTo and LineTo use points[0], QuadraticTo uses points[0..1], and CubicTo uses points[0..2].
  std::array<Point, 3> points{};

  bool operator==(const PathElement&) const = default;
};

class PathAccess final {
public:
  [[nodiscard]] static std::span<const PathElement> Elements(const Path& path) noexcept;
};

[[nodiscard]] Path CreateBorderStrokePath(Rect rect, CornerRadii corner_radii, float width);

} // namespace huxerui::detail
