#include "platform_text_field.h"

#import <UIKit/UIKit.h>

#include <utility>

#include <huxerui/ios/platform_registry.h>

@interface HuxerUIExamplePlatformTextFieldView : NSObject <HUXUIKitPlatformView>
- (instancetype)initWithProperties:(HUXPlatformPayload*)properties
                             events:(id<HUXPlatformEventEmitter>)events;
@end

@implementation HuxerUIExamplePlatformTextFieldView {
  __strong UITextField* _view;
  __strong id<HUXPlatformEventEmitter> _events;
}

- (instancetype)initWithProperties:(HUXPlatformPayload*)properties
                             events:(id<HUXPlatformEventEmitter>)events {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  _view = [[UITextField alloc] initWithFrame:CGRectZero];
  _view.placeholder = @"Edit PlatformView text";
  _view.borderStyle = UITextBorderStyleRoundedRect;
  _view.clearButtonMode = UITextFieldViewModeWhileEditing;
  _events = events;
  [_view addTarget:self action:@selector(textDidChange) forControlEvents:UIControlEventEditingChanged];
  [self updateWithProperties:properties];
  return self;
}

- (UIView*)view {
  return _view;
}

- (void)updateWithProperties:(HUXPlatformPayload*)properties {
  NSString* value = properties.stringValue;
  if (![_view.text isEqualToString:value]) {
    _view.text = value;
  }
}

- (void)dispose {
  [_view removeTarget:self action:@selector(textDidChange) forControlEvents:UIControlEventEditingChanged];
  [_view resignFirstResponder];
  _events = nil;
}

- (void)textDidChange {
  [_events emit:@"changed" payload:[HUXPlatformPayload stringValue:_view.text ?: @""]];
}

@end

@interface HuxerUIExamplePlatformTextFieldFactory : NSObject <HUXUIKitPlatformViewFactory>
@end

@implementation HuxerUIExamplePlatformTextFieldFactory

- (id<HUXUIKitPlatformView>)createWithViewController:(UIViewController*)viewController
                                          properties:(HUXPlatformPayload*)properties
                                              events:(id<HUXPlatformEventEmitter>)events {
  static_cast<void>(viewController);
  return [[HuxerUIExamplePlatformTextFieldView alloc] initWithProperties:properties events:events];
}

@end

namespace huxerui::example {

void InstallPlatformTextField(RootContext& root) {
  ios::ObjectiveCPlatformViewFactory<PlatformTextFieldProperties> factory{
      .factory = [HuxerUIExamplePlatformTextFieldFactory new],
  };
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, std::move(factory));
}

} // namespace huxerui::example
