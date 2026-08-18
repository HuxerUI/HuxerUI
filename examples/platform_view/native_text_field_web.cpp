#include "native_text_field.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <emscripten.h>
#include <emscripten/val.h>

#include <huxerui/web/platform_view.h>

namespace {

using emscripten::val;

std::unordered_map<std::uint32_t, huxerui::PlatformEventSink>& EventRoutes() {
  static std::unordered_map<std::uint32_t, huxerui::PlatformEventSink> routes;
  return routes;
}

std::uint32_t AllocateRoute() {
  static std::uint32_t next_route = 0;
  for (std::uint64_t attempt = 0; attempt < std::numeric_limits<std::uint32_t>::max(); ++attempt) {
    if (++next_route == 0) {
      ++next_route;
    }
    if (!EventRoutes().contains(next_route)) {
      return next_route;
    }
  }
  throw std::logic_error("HuxerUI Web PlatformView example event route space is exhausted");
}

std::string_view TextProperty(const huxerui::PlatformPayload& properties) {
  return properties.AsObject().at(huxerui::example::native_text_field::text_property).AsString();
}

// clang-format off
EM_JS(emscripten::EM_VAL, CreateWebNativeTextField, (std::uint32_t route, const char* text), {
  const input = document.createElement("input");
  input.type = "text";
  input.placeholder = "Edit native text";
  input.value = UTF8ToString(text);
  input.dataset.huxeruiExampleRoute = String(route);
  input.style.font = "16px system-ui, sans-serif";
  input.oninput = () => {
    const value = Module.stringToNewUTF8(input.value);
    try {
      Module._huxerui_example_web_native_text_field_changed(route, value);
    } finally {
      _free(value);
    }
  };
  return Emval.toHandle(input);
});

EM_JS(void, UpdateWebNativeTextField, (emscripten::EM_VAL handle, const char* text), {
  const input = Emval.toValue(handle);
  if (!(input instanceof HTMLInputElement)) {
    throw new Error("HuxerUI Web PlatformView example lost its input element");
  }
  const value = UTF8ToString(text);
  if (input.value !== value) {
    input.value = value;
  }
});

EM_JS(std::uint32_t, DisposeWebNativeTextField, (emscripten::EM_VAL handle), {
  const input = Emval.toValue(handle);
  if (!(input instanceof HTMLInputElement)) {
    return 0;
  }
  input.oninput = null;
  const route = Number(input.dataset.huxeruiExampleRoute);
  delete input.dataset.huxeruiExampleRoute;
  return Number.isSafeInteger(route) && route > 0 ? route : 0;
});
// clang-format on

huxerui::web::PlatformViewFactory NativeTextFieldFactory() {
  return {
      .create =
          [](const huxerui::PlatformPayload& properties, huxerui::PlatformEventSink events) {
            const std::uint32_t route = AllocateRoute();
            EventRoutes().emplace(route, std::move(events));
            try {
              const std::string text{TextProperty(properties)};
              return val::take_ownership(CreateWebNativeTextField(route, text.c_str()));
            } catch (...) {
              EventRoutes().erase(route);
              throw;
            }
          },
      .update =
          [](val element, const huxerui::PlatformPayload& properties) {
            const std::string text{TextProperty(properties)};
            UpdateWebNativeTextField(element.as_handle(), text.c_str());
          },
      .dispose =
          [](val element) {
            const std::uint32_t route = DisposeWebNativeTextField(element.as_handle());
            if (route != 0) {
              EventRoutes().erase(route);
            }
          },
  };
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void
huxerui_example_web_native_text_field_changed(std::uint32_t route, const char* value) noexcept {
  const auto found = EventRoutes().find(route);
  if (found == EventRoutes().end() || value == nullptr) {
    return;
  }
  try {
    found->second(huxerui::example::NativeTextFieldEvents::Changed::Name, huxerui::PlatformPayload(std::string(value)));
  } catch (...) {
  }
}

namespace huxerui::example {

void InstallNativeTextField(RootContext& root) {
  root.Modules().Register(native_text_field::type, NativeTextFieldFactory());
}

} // namespace huxerui::example
