#pragma once

#include <functional>
#include <iosfwd>

#include "platform.h"

namespace huxerui::cli {

/// Rebuilds and relaunches one selected application until interrupted.
int WatchApplication(const PlatformDriver& platform, const PlatformCommandContext& context,
                     const std::function<void()>& build, std::ostream& output, std::ostream& error);

} // namespace huxerui::cli
