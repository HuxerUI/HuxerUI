#pragma once

#include <functional>

#include <huxerui/platform_module.h>

#if !defined(NOMINMAX)
#define NOMINMAX
#define HUXERUI_RESTORE_NOMINMAX
#endif
#include <windows.h>
#if defined(HUXERUI_RESTORE_NOMINMAX)
#undef HUXERUI_RESTORE_NOMINMAX
#undef NOMINMAX
#endif

namespace huxerui::windows {

struct PlatformViewFactory {
  std::function<HWND(HWND parent, const PlatformPayload&, PlatformEventSink)> create;
  std::function<void(HWND, const PlatformPayload&)> update;
  std::function<void(HWND)> dispose;
};

} // namespace huxerui::windows
