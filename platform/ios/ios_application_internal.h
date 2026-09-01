#pragma once

#include <memory>
#include <optional>

#include <huxerui/app.h>

#ifdef __OBJC__
@class NSURL;
#endif

namespace huxerui::detail {

class PermissionTransport;

[[nodiscard]] std::shared_ptr<PermissionTransport> CreateIosPermissionTransport();

#ifdef __OBJC__
[[nodiscard]] std::optional<ApplicationActivation>
DecodeIosApplicationActivation(NSURL* url, bool copy_file_before_use);
#endif

} // namespace huxerui::detail
