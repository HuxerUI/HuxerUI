#include "platform_text_field.h"

#import <UIKit/UIKit.h>

#include <memory>
#include <string_view>
#include <utility>

#include <huxerui/ios/platform_view.h>

namespace {

NSString* NSStringFromUtf8(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

} // namespace

@interface HuxerUIExamplePlatformTextField : UITextField {
@public
  std::shared_ptr<huxerui::PlatformEventSink> huxeruiEventSink;
}
- (void)huxeruiTextDidChange;
@end

@implementation HuxerUIExamplePlatformTextField

- (void)huxeruiTextDidChange {
  if (!huxeruiEventSink) {
    return;
  }
  const char* utf8 = self.text.UTF8String;
  (*huxeruiEventSink)(
      huxerui::example::PlatformTextFieldEvents::Changed::Name,
      huxerui::PlatformPayload(utf8 == nullptr ? "" : utf8)
  );
}

@end

namespace huxerui::example {

namespace {

void ApplyProperties(HuxerUIExamplePlatformTextField* text_field, const PlatformPayload& properties) {
  const std::string_view text = properties.AsObject().at(platform_text_field::text_property).AsString();
  NSString* string_value = NSStringFromUtf8(text);
  if (![text_field.text isEqualToString:string_value]) {
    text_field.text = string_value;
  }
}

UIView* CreatePlatformTextField(const PlatformPayload& properties, PlatformEventSink event_sink) {
  HuxerUIExamplePlatformTextField* text_field = [[HuxerUIExamplePlatformTextField alloc] initWithFrame:CGRectZero];
  text_field.placeholder = @"Edit PlatformView text";
  text_field.borderStyle = UITextBorderStyleRoundedRect;
  text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  text_field->huxeruiEventSink = std::make_shared<PlatformEventSink>(std::move(event_sink));
  [text_field addTarget:text_field
                 action:@selector(huxeruiTextDidChange)
       forControlEvents:UIControlEventEditingChanged];
  ApplyProperties(text_field, properties);
  return text_field;
}

void UpdatePlatformTextField(UIView* view, const PlatformPayload& properties) {
  ApplyProperties(static_cast<HuxerUIExamplePlatformTextField*>(view), properties);
}

void DisposePlatformTextField(UIView* view) {
  auto* text_field = static_cast<HuxerUIExamplePlatformTextField*>(view);
  [text_field removeTarget:text_field
                    action:@selector(huxeruiTextDidChange)
          forControlEvents:UIControlEventEditingChanged];
  [text_field resignFirstResponder];
  text_field->huxeruiEventSink.reset();
}

ios::PlatformViewFactory PlatformTextFieldFactory() {
  return {
      .create = CreatePlatformTextField,
      .update = UpdatePlatformTextField,
      .dispose = DisposePlatformTextField,
  };
}

} // namespace

void InstallPlatformTextField(RootContext& root) {
  root.Modules().Register(platform_text_field::type, PlatformTextFieldFactory());
}

} // namespace huxerui::example
