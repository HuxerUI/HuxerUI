#pragma once

#include <string>

#include <huxerui/event.h>
#include <huxerui/platform_registry.h>
#include <huxerui/root.h>
#include <huxerui/view.h>

namespace huxerui::example {

namespace platform_text_field {

inline constexpr char type[] = "example/PlatformTextField";
} // namespace platform_text_field

struct PlatformTextFieldProperties {
  std::string text;

  [[nodiscard]] static PlatformPayload Encode(const PlatformTextFieldProperties& properties) {
    return PlatformPayload(properties.text);
  }

  bool operator==(const PlatformTextFieldProperties&) const = default;
};

struct PlatformTextFieldEvents {
  struct Changed : Event<void(const std::string&)> {
    static constexpr char Name[] = "changed";
  };
};

View PlatformTextField(std::string value);
void InstallPlatformTextField(RootContext& root);

} // namespace huxerui::example
