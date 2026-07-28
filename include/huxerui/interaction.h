#pragma once

#include <optional>
#include <variant>

#include <huxerui/modifier.h>

namespace huxerui {

namespace detail {

struct DefaultIndication {
  static const ModifierDescriptor &Descriptor();
};

bool IsDefaultIndicationDescriptor(
    const ModifierDescriptor *descriptor) noexcept;
bool IsExplicitIndicationDescriptor(
    const ModifierDescriptor *descriptor) noexcept;

} // namespace detail

struct NoIndication {};

struct StateOverlayIndication {
  Color color = Color::Rgb(0, 0, 0, 0.12F);
  double fade_in_duration = 0.08;
  double fade_out_duration = 0.16;
  Color hover_color = Color::Rgb(0, 0, 0, 0.06F);
};

struct RippleIndication {
  Color color = Color::Rgb(255, 255, 255, 0.28F);
  double expansion_duration = 0.32;
  double fade_out_duration = 0.2;
  Color hover_color = Color::Transparent();
  double hover_fade_in_duration = 0.08;
  double hover_fade_out_duration = 0.16;
};

using IndicationSpec = std::variant<
    NoIndication,
    StateOverlayIndication,
    RippleIndication>;

struct Indication {
  Indication() = default;
  explicit Indication(IndicationSpec value)
      : value(std::move(value)) {}

  static const detail::ModifierDescriptor &Descriptor();

  std::optional<IndicationSpec> value;
};

} // namespace huxerui
