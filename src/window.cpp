#include <huxerui/window.h>

#include <cmath>
#include <stdexcept>

#include <huxerui/modifier.h>

#include "internal.h"

namespace huxerui {

namespace detail {

bool IsValidSystemBarsAppearance(const SystemBarsAppearance& appearance) noexcept {
  const auto finite = [](const Color& color) {
    return std::isfinite(color.red) && std::isfinite(color.green) && std::isfinite(color.blue) &&
           std::isfinite(color.alpha);
  };
  const auto valid_brightness = [](SystemBarContentBrightness brightness) {
    return brightness == SystemBarContentBrightness::Automatic || brightness == SystemBarContentBrightness::Light ||
           brightness == SystemBarContentBrightness::Dark;
  };
  return finite(appearance.status_bar_background) && finite(appearance.navigation_bar_background) &&
         valid_brightness(appearance.status_bar_content) && valid_brightness(appearance.navigation_bar_content);
}

} // namespace detail

namespace {

void ApplySystemBarsAppearance(detail::ViewSpec& spec, const SystemBarsAppearance& appearance) {
  if (!detail::IsValidSystemBarsAppearance(appearance)) {
    throw std::invalid_argument("HuxerUI system bars appearance is invalid");
  }
  spec.properties.system_bars_appearance = appearance;
}

void ApplySafeAreaPadding(detail::ViewSpec& spec, const SafeAreaPadding& padding) {
  spec.properties.safe_area_padding = padding;
}

template <class Modifier, void (*Apply)(detail::ViewSpec&, const Modifier&)>
const detail::ModifierDescriptor& ApplyOnlyModifierDescriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, const void* value) { Apply(spec, *static_cast<const Modifier*>(value)); },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<Modifier>(),
      nullptr,
  };
  return descriptor;
}

} // namespace

SystemBarsAppearance SystemBarsAppearance::Default() {
  return {};
}

const detail::ModifierDescriptor& SystemBarsAppearance::Descriptor() {
  return ApplyOnlyModifierDescriptor<SystemBarsAppearance, ApplySystemBarsAppearance>();
}

const detail::ModifierDescriptor& SafeAreaPadding::Descriptor() {
  return ApplyOnlyModifierDescriptor<SafeAreaPadding, ApplySafeAreaPadding>();
}

} // namespace huxerui
