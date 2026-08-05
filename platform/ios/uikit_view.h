#pragma once

#import <UIKit/UIKit.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {
class IosPlatformAdapter;
class UIKitTextInput;
} // namespace huxerui::detail

@interface HuxerUIView : UIView {
@public
  huxerui::Runtime* huxeruiRuntime;
  huxerui::detail::IosPlatformAdapter* huxeruiAdapter;
  huxerui::detail::UIKitTextInput* huxeruiTextInput;
  __weak id<UITextInputDelegate> huxeruiInputDelegate;
  __strong UITextInputStringTokenizer* huxeruiTokenizer;
  __strong NSDictionary<NSAttributedStringKey, id>* huxeruiMarkedTextStyle;
  __strong NSMutableSet<UITouch*>* huxeruiTouches;
  UITextStorageDirection huxeruiSelectionAffinity;
}
- (void)commitHuxerUIFrame;
- (void)cancelHuxerUITouches;
- (void)huxeruiKeyboardFrameDidChange:(NSNotification*)notification;
- (void)huxeruiResourceConfigurationDidChange:(NSNotification*)notification;
@end
