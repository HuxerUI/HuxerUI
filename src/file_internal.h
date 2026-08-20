#pragma once

#include <memory>
#include <optional>
#include <string>

#include <huxerui/file.h>

namespace huxerui::detail {

struct FileSystemPaths {
  std::optional<std::string> executable_directory;
  std::string data_directory;
  std::string cache_directory;
  std::string temporary_directory;
};

[[nodiscard]] std::shared_ptr<FileSystem> MakeFileSystem(FileSystemPaths paths);

} // namespace huxerui::detail
