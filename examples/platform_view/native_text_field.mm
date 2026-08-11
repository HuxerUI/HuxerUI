#include "native_text_field.h"

#import <AppKit/AppKit.h>

#include <memory>
#include <string>
#include <utility>

#include <huxerui/macos/platform_view.h>
#include <huxerui/platform_view.h>

namespace {

constexpr std::string_view native_text_field_type = "example/NativeTextField";
constexpr std::string_view text_property = "text";

NSString* NativeString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

} // namespace

@interface HuxerUIExampleNativeTextField : NSTextField <NSTextFieldDelegate> {
@public
  std::shared_ptr<huxerui::PlatformEventSink> huxeruiEventSink;
}
@end

@implementation HuxerUIExampleNativeTextField

- (void)controlTextDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (!huxeruiEventSink) {
    return;
  }
  const char* utf8 = self.stringValue.UTF8String;
  (*huxeruiEventSink)(
      std::string(huxerui::example::NativeTextFieldEvents::Changed::PlatformName),
      huxerui::PlatformPayload(utf8 == nullptr ? "" : utf8)
  );
}

@end

namespace huxerui::example {

namespace {

void ApplyProperties(HuxerUIExampleNativeTextField* text_field, const PlatformPayload& properties) {
  const std::string_view text = properties.AsObject().at(std::string(text_property)).AsString();
  NSString* native_text = NativeString(text);
  if (![text_field.stringValue isEqualToString:native_text]) {
    text_field.stringValue = native_text;
  }
}

NSView* CreateNativeTextField(const PlatformPayload& properties, PlatformEventSink event_sink) {
  HuxerUIExampleNativeTextField* text_field = [[HuxerUIExampleNativeTextField alloc] initWithFrame:NSZeroRect];
  text_field.delegate = text_field;
  text_field.placeholderString = @"Edit native text";
  text_field.controlSize = NSControlSizeLarge;
  text_field->huxeruiEventSink = std::make_shared<PlatformEventSink>(std::move(event_sink));
  ApplyProperties(text_field, properties);
  return text_field;
}

void UpdateNativeTextField(NSView* view, const PlatformPayload& properties) {
  ApplyProperties(static_cast<HuxerUIExampleNativeTextField*>(view), properties);
}

void DisposeNativeTextField(NSView* view) {
  auto* text_field = static_cast<HuxerUIExampleNativeTextField*>(view);
  text_field.delegate = nil;
  text_field->huxeruiEventSink.reset();
}

macos::PlatformViewFactory NativeTextFieldFactory() {
  return {
      .create = CreateNativeTextField,
      .update = UpdateNativeTextField,
      .dispose = DisposeNativeTextField,
  };
}

void RegisterNativeTextField(RootContext& root) {
  root.Modules().Register(std::string(native_text_field_type), NativeTextFieldFactory());
}

} // namespace

std::string NativeTextFieldEvents::Changed::Decode(const PlatformPayload& payload) {
  return std::string(payload.AsString());
}

View NativeTextField(std::string value) {
  PlatformPayload properties = PlatformPayload::Object{{std::string(text_property), std::move(value)}};
  return PlatformView(std::string(native_text_field_type), std::move(properties))
      .Events<NativeTextFieldEvents::Changed>();
}

RootHook InstallNativeTextField() {
  return RegisterNativeTextField;
}

} // namespace huxerui::example
