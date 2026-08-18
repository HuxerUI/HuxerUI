#pragma once

#include <string>

#include <huxerui/event.h>
#include <huxerui/platform_module.h>
#include <huxerui/root.h>
#include <huxerui/view.h>

namespace huxerui::example {

namespace platform_text_field {

inline constexpr char type[] = "example/PlatformTextField";
inline constexpr char text_property[] = "text";

} // namespace platform_text_field

struct PlatformTextFieldEvents {
  struct Changed : Event<std::string> {
    static constexpr char Name[] = "changed";

    static std::string Decode(const PlatformPayload& payload);
  };
};

View PlatformTextField(std::string value);
void InstallPlatformTextField(RootContext& root);

} // namespace huxerui::example
