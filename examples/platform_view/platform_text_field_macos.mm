#include "platform_text_field.h"

#import <AppKit/AppKit.h>

#include <memory>
#include <string_view>
#include <utility>

#include <huxerui/macos/platform_view.h>

namespace {

NSString* NSStringFromUtf8(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

} // namespace

@interface HuxerUIExamplePlatformTextField : NSTextField <NSTextFieldDelegate> {
@public
  std::shared_ptr<huxerui::PlatformEventSink> huxeruiEventSink;
}
@end

@implementation HuxerUIExamplePlatformTextField

- (void)controlTextDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (!huxeruiEventSink) {
    return;
  }
  const char* utf8 = self.stringValue.UTF8String;
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
  if (![text_field.stringValue isEqualToString:string_value]) {
    text_field.stringValue = string_value;
  }
}

NSView* CreatePlatformTextField(const PlatformPayload& properties, PlatformEventSink event_sink) {
  HuxerUIExamplePlatformTextField* text_field = [[HuxerUIExamplePlatformTextField alloc] initWithFrame:NSZeroRect];
  text_field.delegate = text_field;
  text_field.placeholderString = @"Edit PlatformView text";
  text_field.controlSize = NSControlSizeLarge;
  text_field->huxeruiEventSink = std::make_shared<PlatformEventSink>(std::move(event_sink));
  ApplyProperties(text_field, properties);
  return text_field;
}

void UpdatePlatformTextField(NSView* view, const PlatformPayload& properties) {
  ApplyProperties(static_cast<HuxerUIExamplePlatformTextField*>(view), properties);
}

void DisposePlatformTextField(NSView* view) {
  auto* text_field = static_cast<HuxerUIExamplePlatformTextField*>(view);
  text_field.delegate = nil;
  text_field->huxeruiEventSink.reset();
}

macos::PlatformViewFactory PlatformTextFieldFactory() {
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
