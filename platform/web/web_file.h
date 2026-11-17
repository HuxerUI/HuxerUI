#pragma once

#include <memory>

#include <huxerui/file_drop.h>

namespace emscripten {
class val;
}

namespace huxerui::detail {

class FileAccessStore;
class FilePickerTransport;

[[nodiscard]] FileDropOffer ReadWebFileDropOffer(const emscripten::val& transfer);
[[nodiscard]] FileDropPreparation CaptureWebFileDrop(const emscripten::val& transfer);

[[nodiscard]] AppDirectories CreateWebAppDirectories();
std::shared_ptr<FileAccessStore> CreateWebFileAccessStore(const AppDirectories& directories);
[[nodiscard]] std::shared_ptr<FilePickerTransport> CreateWebFilePickerTransport();

} // namespace huxerui::detail
