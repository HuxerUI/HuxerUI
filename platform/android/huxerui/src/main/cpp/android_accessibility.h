#pragma once

#include <cstdint>
#include <vector>

#include <huxerui/semantics.h>

namespace huxerui::detail {

inline constexpr std::uint32_t android_semantics_magic = 0x4D535848U;
inline constexpr std::uint32_t android_semantics_version = 1U;

enum class AndroidSemanticAction : std::int32_t {
  Activate = 0,
  Focus = 1,
  SetText = 2,
  SetSelection = 3,
  SetValue = 4,
  Increment = 5,
  Decrement = 6,
  Scroll = 7,
  ShowOnScreen = 8,
  Expand = 9,
  Collapse = 10,
  Dismiss = 11,
  Custom = 12,
};

// Android reserves negative virtual view IDs and exposes only 32-bit IDs. Runtime semantic identities are encoded
// directly after an exact range check so stale actions can be validated without a second identity registry.
[[nodiscard]] std::vector<std::uint8_t> EncodeAndroidSemanticFrame(const SemanticFrame& frame);

} // namespace huxerui::detail
