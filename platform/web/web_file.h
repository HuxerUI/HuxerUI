#pragma once

#include <memory>

namespace huxerui {
class FileSystem;
}

namespace huxerui::detail {

[[nodiscard]] std::shared_ptr<FileSystem> CreateWebFileSystem();

} // namespace huxerui::detail
