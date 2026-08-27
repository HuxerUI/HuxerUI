#pragma once

#include <filesystem>
#include <string_view>

namespace huxerui::cli {

enum class SdkLocationSource {
  Missing,
  Environment,
  Executable,
};

struct SdkLocation {
  std::filesystem::path home;
  SdkLocationSource source = SdkLocationSource::Missing;
};

[[nodiscard]] std::filesystem::path ExecutablePath(std::string_view argument_zero);
[[nodiscard]] SdkLocation LocateHuxerUIHome(const std::filesystem::path& executable_path);
[[nodiscard]] std::filesystem::path ResolveApplicationDevelopmentSkill(const std::filesystem::path& huxerui_home);
[[nodiscard]] std::string_view SdkLocationSourceName(SdkLocationSource source) noexcept;

} // namespace huxerui::cli
