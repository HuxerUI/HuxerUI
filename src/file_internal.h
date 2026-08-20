#pragma once

#include <memory>
#include <optional>
#include <string>
#if defined(__EMSCRIPTEN__)
#include <functional>
#include <string_view>
#endif

#include <huxerui/file.h>

namespace huxerui::detail {

struct FileSystemPaths {
  std::optional<std::string> executable_directory;
  std::string data_directory;
  std::string cache_directory;
  std::string temporary_directory;
};

[[nodiscard]] std::shared_ptr<FileSystem> MakeFileSystem(FileSystemPaths paths);

#if defined(__EMSCRIPTEN__)
[[nodiscard]] bool IsWebPersistentFilePath(std::string_view path) noexcept;
void EnqueueWebFileOperation(std::function<void(std::function<void()>)> operation);
void PersistWebFileSystem(std::function<void(bool)> completion);
#endif

} // namespace huxerui::detail
