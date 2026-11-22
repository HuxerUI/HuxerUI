#include "platform_text_field.h"

#include <huxerui/ohos/platform_registry.h>

namespace huxerui::example {

void InstallPlatformTextField(ApplicationContext& root) {
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type,
      ohos::ArkTsPlatformViewFactory<PlatformTextFieldProperties>{.factory_name = platform_text_field::type});
}

} // namespace huxerui::example
