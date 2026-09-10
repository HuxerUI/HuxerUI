#pragma once

#include <sys/types.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <huxerui/platform_adapter.h>

typedef struct _GtkWidget GtkWidget;

namespace huxerui {

class Runtime;

namespace detail {

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

struct LinuxAppDirectoryEnvironment {
  std::optional<std::string> home_directory;
  std::optional<std::string> passwd_home_directory;
  std::optional<std::string> data_home;
  std::optional<std::string> cache_home;
  std::optional<std::string> runtime_directory;
  std::string fallback_temporary_root;
  uid_t effective_user_id = 0;
};

[[nodiscard]] std::string ResolveLinuxExecutablePath();
[[nodiscard]] AppDirectories
ResolveLinuxAppDirectories(std::string_view executable_path, const LinuxAppDirectoryEnvironment& environment);
[[nodiscard]] AppDirectories
CreateLinuxAppDirectories(std::string_view executable_path, LinuxAppDirectoryEnvironment environment);
[[nodiscard]] AppDirectories CreateLinuxAppDirectories();

} // namespace detail
} // namespace huxerui
