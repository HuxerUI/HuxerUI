#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace huxerui::detail::vector_format {

inline constexpr std::array<std::byte, 8> magic{
    std::byte{'H'},
    std::byte{'U'},
    std::byte{'X'},
    std::byte{'V'},
    std::byte{'E'},
    std::byte{'C'},
    std::byte{0},
    std::byte{0},
};

inline constexpr std::uint32_t current_version = 1;

enum class DrawingOperation : std::uint8_t {
  FillPath = 1,
  StrokePath = 2,
  PushClip = 3,
  PopClip = 4,
  PushTransform = 5,
  PopTransform = 6,
};

enum class BrushKind : std::uint8_t {
  Color = 1,
  LinearGradient = 2,
  RadialGradient = 3,
};

enum class PathVerb : std::uint8_t {
  MoveTo = 1,
  LineTo = 2,
  QuadraticTo = 3,
  CubicTo = 4,
  Close = 5,
};

enum class FillRule : std::uint8_t {
  NonZero = 0,
  EvenOdd = 1,
};

enum class StrokeCap : std::uint8_t {
  Butt = 0,
  Round = 1,
  Square = 2,
};

enum class StrokeJoin : std::uint8_t {
  Miter = 0,
  Round = 1,
  Bevel = 2,
};

} // namespace huxerui::detail::vector_format
