#include "platform_text_field.h"

#import <AppKit/AppKit.h>

#include <utility>

#include <huxerui/macos/platform_registry.h>

@interface HuxerUIExamplePlatformTextFieldView : NSObject <HUXAppKitPlatformView, NSTextFieldDelegate>
- (instancetype)initWithProperties:(HUXPlatformPayload*)properties
                             events:(id<HUXPlatformEventEmitter>)events;
@end

@implementation HuxerUIExamplePlatformTextFieldView {
  __strong NSTextField* _view;
  __strong id<HUXPlatformEventEmitter> _events;
}

- (instancetype)initWithProperties:(HUXPlatformPayload*)properties
                             events:(id<HUXPlatformEventEmitter>)events {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  _view = [[NSTextField alloc] initWithFrame:NSZeroRect];
  _view.delegate = self;
  _view.placeholderString = @"Edit PlatformView text";
  _view.controlSize = NSControlSizeLarge;
  _events = events;
  [self updateWithProperties:properties];
  return self;
}

- (NSView*)view {
  return _view;
}

- (void)updateWithProperties:(HUXPlatformPayload*)properties {
  NSString* value = properties.stringValue;
  if (![_view.stringValue isEqualToString:value]) {
    _view.stringValue = value;
  }
}

- (void)dispose {
  _view.delegate = nil;
  _events = nil;
}

- (void)controlTextDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  [_events emit:@"changed" payload:[HUXPlatformPayload stringValue:_view.stringValue]];
}

@end

@interface HuxerUIExamplePlatformTextFieldFactory : NSObject <HUXAppKitPlatformViewFactory>
@end

@implementation HuxerUIExamplePlatformTextFieldFactory

- (id<HUXAppKitPlatformView>)createWithWindow:(NSWindow*)window
                                   properties:(HUXPlatformPayload*)properties
                                       events:(id<HUXPlatformEventEmitter>)events {
  static_cast<void>(window);
  return [[HuxerUIExamplePlatformTextFieldView alloc] initWithProperties:properties events:events];
}

@end

namespace huxerui::example {

void InstallPlatformTextField(RootContext& root) {
  macos::ObjectiveCPlatformViewFactory<PlatformTextFieldProperties> factory{
      .factory = [HuxerUIExamplePlatformTextFieldFactory new],
  };
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, std::move(factory));
}

} // namespace huxerui::example
