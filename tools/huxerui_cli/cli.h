#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>

namespace huxerui::cli {

int Run(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& sdk_root,
    std::ostream& output,
    std::ostream& error
);

} // namespace huxerui::cli
