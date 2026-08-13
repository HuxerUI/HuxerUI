#include "native_text_field.h"

#import <UIKit/UIKit.h>

#include <memory>
#include <string>
#include <utility>

#include <huxerui/ios/platform_view.h>

namespace {

NSString* NativeString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

} // namespace

@interface HuxerUIExampleNativeTextField : UITextField {
@public
  std::shared_ptr<huxerui::PlatformEventSink> huxeruiEventSink;
}
- (void)huxeruiTextDidChange;
@end

@implementation HuxerUIExampleNativeTextField

- (void)huxeruiTextDidChange {
  if (!huxeruiEventSink) {
    return;
  }
  const char* utf8 = self.text.UTF8String;
  (*huxeruiEventSink)(
      std::string(huxerui::example::NativeTextFieldEvents::Changed::Name),
      huxerui::PlatformPayload(utf8 == nullptr ? "" : utf8)
  );
}

@end

namespace huxerui::example {

namespace {

void ApplyProperties(HuxerUIExampleNativeTextField* text_field, const PlatformPayload& properties) {
  const std::string_view text = properties.AsObject().at(std::string(native_text_field::text_property)).AsString();
  NSString* native_text = NativeString(text);
  if (![text_field.text isEqualToString:native_text]) {
    text_field.text = native_text;
  }
}

UIView* CreateNativeTextField(const PlatformPayload& properties, PlatformEventSink event_sink) {
  HuxerUIExampleNativeTextField* text_field = [[HuxerUIExampleNativeTextField alloc] initWithFrame:CGRectZero];
  text_field.placeholder = @"Edit native text";
  text_field.borderStyle = UITextBorderStyleRoundedRect;
  text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  text_field->huxeruiEventSink = std::make_shared<PlatformEventSink>(std::move(event_sink));
  [text_field addTarget:text_field
                 action:@selector(huxeruiTextDidChange)
       forControlEvents:UIControlEventEditingChanged];
  ApplyProperties(text_field, properties);
  return text_field;
}

void UpdateNativeTextField(UIView* view, const PlatformPayload& properties) {
  ApplyProperties(static_cast<HuxerUIExampleNativeTextField*>(view), properties);
}

void DisposeNativeTextField(UIView* view) {
  auto* text_field = static_cast<HuxerUIExampleNativeTextField*>(view);
  [text_field removeTarget:text_field
                    action:@selector(huxeruiTextDidChange)
          forControlEvents:UIControlEventEditingChanged];
  [text_field resignFirstResponder];
  text_field->huxeruiEventSink.reset();
}

ios::PlatformViewFactory NativeTextFieldFactory() {
  return {
      .create = CreateNativeTextField,
      .update = UpdateNativeTextField,
      .dispose = DisposeNativeTextField,
  };
}

void RegisterNativeTextField(RootContext& root) {
  root.Modules().Register(std::string(native_text_field::type), NativeTextFieldFactory());
}

} // namespace

RootHook InstallNativeTextField() {
  return RegisterNativeTextField;
}

} // namespace huxerui::example
