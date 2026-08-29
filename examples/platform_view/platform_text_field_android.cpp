#include "platform_text_field.h"

#include <huxerui/android/platform_registry.h>

namespace {

constexpr char platform_text_field_class[] = "org.huxerui.examples.platformview.PlatformTextField";

} // namespace

namespace huxerui::example {

void InstallPlatformTextField(RootContext& root) {
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type,
                                                         android::JavaPlatformViewFactory<PlatformTextFieldProperties>{
                                                             .class_name = platform_text_field_class,
                                                         });
}

} // namespace huxerui::example
