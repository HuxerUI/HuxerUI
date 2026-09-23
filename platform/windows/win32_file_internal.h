#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <huxerui/file.h>
#include <huxerui/app.h>

namespace huxerui {

class UiWindow;

namespace detail {

class FilePickerTransport;

class Win32FileDrop final {
public:
  Win32FileDrop(HWND platform_window, UiWindow& ui_window, std::function<float()> scale);
  ~Win32FileDrop();
  Win32FileDrop(const Win32FileDrop&) = delete;
  Win32FileDrop& operator=(const Win32FileDrop&) = delete;

private:
  class Target;
  HWND platform_window_;
  Target* target_;
};

[[nodiscard]] AppDirectories
ResolveWin32AppDirectories(std::wstring_view executable_path, std::wstring_view local_app_data);
[[nodiscard]] AppDirectories
CreateWin32AppDirectories(std::wstring_view executable_path, std::wstring_view local_app_data);
[[nodiscard]] AppDirectories CreateWin32AppDirectories();
[[nodiscard]] std::optional<FileReference> MakeWin32FileReference(std::wstring_view platform_path, bool writable = true);
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateWin32FilePickerTransport(std::function<HWND()> window_provider, UiThreadDispatcher dispatch_to_ui_thread);

} // namespace detail
} // namespace huxerui
