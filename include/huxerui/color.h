#pragma once

namespace huxerui {

struct Color {
  float red = 0.0F;
  float green = 0.0F;
  float blue = 0.0F;
  float alpha = 1.0F;

  static constexpr Color Rgb(int red, int green, int blue, float alpha = 1.0F) noexcept {
    return {
        static_cast<float>(red) / 255.0F,
        static_cast<float>(green) / 255.0F,
        static_cast<float>(blue) / 255.0F,
        alpha,
    };
  }

  static constexpr Color Transparent() noexcept {
    return {0.0F, 0.0F, 0.0F, 0.0F};
  }

  static constexpr Color Black() noexcept {
    return {0.0F, 0.0F, 0.0F, 1.0F};
  }

  static constexpr Color White() noexcept {
    return {1.0F, 1.0F, 1.0F, 1.0F};
  }
};

}  // namespace huxerui
