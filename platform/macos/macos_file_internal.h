#pragma once

#include <functional>
#include <memory>

#ifdef __OBJC__
@class NSWindow;
@class NSURL;
#endif

namespace huxerui {
class FileReference;
class FileSystem;
} // namespace huxerui

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] std::shared_ptr<FileSystem> CreateMacFileSystem();

#ifdef __OBJC__
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateMacFilePickerTransport(std::function<NSWindow*()> window_provider);
[[nodiscard]] FileReference MakeMacFileReference(NSURL* url, bool writable = true);
#endif

} // namespace huxerui::detail
