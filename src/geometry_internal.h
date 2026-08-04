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

inline CornerRadii NormalizeCornerRadii(Rect rect, CornerRadii corner_radii) noexcept {
  float scale = 1.0F;
  const auto constrain_pair = [&scale](float available, float first, float second) {
    const float total = std::max(0.0F, first) + std::max(0.0F, second);
    if (total > 0.0F) {
      scale = std::min(scale, available / total);
    }
  };
  constrain_pair(rect.width, corner_radii.top_left, corner_radii.top_right);
  constrain_pair(rect.width, corner_radii.bottom_left, corner_radii.bottom_right);
  constrain_pair(rect.height, corner_radii.top_left, corner_radii.bottom_left);
  constrain_pair(rect.height, corner_radii.top_right, corner_radii.bottom_right);
  scale = std::clamp(scale, 0.0F, 1.0F);
  corner_radii.top_left = std::max(0.0F, corner_radii.top_left) * scale;
  corner_radii.top_right = std::max(0.0F, corner_radii.top_right) * scale;
  corner_radii.bottom_right = std::max(0.0F, corner_radii.bottom_right) * scale;
  corner_radii.bottom_left = std::max(0.0F, corner_radii.bottom_left) * scale;
  return corner_radii;
}

inline bool RoundedRectContains(Rect rect, CornerRadii corner_radii, Point point) noexcept {
  if (!rect.Contains(point)) {
    return false;
  }
  corner_radii = NormalizeCornerRadii(rect, corner_radii);

  float radius = 0.0F;
  Point center;
  if (point.x < rect.x + corner_radii.top_left && point.y < rect.y + corner_radii.top_left) {
    radius = corner_radii.top_left;
    center = {rect.x + radius, rect.y + radius};
  } else if (point.x > rect.x + rect.width - corner_radii.top_right && point.y < rect.y + corner_radii.top_right) {
    radius = corner_radii.top_right;
    center = {rect.x + rect.width - radius, rect.y + radius};
  } else if (
      point.x > rect.x + rect.width - corner_radii.bottom_right &&
      point.y > rect.y + rect.height - corner_radii.bottom_right
  ) {
    radius = corner_radii.bottom_right;
    center = {rect.x + rect.width - radius, rect.y + rect.height - radius};
  } else if (point.x < rect.x + corner_radii.bottom_left && point.y > rect.y + rect.height - corner_radii.bottom_left) {
    radius = corner_radii.bottom_left;
    center = {rect.x + radius, rect.y + rect.height - radius};
  } else {
    return true;
  }
  const float delta_x = point.x - center.x;
  const float delta_y = point.y - center.y;
  return delta_x * delta_x + delta_y * delta_y <= radius * radius;
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
