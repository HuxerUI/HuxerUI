#pragma once

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <huxerui/platform_adapter.h>

typedef struct _GtkWidget GtkWidget;

namespace huxerui {

class FileSystem;
class Runtime;

namespace detail {

struct FileSystemPaths;

class LinuxFileDrop final {
public:
  LinuxFileDrop(GtkWidget* widget, Runtime& runtime, UIThreadDispatcher dispatcher);
  ~LinuxFileDrop();
  LinuxFileDrop(const LinuxFileDrop&) = delete;
  LinuxFileDrop& operator=(const LinuxFileDrop&) = delete;

private:
  struct State;
  std::unique_ptr<State> state_;
};

struct LinuxFileSystemEnvironment {
  std::optional<std::string> home_directory;
  std::optional<std::string> passwd_home_directory;
  std::optional<std::string> data_home;
  std::optional<std::string> cache_home;
  std::optional<std::string> runtime_directory;
  std::string fallback_temporary_root;
  uid_t effective_user_id = 0;
};

[[nodiscard]] std::string ResolveLinuxExecutablePath();
[[nodiscard]] FileSystemPaths
ResolveLinuxFileSystemPaths(std::string_view executable_path, const LinuxFileSystemEnvironment& environment);
[[nodiscard]] std::shared_ptr<FileSystem>
CreateLinuxFileSystem(std::string_view executable_path, LinuxFileSystemEnvironment environment);
[[nodiscard]] std::shared_ptr<FileSystem> CreateLinuxFileSystem();

} // namespace detail
} // namespace huxerui
