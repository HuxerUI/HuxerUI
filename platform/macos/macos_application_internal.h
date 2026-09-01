#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/gesture.h>

#ifdef __OBJC__
@class NSArray;
#endif

namespace huxerui::detail {

class PermissionTransport;

[[nodiscard]] GestureSettings MacGestureDefaults() noexcept;
[[nodiscard]] std::shared_ptr<PermissionTransport> CreateMacPermissionTransport();

#ifdef __OBJC__
[[nodiscard]] std::optional<std::vector<ApplicationActivation>> DecodeMacApplicationActivations(NSArray* urls);
#endif

} // namespace huxerui::detail
