#pragma once

#include <functional>
#include <memory>

#ifdef __OBJC__
@class NSURL;
@class UIViewController;
#endif

namespace huxerui {
class FileReference;
class FileSystem;
} // namespace huxerui

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] std::shared_ptr<FileSystem> CreateIosFileSystem();

#ifdef __OBJC__
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateIosFilePickerTransport(std::function<UIViewController*()> presenter_provider);
[[nodiscard]] FileReference MakeIosFileReference(NSURL* url);
[[nodiscard]] FileReference MakeCopiedIosFileReference(NSURL* url);
#endif

} // namespace huxerui::detail
