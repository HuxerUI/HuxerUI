#include "platform_text_field.h"

#include <string>
#include <utility>

#include <huxerui/platform_view.h>

namespace huxerui::example {

std::string PlatformTextFieldEvents::Changed::Decode(const PlatformPayload& payload) {
  return std::string(payload.AsString());
}

View PlatformTextField(std::string value) {
  PlatformPayload properties = PlatformPayload::Object{{platform_text_field::text_property, std::move(value)}};
  return PlatformView(platform_text_field::type, std::move(properties)).Events<PlatformTextFieldEvents::Changed>();
}

} // namespace huxerui::example
