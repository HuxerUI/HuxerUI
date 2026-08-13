#pragma once

#include <functional>

#include <huxerui/platform_module.h>

#if defined(__OBJC__)
@class UIView;
#else
class UIView;
#endif

namespace huxerui::ios {

struct PlatformViewFactory {
  std::function<UIView*(const PlatformPayload&, PlatformEventSink)> create;
  std::function<void(UIView*, const PlatformPayload&)> update;
  std::function<void(UIView*)> dispose;
};

} // namespace huxerui::ios
