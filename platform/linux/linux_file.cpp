#include "linux_internal.h"

#include "linux_file_internal.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "io/file_internal.h"

namespace huxerui::detail {

namespace {

namespace fs = std::filesystem;

std::optional<std::string> EnvironmentVariable(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

std::string Utf8Path(const fs::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::optional<std::string> ValidAbsolutePath(const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty() || value->front() != '/') {
    return std::nullopt;
  }
  try {
    return File(*value).Path();
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

std::optional<std::string> PasswdHomeDirectory(uid_t user_id) {
  long initial_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (initial_size <= 0) {
    initial_size = 16 * 1024;
  }
  std::vector<char> buffer(static_cast<std::size_t>(initial_size));
  for (;;) {
    passwd entry{};
    passwd* result = nullptr;
    const int error = getpwuid_r(user_id, &entry, buffer.data(), buffer.size(), &result);
    if (error == 0) {
      if (result == nullptr || result->pw_dir == nullptr) {
        return std::nullopt;
      }
      return std::string(result->pw_dir);
    }
    if (error != ERANGE || buffer.size() > std::numeric_limits<std::size_t>::max() / 2U) {
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2U);
  }
}

bool IsPrivateDirectory(std::string_view path, uid_t user_id) noexcept {
  struct stat status{};
  const std::string value(path);
  if (lstat(value.c_str(), &status) != 0) {
    return false;
  }
  return S_ISDIR(status.st_mode) && status.st_uid == user_id && (status.st_mode & 0777) == 0700;
}

void EnsurePrivateDirectory(std::string_view path, uid_t user_id) {
  const std::string value(path);
  bool created = false;
  if (mkdir(value.c_str(), 0700) == 0) {
    created = true;
  } else if (errno != EEXIST) {
    throw std::runtime_error("HuxerUI Linux file system could not create a private temporary directory");
  }
  if (created && chmod(value.c_str(), 0700) != 0) {
    throw std::runtime_error("HuxerUI Linux file system could not secure a private temporary directory");
  }
  if (!IsPrivateDirectory(path, user_id)) {
    throw std::runtime_error("HuxerUI Linux file system temporary directory is not private");
  }
}

std::string RequiredHomeDirectory(const LinuxFileSystemEnvironment& environment) {
  if (std::optional<std::string> home = ValidAbsolutePath(environment.home_directory)) {
    return std::move(*home);
  }
  if (std::optional<std::string> home = ValidAbsolutePath(environment.passwd_home_directory)) {
    return std::move(*home);
  }
  throw std::runtime_error("HuxerUI Linux home directory could not be resolved");
}

std::string ApplicationChild(std::string_view base, std::string_view identity) {
  return File(base).Child(identity).Path();
}

} // namespace

std::string ResolveLinuxExecutablePath() {
  std::vector<char> buffer(256);
  for (;;) {
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      throw std::runtime_error("HuxerUI Linux executable path could not be resolved");
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      std::string path(buffer.data(), static_cast<std::size_t>(length));
      try {
        File file(path);
        if (!file.Parent().has_value() || file.Name().empty()) {
          throw std::runtime_error("HuxerUI Linux executable path is invalid");
        }
        return file.Path();
      } catch (const std::invalid_argument&) {
        throw std::runtime_error("HuxerUI Linux executable path is not valid UTF-8");
      }
    }
    if (buffer.size() > std::numeric_limits<std::size_t>::max() / 2U) {
      throw std::runtime_error("HuxerUI Linux executable path is too long");
    }
    buffer.resize(buffer.size() * 2U);
  }
}

FileSystemPaths
ResolveLinuxFileSystemPaths(std::string_view executable_path, const LinuxFileSystemEnvironment& environment) {
  if (executable_path.empty() || executable_path.front() != '/') {
    throw std::runtime_error("HuxerUI Linux executable path is invalid");
  }
  File executable = [&] {
    try {
      return File(executable_path);
    } catch (const std::invalid_argument&) {
      throw std::runtime_error("HuxerUI Linux executable path is not valid UTF-8");
    }
  }();
  const std::optional<File> executable_directory = executable.Parent();
  const std::string identity = executable.Name();
  if (!executable_directory.has_value() || identity.empty()) {
    throw std::runtime_error("HuxerUI Linux executable path is invalid");
  }

  std::optional<std::string> data_base = ValidAbsolutePath(environment.data_home);
  std::optional<std::string> cache_base = ValidAbsolutePath(environment.cache_home);
  if (!data_base.has_value() || !cache_base.has_value()) {
    const std::string home = RequiredHomeDirectory(environment);
    if (!data_base.has_value()) {
      data_base = File(home).Resolve(".local/share").Path();
    }
    if (!cache_base.has_value()) {
      cache_base = File(home).Child(".cache").Path();
    }
  }

  std::string temporary_base;
  if (std::optional<std::string> runtime = ValidAbsolutePath(environment.runtime_directory);
      runtime.has_value() && IsPrivateDirectory(*runtime, environment.effective_user_id)) {
    temporary_base = std::move(*runtime);
  } else {
    const std::optional<std::string> fallback = ValidAbsolutePath(environment.fallback_temporary_root);
    if (!fallback.has_value()) {
      throw std::runtime_error("HuxerUI Linux temporary directory could not be resolved");
    }
    temporary_base = File(*fallback).Child("huxerui-" + std::to_string(environment.effective_user_id)).Path();
  }

  return {
      .executable_directory = executable_directory->Path(),
      .data_directory = ApplicationChild(*data_base, identity),
      .cache_directory = ApplicationChild(*cache_base, identity),
      .temporary_directory = ApplicationChild(temporary_base, identity),
  };
}

std::shared_ptr<FileSystem>
CreateLinuxFileSystem(std::string_view executable_path, LinuxFileSystemEnvironment environment) {
  FileSystemPaths paths = ResolveLinuxFileSystemPaths(executable_path, environment);

  const std::optional<std::string> runtime = ValidAbsolutePath(environment.runtime_directory);
  const bool uses_runtime_directory =
      runtime.has_value() && IsPrivateDirectory(*runtime, environment.effective_user_id);
  if (!uses_runtime_directory) {
    const std::optional<File> application_temporary_parent = File(paths.temporary_directory).Parent();
    if (!application_temporary_parent.has_value()) {
      throw std::runtime_error("HuxerUI Linux temporary directory is invalid");
    }
    EnsurePrivateDirectory(application_temporary_parent->Path(), environment.effective_user_id);
  }
  EnsurePrivateDirectory(paths.temporary_directory, environment.effective_user_id);
  return MakeFileSystem(std::move(paths));
}

std::shared_ptr<FileSystem> CreateLinuxFileSystem() {
  const uid_t user_id = geteuid();
  std::error_code temporary_error;
  const fs::path temporary_root = fs::temp_directory_path(temporary_error);
  if (temporary_error) {
    throw std::runtime_error("HuxerUI Linux temporary directory could not be resolved");
  }

  return CreateLinuxFileSystem(
      ResolveLinuxExecutablePath(),
      {
          .home_directory = EnvironmentVariable("HOME"),
          .passwd_home_directory = PasswdHomeDirectory(user_id),
          .data_home = EnvironmentVariable("XDG_DATA_HOME"),
          .cache_home = EnvironmentVariable("XDG_CACHE_HOME"),
          .runtime_directory = EnvironmentVariable("XDG_RUNTIME_DIR"),
          .fallback_temporary_root = Utf8Path(temporary_root),
          .effective_user_id = user_id,
      }
  );
}

} // namespace huxerui::detail
