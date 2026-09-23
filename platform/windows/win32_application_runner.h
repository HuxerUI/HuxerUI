#pragma once

#include <windows.h>

#include <functional>

#include <huxerui/app.h>

namespace huxerui::detail {

using Win32WindowReady = std::function<void(UiThreadDispatcher, HWND)>;

int RunWin32PlatformApplication(const Application& application, Win32WindowReady on_window_ready = {});

} // namespace huxerui::detail
