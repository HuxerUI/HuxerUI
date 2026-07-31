#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include <huxerui/geometry.h>

namespace huxerui::detail {

inline Transform2D ComposeTransform(const Transform2D& outer, const Transform2D& inner) noexcept {
  return {
      outer.m11 * inner.m11 + outer.m21 * inner.m12,
      outer.m12 * inner.m11 + outer.m22 * inner.m12,
      outer.m11 * inner.m21 + outer.m21 * inner.m22,
      outer.m12 * inner.m21 + outer.m22 * inner.m22,
      outer.m11 * inner.translate_x + outer.m21 * inner.translate_y + outer.translate_x,
      outer.m12 * inner.translate_x + outer.m22 * inner.translate_y + outer.translate_y,
  };
}

inline Transform2D TranslationTransform(Point offset) noexcept {
  return {
      1.0F,
      0.0F,
      0.0F,
      1.0F,
      offset.x,
      offset.y,
  };
}

inline std::optional<Transform2D> InverseTransform(const Transform2D& transform) noexcept {
  const float determinant = transform.m11 * transform.m22 - transform.m12 * transform.m21;
  if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001F) {
    return std::nullopt;
  }
  const float inverse_m11 = transform.m22 / determinant;
  const float inverse_m12 = -transform.m12 / determinant;
  const float inverse_m21 = -transform.m21 / determinant;
  const float inverse_m22 = transform.m11 / determinant;
  return Transform2D{
      inverse_m11,
      inverse_m12,
      inverse_m21,
      inverse_m22,
      -(inverse_m11 * transform.translate_x + inverse_m21 * transform.translate_y),
      -(inverse_m12 * transform.translate_x + inverse_m22 * transform.translate_y),
  };
}

inline Transform2D AroundOriginTransform(const Transform2D& linear, Point origin) noexcept {
  return ComposeTransform(
      TranslationTransform(origin),
      ComposeTransform(linear, TranslationTransform({-origin.x, -origin.y}))
  );
}

inline Rect TransformBounds(const Transform2D& transform, Rect rect) noexcept {
  const Point top_left = transform.Apply({rect.x, rect.y});
  const Point top_right = transform.Apply({rect.x + rect.width, rect.y});
  const Point bottom_left = transform.Apply({rect.x, rect.y + rect.height});
  const Point bottom_right = transform.Apply({rect.x + rect.width, rect.y + rect.height});
  const float left = std::min({top_left.x, top_right.x, bottom_left.x, bottom_right.x});
  const float right = std::max({top_left.x, top_right.x, bottom_left.x, bottom_right.x});
  const float top = std::min({top_left.y, top_right.y, bottom_left.y, bottom_right.y});
  const float bottom = std::max({top_left.y, top_right.y, bottom_left.y, bottom_right.y});
  return {
      left,
      top,
      right - left,
      bottom - top,
  };
}

inline std::optional<Rect> InverseTransformBounds(const Transform2D& transform, Rect rect) noexcept {
  const std::optional<Point> top_left = transform.Inverse({rect.x, rect.y});
  const std::optional<Point> top_right = transform.Inverse({rect.x + rect.width, rect.y});
  const std::optional<Point> bottom_left = transform.Inverse({rect.x, rect.y + rect.height});
  const std::optional<Point> bottom_right = transform.Inverse({rect.x + rect.width, rect.y + rect.height});
  if (!top_left || !top_right || !bottom_left || !bottom_right) {
    return std::nullopt;
  }
  const float left = std::min({top_left->x, top_right->x, bottom_left->x, bottom_right->x});
  const float right = std::max({top_left->x, top_right->x, bottom_left->x, bottom_right->x});
  const float top = std::min({top_left->y, top_right->y, bottom_left->y, bottom_right->y});
  const float bottom = std::max({top_left->y, top_right->y, bottom_left->y, bottom_right->y});
  return Rect{
      left,
      top,
      right - left,
      bottom - top,
  };
}

} // namespace huxerui::detail
