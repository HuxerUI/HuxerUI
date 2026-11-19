#pragma once

#include <huxerui/huxerui.h>

#include <gauge/version.h>

namespace gauge {

// A value the interface offers is a constant rather than a macro, so the
// generated interface exports it like any other name.
inline constexpr int max_level = 10;

[[nodiscard]] constexpr int Clamp(int level) noexcept {
  return level < 0 ? 0 : (level > max_level ? max_level : level);
}

huxerui::View Meter(int level);

} // namespace gauge
