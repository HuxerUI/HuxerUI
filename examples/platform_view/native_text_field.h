#pragma once

#include <string>
#include <string_view>

#include <huxerui/event.h>
#include <huxerui/platform_module.h>
#include <huxerui/root.h>
#include <huxerui/view.h>

namespace huxerui::example {

struct NativeTextFieldEvents {
  struct Changed : Event<std::string> {
    static constexpr std::string_view Name = "changed";

    static std::string Decode(const PlatformPayload& payload);
  };
};

View NativeTextField(std::string value);
RootHook InstallNativeTextField();

} // namespace huxerui::example
