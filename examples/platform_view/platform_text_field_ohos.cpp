#include "platform_text_field.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <huxerui/ohos/platform_registry.h>

namespace huxerui::example {
namespace {

void SetText(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle node, ArkUI_NodeAttributeType type, const char* text) {
  const ArkUI_AttributeItem attribute{.value = nullptr, .size = 0, .string = text, .object = nullptr};
  if (api.setAttribute(node, type, &attribute) != 0) {
    throw std::runtime_error("HuxerUI could not set an OHOS TextInput attribute");
  }
}

struct TextFieldInstance {
  TextFieldInstance(ArkUI_NativeNodeAPI_1& api, PlatformEventEmitter events)
      : api(api), view(api.createNode(ARKUI_NODE_TEXT_INPUT), api.disposeNode), events(std::move(events)) {
    if (!view) {
      throw std::runtime_error("HuxerUI could not create OHOS TextInput");
    }
  }

  ~TextFieldInstance() {
    api.unregisterNodeEvent(view.get(), NODE_TEXT_INPUT_ON_CHANGE);
    api.removeNodeEventReceiver(view.get(), OnChange);
  }

  static void OnChange(ArkUI_NodeEvent* event) {
    if (OH_ArkUI_NodeEvent_GetEventType(event) != NODE_TEXT_INPUT_ON_CHANGE) {
      return;
    }
    auto& entry = *static_cast<TextFieldInstance*>(OH_ArkUI_NodeEvent_GetUserData(event));
    const auto* data = OH_ArkUI_NodeEvent_GetStringAsyncEvent(event);
    if (data && data->pStr) {
      entry.events.Emit<PlatformTextFieldEvents::Changed>(std::string(data->pStr));
    }
  }

  ArkUI_NativeNodeAPI_1& api;
  std::unique_ptr<std::remove_pointer_t<ArkUI_NodeHandle>, void (*)(ArkUI_NodeHandle)> view;
  PlatformEventEmitter events;
};

void UpdateText(TextFieldInstance& instance, const PlatformTextFieldProperties& properties) {
  const auto* current = instance.api.getAttribute(instance.view.get(), NODE_TEXT_INPUT_TEXT);
  if (!current || !current->string || current->string != properties.text) {
    SetText(instance.api, instance.view.get(), NODE_TEXT_INPUT_TEXT, properties.text.c_str());
  }
}

} // namespace

void InstallPlatformTextField(ApplicationContext& root) {
  ohos::PlatformViewFactory<PlatformTextFieldProperties, TextFieldInstance> factory{
      .create = [](ArkUI_NativeNodeAPI_1& api, const PlatformTextFieldProperties& properties,
                   PlatformEventEmitter events) {
        auto instance = std::make_shared<TextFieldInstance>(api, std::move(events));
        SetText(api, instance->view.get(), NODE_TEXT_INPUT_PLACEHOLDER, "Edit PlatformView text");
        UpdateText(*instance, properties);
        if (api.addNodeEventReceiver(instance->view.get(), TextFieldInstance::OnChange) != 0 ||
            api.registerNodeEvent(instance->view.get(), NODE_TEXT_INPUT_ON_CHANGE, 0, instance.get()) != 0) {
          throw std::runtime_error("HuxerUI could not listen to changes from OHOS TextInput");
        }
        return instance;
      },
      .view = [](const std::shared_ptr<TextFieldInstance>& instance) { return instance->view.get(); },
      .update = UpdateText,
      .dispose = {},
  };
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, std::move(factory));
}

} // namespace huxerui::example
