#pragma once

#include <memory>

#include <huxerui/file_drop.h>

namespace huxerui {
class FileSystem;
}

namespace emscripten {
class val;
}

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] FileDropOffer ReadWebFileDropOffer(const emscripten::val& transfer);
[[nodiscard]] FileDropPreparation CaptureWebFileDrop(const emscripten::val& transfer);

[[nodiscard]] std::shared_ptr<FileSystem> CreateWebFileSystem();
[[nodiscard]] std::shared_ptr<FilePickerTransport> CreateWebFilePickerTransport();

} // namespace huxerui::detail
