#pragma once

#include <functional>

#include <emscripten/val.h>

#include <huxerui/platform_module.h>

namespace huxerui::web {

struct PlatformViewFactory {
  std::function<emscripten::val(const PlatformPayload&, PlatformEventSink)> create;
  std::function<void(emscripten::val, const PlatformPayload&)> update;
  std::function<void(emscripten::val)> dispose;
};

} // namespace huxerui::web
