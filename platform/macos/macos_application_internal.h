#pragma once

#include <optional>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/gesture.h>

#ifdef __OBJC__
@class NSArray;
#endif

namespace huxerui::detail {

[[nodiscard]] GestureSettings MacGestureDefaults() noexcept;

#ifdef __OBJC__
[[nodiscard]] std::optional<std::vector<ApplicationActivation>> DecodeMacApplicationActivations(NSArray* urls);
#endif

} // namespace huxerui::detail
