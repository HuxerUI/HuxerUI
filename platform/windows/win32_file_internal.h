#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <huxerui/file.h>
#include <huxerui/platform_adapter.h>

namespace huxerui {

class FileSystem;

namespace detail {

class FilePickerTransport;
struct FileSystemPaths;

[[nodiscard]] FileSystemPaths
ResolveWin32FileSystemPaths(std::wstring_view executable_path, std::wstring_view local_app_data);
[[nodiscard]] std::shared_ptr<FileSystem>
CreateWin32FileSystem(std::wstring_view executable_path, std::wstring_view local_app_data);
[[nodiscard]] std::shared_ptr<FileSystem> CreateWin32FileSystem();
[[nodiscard]] std::optional<FileReference> MakeWin32FileReference(std::wstring_view platform_path);
[[nodiscard]] std::shared_ptr<FilePickerTransport>
CreateWin32FilePickerTransport(std::function<HWND()> window_provider, UIThreadDispatcher dispatch_to_ui_thread);

} // namespace detail
} // namespace huxerui
