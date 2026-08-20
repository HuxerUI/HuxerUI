#pragma once

#include <memory>

namespace huxerui {
class FileSystem;
}

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] std::shared_ptr<FileSystem> CreateWebFileSystem();
[[nodiscard]] std::shared_ptr<FilePickerTransport> CreateWebFilePickerTransport();

} // namespace huxerui::detail
