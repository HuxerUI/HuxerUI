#pragma once

#include <huxerui/paint.h>

namespace huxerui::detail {

using RasterImage = decltype(DrawImageCommand::image);

inline Size RasterImageSize(const RasterImage& image) noexcept {
  return std::visit([](const auto& value) { return value.IntrinsicSize(); }, image);
}

inline float RasterImageScale(const RasterImage& image) noexcept {
  return std::visit([](const auto& value) { return value.Scale(); }, image);
}

std::uint64_t RasterImageIdentity(const RasterImage& image) noexcept;

void ValidateColor(Color color, const char* message);
void ValidateCornerRadii(CornerRadii radii, const char* message);
void ValidateGradient(const LinearGradient& gradient);
void ValidateGradient(const RadialGradient& gradient);
void ValidateBrush(const Brush& brush);
Brush ModulateBrush(const Brush& brush, std::optional<Color> tint, float opacity);
Transform2D ResolveGradientTransform(Rect rect, const LinearGradient& gradient) noexcept;
Transform2D ResolveGradientTransform(Rect rect, const RadialGradient& gradient) noexcept;
void PaintVisualFill(PaintContext& context, Rect bounds, const VisualFill& fill, CornerRadii corner_radii,
                     float opacity = 1.0F);

} // namespace huxerui::detail
