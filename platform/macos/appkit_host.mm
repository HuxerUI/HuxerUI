#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <QuartzCore/CADisplayLink.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "internal.h"

namespace huxerui::detail {
class MacPlatformHost;
}

@interface HuxerUIHostView : NSView {
@public
  huxerui::detail::Runtime *huxeruiRuntime;
  huxerui::detail::MacPlatformHost *huxeruiHost;
  NSPoint huxeruiPointerPosition;
  NSTrackingArea *huxeruiTrackingArea;
}
- (void)sendPointerEvent:(NSEvent *)event
                    type:(huxerui::PointerEventType)type;
- (void)sendKeyEvent:(NSEvent *)event
                 type:(huxerui::KeyEventType)type;
- (void)cancelPointer;
@end

@interface HuxerUIApplicationDelegate : NSObject <NSApplicationDelegate>
@end

@interface HuxerUIFrameScheduler : NSObject
- (instancetype)initWithView:(HuxerUIHostView *)view;
- (void)requestFrameAfter:(double)delaySeconds;
- (void)shutdown;
@end

@interface HuxerUIFrameScheduler ()
- (void)armForGeneration:(NSUInteger)generation;
- (void)display;
@end

@implementation HuxerUIFrameScheduler {
  __weak HuxerUIHostView *_view;
  __strong CADisplayLink *_displayLink;
  NSUInteger _generation;
}

- (instancetype)initWithView:(HuxerUIHostView *)view {
  self = [super init];
  if (self == nil) {
    return nil;
  }

  _view = view;
  if (@available(macOS 14.0, *)) {
    _displayLink = [view displayLinkWithTarget:self
                                      selector:@selector(displayLinkDidFire:)];
    _displayLink.paused = YES;
    [_displayLink addToRunLoop:NSRunLoop.mainRunLoop
                       forMode:NSRunLoopCommonModes];
  }
  return self;
}

- (void)requestFrameAfter:(double)delaySeconds {
  const NSUInteger generation = ++_generation;
  if (delaySeconds > 0.0) {
    const auto nanoseconds = static_cast<std::int64_t>(
        delaySeconds * static_cast<double>(NSEC_PER_SEC));
    __weak HuxerUIFrameScheduler *scheduler = self;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, nanoseconds),
        dispatch_get_main_queue(), ^{
          HuxerUIFrameScheduler *strongScheduler = scheduler;
          if (strongScheduler != nil &&
              strongScheduler->_generation == generation) {
            [strongScheduler armForGeneration:generation];
          }
        });
    return;
  }
  [self armForGeneration:generation];
}

- (void)armForGeneration:(NSUInteger)generation {
  if (_generation != generation) {
    return;
  }
  if (_displayLink != nil) {
    _displayLink.paused = NO;
    return;
  }

  __weak HuxerUIFrameScheduler *scheduler = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    HuxerUIFrameScheduler *strongScheduler = scheduler;
    if (strongScheduler != nil &&
        strongScheduler->_generation == generation) {
      [strongScheduler display];
    }
  });
}

- (void)displayLinkDidFire:(CADisplayLink *)displayLink {
  displayLink.paused = YES;
  [self display];
}

- (void)display {
  HuxerUIHostView *view = _view;
  if (view != nil && view.window != nil) {
    [view setNeedsDisplay:YES];
  }
}

- (void)shutdown {
  ++_generation;
  [_displayLink invalidate];
  _displayLink = nil;
  _view = nil;
}

@end

namespace huxerui::detail {

namespace {

CFStringRef CreateString(std::string_view text) {
  return CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(text.data()),
      static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
}

CTFontRef CreateFont(float font_size) {
  return CTFontCreateUIFontForLanguage(
      kCTFontUIFontSystem, static_cast<CGFloat>(font_size), nullptr);
}

CFAttributedStringRef CreateAttributedString(std::string_view text,
                                             float font_size) {
  CFStringRef string = CreateString(text);
  CTFontRef font = CreateFont(font_size);
  const void *keys[] = {
      kCTFontAttributeName,
      kCTForegroundColorFromContextAttributeName,
  };
  const void *values[] = {
      font,
      kCFBooleanTrue,
  };
  CFDictionaryRef attributes = CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFAttributedStringRef attributed =
      CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
  CFRelease(attributes);
  CFRelease(font);
  CFRelease(string);
  return attributed;
}

CTLineRef CreateLine(std::string_view text, float font_size) {
  CFAttributedStringRef attributed = CreateAttributedString(text, font_size);
  CTLineRef line = CTLineCreateWithAttributedString(attributed);
  CFRelease(attributed);
  return line;
}

void SetFillColor(CGContextRef context, Color color) {
  CGContextSetRGBFillColor(context, color.red, color.green, color.blue,
                           color.alpha);
}

void SetStrokeColor(CGContextRef context, Color color) {
  CGContextSetRGBStrokeColor(
      context, color.red, color.green, color.blue, color.alpha);
}

Key TranslateKey(unsigned short key_code) {
  switch (key_code) {
  case 48:
    return Key::Tab;
  case 36:
  case 76:
    return Key::Enter;
  case 49:
    return Key::Space;
  case 53:
    return Key::Escape;
  case 51:
    return Key::Backspace;
  case 117:
    return Key::Delete;
  case 123:
    return Key::ArrowLeft;
  case 124:
    return Key::ArrowRight;
  case 125:
    return Key::ArrowDown;
  case 126:
    return Key::ArrowUp;
  case 115:
    return Key::Home;
  case 119:
    return Key::End;
  case 116:
    return Key::PageUp;
  case 121:
    return Key::PageDown;
  default:
    return Key::Unknown;
  }
}

} // namespace

class MacPlatformHost final : public PlatformHost, public TextService {
public:
  int Run(Runtime &runtime, const AppOptions &options) override {
    @autoreleasepool {
      NSApplication *application = [NSApplication sharedApplication];
      [application setActivationPolicy:NSApplicationActivationPolicyRegular];

      delegate_ = [[HuxerUIApplicationDelegate alloc] init];
      application.delegate = delegate_;

      const NSRect frame = NSMakeRect(0.0, 0.0, options.width, options.height);
      const NSWindowStyleMask style =
          NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
      window_ = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:style
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
      window_.title = [NSString stringWithUTF8String:options.title.c_str()];
      window_.acceptsMouseMovedEvents = YES;

      view_ = [[HuxerUIHostView alloc] initWithFrame:frame];
      view_->huxeruiRuntime = &runtime;
      view_->huxeruiHost = this;
      frame_scheduler_ =
          [[HuxerUIFrameScheduler alloc] initWithView:view_];
      window_.contentView = view_;
      [window_ center];
      [window_ makeKeyAndOrderFront:nil];
      [window_ makeFirstResponder:view_];

      [application finishLaunching];
      [application activateIgnoringOtherApps:YES];
      [view_ setNeedsDisplay:YES];
      [application run];
      view_->huxeruiRuntime = nullptr;
      view_->huxeruiHost = nullptr;
      [frame_scheduler_ shutdown];
      frame_scheduler_ = nil;
    }
    return 0;
  }

  void RequestFrame(double delay_seconds) override {
    if (frame_scheduler_) {
      [frame_scheduler_ requestFrameAfter:delay_seconds];
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
               Clock::now().time_since_epoch())
        .count();
  }

  TextService &Text() override { return *this; }

  Size MeasureText(std::string_view text, float font_size,
                   float max_width) override {
    if (std::isfinite(max_width)) {
      if (max_width <= 0.0F) {
        return {};
      }
      CFAttributedStringRef attributed =
          CreateAttributedString(text, font_size);
      CTFramesetterRef framesetter =
          CTFramesetterCreateWithAttributedString(attributed);
      const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
          framesetter, CFRangeMake(0, 0), nullptr,
          CGSizeMake(max_width, CGFLOAT_MAX), nullptr);
      CFRelease(framesetter);
      CFRelease(attributed);
      return {
          std::ceil(std::min(static_cast<float>(suggested.width), max_width)),
          std::ceil(static_cast<float>(suggested.height)),
      };
    }

    CTLineRef line = CreateLine(text, font_size);
    CGFloat ascent = 0.0;
    CGFloat descent = 0.0;
    CGFloat leading = 0.0;
    const double width =
        CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CFRelease(line);

    return {
        std::ceil(static_cast<float>(width)),
        std::ceil(static_cast<float>(ascent + descent + leading)),
    };
  }

  void Render(const DisplayList &display_list, CGContextRef context) {
    SetFillColor(context, Color::Rgb(247, 248, 250));
    CGContextFillRect(context, NSRectToCGRect(view_.bounds));

    for (const DrawCommand &command : display_list.Commands()) {
      std::visit(
          [this, context](const auto &value) { RenderCommand(context, value); },
          command);
    }
  }

private:
  void RenderCommand(CGContextRef context, const DrawRectCommand &command) {
    SetFillColor(context, command.color);
    const CGRect rect = CGRectMake(command.rect.x, command.rect.y,
                                   command.rect.width, command.rect.height);
    if (command.corner_radius > 0.0F) {
      CGPathRef path = CGPathCreateWithRoundedRect(
          rect, command.corner_radius, command.corner_radius, nullptr);
      CGContextAddPath(context, path);
      CGContextFillPath(context);
      CGPathRelease(path);
    } else {
      CGContextFillRect(context, rect);
    }
  }

  void RenderCommand(CGContextRef context, const DrawTextCommand &command) {
    if (command.align == TextAlign::Leading) {
      if (command.rect.width <= 0.0F || command.rect.height <= 0.0F) {
        return;
      }
      CFAttributedStringRef attributed =
          CreateAttributedString(command.text, command.font_size);
      CTFramesetterRef framesetter =
          CTFramesetterCreateWithAttributedString(attributed);
      CGPathRef path = CGPathCreateWithRect(
          CGRectMake(0.0, 0.0, command.rect.width, command.rect.height),
          nullptr);
      CTFrameRef frame = CTFramesetterCreateFrame(
          framesetter, CFRangeMake(0, 0), path, nullptr);

      CGContextSaveGState(context);
      CGContextTranslateCTM(context, command.rect.x,
                            command.rect.y + command.rect.height);
      CGContextScaleCTM(context, 1.0, -1.0);
      CGContextSetTextMatrix(context, CGAffineTransformIdentity);
      SetFillColor(context, command.color);
      CTFrameDraw(frame, context);
      CGContextRestoreGState(context);

      CFRelease(frame);
      CGPathRelease(path);
      CFRelease(framesetter);
      CFRelease(attributed);
      return;
    }

    CTLineRef line = CreateLine(command.text, command.font_size);
    CGFloat ascent = 0.0;
    CGFloat descent = 0.0;
    CGFloat leading = 0.0;
    const double width =
        CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    const CGFloat text_height = ascent + descent + leading;

    CGFloat x = command.rect.x;
    if (command.align == TextAlign::Center) {
      x += std::max(0.0F,
                    (command.rect.width - static_cast<float>(width)) * 0.5F);
    }

    const CGFloat bottom_padding = std::max(
        0.0F, (command.rect.height - static_cast<float>(text_height)) * 0.5F);
    const CGFloat baseline_from_bottom = bottom_padding + descent;

    CGContextSaveGState(context);
    CGContextTranslateCTM(context, 0.0, command.rect.y + command.rect.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    SetFillColor(context, command.color);
    CGContextSetTextPosition(context, x, baseline_from_bottom);
    CTLineDraw(line, context);
    CGContextRestoreGState(context);
    CFRelease(line);
  }

  void RenderCommand(CGContextRef context, const DrawCircleCommand &command) {
    if (command.radius <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetFillColor(context, command.color);
    const float diameter = command.radius * 2.0F;
    CGContextFillEllipseInRect(
        context,
        CGRectMake(
            command.center.x - command.radius,
            command.center.y - command.radius,
            diameter, diameter));
  }

  void RenderCommand(CGContextRef context, const DrawBorderCommand &command) {
    if (command.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    const float inset = command.width * 0.5F;
    const CGRect rect = CGRectMake(
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.width),
        std::max(0.0F, command.rect.height - command.width));
    const float radius =
        std::max(0.0F, command.corner_radius - inset);
    CGPathRef path = CGPathCreateWithRoundedRect(
        rect, radius, radius, nullptr);
    CGContextSaveGState(context);
    SetStrokeColor(context, command.color);
    CGContextSetLineWidth(context, command.width);
    CGContextAddPath(context, path);
    CGContextStrokePath(context);
    CGContextRestoreGState(context);
    CGPathRelease(path);
  }

  void RenderCommand(CGContextRef context, const PushClipCommand &command) {
    CGContextSaveGState(context);
    const CGRect rect =
        CGRectMake(
            command.rect.x, command.rect.y,
            command.rect.width, command.rect.height);
    if (command.corner_radius <= 0.0F) {
      CGContextClipToRect(context, rect);
      return;
    }
    const float radius = std::min(
        command.corner_radius,
        std::min(
            command.rect.width,
            command.rect.height) * 0.5F);
    CGPathRef path = CGPathCreateWithRoundedRect(
        rect, radius, radius, nullptr);
    CGContextAddPath(context, path);
    CGContextClip(context);
    CGPathRelease(path);
  }

  void RenderCommand(CGContextRef context, const PopClipCommand &command) {
    static_cast<void>(command);
    CGContextRestoreGState(context);
  }

  __strong NSWindow *window_ = nil;
  __strong HuxerUIHostView *view_ = nil;
  __strong HuxerUIApplicationDelegate *delegate_ = nil;
  __strong HuxerUIFrameScheduler *frame_scheduler_ = nil;
};

std::unique_ptr<PlatformHost> CreateDefaultPlatformHost() {
  return std::make_unique<MacPlatformHost>();
}

} // namespace huxerui::detail

@implementation HuxerUIHostView

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)updateTrackingAreas {
  if (huxeruiTrackingArea != nil) {
    [self removeTrackingArea:huxeruiTrackingArea];
  }
  huxeruiTrackingArea = [[NSTrackingArea alloc]
      initWithRect:NSZeroRect
           options:NSTrackingMouseEnteredAndExited |
                   NSTrackingMouseMoved |
                   NSTrackingActiveInKeyWindow |
                   NSTrackingInVisibleRect
             owner:self
          userInfo:nil];
  [self addTrackingArea:huxeruiTrackingArea];
  [super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  if (huxeruiRuntime == nullptr || huxeruiHost == nullptr) {
    return;
  }

  const NSRect bounds = self.bounds;
  huxeruiRuntime->SetViewport({
      static_cast<float>(bounds.size.width),
      static_cast<float>(bounds.size.height),
  });
  const huxerui::DisplayList &displayList = huxeruiRuntime->BuildFrame();
  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  huxeruiHost->Render(displayList, context);
}

- (void)sendPointerEvent:(NSEvent *)event
                    type:(huxerui::PointerEventType)type {
  if (huxeruiRuntime == nullptr) {
    return;
  }

  const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  huxeruiPointerPosition = point;
  huxeruiRuntime->HandlePointerEvent({
      type,
      0,
      {
          static_cast<float>(point.x),
          static_cast<float>(point.y),
      },
  });
}

- (void)cancelPointer {
  if (huxeruiRuntime == nullptr) {
    return;
  }
  huxeruiRuntime->HandlePointerEvent({
      huxerui::PointerEventType::Cancel,
      0,
      {
          static_cast<float>(huxeruiPointerPosition.x),
          static_cast<float>(huxeruiPointerPosition.y),
      },
  });
}

- (void)sendKeyEvent:(NSEvent *)event
                 type:(huxerui::KeyEventType)type {
  if (huxeruiRuntime == nullptr) {
    return;
  }
  const NSEventModifierFlags flags = event.modifierFlags;
  const char *characters =
      event.characters == nil ? nullptr : event.characters.UTF8String;
  huxeruiRuntime->HandleKeyEvent({
      type,
      huxerui::detail::TranslateKey(event.keyCode),
      characters == nullptr ? std::string{} : std::string(characters),
      {
          static_cast<bool>(flags & NSEventModifierFlagShift),
          static_cast<bool>(flags & NSEventModifierFlagControl),
          static_cast<bool>(flags & NSEventModifierFlagOption),
          static_cast<bool>(flags & NSEventModifierFlagCommand),
      },
      static_cast<bool>(event.isARepeat),
  });
}

- (void)mouseDown:(NSEvent *)event {
  [self.window makeFirstResponder:self];
  [self sendPointerEvent:event type:huxerui::PointerEventType::Down];
}

- (void)mouseMoved:(NSEvent *)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Move];
}

- (void)mouseDragged:(NSEvent *)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Move];
}

- (void)mouseExited:(NSEvent *)event {
  static_cast<void>(event);
  [self cancelPointer];
}

- (void)mouseUp:(NSEvent *)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Up];
}

- (void)keyDown:(NSEvent *)event {
  [self sendKeyEvent:event type:huxerui::KeyEventType::Down];
}

- (void)keyUp:(NSEvent *)event {
  [self sendKeyEvent:event type:huxerui::KeyEventType::Up];
}

- (void)cancelOperation:(id)sender {
  static_cast<void>(sender);
  [self cancelPointer];
}

- (void)viewWillMoveToWindow:(NSWindow *)newWindow {
  if (newWindow == nil) {
    [self cancelPointer];
  }
  [super viewWillMoveToWindow:newWindow];
}

- (void)scrollWheel:(NSEvent *)event {
  if (huxeruiRuntime == nullptr) {
    return;
  }

  const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  const float scale = event.hasPreciseScrollingDeltas ? 1.0F : 12.0F;
  huxeruiRuntime->HandleScrollEvent({
      {
          static_cast<float>(point.x),
          static_cast<float>(point.y),
      },
      static_cast<float>(-event.scrollingDeltaX) * scale,
      static_cast<float>(-event.scrollingDeltaY) * scale,
  });
}

@end

@implementation HuxerUIApplicationDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender {
  static_cast<void>(sender);
  return YES;
}

@end
