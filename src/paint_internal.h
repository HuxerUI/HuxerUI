#pragma once

#include <huxerui/paint.h>

namespace huxerui::detail {

void ValidateColor(Color color, const char* message);
void ValidateCornerRadii(CornerRadii radii, const char* message);
void ValidateGradient(const LinearGradient& gradient);
void ValidateGradient(const RadialGradient& gradient);
Transform2D ResolveGradientTransform(Rect rect, const LinearGradient& gradient) noexcept;
Transform2D ResolveGradientTransform(Rect rect, const RadialGradient& gradient) noexcept;
void PaintVisualFill(PaintContext& context, Rect bounds, const VisualFill& fill, CornerRadii corner_radii,
                     float opacity = 1.0F);

} // namespace huxerui::detail
