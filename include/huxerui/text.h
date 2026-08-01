#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <huxerui/color.h>
#include <huxerui/geometry.h>

namespace huxerui {

enum class FontFamilyKind {
  System,
  Monospace,
  Named,
};

enum class FontWeight : std::uint16_t {
  Thin = 100,
  ExtraLight = 200,
  Light = 300,
  Regular = 400,
  Medium = 500,
  SemiBold = 600,
  Bold = 700,
  ExtraBold = 800,
  Black = 900,
};

enum class FontSlant {
  Normal,
  Italic,
};

class Font {
public:
  Font() = default;

  static Font System(float size = 14.0F);
  static Font Monospace(float size = 14.0F);
  static Font Named(std::string family, float size = 14.0F);

  [[nodiscard]] Font WithSize(float size) const;
  [[nodiscard]] Font WithWeight(FontWeight weight) const;
  [[nodiscard]] Font WithSlant(FontSlant slant) const;

  [[nodiscard]] FontFamilyKind FamilyKind() const noexcept {
    return family_kind_;
  }

  [[nodiscard]] std::string_view FamilyName() const noexcept {
    return family_name_;
  }

  [[nodiscard]] float Size() const noexcept {
    return size_;
  }

  [[nodiscard]] FontWeight Weight() const noexcept {
    return weight_;
  }

  [[nodiscard]] FontSlant Slant() const noexcept {
    return slant_;
  }

  bool operator==(const Font&) const = default;

private:
  Font(FontFamilyKind family_kind, std::string family_name, float size);

  FontFamilyKind family_kind_ = FontFamilyKind::System;
  std::string family_name_;
  float size_ = 14.0F;
  FontWeight weight_ = FontWeight::Regular;
  FontSlant slant_ = FontSlant::Normal;
};

enum class TextDecoration : std::uint8_t {
  None = 0,
  Underline = 1 << 0,
  StrikeThrough = 1 << 1,
};

constexpr TextDecoration operator|(TextDecoration left, TextDecoration right) noexcept {
  return static_cast<TextDecoration>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

constexpr TextDecoration operator&(TextDecoration left, TextDecoration right) noexcept {
  return static_cast<TextDecoration>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
}

constexpr bool HasTextDecoration(TextDecoration value, TextDecoration decoration) noexcept {
  return (value & decoration) != TextDecoration::None;
}

struct TextStyle {
  Font font = Font::System();
  Color foreground = Color::Rgb(31, 35, 40);
  TextDecoration decoration = TextDecoration::None;

  static TextStyle Default();

  bool operator==(const TextStyle&) const = default;
};

enum class TextDirection {
  Auto,
  LeftToRight,
  RightToLeft,
};

struct TextShapingOptions {
  TextDirection direction = TextDirection::Auto;
  std::string locale;

  bool operator==(const TextShapingOptions&) const = default;
};

enum class TextAlign {
  Leading,
  Center,
  Trailing,
};

enum class TextWrap {
  NoWrap,
  Word,
};

struct TextLayoutOptions {
  TextShapingOptions shaping;
  TextAlign align = TextAlign::Leading;
  TextWrap wrap = TextWrap::Word;

  bool operator==(const TextLayoutOptions&) const = default;
};

struct FontMetrics {
  float ascent = 0.0F;
  float descent = 0.0F;
  float leading = 0.0F;
  float underline_position = 0.0F;
  float underline_thickness = 0.0F;
  float strike_through_position = 0.0F;
  float strike_through_thickness = 0.0F;

  [[nodiscard]] float LineHeight() const noexcept {
    return ascent + descent + leading;
  }

  bool operator==(const FontMetrics&) const = default;
};

struct TextRunMetrics {
  float advance = 0.0F;
  // This conservative painted bound uses a baseline origin at (0, 0); ascent extends into negative y coordinates.
  Rect visual_bounds;
  FontMetrics font_metrics;

  bool operator==(const TextRunMetrics&) const = default;
};

struct TextLayoutMetrics {
  Size size;
  float first_baseline = 0.0F;
  float last_baseline = 0.0F;
  std::size_t line_count = 0;

  bool operator==(const TextLayoutMetrics&) const = default;
};

class TextMeasurer {
public:
  virtual ~TextMeasurer() = default;

  [[nodiscard]] virtual FontMetrics Metrics(const Font& font) = 0;
  [[nodiscard]] virtual TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) = 0;
  [[nodiscard]] virtual TextLayoutMetrics MeasureText(
      std::string_view text,
      const TextStyle& style,
      float max_width = std::numeric_limits<float>::infinity(),
      const TextLayoutOptions& options = {}
  ) = 0;
};

TextMeasurer& UseTextMeasurer();

} // namespace huxerui
