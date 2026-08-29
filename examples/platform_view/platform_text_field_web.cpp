#include "platform_text_field.h"

#include <emscripten/val.h>

#include <huxerui/web/platform_registry.h>

namespace {

constexpr char platform_text_field_factory[] = "huxeruiExamplePlatformTextFieldFactory";

} // namespace

namespace huxerui::example {

void InstallPlatformTextField(RootContext& root) {
  root.RegisterPlatformView<PlatformTextFieldProperties>(
      platform_text_field::type,
      web::JavaScriptPlatformViewFactory<PlatformTextFieldProperties>{
          .factory = emscripten::val::module_property(platform_text_field_factory),
      });
}

} // namespace huxerui::example
