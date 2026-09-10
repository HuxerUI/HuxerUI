#include "sdk.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include "process_runner.h"

#include <cmrc/cmrc.hpp>

CMRC_DECLARE(huxerui_cli_installers);

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace huxerui::cli {
namespace {

std::filesystem::path InstalledCMakeDirectory(const std::filesystem::path& root) {
  for (const std::string_view directory :
       {std::string_view{"lib"}, std::string_view{"lib64"}, std::string_view{"share"}}) {
    const std::filesystem::path candidate = root / directory / "cmake/HuxerUI";
    if (std::filesystem::is_regular_file(candidate / "HuxerUIConfig.cmake")) {
      return candidate;
    }
  }

  const std::filesystem::path library_root = root / "lib";
  std::error_code error;
  std::filesystem::directory_iterator entries(library_root, error);
  for (const std::filesystem::directory_entry& entry : entries) {
    const std::filesystem::path candidate = entry.path() / "cmake/HuxerUI";
    if (entry.is_directory() && std::filesystem::is_regular_file(candidate / "HuxerUIConfig.cmake")) {
      return candidate;
    }
  }
  return {};
}

enum class SdkLayout {
  Source,
  Installed,
};

std::filesystem::path ApplicationDevelopmentSkillDirectory(const std::filesystem::path& huxerui_home,
                                                           SdkLayout layout) {
  if (layout == SdkLayout::Installed) {
    return huxerui_home / "share/huxerui/skills/huxerui-app-development";
  }
  return huxerui_home / "skills/huxerui-app-development";
}

bool IsSourceHome(const std::filesystem::path& path) {
  return std::filesystem::is_regular_file(path / "CMakeLists.txt") &&
         std::filesystem::is_regular_file(path / "include/huxerui/huxerui.h") &&
         std::filesystem::is_regular_file(path / "cmake/HuxerUIApp.cmake") &&
         std::filesystem::is_directory(path / "tools/prebuilt") &&
         std::filesystem::is_directory(path / "resources") &&
         std::filesystem::is_directory(ApplicationDevelopmentSkillDirectory(path, SdkLayout::Source));
}

bool IsInstalledHome(const std::filesystem::path& path) {
  return std::filesystem::is_regular_file(path / "include/huxerui/huxerui.h") &&
         !InstalledCMakeDirectory(path).empty() && std::filesystem::is_directory(path / "share/huxerui/tools") &&
         std::filesystem::is_regular_file(path / "share/huxerui/resources/huxerui/resources.bin") &&
         std::filesystem::is_directory(ApplicationDevelopmentSkillDirectory(path, SdkLayout::Installed));
}

std::filesystem::path Normalize(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
  return error ? std::filesystem::absolute(path) : normalized;
}

} // namespace

std::filesystem::path ExecutablePath(std::string_view argument_zero) {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0 && length < buffer.size()) {
    buffer.resize(length);
    return Normalize(std::filesystem::path(buffer));
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    return Normalize(std::filesystem::path(buffer.c_str()));
  }
#elif defined(__linux__)
  std::array<char, 4096> buffer{};
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length > 0 && static_cast<std::size_t>(length) < buffer.size()) {
    return Normalize(std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length))));
  }
#endif
  return Normalize(std::filesystem::path(std::string(argument_zero)));
}

SdkLocation LocateHuxerUIHome(const std::filesystem::path& executable_path) {
  if (const std::optional<std::string> environment = ReadEnvironmentVariable("HUXERUI_HOME")) {
    return {environment->empty() ? std::filesystem::path{} : Normalize(*environment), SdkLocationSource::Environment};
  }

  const std::filesystem::path executable_directory = executable_path.parent_path();
  const std::array candidates{
      executable_directory.parent_path(),
      executable_directory.parent_path().parent_path(),
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsInstalledHome(candidate)) {
      return {Normalize(candidate), SdkLocationSource::Executable};
    }
  }
  return {};
}

std::filesystem::path ResolveHuxerUISource(const std::filesystem::path& path) {
  const std::filesystem::path source = Normalize(path);
  if (!IsSourceHome(source)) {
    throw std::runtime_error("HuxerUI source checkout is invalid: " + source.string());
  }
  return source;
}

std::string BuildHomeKey(const std::filesystem::path& home) {
  const std::string location = Normalize(home).generic_string();
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char character : location) {
    hash = (hash ^ character) * 1099511628211ULL;
  }
  std::ostringstream key;
  key << "home-" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return key.str();
}

std::filesystem::path ResolveBuildHome(const std::filesystem::path& sdk_home,
    const std::optional<std::filesystem::path>& source, const std::filesystem::path& working_directory) {
  if (source) {
    return ResolveHuxerUISource(source->is_absolute() ? *source : working_directory / *source);
  }
  const auto home = sdk_home.empty() ? std::filesystem::path{} : Normalize(sdk_home);
  if (home.empty() || !IsInstalledHome(home)) {
    throw std::runtime_error("HuxerUI build requires an installed SDK in HUXERUI_HOME or an explicit --source <path>");
  }
  return home;
}

void ValidateSdkUpdate(const SdkLocation& sdk, const std::filesystem::path& executable_path) {
  if (!sdk.home.empty() && IsSourceHome(sdk.home)) {
    throw std::runtime_error("HuxerUI source checkouts must be updated through the source workflow");
  }
  if (sdk.home.empty() || !IsInstalledHome(sdk.home)) {
    throw std::runtime_error("HuxerUI update requires an installed SDK");
  }
#if defined(_WIN32)
  constexpr std::string_view executable_name = "huxerui.exe";
#else
  constexpr std::string_view executable_name = "huxerui";
#endif
  std::error_code error;
  if (!std::filesystem::equivalent(executable_path, sdk.home / "bin" / executable_name, error) || error) {
    throw std::runtime_error("HuxerUI CLI and HUXERUI_HOME select different installations; use the selected SDK's "
                             "bin/huxerui or correct HUXERUI_HOME");
  }
}

int UpdateSdk(const SdkLocation& sdk, std::string_view target_version, bool check_only, bool assume_yes,
              std::ostream& output) {
  ValidateSdkUpdate(sdk, ExecutablePath({}));
  std::random_device random;
  std::filesystem::path directory;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const auto candidate = std::filesystem::temp_directory_path() / ("huxerui-update-" + std::to_string(random()));
    if (std::filesystem::create_directory(candidate)) {
      directory = candidate;
      break;
    }
  }
  if (directory.empty()) {
    throw std::runtime_error("HuxerUI cannot create an updater temporary directory");
  }
  bool handed_off = false;
  const auto cleanup = [&] {
    if (!handed_off) {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
  };
  try {
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
#if defined(_WIN32)
    constexpr std::string_view script_name = "install.ps1";
    ProcessCommand command{"powershell.exe", {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File"}, directory};
#else
    constexpr std::string_view script_name = "install.sh";
    ProcessCommand command{"sh", {}, directory};
#endif
    const auto resource = cmrc::huxerui_cli_installers::get_filesystem().open(std::string(script_name));
    const auto script = directory / script_name;
    std::ofstream stream(script, std::ios::binary);
    stream.write(resource.begin(), resource.end() - resource.begin());
    stream.close();
    if (!stream) {
      throw std::runtime_error("HuxerUI cannot write the temporary installer");
    }
    command.arguments.push_back(script.string());
#if defined(_WIN32)
    command.arguments.insert(command.arguments.end(), {"-Update", "-Prefix", sdk.home.string(), "-WaitForCli",
                                                       std::to_string(GetCurrentProcessId())});
    if (check_only) {
      command.arguments.push_back("-Check");
    }
    if (assume_yes) {
      command.arguments.push_back("-Yes");
    }
    if (!target_version.empty()) {
      command.arguments.insert(command.arguments.end(), {"-Version", std::string(target_version)});
    }
#else
    command.arguments.insert(command.arguments.end(), {"--update", "--prefix", sdk.home.string()});
    if (check_only) {
      command.arguments.push_back("--check");
    }
    if (assume_yes) {
      command.arguments.push_back("--yes");
    }
    if (!target_version.empty()) {
      command.arguments.insert(command.arguments.end(), {"--version", std::string(target_version)});
    }
#endif
    output.flush();
    const int result = RunProcess(command);
#if defined(_WIN32)
    if (result == 10) {
      handed_off = true;
      output << "HuxerUI update handed off; final result: " << (directory / "update.log").string() << '\n';
    }
#endif
    cleanup();
    return handed_off ? 0 : result;
  } catch (...) {
    cleanup();
    throw;
  }
}

bool IsMcppSourcePackage(const std::filesystem::path& huxerui_home) {
  return !huxerui_home.empty() && IsSourceHome(huxerui_home) &&
         std::filesystem::is_regular_file(huxerui_home / "mcpp.toml") &&
         std::filesystem::is_regular_file(huxerui_home / "build.mcpp");
}

std::filesystem::path ResolveApplicationDevelopmentSkill(const std::filesystem::path& huxerui_home) {
  if (huxerui_home.empty()) {
    for (auto directory = ExecutablePath({}).parent_path(); !directory.empty(); directory = directory.parent_path()) {
      const auto skill = ApplicationDevelopmentSkillDirectory(directory, SdkLayout::Source);
      if (std::filesystem::is_regular_file(skill / "SKILL.md")) {
        return skill;
      }
      if (directory == directory.root_path()) {
        break;
      }
    }
    throw std::runtime_error("HuxerUI application development skill is unavailable; install the SDK or use --agent none");
  }
  for (const SdkLayout layout : {SdkLayout::Installed, SdkLayout::Source}) {
    const std::filesystem::path skill = ApplicationDevelopmentSkillDirectory(huxerui_home, layout);
    if (std::filesystem::is_regular_file(skill / "SKILL.md")) {
      return skill;
    }
  }
  throw std::runtime_error("HuxerUI SDK application development skill is missing");
}

std::string_view SdkLocationSourceName(SdkLocationSource source) noexcept {
  switch (source) {
  case SdkLocationSource::Missing:
    return "missing";
  case SdkLocationSource::Environment:
    return "environment";
  case SdkLocationSource::Executable:
    return "executable";
  }
  return "missing";
}

} // namespace huxerui::cli
