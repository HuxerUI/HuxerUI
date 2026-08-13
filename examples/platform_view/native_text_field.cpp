#include "native_text_field.h"

#include <string>
#include <utility>

#include <huxerui/platform_view.h>

namespace huxerui::example {

std::string NativeTextFieldEvents::Changed::Decode(const PlatformPayload& payload) {
  return std::string(payload.AsString());
}

View NativeTextField(std::string value) {
  PlatformPayload properties = PlatformPayload::Object{{native_text_field::text_property, std::move(value)}};
  return PlatformView(native_text_field::type, std::move(properties)).Events<NativeTextFieldEvents::Changed>();
}

} // namespace huxerui::example
