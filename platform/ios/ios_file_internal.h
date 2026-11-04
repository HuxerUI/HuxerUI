#pragma once

#include <functional>
#include <memory>

#ifdef __OBJC__
@class NSURL;
@class NSError;
@class UIViewController;
@class HuxerUIView;
#endif

namespace huxerui {
class FileReference;
struct AppDirectories;
} // namespace huxerui

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] AppDirectories CreateIosAppDirectories();

#ifdef __OBJC__
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateIosFilePickerTransport(std::function<UIViewController*()> presenter_provider);
void InstallIosFileDrop(HuxerUIView* view);
[[nodiscard]] FileReference MakeIosFileReference(NSURL* url, bool writable = true);
[[nodiscard]] FileReference MakeCopiedIosFileReference(NSURL* url, NSError** error = nullptr);
#endif

} // namespace huxerui::detail
