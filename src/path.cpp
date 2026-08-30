#include <huxerui/vector.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "geometry_internal.h"
#include "path_internal.h"

namespace huxerui {
namespace {

using detail::PathElement;
using detail::PathVerb;

bool IsFinite(Point point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

void RequirePoint(Point point) {
  if (!IsFinite(point)) {
    throw std::invalid_argument("HuxerUI path points must be finite");
  }
}

float QuadraticValue(float start, float control, float end, float time) noexcept {
  const float inverse = 1.0F - time;
  return inverse * inverse * start + 2.0F * inverse * time * control + time * time * end;
}

float CubicValue(float start, float first_control, float second_control, float end, float time) noexcept {
  const float inverse = 1.0F - time;
  return inverse * inverse * inverse * start + 3.0F * inverse * inverse * time * first_control +
         3.0F * inverse * time * time * second_control + time * time * time * end;
}

template <class Include> void IncludeQuadraticExtrema(float start, float control, float end, Include&& include) {
  const float denominator = start - 2.0F * control + end;
  if (std::abs(denominator) <= 0.000001F) {
    return;
  }
  const float time = (start - control) / denominator;
  if (time > 0.0F && time < 1.0F) {
    include(time);
  }
}

template <class Include>
void IncludeCubicExtrema(float start, float first_control, float second_control, float end, Include&& include) {
  const float a = -start + 3.0F * first_control - 3.0F * second_control + end;
  const float b = 2.0F * (start - 2.0F * first_control + second_control);
  const float c = first_control - start;
  if (std::abs(a) <= 0.000001F) {
    if (std::abs(b) > 0.000001F) {
      const float time = -c / b;
      if (time > 0.0F && time < 1.0F) {
        include(time);
      }
    }
    return;
  }

  const float discriminant = b * b - 4.0F * a * c;
  if (discriminant < 0.0F) {
    return;
  }
  const float root = std::sqrt(discriminant);
  const float first_time = (-b + root) / (2.0F * a);
  const float second_time = (-b - root) / (2.0F * a);
  if (first_time > 0.0F && first_time < 1.0F) {
    include(first_time);
  }
  if (second_time > 0.0F && second_time < 1.0F && second_time != first_time) {
    include(second_time);
  }
}

} // namespace

struct Path::Data {
  std::vector<PathElement> elements;
  Point current;
  Point contour_start;
  bool has_current = false;
  bool has_drawable_segment = false;
  bool has_bounds = false;
  float minimum_x = 0.0F;
  float minimum_y = 0.0F;
  float maximum_x = 0.0F;
  float maximum_y = 0.0F;

  void Include(Point point) noexcept {
    if (!has_bounds) {
      minimum_x = maximum_x = point.x;
      minimum_y = maximum_y = point.y;
      has_bounds = true;
      return;
    }
    minimum_x = std::min(minimum_x, point.x);
    minimum_y = std::min(minimum_y, point.y);
    maximum_x = std::max(maximum_x, point.x);
    maximum_y = std::max(maximum_y, point.y);
  }
};

Path::Path() : data_(std::make_shared<Data>()) {}

Path& Path::MoveTo(Point point) {
  RequirePoint(point);
  EnsureUnique();
  data_->elements.push_back({PathVerb::MoveTo, {point, {}, {}}});
  data_->current = point;
  data_->contour_start = point;
  data_->has_current = true;
  return *this;
}

Path& Path::LineTo(Point point) {
  RequirePoint(point);
  if (!data_ || !data_->has_current) {
    throw std::logic_error("HuxerUI path LineTo requires an active contour");
  }
  EnsureUnique();
  data_->elements.push_back({PathVerb::LineTo, {point, {}, {}}});
  data_->Include(data_->current);
  data_->Include(point);
  data_->current = point;
  data_->has_drawable_segment = true;
  return *this;
}

Path& Path::QuadraticTo(Point control, Point end) {
  RequirePoint(control);
  RequirePoint(end);
  if (!data_ || !data_->has_current) {
    throw std::logic_error("HuxerUI path QuadraticTo requires an active contour");
  }
  EnsureUnique();
  const Point start = data_->current;
  data_->elements.push_back({PathVerb::QuadraticTo, {control, end, {}}});
  data_->Include(start);
  data_->Include(end);
  IncludeQuadraticExtrema(start.x, control.x, end.x, [&](float time) {
    data_->Include({QuadraticValue(start.x, control.x, end.x, time), QuadraticValue(start.y, control.y, end.y, time)});
  });
  IncludeQuadraticExtrema(start.y, control.y, end.y, [&](float time) {
    data_->Include({QuadraticValue(start.x, control.x, end.x, time), QuadraticValue(start.y, control.y, end.y, time)});
  });
  data_->current = end;
  data_->has_drawable_segment = true;
  return *this;
}

Path& Path::CubicTo(Point first_control, Point second_control, Point end) {
  RequirePoint(first_control);
  RequirePoint(second_control);
  RequirePoint(end);
  if (!data_ || !data_->has_current) {
    throw std::logic_error("HuxerUI path CubicTo requires an active contour");
  }
  EnsureUnique();
  const Point start = data_->current;
  data_->elements.push_back({PathVerb::CubicTo, {first_control, second_control, end}});
  data_->Include(start);
  data_->Include(end);
  IncludeCubicExtrema(start.x, first_control.x, second_control.x, end.x, [&](float time) {
    data_->Include({
        CubicValue(start.x, first_control.x, second_control.x, end.x, time),
        CubicValue(start.y, first_control.y, second_control.y, end.y, time),
    });
  });
  IncludeCubicExtrema(start.y, first_control.y, second_control.y, end.y, [&](float time) {
    data_->Include({
        CubicValue(start.x, first_control.x, second_control.x, end.x, time),
        CubicValue(start.y, first_control.y, second_control.y, end.y, time),
    });
  });
  data_->current = end;
  data_->has_drawable_segment = true;
  return *this;
}

Path& Path::Close() {
  if (!data_ || !data_->has_current) {
    throw std::logic_error("HuxerUI path Close requires an active contour");
  }
  EnsureUnique();
  data_->elements.push_back({PathVerb::Close, {}});
  if (data_->current != data_->contour_start) {
    data_->has_drawable_segment = true;
  }
  data_->has_current = false;
  return *this;
}

void Path::Reset() {
  data_ = std::make_shared<Data>();
}

Path Path::RoundedRect(Rect rect, CornerRadii corner_radii) {
  if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
      rect.width < 0.0F || rect.height < 0.0F) {
    throw std::invalid_argument("HuxerUI rounded rectangle must be finite with non-negative dimensions");
  }
  const float radii[] = {
      corner_radii.top_left,
      corner_radii.top_right,
      corner_radii.bottom_right,
      corner_radii.bottom_left,
  };
  for (const float radius : radii) {
    if (!std::isfinite(radius) || radius < 0.0F) {
      throw std::invalid_argument("HuxerUI corner radii must be finite and non-negative");
    }
  }

  corner_radii = detail::NormalizeCornerRadii(rect, corner_radii);

  const float right = rect.x + rect.width;
  const float bottom = rect.y + rect.height;
  constexpr float cubic_circle = 0.5522847498F;
  Path path;
  path.MoveTo({rect.x + corner_radii.top_left, rect.y})
      .LineTo({right - corner_radii.top_right, rect.y})
      .CubicTo(
          {right - corner_radii.top_right * (1.0F - cubic_circle), rect.y},
          {right, rect.y + corner_radii.top_right * (1.0F - cubic_circle)},
          {right, rect.y + corner_radii.top_right}
      )
      .LineTo({right, bottom - corner_radii.bottom_right})
      .CubicTo(
          {right, bottom - corner_radii.bottom_right * (1.0F - cubic_circle)},
          {right - corner_radii.bottom_right * (1.0F - cubic_circle), bottom},
          {right - corner_radii.bottom_right, bottom}
      )
      .LineTo({rect.x + corner_radii.bottom_left, bottom})
      .CubicTo(
          {rect.x + corner_radii.bottom_left * (1.0F - cubic_circle), bottom},
          {rect.x, bottom - corner_radii.bottom_left * (1.0F - cubic_circle)},
          {rect.x, bottom - corner_radii.bottom_left}
      )
      .LineTo({rect.x, rect.y + corner_radii.top_left})
      .CubicTo(
          {rect.x, rect.y + corner_radii.top_left * (1.0F - cubic_circle)},
          {rect.x + corner_radii.top_left * (1.0F - cubic_circle), rect.y},
          {rect.x + corner_radii.top_left, rect.y}
      )
      .Close();
  return path;
}

Path detail::CreateBorderStrokePath(Rect rect, CornerRadii corner_radii, float width) {
  const float inset = width * 0.5F;
  const Rect centerline{
      rect.x + inset,
      rect.y + inset,
      std::max(0.0F, rect.width - width),
      std::max(0.0F, rect.height - width),
  };
  return Path::RoundedRect(
      centerline,
      {
          std::max(0.0F, corner_radii.top_left - inset),
          std::max(0.0F, corner_radii.top_right - inset),
          std::max(0.0F, corner_radii.bottom_right - inset),
          std::max(0.0F, corner_radii.bottom_left - inset),
      }
  );
}

bool Path::IsEmpty() const noexcept {
  return !data_ || !data_->has_drawable_segment;
}

Rect Path::Bounds() const noexcept {
  if (!data_ || !data_->has_bounds) {
    return {};
  }
  return {
      data_->minimum_x,
      data_->minimum_y,
      data_->maximum_x - data_->minimum_x,
      data_->maximum_y - data_->minimum_y,
  };
}

bool Path::operator==(const Path& other) const noexcept {
  if (data_ == other.data_) {
    return true;
  }
  if (!data_) {
    return other.data_->elements.empty();
  }
  if (!other.data_) {
    return data_->elements.empty();
  }
  return data_->elements == other.data_->elements;
}

void Path::EnsureUnique() {
  if (!data_) {
    data_ = std::make_shared<Data>();
  } else if (data_.use_count() != 1) {
    data_ = std::make_shared<Data>(*data_);
  }
}

std::span<const detail::PathElement> detail::PathAccess::Elements(const Path& path) noexcept {
  if (!path.data_) {
    return {};
  }
  return path.data_->elements;
}

} // namespace huxerui
