#include <huxerui/vector.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

#include <huxerui/paint.h>

#include "resource_internal.h"
#include "vector_format.h"
#include "internal_access.h"

namespace huxerui {

namespace {

class VectorReader {
public:
  explicit VectorReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  std::uint8_t U8() {
    Require(1);
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
  }

  std::uint32_t U32() {
    Require(4);
    const std::uint32_t value =
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 3])) << 24U);
    offset_ += 4;
    return value;
  }

  float F32() {
    return std::bit_cast<float>(U32());
  }

  [[nodiscard]] bool AtEnd() const noexcept {
    return offset_ == bytes_.size();
  }

  [[nodiscard]] std::size_t RemainingBytes() const noexcept {
    return bytes_.size() - offset_;
  }

private:
  void Require(std::size_t count) const {
    if (count > bytes_.size() - offset_) {
      throw std::logic_error("HuxerUI vector payload is truncated");
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

Point ReadPoint(VectorReader& reader) {
  return {reader.F32(), reader.F32()};
}

Color ReadColor(VectorReader& reader) {
  return {reader.F32(), reader.F32(), reader.F32(), reader.F32()};
}

Rect ReadRect(VectorReader& reader) {
  return {reader.F32(), reader.F32(), reader.F32(), reader.F32()};
}

Transform2D ReadTransform(VectorReader& reader) {
  return {reader.F32(), reader.F32(), reader.F32(), reader.F32(), reader.F32(), reader.F32()};
}

std::vector<GradientStop> ReadGradientStops(VectorReader& reader) {
  const std::uint32_t count = reader.U32();
  constexpr std::size_t bytes_per_stop = sizeof(float) * 5;
  if (count > reader.RemainingBytes() / bytes_per_stop) {
    throw std::logic_error("HuxerUI vector payload is truncated");
  }
  std::vector<GradientStop> stops;
  stops.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    stops.push_back({reader.F32(), ReadColor(reader)});
  }
  return stops;
}

Brush ReadBrush(VectorReader& reader) {
  switch (static_cast<detail::vector_format::BrushKind>(reader.U8())) {
  case detail::vector_format::BrushKind::Color:
    return ReadColor(reader);
  case detail::vector_format::BrushKind::LinearGradient: {
    const Point start = ReadPoint(reader);
    const Point end = ReadPoint(reader);
    const Transform2D transform = ReadTransform(reader);
    return LinearGradient{start, end, ReadGradientStops(reader), transform};
  }
  case detail::vector_format::BrushKind::RadialGradient: {
    const Point center = ReadPoint(reader);
    const Size radius{reader.F32(), reader.F32()};
    const Transform2D transform = ReadTransform(reader);
    return RadialGradient{center, radius, ReadGradientStops(reader), transform};
  }
  default:
    throw std::logic_error("HuxerUI vector payload contains an unknown brush kind");
  }
}

StrokeCap ReadStrokeCap(VectorReader& reader) {
  switch (static_cast<detail::vector_format::StrokeCap>(reader.U8())) {
  case detail::vector_format::StrokeCap::Butt:
    return StrokeCap::Butt;
  case detail::vector_format::StrokeCap::Round:
    return StrokeCap::Round;
  case detail::vector_format::StrokeCap::Square:
    return StrokeCap::Square;
  default:
    throw std::logic_error("HuxerUI vector payload contains invalid stroke configuration");
  }
}

StrokeJoin ReadStrokeJoin(VectorReader& reader) {
  switch (static_cast<detail::vector_format::StrokeJoin>(reader.U8())) {
  case detail::vector_format::StrokeJoin::Miter:
    return StrokeJoin::Miter;
  case detail::vector_format::StrokeJoin::Round:
    return StrokeJoin::Round;
  case detail::vector_format::StrokeJoin::Bevel:
    return StrokeJoin::Bevel;
  default:
    throw std::logic_error("HuxerUI vector payload contains invalid stroke configuration");
  }
}

StrokeStyle ReadStrokeStyle(VectorReader& reader) {
  const float width = reader.F32();
  const StrokeCap cap = ReadStrokeCap(reader);
  const StrokeJoin join = ReadStrokeJoin(reader);
  const float miter_limit = reader.F32();
  const std::uint32_t dash_count = reader.U32();
  if (reader.RemainingBytes() < sizeof(float) ||
      dash_count > (reader.RemainingBytes() - sizeof(float)) / sizeof(float)) {
    throw std::logic_error("HuxerUI vector payload is truncated");
  }
  std::vector<float> dash_pattern;
  dash_pattern.reserve(dash_count);
  for (std::uint32_t index = 0; index < dash_count; ++index) {
    dash_pattern.push_back(reader.F32());
  }
  return {
      .width = width,
      .cap = cap,
      .join = join,
      .miter_limit = miter_limit,
      .dash_pattern = std::move(dash_pattern),
      .dash_offset = reader.F32(),
  };
}

Path ReadPath(VectorReader& reader) {
  Path path;
  const std::uint32_t count = reader.U32();
  for (std::uint32_t index = 0; index < count; ++index) {
    switch (static_cast<detail::vector_format::PathVerb>(reader.U8())) {
    case detail::vector_format::PathVerb::MoveTo:
      path.MoveTo(ReadPoint(reader));
      break;
    case detail::vector_format::PathVerb::LineTo:
      path.LineTo(ReadPoint(reader));
      break;
    case detail::vector_format::PathVerb::QuadraticTo: {
      const Point control = ReadPoint(reader);
      path.QuadraticTo(control, ReadPoint(reader));
      break;
    }
    case detail::vector_format::PathVerb::CubicTo: {
      const Point first_control = ReadPoint(reader);
      const Point second_control = ReadPoint(reader);
      path.CubicTo(first_control, second_control, ReadPoint(reader));
      break;
    }
    case detail::vector_format::PathVerb::Close:
      path.Close();
      break;
    default:
      throw std::logic_error("HuxerUI vector payload contains an unknown path operation");
    }
  }
  return path;
}

PathFillRule ReadFillRule(VectorReader& reader) {
  switch (static_cast<detail::vector_format::FillRule>(reader.U8())) {
  case detail::vector_format::FillRule::NonZero:
    return PathFillRule::NonZero;
  case detail::vector_format::FillRule::EvenOdd:
    return PathFillRule::EvenOdd;
  default:
    throw std::logic_error("HuxerUI vector payload contains an invalid fill rule");
  }
}

bool IsValid(Rect rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width > 0.0F && rect.height > 0.0F;
}

bool IsValid(Size size) noexcept {
  return std::isfinite(size.width) && std::isfinite(size.height) && size.width > 0.0F && size.height > 0.0F;
}

void RequireGeometry(Rect view_box, Size intrinsic_size) {
  if (!IsValid(view_box)) {
    throw std::invalid_argument("HuxerUI vector view box must be finite with positive dimensions");
  }
  if (!IsValid(intrinsic_size)) {
    throw std::invalid_argument("HuxerUI vector intrinsic size must be finite with positive dimensions");
  }
}

} // namespace

struct VectorAsset::Data {
  Rect view_box;
  Size intrinsic_size;
  PaintSequence sequence;
};

struct VectorBuilder::Impl {
  explicit Impl(Rect view_box) : context(sequence, view_box) {}

  PaintSequence sequence;
  PaintContext context;
};

VectorBuilder::VectorBuilder(Rect view_box) : impl_(std::make_unique<Impl>(view_box)) {}

VectorBuilder::~VectorBuilder() = default;

void VectorBuilder::FillPath(Path path, Brush brush, PathFillRule fill_rule) {
  impl_->context.FillPath(std::move(path), std::move(brush), fill_rule);
}

void VectorBuilder::FillPath(Path path, Brush brush, Rect brush_bounds, PathFillRule fill_rule) {
  impl_->context.FillPath(std::move(path), std::move(brush), brush_bounds, fill_rule);
}

void VectorBuilder::StrokePath(Path path, Brush brush, StrokeStyle style) {
  impl_->context.StrokePath(std::move(path), std::move(brush), std::move(style));
}

void VectorBuilder::StrokePath(Path path, Brush brush, Rect brush_bounds, StrokeStyle style) {
  impl_->context.StrokePath(std::move(path), std::move(brush), brush_bounds, std::move(style));
}

void VectorBuilder::PushClip(Path path, PathFillRule fill_rule) {
  impl_->context.PushPathClip(std::move(path), fill_rule);
}

void VectorBuilder::PopClip() {
  impl_->context.PopClip();
}

void VectorBuilder::PushTransform(Transform2D transform) {
  impl_->context.PushTransform(transform);
}

void VectorBuilder::PopTransform() {
  impl_->context.PopTransform();
}

VectorAsset VectorAsset::Create(Size intrinsic_size, const std::function<void(VectorBuilder&)>& build) {
  return Create({0.0F, 0.0F, intrinsic_size.width, intrinsic_size.height}, intrinsic_size, build);
}

VectorAsset VectorAsset::Create(Rect view_box, Size intrinsic_size, const std::function<void(VectorBuilder&)>& build) {
  RequireGeometry(view_box, intrinsic_size);
  if (!build) {
    throw std::invalid_argument("HuxerUI vector builder function must not be empty");
  }
  auto data = std::make_shared<Data>();
  data->view_box = view_box;
  data->intrinsic_size = intrinsic_size;
  VectorBuilder builder(view_box);
  build(builder);
  builder.impl_->context.Finish();
  data->sequence = std::move(builder.impl_->sequence);
  return VectorAsset(std::move(data));
}

Rect VectorAsset::ViewBox() const noexcept {
  return data_ ? data_->view_box : Rect{};
}

Size VectorAsset::IntrinsicSize() const noexcept {
  return data_ ? data_->intrinsic_size : Size{};
}

bool VectorAsset::HasValue() const noexcept {
  return static_cast<bool>(data_);
}

bool VectorAsset::operator==(const VectorAsset& other) const noexcept {
  return data_ == other.data_ || (data_ && other.data_ && data_->view_box == other.data_->view_box &&
                                  data_->intrinsic_size == other.data_->intrinsic_size &&
                                  data_->sequence.Commands() == other.data_->sequence.Commands());
}

const PaintSequence& detail::InternalAccess::Sequence(const VectorAsset& asset) noexcept {
  static const PaintSequence empty;
  return asset.data_ ? asset.data_->sequence : empty;
}

bool detail::InternalAccess::IsVectorPayload(const RawAsset& asset) noexcept {
  const std::span<const std::byte> bytes = asset.Bytes();
  return bytes.size() >= vector_format::magic.size() &&
         std::equal(vector_format::magic.begin(), vector_format::magic.end(), bytes.begin());
}

VectorAsset detail::InternalAccess::VectorFromRaw(RawAsset asset) {
  if (!IsVectorPayload(asset)) {
    throw std::invalid_argument("HuxerUI image resource is not a vector payload");
  }
  try {
    VectorReader reader(asset.Bytes().subspan(vector_format::magic.size()));
    if (reader.U32() != vector_format::current_version) {
      throw std::logic_error("HuxerUI vector payload version is unsupported");
    }
    const Rect view_box{reader.F32(), reader.F32(), reader.F32(), reader.F32()};
    const Size intrinsic_size{reader.F32(), reader.F32()};
    const std::uint32_t count = reader.U32();
    VectorAsset result = VectorAsset::Create(view_box, intrinsic_size, [&](VectorBuilder& builder) {
      for (std::uint32_t index = 0; index < count; ++index) {
        switch (static_cast<vector_format::DrawingOperation>(reader.U8())) {
        case vector_format::DrawingOperation::FillPath: {
          const PathFillRule fill_rule = ReadFillRule(reader);
          Brush brush = ReadBrush(reader);
          const Rect brush_bounds = ReadRect(reader);
          builder.FillPath(ReadPath(reader), std::move(brush), brush_bounds, fill_rule);
          break;
        }
        case vector_format::DrawingOperation::StrokePath: {
          Brush brush = ReadBrush(reader);
          const Rect brush_bounds = ReadRect(reader);
          StrokeStyle style = ReadStrokeStyle(reader);
          builder.StrokePath(ReadPath(reader), std::move(brush), brush_bounds, std::move(style));
          break;
        }
        case vector_format::DrawingOperation::PushClip: {
          const PathFillRule fill_rule = ReadFillRule(reader);
          builder.PushClip(ReadPath(reader), fill_rule);
          break;
        }
        case vector_format::DrawingOperation::PopClip:
          builder.PopClip();
          break;
        case vector_format::DrawingOperation::PushTransform:
          builder.PushTransform({reader.F32(), reader.F32(), reader.F32(), reader.F32(), reader.F32(), reader.F32()});
          break;
        case vector_format::DrawingOperation::PopTransform:
          builder.PopTransform();
          break;
        default:
          throw std::logic_error("HuxerUI vector payload contains an unknown drawing operation");
        }
      }
    });
    if (!reader.AtEnd()) {
      throw std::logic_error("HuxerUI vector payload contains trailing data");
    }
    return result;
  } catch (const std::invalid_argument& error) {
    throw std::logic_error(std::string("HuxerUI vector payload is invalid: ") + error.what());
  }
}

} // namespace huxerui
