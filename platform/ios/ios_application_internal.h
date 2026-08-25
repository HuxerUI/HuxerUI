#pragma once

#include <optional>

#include <huxerui/app.h>

#ifdef __OBJC__
@class NSURL;
#endif

namespace huxerui::detail {

#ifdef __OBJC__
[[nodiscard]] std::optional<ApplicationActivation>
DecodeIosApplicationActivation(NSURL* url, bool copy_file_before_use);
#endif

} // namespace huxerui::detail
