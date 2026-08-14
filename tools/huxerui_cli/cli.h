#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>

#include "sdk.h"

namespace huxerui::cli {

int Run(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& working_directory,
    const SdkLocation& sdk,
    std::ostream& output,
    std::ostream& error
);

} // namespace huxerui::cli
