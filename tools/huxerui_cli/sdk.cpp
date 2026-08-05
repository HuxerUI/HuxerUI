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

bool IsSdkRoot(const std::filesystem::path& path) {
  const bool has_headers = std::filesystem::is_regular_file(path / "include/huxerui/huxerui.h");
  const bool source = std::filesystem::is_regular_file(path / "cmake/HuxerUIApp.cmake");
  const bool installed = !InstalledCMakeDirectory(path).empty();
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

std::filesystem::path LocateSdkRoot(const std::filesystem::path& executable_path) {
  if (const std::optional<std::string> environment = ReadEnvironmentVariable("HUXERUI_SDK_ROOT")) {
    const std::filesystem::path root = Normalize(*environment);
    if (!IsSdkRoot(root)) {
      throw std::runtime_error("HUXERUI_SDK_ROOT is not a HuxerUI SDK: " + root.string());
    }
    return root;
  }

  const std::filesystem::path executable_directory = executable_path.parent_path();
  const std::array candidates{
      executable_directory.parent_path(),
      executable_directory.parent_path().parent_path(),
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsSdkRoot(candidate)) {
      return Normalize(candidate);
    }
  }
  return {};
}

std::filesystem::path LocateSdkCMakeFile(const std::filesystem::path& sdk_root, std::string_view name) {
  const std::filesystem::path source_file = sdk_root / "cmake" / name;
  if (std::filesystem::is_regular_file(source_file)) {
    return source_file;
  }
  const std::filesystem::path installed_directory = InstalledCMakeDirectory(sdk_root);
  if (!installed_directory.empty()) {
    const std::filesystem::path installed_file = installed_directory / name;
    if (std::filesystem::is_regular_file(installed_file)) {
      return installed_file;
    }
  }
  throw std::runtime_error("HuxerUI SDK CMake file is missing: " + std::string(name));
}

} // namespace huxerui::cli
