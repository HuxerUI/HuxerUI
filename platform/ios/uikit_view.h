#pragma once

#import <UIKit/UIKit.h>

namespace huxerui {
class UiWindow;
}

namespace huxerui::detail {
class IosUiWindow;
} // namespace huxerui::detail

@interface HuxerUIView : UIView {
@public
  huxerui::UiWindow* huxeruiWindow;
  huxerui::detail::IosUiWindow* huxeruiPlatformWindow;
  __strong NSMutableSet<UITouch*>* huxeruiTouches;
  __strong NSMutableDictionary<NSNumber*, NSNumber*>* huxeruiPointerButtons;
  __strong id<UIDropInteractionDelegate> huxeruiFileDropDelegate;
}
- (void)commitHuxerUIFrame;
- (void)cancelHuxerUITouches;
- (void)huxeruiKeyboardFrameDidChange:(NSNotification*)notification;
- (void)huxeruiResourceConfigurationDidChange:(NSNotification*)notification;
@end
