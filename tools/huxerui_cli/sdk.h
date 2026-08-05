#pragma once

#include <filesystem>
#include <string_view>

namespace huxerui::cli {

[[nodiscard]] std::filesystem::path ExecutablePath(std::string_view argument_zero);
[[nodiscard]] std::filesystem::path LocateSdkRoot(const std::filesystem::path& executable_path);
[[nodiscard]] std::filesystem::path LocateSdkCMakeFile(const std::filesystem::path& sdk_root, std::string_view name);

} // namespace huxerui::cli
