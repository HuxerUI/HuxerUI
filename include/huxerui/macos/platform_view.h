#pragma once

#include <functional>

#include <huxerui/platform_module.h>

#if defined(__OBJC__)
@class NSView;
#else
class NSView;
#endif

namespace huxerui::macos {

struct PlatformViewFactory {
  std::function<NSView*(const PlatformPayload&, PlatformEventSink)> create;
  std::function<void(NSView*, const PlatformPayload&)> update;
  std::function<void(NSView*)> dispose;
};

} // namespace huxerui::macos
