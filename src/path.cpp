#include <huxerui/vector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

#include "geometry_internal.h"
#include "path_internal.h"

namespace huxerui {
namespace {

using detail::PathElement;
using detail::PathVerb;

struct CubicSegment {
  Point first_control;
  Point second_control;
  Point end;
};

struct NormalizedArc {
  std::array<CubicSegment, 4> segments;
  std::size_t count = 0;
};

bool IsFinite(Point point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

void RequirePoint(Point point) {
  if (!IsFinite(point)) {
    throw std::invalid_argument("HuxerUI path points must be finite");
  }
}

bool IsLargeArc(ArcSize size) {
  switch (size) {
  case ArcSize::Small:
    return false;
  case ArcSize::Large:
    return true;
  }
  throw std::invalid_argument("HuxerUI path arc size is invalid");
}

bool IsClockwiseArc(ArcDirection direction) {
  switch (direction) {
  case ArcDirection::CounterClockwise:
    return false;
  case ArcDirection::Clockwise:
    return true;
  }
  throw std::invalid_argument("HuxerUI path arc direction is invalid");
}

Point ArcPoint(double x, double y) {
  constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > maximum || std::abs(y) > maximum) {
    throw std::invalid_argument("HuxerUI path arc geometry is not representable");
  }
  return {static_cast<float>(x), static_cast<float>(y)};
}

NormalizedArc NormalizeArc(Point start, Size radii, float x_axis_rotation, bool large, bool clockwise, Point end) {
  constexpr double pi = std::numbers::pi_v<double>;
  constexpr double full_circle = pi * 2.0;
  constexpr double maximum_segment_sweep = pi * 0.5;

  double radius_x = static_cast<double>(radii.width);
  double radius_y = static_cast<double>(radii.height);
  const double rotation = std::remainder(static_cast<double>(x_axis_rotation), full_circle);
  const double cosine = std::cos(rotation);
  const double sine = std::sin(rotation);
  const double half_delta_x = (static_cast<double>(start.x) - static_cast<double>(end.x)) * 0.5;
  const double half_delta_y = (static_cast<double>(start.y) - static_cast<double>(end.y)) * 0.5;
  const double local_start_x = cosine * half_delta_x + sine * half_delta_y;
  const double local_start_y = -sine * half_delta_x + cosine * half_delta_y;

  const double radii_scale = local_start_x * local_start_x / (radius_x * radius_x) +
                             local_start_y * local_start_y / (radius_y * radius_y);
  if (radii_scale > 1.0) {
    const double scale = std::sqrt(radii_scale);
    radius_x *= scale;
    radius_y *= scale;
  }
  if (!std::isfinite(radius_x) || !std::isfinite(radius_y)) {
    throw std::invalid_argument("HuxerUI path arc geometry is not representable");
  }

  const double radius_x_squared = radius_x * radius_x;
  const double radius_y_squared = radius_y * radius_y;
  const double local_start_x_squared = local_start_x * local_start_x;
  const double local_start_y_squared = local_start_y * local_start_y;
  const double numerator = std::max(
      0.0,
      radius_x_squared * radius_y_squared - radius_x_squared * local_start_y_squared -
          radius_y_squared * local_start_x_squared
  );
  const double denominator =
      radius_x_squared * local_start_y_squared + radius_y_squared * local_start_x_squared;
  const double direction = large == clockwise ? -1.0 : 1.0;
  const double center_factor = direction * std::sqrt(numerator / denominator);
  const double local_center_x = center_factor * radius_x * local_start_y / radius_y;
  const double local_center_y = center_factor * -radius_y * local_start_x / radius_x;
  const double center_x = cosine * local_center_x - sine * local_center_y +
                          (static_cast<double>(start.x) + static_cast<double>(end.x)) * 0.5;
  const double center_y = sine * local_center_x + cosine * local_center_y +
                          (static_cast<double>(start.y) + static_cast<double>(end.y)) * 0.5;

  const auto angle = [](double first_x, double first_y, double second_x, double second_y) {
    return std::atan2(
        first_x * second_y - first_y * second_x,
        first_x * second_x + first_y * second_y
    );
  };
  double start_angle = angle(
      1.0,
      0.0,
      (local_start_x - local_center_x) / radius_x,
      (local_start_y - local_center_y) / radius_y
  );
  double sweep_angle = angle(
      (local_start_x - local_center_x) / radius_x,
      (local_start_y - local_center_y) / radius_y,
      (-local_start_x - local_center_x) / radius_x,
      (-local_start_y - local_center_y) / radius_y
  );
  if (!clockwise && sweep_angle > 0.0) {
    sweep_angle -= full_circle;
  } else if (clockwise && sweep_angle < 0.0) {
    sweep_angle += full_circle;
  }
  if (!std::isfinite(start_angle) || !std::isfinite(sweep_angle)) {
    throw std::invalid_argument("HuxerUI path arc geometry is not representable");
  }

  NormalizedArc result{};
  result.count =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(std::abs(sweep_angle) / maximum_segment_sweep)));
  if (result.count > result.segments.size()) {
    throw std::logic_error("HuxerUI path arc normalization exceeded four cubic segments");
  }
  const double segment_sweep = sweep_angle / static_cast<double>(result.count);
  const auto map = [&](double x, double y) {
    return ArcPoint(
        center_x + cosine * radius_x * x - sine * radius_y * y,
        center_y + sine * radius_x * x + cosine * radius_y * y
    );
  };
  for (std::size_t index = 0; index < result.count; ++index) {
    const double next_angle = start_angle + segment_sweep;
    const double control_scale = 4.0 / 3.0 * std::tan(segment_sweep * 0.25);
    result.segments[index] = {
        map(
            std::cos(start_angle) - control_scale * std::sin(start_angle),
            std::sin(start_angle) + control_scale * std::cos(start_angle)
        ),
        map(
            std::cos(next_angle) + control_scale * std::sin(next_angle),
            std::sin(next_angle) - control_scale * std::cos(next_angle)
        ),
        index + 1 == result.count ? end : map(std::cos(next_angle), std::sin(next_angle)),
    };
    start_angle = next_angle;
  }
  return result;
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

  void AppendCubic(Point first_control, Point second_control, Point end) {
    const Point start = current;
    elements.push_back({PathVerb::CubicTo, {first_control, second_control, end}});
    Include(start);
    Include(end);
    IncludeCubicExtrema(start.x, first_control.x, second_control.x, end.x, [&](float time) {
      Include({
          CubicValue(start.x, first_control.x, second_control.x, end.x, time),
          CubicValue(start.y, first_control.y, second_control.y, end.y, time),
      });
    });
    IncludeCubicExtrema(start.y, first_control.y, second_control.y, end.y, [&](float time) {
      Include({
          CubicValue(start.x, first_control.x, second_control.x, end.x, time),
          CubicValue(start.y, first_control.y, second_control.y, end.y, time),
      });
    });
    current = end;
    has_drawable_segment = true;
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
  data_->AppendCubic(first_control, second_control, end);
  return *this;
}

Path& Path::ArcTo(Size radii, float x_axis_rotation, ArcSize size, ArcDirection direction, Point end) {
  RequirePoint(end);
  if (!std::isfinite(radii.width) || !std::isfinite(radii.height) || radii.width < 0.0F || radii.height < 0.0F) {
    throw std::invalid_argument("HuxerUI path arc radii must be finite and non-negative");
  }
  if (!std::isfinite(x_axis_rotation)) {
    throw std::invalid_argument("HuxerUI path arc rotation must be finite radians");
  }
  const bool large = IsLargeArc(size);
  const bool clockwise = IsClockwiseArc(direction);
  if (!data_ || !data_->has_current) {
    throw std::logic_error("HuxerUI path ArcTo requires an active contour");
  }
  if (data_->current == end) {
    return *this;
  }
  if (radii.width == 0.0F || radii.height == 0.0F) {
    return LineTo(end);
  }

  const NormalizedArc arc = NormalizeArc(data_->current, radii, x_axis_rotation, large, clockwise, end);
  EnsureUnique();
  data_->elements.reserve(data_->elements.size() + arc.count);
  for (std::size_t index = 0; index < arc.count; ++index) {
    const CubicSegment& segment = arc.segments[index];
    data_->AppendCubic(segment.first_control, segment.second_control, segment.end);
  }
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
