#include "platform_text_field.h"

#import <UIKit/UIKit.h>

#include <memory>
#include <string_view>
#include <utility>

#include <huxerui/ios/platform_registry.h>

namespace {

NSString* NSStringFromUtf8(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

} // namespace

@interface HuxerUIExamplePlatformTextField : UITextField {
@public
  std::shared_ptr<huxerui::PlatformEventEmitter> huxeruiEvents;
}
- (void)huxeruiTextDidChange;
@end

@implementation HuxerUIExamplePlatformTextField

- (void)huxeruiTextDidChange {
  if (!huxeruiEvents) {
    return;
  }
  const char* utf8 = self.text.UTF8String;
  huxeruiEvents->Emit<huxerui::example::PlatformTextFieldEvents::Changed>(std::string(utf8 == nullptr ? "" : utf8));
}

@end

namespace huxerui::example {

namespace {

struct PlatformTextFieldInstance {
  __strong HuxerUIExamplePlatformTextField* view = nil;
};

void ApplyProperties(HuxerUIExamplePlatformTextField* text_field, const PlatformTextFieldProperties& properties) {
  NSString* string_value = NSStringFromUtf8(properties.text);
  if (![text_field.text isEqualToString:string_value]) {
    text_field.text = string_value;
  }
}

std::shared_ptr<PlatformTextFieldInstance>
CreatePlatformTextField(UIViewController*, const PlatformTextFieldProperties& properties, PlatformEventEmitter events) {
  HuxerUIExamplePlatformTextField* text_field = [[HuxerUIExamplePlatformTextField alloc] initWithFrame:CGRectZero];
  text_field.placeholder = @"Edit PlatformView text";
  text_field.borderStyle = UITextBorderStyleRoundedRect;
  text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  text_field->huxeruiEvents = std::make_shared<PlatformEventEmitter>(std::move(events));
  [text_field addTarget:text_field
                 action:@selector(huxeruiTextDidChange)
       forControlEvents:UIControlEventEditingChanged];
  ApplyProperties(text_field, properties);
  auto instance = std::make_shared<PlatformTextFieldInstance>();
  instance->view = text_field;
  return instance;
}

void UpdatePlatformTextField(PlatformTextFieldInstance& instance, const PlatformTextFieldProperties& properties) {
  ApplyProperties(instance.view, properties);
}

void DisposePlatformTextField(PlatformTextFieldInstance& instance) {
  HuxerUIExamplePlatformTextField* text_field = instance.view;
  [text_field removeTarget:text_field
                    action:@selector(huxeruiTextDidChange)
          forControlEvents:UIControlEventEditingChanged];
  [text_field resignFirstResponder];
  text_field->huxeruiEvents.reset();
}

ios::PlatformViewFactory<PlatformTextFieldProperties, PlatformTextFieldInstance> PlatformTextFieldFactory() {
  return {
      .create = CreatePlatformTextField,
      .view = [](const std::shared_ptr<PlatformTextFieldInstance>& instance) -> UIView* { return instance->view; },
      .update = UpdatePlatformTextField,
      .dispose = DisposePlatformTextField,
  };
}

} // namespace

void InstallPlatformTextField(RootContext& root) {
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, PlatformTextFieldFactory());
}

} // namespace huxerui::example
