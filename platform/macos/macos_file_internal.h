#pragma once

#include <functional>
#include <memory>

#include <huxerui/file_drop.h>

#ifdef __OBJC__
@class NSWindow;
@class NSURL;
@class NSPasteboard;
#endif

namespace huxerui {
class FileReference;
} // namespace huxerui

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] AppDirectories CreateMacAppDirectories();

#ifdef __OBJC__
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateMacFilePickerTransport(std::function<NSWindow*()> window_provider);
[[nodiscard]] FileDropPreparation CaptureMacFileDrop(NSPasteboard* pasteboard);
[[nodiscard]] FileReference MakeMacFileReference(NSURL* url, bool writable = true);
#endif

} // namespace huxerui::detail
