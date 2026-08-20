#pragma once

#include <memory>

namespace huxerui {
class FileSystem;
}

namespace huxerui::detail {

[[nodiscard]] std::shared_ptr<FileSystem> CreateIosFileSystem();

} // namespace huxerui::detail
