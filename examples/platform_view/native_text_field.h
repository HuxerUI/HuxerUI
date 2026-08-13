#pragma once

#include <string>

#include <huxerui/event.h>
#include <huxerui/platform_module.h>
#include <huxerui/root.h>
#include <huxerui/view.h>

namespace huxerui::example {

namespace native_text_field {

inline constexpr char type[] = "example/NativeTextField";
inline constexpr char text_property[] = "text";

} // namespace native_text_field

struct NativeTextFieldEvents {
  struct Changed : Event<std::string> {
    static constexpr char Name[] = "changed";

    static std::string Decode(const PlatformPayload& payload);
  };
};

View NativeTextField(std::string value);
void InstallNativeTextField(RootContext& root);

} // namespace huxerui::example
