#include "platform_text_field.h"

#include <string>
#include <utility>

#include <huxerui/platform_registry.h>

namespace huxerui::example {

View PlatformTextField(std::string value) {
  return PlatformView(platform_text_field::type, PlatformTextFieldProperties{std::move(value)});
}

} // namespace huxerui::example
