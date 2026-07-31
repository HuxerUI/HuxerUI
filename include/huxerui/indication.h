#pragma once

#include <optional>
#include <utility>
#include <variant>

#include <huxerui/modifier.h>

namespace huxerui {

struct NoIndication {
  bool operator==(const NoIndication&) const = default;
};

struct StateOverlayIndication {
  Color color = Color::Rgb(0, 0, 0, 0.12F);
  double fade_in_duration = 0.08;
  double fade_out_duration = 0.16;
  Color hover_color = Color::Rgb(0, 0, 0, 0.06F);

  bool operator==(const StateOverlayIndication&) const = default;
};

struct RippleIndication {
  Color color = Color::Rgb(255, 255, 255, 0.28F);
  double expansion_duration = 0.32;
  double fade_out_duration = 0.2;
  Color hover_color = Color::Transparent();
  double hover_fade_in_duration = 0.08;
  double hover_fade_out_duration = 0.16;

  bool operator==(const RippleIndication&) const = default;
};

using IndicationSpec = std::variant<NoIndication, StateOverlayIndication, RippleIndication>;

struct Indication {
  Indication() = default;
  explicit Indication(IndicationSpec value) : value(std::move(value)) {}

  static const detail::ModifierDescriptor& Descriptor();

  std::optional<IndicationSpec> value;

  bool operator==(const Indication&) const = default;
};

} // namespace huxerui
