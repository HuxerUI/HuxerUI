#pragma once

#include <array>
#include <string_view>

#include <huxerui/animation.h>

namespace transition_studio {

enum class Effect {
  Prism,
  SplitGate,
  Mosaic,
  Iris,
  IrisClose,
  Diagonal,
  Shutters,
  Orbit,
  Curtain,
  Tear,
};

struct EffectInfo {
  Effect effect;
  std::string_view name;
  std::string_view category;
  std::string_view description;
};

inline constexpr std::array effects{
    EffectInfo{Effect::Prism, "Prism sweep", "STAGGERED BANDS", "Five diagonal ribbons sweep in with luminous edges."},
    EffectInfo{Effect::SplitGate, "Split gate", "TWO PANELS", "Two panels turn outward as the next world settles behind."},
    EffectInfo{Effect::Mosaic, "Cascade mosaic", "STAGGERED TILES", "Twelve tiles rotate and assemble in a diagonal wave."},
    EffectInfo{Effect::Iris, "Circular reveal", "CLIP", "A new world opens from the center, or from your touch."},
    EffectInfo{Effect::IrisClose, "Circular close", "REVERSED CLIP",
               "The departing scene contracts to a circle and disappears."},
    EffectInfo{Effect::Diagonal, "Diagonal sweep", "CUSTOM PATH", "A sharp diagonal cuts across the entire composition."},
    EffectInfo{Effect::Shutters, "Venetian blinds", "CUSTOM PATH", "Twelve horizontal apertures open in unison."},
    EffectInfo{Effect::Orbit, "Orbit & settle", "CUSTOM TRANSFORM", "Rotation, scale and opacity land on one shared beat."},
    EffectInfo{Effect::Curtain, "Curtain lift", "OUTGOING ABOVE", "The departing scene lifts away to reveal the next."},
    EffectInfo{Effect::Tear, "Crimson rift", "FRAGMENTS + PAINT",
               "A glowing jagged seam tears the old page apart, scattering a few embers."},
};

const EffectInfo& Describe(Effect effect);
huxerui::TransitionSpec MakeTransition(Effect effect, bool slow);

} // namespace transition_studio
