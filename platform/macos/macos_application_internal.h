#pragma once

#include <optional>
#include <vector>

#include <huxerui/app.h>

#ifdef __OBJC__
@class NSArray;
#endif

namespace huxerui::detail {

#ifdef __OBJC__
[[nodiscard]] std::optional<std::vector<ApplicationActivation>> DecodeMacApplicationActivations(NSArray* urls);
#endif

} // namespace huxerui::detail
