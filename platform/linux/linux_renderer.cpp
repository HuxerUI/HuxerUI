#include "linux_internal.h"
#include "linux_renderer.h"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "external_texture_internal.h"
#include "linux_external_texture_internal.h"
#include "linux_text_renderer_internal.h"
#include "path_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kTau = 2.0F * kPi;
constexpr std::uint32_t kDefaultBackgroundPixel = 0xFFF7F8FAU;
constexpr std::array<Point, 4> kCoverageSamples{{
    {0.25F, 0.25F},
    {0.75F, 0.25F},
    {0.25F, 0.75F},
    {0.75F, 0.75F},
}};

std::uint8_t Byte(float value) noexcept {
  return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::uint32_t PremultipliedPixel(Color color, float coverage = 1.0F) noexcept {
  const float alpha = std::clamp(color.alpha * coverage, 0.0F, 1.0F);
  return static_cast<std::uint32_t>(Byte(alpha)) << 24U | static_cast<std::uint32_t>(Byte(color.red * alpha)) << 16U |
         static_cast<std::uint32_t>(Byte(color.green * alpha)) << 8U |
         static_cast<std::uint32_t>(Byte(color.blue * alpha));
}

std::uint32_t ModulatePremultiplied(std::uint32_t pixel, float opacity) noexcept {
  const std::uint32_t factor = static_cast<std::uint32_t>(Byte(opacity));
  const auto multiply = [factor](std::uint32_t value) { return (value * factor + 127U) / 255U; };
  return multiply((pixel >> 24U) & 0xFFU) << 24U | multiply((pixel >> 16U) & 0xFFU) << 16U |
         multiply((pixel >> 8U) & 0xFFU) << 8U | multiply(pixel & 0xFFU);
}

std::uint32_t BlendPremultiplied(std::uint32_t destination, std::uint32_t source) noexcept {
  const std::uint32_t inverse = 255U - (source >> 24U);
  const auto blend = [inverse](std::uint32_t source_channel, std::uint32_t destination_channel) {
    return std::min(255U, source_channel + (destination_channel * inverse + 127U) / 255U);
  };
  return blend((source >> 24U) & 0xFFU, (destination >> 24U) & 0xFFU) << 24U |
         blend((source >> 16U) & 0xFFU, (destination >> 16U) & 0xFFU) << 16U |
         blend((source >> 8U) & 0xFFU, (destination >> 8U) & 0xFFU) << 8U | blend(source & 0xFFU, destination & 0xFFU);
}

Transform2D Multiply(const Transform2D& left, const Transform2D& right) noexcept {
  return {
      .m11 = left.m11 * right.m11 + left.m21 * right.m12,
      .m12 = left.m12 * right.m11 + left.m22 * right.m12,
      .m21 = left.m11 * right.m21 + left.m21 * right.m22,
      .m22 = left.m12 * right.m21 + left.m22 * right.m22,
      .translate_x = left.m11 * right.translate_x + left.m21 * right.translate_y + left.translate_x,
      .translate_y = left.m12 * right.translate_x + left.m22 * right.translate_y + left.translate_y,
  };
}

Transform2D Translation(Point offset) noexcept {
  return {.translate_x = offset.x, .translate_y = offset.y};
}

float Distance(Point left, Point right) noexcept {
  return std::hypot(left.x - right.x, left.y - right.y);
}

float SegmentProjection(Point point, Point start, Point end) noexcept {
  const Point delta = end - start;
  const float squared = delta.x * delta.x + delta.y * delta.y;
  if (squared <= 0.000001F) {
    return 0.0F;
  }
  return ((point.x - start.x) * delta.x + (point.y - start.y) * delta.y) / squared;
}

struct FlatPath final {
  std::vector<std::vector<Point>> contours;
  Rect bounds;
};

Rect BoundsFor(const std::vector<std::vector<Point>>& contours) noexcept {
  float left = std::numeric_limits<float>::infinity();
  float top = std::numeric_limits<float>::infinity();
  float right = -std::numeric_limits<float>::infinity();
  float bottom = -std::numeric_limits<float>::infinity();
  for (const std::vector<Point>& contour : contours) {
    for (Point point : contour) {
      left = std::min(left, point.x);
      top = std::min(top, point.y);
      right = std::max(right, point.x);
      bottom = std::max(bottom, point.y);
    }
  }
  if (!std::isfinite(left)) {
    return {};
  }
  return {left, top, std::max(0.0F, right - left), std::max(0.0F, bottom - top)};
}

float DistanceToLine(Point point, Point start, Point end) noexcept {
  const Point delta = end - start;
  const float length = std::hypot(delta.x, delta.y);
  if (length <= 0.000001F) {
    return Distance(point, start);
  }
  return std::abs(delta.x * (start.y - point.y) - (start.x - point.x) * delta.y) / length;
}

FlatPath TransformFlatPath(const FlatPath& path, const Transform2D& transform) {
  FlatPath result;
  result.contours.reserve(path.contours.size());
  for (const std::vector<Point>& contour : path.contours) {
    std::vector<Point>& transformed = result.contours.emplace_back();
    transformed.reserve(contour.size());
    for (Point point : contour) {
      transformed.push_back(transform.Apply(point));
    }
  }
  result.bounds = BoundsFor(result.contours);
  return result;
}

FlatPath FlattenPath(const Path& path, const Transform2D& transform, bool apply_transform = true) {
  FlatPath result;
  std::vector<Point>* contour = nullptr;
  Point current{};
  Point contour_start{};
  auto append = [&](Point point) {
    if (contour == nullptr) {
      result.contours.emplace_back();
      contour = &result.contours.back();
    }
    contour->push_back(apply_transform ? transform.Apply(point) : point);
    current = point;
  };
  const auto append_quadratic = [&](auto&& self, Point start, Point control, Point end, int depth) -> void {
    const Point device_start = transform.Apply(start);
    const Point device_control = transform.Apply(control);
    const Point device_end = transform.Apply(end);
    if (depth >= 12 || DistanceToLine(device_control, device_start, device_end) <= 0.25F) {
      append(end);
      return;
    }
    const Point start_control = (start + control) * 0.5F;
    const Point control_end = (control + end) * 0.5F;
    const Point middle = (start_control + control_end) * 0.5F;
    self(self, start, start_control, middle, depth + 1);
    self(self, middle, control_end, end, depth + 1);
  };
  const auto append_cubic = [&](auto&& self, Point start, Point first, Point second, Point end, int depth) -> void {
    const Point device_start = transform.Apply(start);
    const Point device_first = transform.Apply(first);
    const Point device_second = transform.Apply(second);
    const Point device_end = transform.Apply(end);
    if (depth >= 12 || std::max(
                           DistanceToLine(device_first, device_start, device_end),
                           DistanceToLine(device_second, device_start, device_end)
                       ) <= 0.25F) {
      append(end);
      return;
    }
    const Point start_first = (start + first) * 0.5F;
    const Point first_second = (first + second) * 0.5F;
    const Point second_end = (second + end) * 0.5F;
    const Point left_control = (start_first + first_second) * 0.5F;
    const Point right_control = (first_second + second_end) * 0.5F;
    const Point middle = (left_control + right_control) * 0.5F;
    self(self, start, start_first, left_control, middle, depth + 1);
    self(self, middle, right_control, second_end, end, depth + 1);
  };
  for (const PathElement& element : PathAccess::Elements(path)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      result.contours.emplace_back();
      contour = &result.contours.back();
      current = element.points[0];
      contour_start = current;
      contour->push_back(apply_transform ? transform.Apply(current) : current);
      break;
    case PathVerb::LineTo:
      append(element.points[0]);
      break;
    case PathVerb::QuadraticTo: {
      const Point start = current;
      append_quadratic(append_quadratic, start, element.points[0], element.points[1], 0);
      break;
    }
    case PathVerb::CubicTo: {
      const Point start = current;
      append_cubic(append_cubic, start, element.points[0], element.points[1], element.points[2], 0);
      break;
    }
    case PathVerb::Close:
      if (contour != nullptr && !contour->empty()) {
        const Point start = apply_transform ? transform.Apply(contour_start) : contour_start;
        if (contour->back() != start) {
          contour->push_back(start);
        }
      }
      current = contour_start;
      break;
    }
  }
  result.contours.erase(
      std::remove_if(
          result.contours.begin(),
          result.contours.end(),
          [](const auto& value) { return value.size() < 2; }
      ),
      result.contours.end()
  );
  result.bounds = BoundsFor(result.contours);
  return result;
}

FlatPath RoundedRect(Rect rect, float radius, const Transform2D& transform) {
  return FlattenPath(Path::RoundedRect(rect, CornerRadii(std::max(0.0F, radius))), transform);
}

FlatPath Ellipse(Point center, float radius, const Transform2D& transform) {
  FlatPath result;
  const float scale = std::max(std::hypot(transform.m11, transform.m12), std::hypot(transform.m21, transform.m22));
  const int segments = std::clamp(static_cast<int>(std::ceil(kPi * radius * scale)), 24, 4096);
  result.contours.emplace_back();
  for (int index = 0; index <= segments; ++index) {
    const float angle = kTau * static_cast<float>(index) / static_cast<float>(segments);
    result.contours.back().push_back(transform.Apply({
        center.x + std::cos(angle) * radius,
        center.y + std::sin(angle) * radius,
    }));
  }
  result.bounds = BoundsFor(result.contours);
  return result;
}

FlatPath
Arc(Point center, float radius, float start, float sweep, const Transform2D& transform, bool apply_transform = true) {
  FlatPath result;
  const float scale = std::max(std::hypot(transform.m11, transform.m12), std::hypot(transform.m21, transform.m22));
  const int segments =
      std::clamp(static_cast<int>(std::ceil(std::abs(sweep) * std::max(8.0F, radius * scale) * 0.5F)), 2, 4096);
  result.contours.emplace_back();
  for (int index = 0; index <= segments; ++index) {
    const float angle = start + sweep * static_cast<float>(index) / static_cast<float>(segments);
    const Point point{
        center.x + std::cos(angle) * radius,
        center.y + std::sin(angle) * radius,
    };
    result.contours.back().push_back(apply_transform ? transform.Apply(point) : point);
  }
  result.bounds = BoundsFor(result.contours);
  return result;
}

int WindingForContour(Point point, const std::vector<Point>& contour) noexcept {
  int winding = 0;
  for (std::size_t index = 0; index < contour.size(); ++index) {
    const Point first = contour[index];
    const Point second = contour[(index + 1U) % contour.size()];
    if ((first.y <= point.y && second.y > point.y) || (first.y > point.y && second.y <= point.y)) {
      const float x = first.x + (point.y - first.y) * (second.x - first.x) / (second.y - first.y);
      if (x > point.x) {
        winding += second.y > first.y ? 1 : -1;
      }
    }
  }
  return winding;
}

bool Contains(const FlatPath& path, Point point, PathFillRule rule) noexcept {
  int winding = 0;
  for (const std::vector<Point>& contour : path.contours) {
    winding += WindingForContour(point, contour);
  }
  return rule == PathFillRule::EvenOdd ? std::abs(winding) % 2 == 1 : winding != 0;
}

bool TriangleContains(Point point, Point first, Point second, Point third) noexcept {
  const auto cross = [](Point left, Point right) { return left.x * right.y - left.y * right.x; };
  const float one = cross(second - first, point - first);
  const float two = cross(third - second, point - second);
  const float three = cross(first - third, point - third);
  return (one >= 0.0F && two >= 0.0F && three >= 0.0F) || (one <= 0.0F && two <= 0.0F && three <= 0.0F);
}

std::optional<Point> LineIntersection(Point origin_a, Point direction_a, Point origin_b, Point direction_b) noexcept {
  const float determinant = direction_a.x * direction_b.y - direction_a.y * direction_b.x;
  if (std::abs(determinant) <= 0.000001F) {
    return std::nullopt;
  }
  const Point delta = origin_b - origin_a;
  const float amount = (delta.x * direction_b.y - delta.y * direction_b.x) / determinant;
  return origin_a + direction_a * amount;
}

bool JoinContains(
    Point point, Point previous, Point vertex, Point next, float half, StrokeJoin join, float miter_limit
) noexcept {
  const Point incoming = vertex - previous;
  const Point outgoing = next - vertex;
  const float incoming_length = std::hypot(incoming.x, incoming.y);
  const float outgoing_length = std::hypot(outgoing.x, outgoing.y);
  if (incoming_length <= 0.000001F || outgoing_length <= 0.000001F) {
    return false;
  }
  if (join == StrokeJoin::Round) {
    return Distance(point, vertex) <= half;
  }
  const Point first_direction = incoming * (1.0F / incoming_length);
  const Point second_direction = outgoing * (1.0F / outgoing_length);
  const float turn = first_direction.x * second_direction.y - first_direction.y * second_direction.x;
  if (std::abs(turn) <= 0.000001F) {
    return false;
  }
  const float side = turn > 0.0F ? -1.0F : 1.0F;
  const Point first_normal{-first_direction.y * side, first_direction.x * side};
  const Point second_normal{-second_direction.y * side, second_direction.x * side};
  const Point first_outer = vertex + first_normal * half;
  const Point second_outer = vertex + second_normal * half;
  if (join == StrokeJoin::Miter) {
    const std::optional<Point> miter = LineIntersection(first_outer, first_direction, second_outer, second_direction);
    if (miter.has_value() && Distance(*miter, vertex) <= std::max(1.0F, miter_limit) * half) {
      return TriangleContains(point, first_outer, *miter, second_outer);
    }
  }
  return TriangleContains(point, vertex, first_outer, second_outer);
}

bool StrokeContains(
    const FlatPath& path, Point point, float width, StrokeCap cap, StrokeJoin join, float miter_limit
) noexcept {
  const float half = std::max(0.0F, width * 0.5F);
  for (const std::vector<Point>& contour : path.contours) {
    const bool closed = contour.size() > 2 && contour.front() == contour.back();
    for (std::size_t index = 0; index + 1 < contour.size(); ++index) {
      const Point start = contour[index];
      const Point end = contour[index + 1];
      const float projection = SegmentProjection(point, start, end);
      const float length = Distance(start, end);
      float minimum = 0.0F;
      float maximum = 1.0F;
      if (!closed && cap == StrokeCap::Square && length > 0.000001F) {
        if (index == 0) {
          minimum -= half / length;
        }
        if (index + 2 == contour.size()) {
          maximum += half / length;
        }
      }
      if (projection >= minimum && projection <= maximum &&
          Distance(point, start + (end - start) * projection) <= half) {
        return true;
      }
    }
    if (!closed && cap == StrokeCap::Round &&
        (Distance(point, contour.front()) <= half || Distance(point, contour.back()) <= half)) {
      return true;
    }
    const std::size_t unique_points = closed ? contour.size() - 1U : contour.size();
    const std::size_t first_join = closed ? 0U : 1U;
    const std::size_t end_join = closed ? unique_points : unique_points - 1U;
    for (std::size_t index = first_join; index < end_join; ++index) {
      const Point previous = contour[(index + unique_points - 1U) % unique_points];
      const Point vertex = contour[index];
      const Point next = contour[(index + 1U) % unique_points];
      if (JoinContains(point, previous, vertex, next, half, join, miter_limit)) {
        return true;
      }
    }
  }
  return false;
}

Color Interpolate(Color start, Color end, float amount) noexcept {
  return {
      start.red + (end.red - start.red) * amount,
      start.green + (end.green - start.green) * amount,
      start.blue + (end.blue - start.blue) * amount,
      start.alpha + (end.alpha - start.alpha) * amount,
  };
}

Color GradientColor(const std::vector<GradientStop>& stops, float offset) noexcept {
  if (stops.empty()) {
    return Color::Transparent();
  }
  if (offset <= stops.front().offset) {
    return stops.front().color;
  }
  if (offset >= stops.back().offset) {
    return stops.back().color;
  }
  for (std::size_t index = 1; index < stops.size(); ++index) {
    if (offset <= stops[index].offset) {
      const GradientStop& first = stops[index - 1];
      const GradientStop& second = stops[index];
      const float span = std::max(0.000001F, second.offset - first.offset);
      return Interpolate(first.color, second.color, (offset - first.offset) / span);
    }
  }
  return stops.back().color;
}

struct ImageBuffer final {
  int width = 0;
  int height = 0;
  std::vector<std::uint32_t> pixels;

  [[nodiscard]] bool Empty() const noexcept {
    return width <= 0 || height <= 0 || pixels.empty();
  }
};

ImageBuffer PremultiplySurface(SDL_Surface& surface) {
  ImageBuffer result{.width = surface.w, .height = surface.h, .pixels = {}};
  if (result.width <= 0 || result.height <= 0) {
    return result;
  }
  const bool locked = SDL_MUSTLOCK(&surface);
  if (locked && !SDL_LockSurface(&surface)) {
    return {};
  }
  result.pixels.resize(static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.height));
  const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(surface.format);
  if (details == nullptr) {
    if (locked) {
      SDL_UnlockSurface(&surface);
    }
    return {};
  }
  for (int y = 0; y < result.height; ++y) {
    const auto* source = static_cast<const std::uint8_t*>(surface.pixels) + y * surface.pitch;
    for (int x = 0; x < result.width; ++x) {
      std::uint32_t pixel = 0;
      std::memcpy(&pixel, source + x * details->bytes_per_pixel, details->bytes_per_pixel);
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
      std::uint8_t alpha = 0;
      SDL_GetRGBA(pixel, details, SDL_GetSurfacePalette(&surface), &red, &green, &blue, &alpha);
      const auto premultiply = [alpha](std::uint8_t channel) {
        return static_cast<std::uint32_t>((static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U);
      };
      result
          .pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(result.width) + static_cast<std::size_t>(x)] =
          static_cast<std::uint32_t>(alpha) << 24U | premultiply(red) << 16U | premultiply(green) << 8U |
          premultiply(blue);
    }
  }
  if (locked) {
    SDL_UnlockSurface(&surface);
  }
  return result;
}

std::uint32_t SampleImage(const ImageBuffer& image, float x, float y, ImageSampling sampling) noexcept {
  if (image.Empty()) {
    return 0U;
  }
  const auto at = [&image](int px, int py) {
    px = std::clamp(px, 0, image.width - 1);
    py = std::clamp(py, 0, image.height - 1);
    return image
        .pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(px)];
  };
  if (sampling == ImageSampling::Nearest) {
    return at(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
  }
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const float tx = x - static_cast<float>(x0);
  const float ty = y - static_cast<float>(y0);
  const std::array<std::uint32_t, 4> samples{at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
  std::uint32_t result = 0;
  for (int shift : {0, 8, 16, 24}) {
    const float top = static_cast<float>((samples[0] >> shift) & 0xFFU) * (1.0F - tx) +
                      static_cast<float>((samples[1] >> shift) & 0xFFU) * tx;
    const float bottom = static_cast<float>((samples[2] >> shift) & 0xFFU) * (1.0F - tx) +
                         static_cast<float>((samples[3] >> shift) & 0xFFU) * tx;
    result |= static_cast<std::uint32_t>(std::lround(top * (1.0F - ty) + bottom * ty)) << shift;
  }
  return result;
}

struct ClipShape final {
  FlatPath path;
  PathFillRule rule = PathFillRule::NonZero;
};

struct CanvasState final {
  Transform2D transform;
  std::vector<ClipShape> clips;
};

class CpuCanvas final {
public:
  CpuCanvas(SDL_Surface& surface, float scale_x, float scale_y)
      : width_(surface.w), height_(surface.h), pitch_(surface.pitch / 4),
        pixels_(static_cast<std::uint32_t*>(surface.pixels)) {
    state_.transform = {.m11 = scale_x, .m22 = scale_y};
  }

  CpuCanvas(SDL_Surface& surface, CanvasState state)
      : width_(surface.w), height_(surface.h), pitch_(surface.pitch / 4),
        pixels_(static_cast<std::uint32_t*>(surface.pixels)), state_(std::move(state)) {}

  void Clear(std::uint32_t pixel = 0U) noexcept {
    for (int y = 0; y < height_; ++y) {
      std::fill_n(pixels_ + y * pitch_, width_, pixel);
    }
  }

  void Save() {
    stack_.push_back(state_);
  }

  void Restore() {
    if (stack_.empty()) {
      throw std::logic_error("HuxerUI Linux renderer state stack is unbalanced");
    }
    state_ = std::move(stack_.back());
    stack_.pop_back();
  }

  void Transform(const Transform2D& transform) noexcept {
    state_.transform = Multiply(state_.transform, transform);
  }

  const Transform2D& CurrentTransform() const noexcept {
    return state_.transform;
  }

  const CanvasState& CurrentState() const noexcept {
    return state_;
  }

  void PushClip(const Path& path, PathFillRule rule) {
    Save();
    state_.clips.push_back({FlattenPath(path, state_.transform), rule});
  }

  bool Allows(Point point) const noexcept {
    return std::ranges::all_of(state_.clips, [point](const ClipShape& clip) {
      return Contains(clip.path, point, clip.rule);
    });
  }

  void Blend(int x, int y, std::uint32_t source) noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || source == 0U) {
      return;
    }
    pixels_[y * pitch_ + x] = BlendPremultiplied(pixels_[y * pitch_ + x], source);
  }

  template <class Predicate, class Paint> void Rasterize(Rect bounds, Predicate&& predicate, Paint&& paint) {
    const int left = std::max(0, static_cast<int>(std::floor(bounds.x)) - 1);
    const int top = std::max(0, static_cast<int>(std::floor(bounds.y)) - 1);
    const int right = std::min(width_, static_cast<int>(std::ceil(bounds.x + bounds.width)) + 1);
    const int bottom = std::min(height_, static_cast<int>(std::ceil(bounds.y + bounds.height)) + 1);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        float coverage = 0.0F;
        for (Point sample : kCoverageSamples) {
          const Point device{static_cast<float>(x) + sample.x, static_cast<float>(y) + sample.y};
          if (Allows(device) && std::invoke(predicate, device)) {
            coverage += 0.25F;
          }
        }
        if (coverage <= 0.0F) {
          continue;
        }
        const Point center{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
        Blend(x, y, PremultipliedPixel(std::invoke(paint, center), coverage));
      }
    }
  }

  void Fill(const FlatPath& path, PathFillRule rule, Color color) {
    Rasterize(
        path.bounds,
        [&path, rule](Point point) { return Contains(path, point, rule); },
        [color](Point) { return color; }
    );
  }

  void Stroke(
      const FlatPath& path,
      float width,
      Color color,
      StrokeCap cap = StrokeCap::Butt,
      StrokeJoin join = StrokeJoin::Miter,
      float miter_limit = 4.0F
  ) {
    const float scale = std::max(
        std::hypot(state_.transform.m11, state_.transform.m12),
        std::hypot(state_.transform.m21, state_.transform.m22)
    );
    const FlatPath device_path = TransformFlatPath(path, state_.transform);
    const float device_width = std::max(0.0F, width * scale);
    const float overflow =
        join == StrokeJoin::Miter ? device_width * 0.5F * std::max(1.0F, miter_limit) : device_width * 0.5F;
    Rect bounds = device_path.bounds;
    bounds.x -= overflow;
    bounds.y -= overflow;
    bounds.width += overflow * 2.0F;
    bounds.height += overflow * 2.0F;
    Rasterize(
        bounds,
        [this, &path, width, cap, join, miter_limit](Point point) {
          const std::optional<Point> local = state_.transform.Inverse(point);
          return local.has_value() && StrokeContains(path, *local, width, cap, join, miter_limit);
        },
        [color](Point) { return color; }
    );
  }

  void DrawImage(const ImageBuffer& image, Rect source, Rect destination, ImageSampling sampling, float opacity) {
    if (image.Empty() || source.IsEmpty() || destination.IsEmpty() || opacity <= 0.0F) {
      return;
    }
    const FlatPath destination_path = RoundedRect(destination, 0.0F, state_.transform);
    const Transform2D transform = state_.transform;
    Rasterize(
        destination_path.bounds,
        [&destination_path](Point point) { return Contains(destination_path, point, PathFillRule::NonZero); },
        [=, &image](Point device) {
          const std::optional<Point> local = transform.Inverse(device);
          if (!local.has_value()) {
            return Color::Transparent();
          }
          const float u = std::clamp((local->x - destination.x) / destination.width, 0.0F, 1.0F);
          const float v = std::clamp((local->y - destination.y) / destination.height, 0.0F, 1.0F);
          const float sampling_offset = sampling == ImageSampling::Linear ? 0.5F : 0.0F;
          const std::uint32_t pixel = SampleImage(
              image,
              source.x + u * source.width - sampling_offset,
              source.y + v * source.height - sampling_offset,
              sampling
          );
          const float alpha = static_cast<float>((pixel >> 24U) & 0xFFU) / 255.0F;
          if (alpha <= 0.0F) {
            return Color::Transparent();
          }
          return Color{
              static_cast<float>((pixel >> 16U) & 0xFFU) / (255.0F * alpha),
              static_cast<float>((pixel >> 8U) & 0xFFU) / (255.0F * alpha),
              static_cast<float>(pixel & 0xFFU) / (255.0F * alpha),
              alpha * std::clamp(opacity, 0.0F, 1.0F),
          };
        }
    );
  }

  void Composite(const SDL_Surface& layer, float opacity) noexcept {
    const int rows = std::min(height_, layer.h);
    const int columns = std::min(width_, layer.w);
    const int source_pitch = layer.pitch / 4;
    const auto* source = static_cast<const std::uint32_t*>(layer.pixels);
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < columns; ++x) {
        Blend(x, y, ModulatePremultiplied(source[y * source_pitch + x], opacity));
      }
    }
  }

  int Width() const noexcept {
    return width_;
  }

  int Height() const noexcept {
    return height_;
  }

private:
  int width_ = 0;
  int height_ = 0;
  int pitch_ = 0;
  std::uint32_t* pixels_ = nullptr;
  CanvasState state_;
  std::vector<CanvasState> stack_;
};

std::vector<std::uint8_t> BoxBlur(std::vector<std::uint8_t> source, int width, int height, int radius) {
  if (radius <= 0 || width <= 0 || height <= 0) {
    return source;
  }
  std::vector<std::uint8_t> scratch(source.size());
  const int diameter = radius * 2 + 1;
  for (int y = 0; y < height; ++y) {
    int sum = 0;
    for (int x = -radius; x <= radius; ++x) {
      sum += source[static_cast<std::size_t>(y * width + std::clamp(x, 0, width - 1))];
    }
    for (int x = 0; x < width; ++x) {
      scratch[static_cast<std::size_t>(y * width + x)] = static_cast<std::uint8_t>(sum / diameter);
      sum -= source[static_cast<std::size_t>(y * width + std::clamp(x - radius, 0, width - 1))];
      sum += source[static_cast<std::size_t>(y * width + std::clamp(x + radius + 1, 0, width - 1))];
    }
  }
  for (int x = 0; x < width; ++x) {
    int sum = 0;
    for (int y = -radius; y <= radius; ++y) {
      sum += scratch[static_cast<std::size_t>(std::clamp(y, 0, height - 1) * width + x)];
    }
    for (int y = 0; y < height; ++y) {
      source[static_cast<std::size_t>(y * width + x)] = static_cast<std::uint8_t>(sum / diameter);
      sum -= scratch[static_cast<std::size_t>(std::clamp(y - radius, 0, height - 1) * width + x)];
      sum += scratch[static_cast<std::size_t>(std::clamp(y + radius + 1, 0, height - 1) * width + x)];
    }
  }
  return source;
}

} // namespace

struct LinuxRenderer::State final {
  struct CachedImage final {
    std::uint64_t identity = 0;
    ImageBuffer image;
    std::uint64_t bytes = 0;
  };

  struct CachedExternalTexture final {
    std::weak_ptr<LinuxExternalTextureState> source;
    ImageBuffer image;
  };

  ImageBuffer* ImageFor(const ImageAsset& image) {
    const std::uint64_t identity = ResourceAccess::ImageIdentity(image);
    const auto existing = std::ranges::find(images, identity, &CachedImage::identity);
    if (existing != images.end()) {
      std::rotate(existing, existing + 1, images.end());
      return &images.back().image;
    }
    const std::span<const std::byte> bytes = image.EncodedBytes();
    SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
    SDL_Surface* decoded = stream != nullptr ? IMG_Load_IO(stream, true) : nullptr;
    if (decoded == nullptr) {
      return nullptr;
    }
    ImageBuffer buffer = PremultiplySurface(*decoded);
    SDL_DestroySurface(decoded);
    if (buffer.Empty()) {
      return nullptr;
    }
    const std::uint64_t decoded_bytes = static_cast<std::uint64_t>(buffer.pixels.size()) * sizeof(std::uint32_t);
    while (!images.empty() && (images.size() >= kMaxImages || image_cache_bytes + decoded_bytes > kImageCacheBudget)) {
      image_cache_bytes -= images.front().bytes;
      images.erase(images.begin());
    }
    images.push_back({identity, std::move(buffer), decoded_bytes});
    image_cache_bytes += decoded_bytes;
    return &images.back().image;
  }

  ImageBuffer* ExternalTextureFor(const ExternalTexture& texture) {
    const std::shared_ptr<LinuxExternalTextureState> source =
        std::dynamic_pointer_cast<LinuxExternalTextureState>(ExternalTextureState::From(texture));
    if (!source) {
      throw std::logic_error("HuxerUI external texture does not contain a Linux frame source");
    }
    std::erase_if(external_textures, [](const CachedExternalTexture& entry) {
      const std::shared_ptr<LinuxExternalTextureState> retained = entry.source.lock();
      return !retained || !retained->IsActive();
    });
    auto entry = std::ranges::find_if(external_textures, [&source](const CachedExternalTexture& candidate) {
      return candidate.source.lock() == source;
    });
    if (entry == external_textures.end()) {
      entry = external_textures.insert(external_textures.end(), {.source = source, .image = {}});
    }
    if (std::optional<LinuxExternalTextureFrame> frame = source->AcquireLatestFrame()) {
      entry->image.width = frame->PixelWidth();
      entry->image.height = frame->PixelHeight();
      entry->image.pixels.resize(
          static_cast<std::size_t>(frame->PixelWidth()) * static_cast<std::size_t>(frame->PixelHeight())
      );
      std::memcpy(entry->image.pixels.data(), frame->Pixels().data(), frame->Pixels().size());
    }
    return entry->image.Empty() ? nullptr : &entry->image;
  }

  LinuxTextRenderer text;
  std::vector<CachedImage> images;
  std::vector<CachedExternalTexture> external_textures;
  std::uint64_t image_cache_bytes = 0;
  static constexpr std::uint64_t kImageCacheBudget = 64U * 1024U * 1024U;
  static constexpr std::size_t kMaxImages = 64;
};

namespace {

class ScenePainter final {
public:
  ScenePainter(LinuxRenderer::State& state, CpuCanvas& canvas) : state_(state), canvas_(canvas) {}

  void Draw(const RenderScene& scene) {
    if (scene.root != nullptr) {
      DrawNode(*scene.root, canvas_);
    }
  }

private:
  void DrawNode(const RenderNode& node, CpuCanvas& canvas) {
    const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
    if (!node.visible || opacity <= 0.0F) {
      return;
    }
    canvas.Save();
    canvas.Transform(Translation(node.offset));
    canvas.Transform(node.transform);
    if (opacity < 1.0F) {
      std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> layer(
          SDL_CreateSurface(canvas.Width(), canvas.Height(), SDL_PIXELFORMAT_ARGB8888),
          SDL_DestroySurface
      );
      if (!layer) {
        throw std::runtime_error("HuxerUI Linux could not allocate an opacity layer: " + std::string(SDL_GetError()));
      }
      CpuCanvas layer_canvas(*layer, canvas.CurrentState());
      layer_canvas.Clear();
      DrawNodeContents(node, layer_canvas);
      canvas.Composite(*layer, opacity);
    } else {
      DrawNodeContents(node, canvas);
    }
    canvas.Restore();
  }

  void DrawNodeContents(const RenderNode& node, CpuCanvas& canvas) {
    DrawSequence(node.content, canvas);
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this, &canvas](const auto& value) { DrawCommand(value, canvas); }, clip);
    }
    canvas.Save();
    canvas.Transform(node.children_transform);
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        DrawNode(*child, canvas);
      }
    }
    canvas.Restore();
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
      canvas.Restore();
    }
    DrawSequence(node.foreground, canvas);
  }

  void DrawSequence(const PaintSequence& sequence, CpuCanvas& canvas) {
    for (const PaintCommand& command : sequence.Commands()) {
      std::visit([this, &canvas](const auto& value) { DrawCommand(value, canvas); }, command);
    }
  }

  void DrawCommand(const DrawRectCommand& command, CpuCanvas& canvas) {
    if (!command.rect.IsEmpty() && command.color.alpha > 0.0F) {
      canvas.Fill(
          RoundedRect(command.rect, command.corner_radius, canvas.CurrentTransform()),
          PathFillRule::NonZero,
          command.color
      );
    }
  }

  void DrawCommand(const DrawLinearGradientCommand& command, CpuCanvas& canvas) {
    if (command.rect.IsEmpty()) {
      return;
    }
    const FlatPath path = RoundedRect(command.rect, command.corner_radius, canvas.CurrentTransform());
    const Transform2D transform = canvas.CurrentTransform();
    const Point start{
        command.rect.x + command.gradient.start.x * command.rect.width,
        command.rect.y + command.gradient.start.y * command.rect.height,
    };
    const Point end{
        command.rect.x + command.gradient.end.x * command.rect.width,
        command.rect.y + command.gradient.end.y * command.rect.height,
    };
    const Point delta = end - start;
    const float length_squared = std::max(0.000001F, delta.x * delta.x + delta.y * delta.y);
    canvas.Rasterize(
        path.bounds,
        [&path](Point point) { return Contains(path, point, PathFillRule::NonZero); },
        [=, &command](Point device) {
          const Point local = transform.Inverse(device).value_or(start);
          const float offset = ((local.x - start.x) * delta.x + (local.y - start.y) * delta.y) / length_squared;
          return GradientColor(command.gradient.stops, offset);
        }
    );
  }

  void DrawCommand(const DrawRadialGradientCommand& command, CpuCanvas& canvas) {
    if (command.rect.IsEmpty()) {
      return;
    }
    const FlatPath path = RoundedRect(command.rect, command.corner_radius, canvas.CurrentTransform());
    const Transform2D transform = canvas.CurrentTransform();
    const Point center{
        command.rect.x + command.gradient.center.x * command.rect.width,
        command.rect.y + command.gradient.center.y * command.rect.height,
    };
    const Size radius{
        std::max(0.001F, command.gradient.radius.width * command.rect.width),
        std::max(0.001F, command.gradient.radius.height * command.rect.height),
    };
    canvas.Rasterize(
        path.bounds,
        [&path](Point point) { return Contains(path, point, PathFillRule::NonZero); },
        [=, &command](Point device) {
          const Point local = transform.Inverse(device).value_or(center);
          return GradientColor(
              command.gradient.stops,
              std::hypot((local.x - center.x) / radius.width, (local.y - center.y) / radius.height)
          );
        }
    );
  }

  void DrawCommand(const DrawTextCommand& command, CpuCanvas& canvas) {
    if (command.text.empty() || command.rect.IsEmpty() || command.style.foreground.alpha <= 0.0F) {
      return;
    }
    const Transform2D transform = canvas.CurrentTransform();
    const float raster_scale =
        std::max(std::hypot(transform.m11, transform.m12), std::hypot(transform.m21, transform.m22));
    LinuxRenderedText rendered =
        state_.text.Render(command.text, command.style, command.rect.width, command.options, raster_scale);
    if (!rendered.surface) {
      return;
    }
    const float remaining = std::max(0.0F, command.rect.height - rendered.metrics.size.height);
    const float vertical_offset = command.options.vertical_align == TextVerticalAlign::Center   ? remaining * 0.5F
                                  : command.options.vertical_align == TextVerticalAlign::Bottom ? remaining
                                                                                                : 0.0F;
    const ImageBuffer image = PremultiplySurface(*rendered.surface);
    canvas.PushClip(Path::RoundedRect(command.rect, {}), PathFillRule::NonZero);
    canvas.DrawImage(
        image,
        {0.0F, 0.0F, static_cast<float>(image.width), static_cast<float>(image.height)},
        {
            command.rect.x + command.paragraph_offset.x,
            command.rect.y + vertical_offset + command.paragraph_offset.y,
            static_cast<float>(image.width) / rendered.raster_scale,
            static_cast<float>(image.height) / rendered.raster_scale,
        },
        ImageSampling::Linear,
        1.0F
    );
    canvas.Restore();
  }

  void DrawCommand(const DrawTextRunsCommand& command, CpuCanvas& canvas) {
    for (const TextRun& run : command.runs) {
      if (run.text.empty() || run.bounds.IsEmpty() || run.style.foreground.alpha <= 0.0F) {
        continue;
      }
      const Transform2D transform = canvas.CurrentTransform();
      const float raster_scale =
          std::max(std::hypot(transform.m11, transform.m12), std::hypot(transform.m21, transform.m22));
      LinuxRenderedText rendered = state_.text.Render(
          run.text,
          run.style,
          std::numeric_limits<float>::infinity(),
          {.shaping = run.shaping, .wrap = TextWrap::NoWrap},
          raster_scale
      );
      const ImageBuffer image = rendered.surface ? PremultiplySurface(*rendered.surface) : ImageBuffer{};
      if (!image.Empty()) {
        canvas.DrawImage(
            image,
            {0.0F, 0.0F, static_cast<float>(image.width), static_cast<float>(image.height)},
            {
                run.baseline_origin.x,
                run.baseline_origin.y - rendered.metrics.first_baseline,
                static_cast<float>(image.width) / rendered.raster_scale,
                static_cast<float>(image.height) / rendered.raster_scale,
            },
            ImageSampling::Linear,
            1.0F
        );
      }
    }
  }

  void DrawCommand(const DrawImageCommand& command, CpuCanvas& canvas) {
    if (ImageBuffer* image = state_.ImageFor(command.image); image != nullptr) {
      const float scale = command.image.Scale();
      canvas.DrawImage(
          *image,
          {
              command.source.x * scale,
              command.source.y * scale,
              command.source.width * scale,
              command.source.height * scale,
          },
          command.destination,
          command.sampling,
          command.opacity
      );
    }
  }

  void DrawCommand(const DrawExternalTextureCommand& command, CpuCanvas& canvas) {
    ImageBuffer* image = state_.ExternalTextureFor(command.texture);
    if (image == nullptr) {
      return;
    }
    const Size intrinsic = command.texture.IntrinsicSize();
    canvas.DrawImage(
        *image,
        {
            command.source.x * static_cast<float>(image->width) / std::max(0.001F, intrinsic.width),
            command.source.y * static_cast<float>(image->height) / std::max(0.001F, intrinsic.height),
            command.source.width * static_cast<float>(image->width) / std::max(0.001F, intrinsic.width),
            command.source.height * static_cast<float>(image->height) / std::max(0.001F, intrinsic.height),
        },
        command.destination,
        command.sampling,
        command.opacity
    );
  }

  void DrawCommand(const DrawCircleCommand& command, CpuCanvas& canvas) {
    if (command.radius > 0.0F && command.color.alpha > 0.0F) {
      canvas.Fill(
          Ellipse(command.center, command.radius, canvas.CurrentTransform()),
          PathFillRule::NonZero,
          command.color
      );
    }
  }

  void DrawCommand(const DrawArcCommand& command, CpuCanvas& canvas) {
    if (command.radius > 0.0F && command.width > 0.0F && command.color.alpha > 0.0F && command.sweep_angle != 0.0F) {
      canvas.Stroke(
          Arc(command.center,
              command.radius,
              command.start_angle,
              command.sweep_angle,
              canvas.CurrentTransform(),
              false),
          command.width,
          command.color,
          command.cap
      );
    }
  }

  void DrawCommand(const DrawBorderCommand& command, CpuCanvas& canvas) {
    if (command.rect.IsEmpty() || command.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    const Rect outer = command.rect;
    const Rect inner{
        command.rect.x + command.width,
        command.rect.y + command.width,
        std::max(0.0F, command.rect.width - command.width * 2.0F),
        std::max(0.0F, command.rect.height - command.width * 2.0F),
    };
    const FlatPath outer_path = RoundedRect(outer, command.corner_radius, canvas.CurrentTransform());
    const FlatPath inner_path =
        RoundedRect(inner, std::max(0.0F, command.corner_radius - command.width), canvas.CurrentTransform());
    canvas.Rasterize(
        outer_path.bounds,
        [&outer_path, &inner_path](Point point) {
          return Contains(outer_path, point, PathFillRule::NonZero) &&
                 !Contains(inner_path, point, PathFillRule::NonZero);
        },
        [&command](Point) { return command.color; }
    );
  }

  void DrawShadow(
      const FlatPath& shifted,
      const FlatPath& caster,
      PathFillRule rule,
      Color color,
      float blur_radius,
      CpuCanvas& canvas
  ) {
    if (color.alpha <= 0.0F) {
      return;
    }
    const float scale = std::max(
        std::hypot(canvas.CurrentTransform().m11, canvas.CurrentTransform().m12),
        std::hypot(canvas.CurrentTransform().m21, canvas.CurrentTransform().m22)
    );
    const int blur = std::max(0, static_cast<int>(std::ceil(blur_radius * scale * 0.57735F)));
    const int left = std::max(0, static_cast<int>(std::floor(shifted.bounds.x)) - blur - 1);
    const int top = std::max(0, static_cast<int>(std::floor(shifted.bounds.y)) - blur - 1);
    const int right =
        std::min(canvas.Width(), static_cast<int>(std::ceil(shifted.bounds.x + shifted.bounds.width)) + blur + 1);
    const int bottom =
        std::min(canvas.Height(), static_cast<int>(std::ceil(shifted.bounds.y + shifted.bounds.height)) + blur + 1);
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0) {
      return;
    }
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const Point point{static_cast<float>(left + x) + 0.5F, static_cast<float>(top + y) + 0.5F};
        if (canvas.Allows(point) && Contains(shifted, point, rule)) {
          mask[static_cast<std::size_t>(y * width + x)] = 255U;
        }
      }
    }
    mask = BoxBlur(std::move(mask), width, height, blur);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const Point point{static_cast<float>(left + x) + 0.5F, static_cast<float>(top + y) + 0.5F};
        if (!canvas.Allows(point) || Contains(caster, point, rule)) {
          continue;
        }
        const float coverage = static_cast<float>(mask[static_cast<std::size_t>(y * width + x)]) / 255.0F;
        canvas.Blend(left + x, top + y, PremultipliedPixel(color, coverage));
      }
    }
  }

  void DrawCommand(const DrawShadowCommand& command, CpuCanvas& canvas) {
    const ResolvedShadow resolved = ResolveShadow(command);
    const FlatPath caster = RoundedRect(resolved.caster, resolved.corner_radius, canvas.CurrentTransform());
    if (command.blur_radius <= 0.0F) {
      canvas.Fill(caster, PathFillRule::NonZero, command.color);
      return;
    }
    DrawShadow(caster, caster, PathFillRule::NonZero, command.color, command.blur_radius, canvas);
  }

  void DrawCommand(const FillPathCommand& command, CpuCanvas& canvas) {
    canvas.Fill(FlattenPath(command.path, canvas.CurrentTransform()), command.fill_rule, command.color);
  }

  void DrawCommand(const StrokePathCommand& command, CpuCanvas& canvas) {
    canvas.Stroke(
        FlattenPath(command.path, canvas.CurrentTransform(), false),
        command.width,
        command.color,
        command.cap,
        command.join,
        command.miter_limit
    );
  }

  void DrawCommand(const DrawPathShadowCommand& command, CpuCanvas& canvas) {
    const FlatPath shifted =
        FlattenPath(command.path, Multiply(canvas.CurrentTransform(), Translation(command.offset)));
    if (command.blur_radius <= 0.0F) {
      canvas.Fill(shifted, command.fill_rule, command.color);
      return;
    }
    DrawShadow(shifted, shifted, command.fill_rule, command.color, command.blur_radius, canvas);
  }

  void DrawCommand(const PushClipCommand& command, CpuCanvas& canvas) {
    canvas.PushClip(Path::RoundedRect(command.rect, CornerRadii(command.corner_radius)), PathFillRule::NonZero);
  }

  void DrawCommand(const PushPathClipCommand& command, CpuCanvas& canvas) {
    canvas.PushClip(command.path, command.fill_rule);
  }

  void DrawCommand(const PopClipCommand&, CpuCanvas& canvas) {
    canvas.Restore();
  }

  void DrawCommand(const PushTransformCommand& command, CpuCanvas& canvas) {
    canvas.Save();
    canvas.Transform(command.transform);
  }

  void DrawCommand(const PopTransformCommand&, CpuCanvas& canvas) {
    canvas.Restore();
  }

  void DrawCommand(const PlacePlatformViewCommand&, CpuCanvas&) {
    throw std::logic_error("HuxerUI Linux adapter does not support PlatformView composition yet");
  }

  LinuxRenderer::State& state_;
  CpuCanvas& canvas_;
};

} // namespace

LinuxRenderer::LinuxRenderer() : state_(std::make_unique<State>()) {}

LinuxRenderer::~LinuxRenderer() = default;

void LinuxRenderer::Initialize() {
  if (!state_) {
    state_ = std::make_unique<State>();
  }
}

void LinuxRenderer::Discard() noexcept {
  state_.reset();
}

FontMetrics LinuxRenderer::Metrics(const Font& font) {
  return state_->text.Metrics(font);
}

TextRunMetrics
LinuxRenderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  return state_->text.MeasureRun(text, style, options);
}

TextLayoutMetrics LinuxRenderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return state_->text.MeasureText(text, style, max_width, options);
}

std::unique_ptr<TextLayout> LinuxRenderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return state_->text.CreateTextLayout(text, style, max_width, options);
}

void LinuxRenderer::Draw(SDL_Surface* surface, const RenderFrame& frame, float scale_x, float scale_y) {
  if (surface == nullptr || surface->format != SDL_PIXELFORMAT_ARGB8888) {
    throw std::invalid_argument("HuxerUI Linux renderer requires an ARGB8888 SDL surface");
  }
  CpuCanvas canvas(*surface, std::max(0.001F, scale_x), std::max(0.001F, scale_y));
  canvas.Clear(kDefaultBackgroundPixel);
  ScenePainter(*state_, canvas).Draw(frame.scene);
}

} // namespace huxerui::detail
