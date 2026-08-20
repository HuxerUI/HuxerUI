#include "win32_file_internal.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "file_internal.h"

namespace huxerui::detail {

namespace {

namespace fs = std::filesystem;

std::string PublicPath(const fs::path& path) {
  const std::u8string value = path.lexically_normal().generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::wstring ResolveExecutablePath() {
  std::wstring path(260, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
      throw std::runtime_error("HuxerUI Windows file system could not resolve the executable path");
    }
    if (length + 1 < path.size()) {
      path.resize(length);
      return path;
    }
    if (path.size() >= 32768) {
      throw std::runtime_error("HuxerUI Windows executable path exceeds the supported length");
    }
    path.resize(std::min<std::size_t>(path.size() * 2, 32768));
  }
}

std::wstring ResolveLocalAppData() {
  PWSTR value = nullptr;
  const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value);
  if (FAILED(result) || value == nullptr) {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
    throw std::runtime_error("HuxerUI Windows file system could not resolve Local App Data");
  }
  std::wstring path(value);
  CoTaskMemFree(value);
  return path;
}

} // namespace

FileSystemPaths ResolveWin32FileSystemPaths(std::wstring_view executable_path, std::wstring_view local_app_data) {
  if (executable_path.empty() || local_app_data.empty()) {
    throw std::runtime_error("HuxerUI Windows file system paths must not be empty");
  }

  const fs::path executable(executable_path);
  const fs::path executable_directory = executable.parent_path();
  const fs::path identity = executable.stem();
  const fs::path local_root(local_app_data);
  if (!executable.is_absolute() || executable_directory.empty() || identity.empty() || !local_root.is_absolute()) {
    throw std::runtime_error("HuxerUI Windows file system paths must be absolute and identify an application");
  }

  const fs::path application_root = local_root / identity;
  return {
      .executable_directory = PublicPath(executable_directory),
      .data_directory = PublicPath(application_root / L"data"),
      .cache_directory = PublicPath(application_root / L"cache"),
      .temporary_directory = PublicPath(application_root / L"temporary"),
  };
}

std::shared_ptr<FileSystem> CreateWin32FileSystem(std::wstring_view executable_path, std::wstring_view local_app_data) {
  return MakeFileSystem(ResolveWin32FileSystemPaths(executable_path, local_app_data));
}

std::shared_ptr<FileSystem> CreateWin32FileSystem() {
  return CreateWin32FileSystem(ResolveExecutablePath(), ResolveLocalAppData());
}

} // namespace huxerui::detail
