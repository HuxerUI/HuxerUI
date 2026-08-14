#include "sdk.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "process_runner.h"

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

bool IsSdkHome(const std::filesystem::path& path) {
  const bool has_headers = std::filesystem::is_regular_file(path / "include/huxerui/huxerui.h");
  const bool source = std::filesystem::is_regular_file(path / "cmake/HuxerUIApp.cmake") &&
                      std::filesystem::is_directory(path / "tools/prebuilt") &&
                      std::filesystem::is_directory(path / "resources");
  const bool installed = !InstalledCMakeDirectory(path).empty() &&
                         std::filesystem::is_directory(path / "share/huxerui/tools") &&
                         std::filesystem::is_regular_file(path / "share/huxerui/resources/huxerui/resources.bin");
  return has_headers && (source || installed);
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
    if (environment->empty()) {
      throw std::runtime_error("HUXERUI_HOME is empty");
    }
    const std::filesystem::path home = Normalize(*environment);
    if (!IsSdkHome(home)) {
      throw std::runtime_error("HUXERUI_HOME is not a HuxerUI SDK or source checkout: " + home.string());
    }
    return {home, SdkLocationSource::Environment};
  }

  const std::filesystem::path executable_directory = executable_path.parent_path();
  const std::array candidates{
      executable_directory.parent_path(),
      executable_directory.parent_path().parent_path(),
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsSdkHome(candidate)) {
      return {Normalize(candidate), SdkLocationSource::Executable};
    }
  }
  return {};
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
