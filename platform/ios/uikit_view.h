#pragma once

#import <UIKit/UIKit.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {
class IosPlatformAdapter;
} // namespace huxerui::detail

@interface HuxerUIView : UIView {
@public
  huxerui::Runtime* huxeruiRuntime;
  huxerui::detail::IosPlatformAdapter* huxeruiAdapter;
  __strong NSMutableSet<UITouch*>* huxeruiTouches;
}
- (void)commitHuxerUIFrame;
- (void)cancelHuxerUITouches;
- (void)huxeruiKeyboardFrameDidChange:(NSNotification*)notification;
- (void)huxeruiResourceConfigurationDidChange:(NSNotification*)notification;
@end
