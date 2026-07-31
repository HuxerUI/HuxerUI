#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace huxerui {

struct Point {
  float x = 0.0F;
  float y = 0.0F;

  bool operator==(const Point&) const = default;
};

struct Size {
  float width = 0.0F;
  float height = 0.0F;

  bool operator==(const Size&) const = default;
};

struct Rect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;

  bool operator==(const Rect&) const = default;

  [[nodiscard]] bool Contains(Point point) const noexcept {
    return point.x >= x && point.x <= x + width && point.y >= y && point.y <= y + height;
  }

  [[nodiscard]] bool IsEmpty() const noexcept {
    return width <= 0.0F || height <= 0.0F;
  }

  [[nodiscard]] bool Intersects(const Rect& other) const noexcept {
    return !IsEmpty() && !other.IsEmpty() && x < other.x + other.width && x + width > other.x &&
           y < other.y + other.height && y + height > other.y;
  }

  [[nodiscard]] Rect Intersection(const Rect& other) const noexcept {
    const float left = std::max(x, other.x);
    const float top = std::max(y, other.y);
    const float right = std::min(x + width, other.x + other.width);
    const float bottom = std::min(y + height, other.y + other.height);
    return {
        left,
        top,
        std::max(0.0F, right - left),
        std::max(0.0F, bottom - top),
    };
  }
};

struct Transform2D {
  float m11 = 1.0F;
  float m12 = 0.0F;
  float m21 = 0.0F;
  float m22 = 1.0F;
  float translate_x = 0.0F;
  float translate_y = 0.0F;

  bool operator==(const Transform2D&) const = default;

  [[nodiscard]] bool IsIdentity() const noexcept {
    return m11 == 1.0F && m12 == 0.0F && m21 == 0.0F && m22 == 1.0F && translate_x == 0.0F &&
           translate_y == 0.0F;
  }

  [[nodiscard]] Point Apply(Point point) const noexcept {
    return {
        m11 * point.x + m21 * point.y + translate_x,
        m12 * point.x + m22 * point.y + translate_y,
    };
  }

  [[nodiscard]] std::optional<Point> Inverse(Point point) const noexcept {
    const float determinant = m11 * m22 - m12 * m21;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001F) {
      return std::nullopt;
    }
    const float x = point.x - translate_x;
    const float y = point.y - translate_y;
    return Point{
        (m22 * x - m21 * y) / determinant,
        (-m12 * x + m11 * y) / determinant,
    };
  }
};

struct EdgeInsets {
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
  float left = 0.0F;

  bool operator==(const EdgeInsets&) const = default;

  static EdgeInsets All(float value) noexcept {
    return {value, value, value, value};
  }

  static EdgeInsets Symmetric(float horizontal, float vertical) noexcept {
    return {vertical, horizontal, vertical, horizontal};
  }

  [[nodiscard]] float Horizontal() const noexcept {
    return left + right;
  }

  [[nodiscard]] float Vertical() const noexcept {
    return top + bottom;
  }
};

struct Constraints {
  float min_width = 0.0F;
  float max_width = std::numeric_limits<float>::infinity();
  float min_height = 0.0F;
  float max_height = std::numeric_limits<float>::infinity();

  bool operator==(const Constraints&) const = default;

  [[nodiscard]] Size Constrain(Size size) const noexcept {
    return {
        std::clamp(size.width, min_width, max_width),
        std::clamp(size.height, min_height, max_height),
    };
  }

  [[nodiscard]] float ConstrainWidth(float width) const noexcept {
    return std::clamp(width, min_width, max_width);
  }

  [[nodiscard]] float ConstrainHeight(float height) const noexcept {
    return std::clamp(height, min_height, max_height);
  }

  [[nodiscard]] bool HasBoundedWidth() const noexcept {
    return std::isfinite(max_width);
  }

  [[nodiscard]] bool HasBoundedHeight() const noexcept {
    return std::isfinite(max_height);
  }

  [[nodiscard]] Constraints Loose() const noexcept {
    return {0.0F, max_width, 0.0F, max_height};
  }

  [[nodiscard]] Constraints LooseWidth() const noexcept {
    return {0.0F, max_width, min_height, max_height};
  }

  [[nodiscard]] Constraints LooseHeight() const noexcept {
    return {min_width, max_width, 0.0F, max_height};
  }

  [[nodiscard]] Constraints TightWidth(float width) const noexcept {
    const float constrained = ConstrainWidth(width);
    return {constrained, constrained, min_height, max_height};
  }

  [[nodiscard]] Constraints TightHeight(float height) const noexcept {
    const float constrained = ConstrainHeight(height);
    return {min_width, max_width, constrained, constrained};
  }

  [[nodiscard]] Constraints Deflate(EdgeInsets insets) const noexcept {
    const float horizontal = insets.Horizontal();
    const float vertical = insets.Vertical();
    return {
        std::max(0.0F, min_width - horizontal),
        std::max(0.0F, max_width - horizontal),
        std::max(0.0F, min_height - vertical),
        std::max(0.0F, max_height - vertical),
    };
  }
};

} // namespace huxerui
