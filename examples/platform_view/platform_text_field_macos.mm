#include "platform_text_field.h"

#import <AppKit/AppKit.h>

#include <memory>
#include <string_view>
#include <utility>

#include <huxerui/macos/platform_registry.h>

namespace {

NSString* NSStringFromUtf8(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

} // namespace

@interface HuxerUIExamplePlatformTextField : NSTextField <NSTextFieldDelegate> {
@public
  std::shared_ptr<huxerui::PlatformEventEmitter> huxeruiEvents;
}
@end

@implementation HuxerUIExamplePlatformTextField

- (void)controlTextDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (!huxeruiEvents) {
    return;
  }
  const char* utf8 = self.stringValue.UTF8String;
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
  if (![text_field.stringValue isEqualToString:string_value]) {
    text_field.stringValue = string_value;
  }
}

std::shared_ptr<PlatformTextFieldInstance>
CreatePlatformTextField(NSWindow*, const PlatformTextFieldProperties& properties, PlatformEventEmitter events) {
  HuxerUIExamplePlatformTextField* text_field = [[HuxerUIExamplePlatformTextField alloc] initWithFrame:NSZeroRect];
  text_field.delegate = text_field;
  text_field.placeholderString = @"Edit PlatformView text";
  text_field.controlSize = NSControlSizeLarge;
  text_field->huxeruiEvents = std::make_shared<PlatformEventEmitter>(std::move(events));
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
  text_field.delegate = nil;
  text_field->huxeruiEvents.reset();
}

macos::PlatformViewFactory<PlatformTextFieldProperties, PlatformTextFieldInstance> PlatformTextFieldFactory() {
  return {
      .create = CreatePlatformTextField,
      .view = [](const std::shared_ptr<PlatformTextFieldInstance>& instance) -> NSView* { return instance->view; },
      .update = UpdatePlatformTextField,
      .dispose = DisposePlatformTextField,
  };
}

} // namespace

void InstallPlatformTextField(RootContext& root) {
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, PlatformTextFieldFactory());
}

} // namespace huxerui::example
