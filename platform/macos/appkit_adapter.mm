#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CADisplayLink.h>
#import <dispatch/dispatch.h>
#import <mach/mach.h>
#import <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/app.h>

#include "appkit_accessibility.h"
#include "appkit_renderer.h"
#include "appkit_text_input.h"
#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {
class MacPlatformAdapter;
}

namespace {

double TimevalSeconds(const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

} // namespace

@interface HuxerUIView : NSView {
@public
  huxerui::Runtime* huxeruiRuntime;
  huxerui::detail::MacPlatformAdapter* huxeruiAdapter;
  NSPoint huxeruiPointerPosition;
  NSTrackingArea* huxeruiTrackingArea;
  NSEventModifierFlags huxeruiModifierFlags;
}
- (void)sendPointerEvent:(NSEvent*)event type:(huxerui::PointerEventType)type;
- (void)sendKeyEvent:(NSEvent*)event type:(huxerui::KeyEventType)type;
- (void)resourceConfigurationDidChange:(NSNotification*)notification;
- (void)cancelPointer;
- (void)commitHuxerUIFrame;
@end

@interface HuxerUIApplicationDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
@public
  huxerui::detail::MacPlatformAdapter* huxeruiAdapter;
}
@end

@interface HuxerUIFrameScheduler : NSObject
- (instancetype)initWithView:(HuxerUIView*)view;
- (void)requestFrameAfter:(double)delaySeconds;
- (void)shutdown;
@end

@interface HuxerUIFrameScheduler ()
- (void)armForGeneration:(NSUInteger)generation;
- (void)display;
@end

@implementation HuxerUIFrameScheduler {
  __weak HuxerUIView* _view;
  __strong CADisplayLink* _displayLink;
  NSUInteger _generation;
}

- (instancetype)initWithView:(HuxerUIView*)view {
  self = [super init];
  if (self == nil) {
    return nil;
  }

  _view = view;
  if (@available(macOS 14.0, *)) {
    _displayLink = [view displayLinkWithTarget:self selector:@selector(displayLinkDidFire:)];
    _displayLink.paused = YES;
    [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  }
  return self;
}

- (void)requestFrameAfter:(double)delaySeconds {
  const NSUInteger generation = ++_generation;
  if (delaySeconds > 0.0) {
    const auto nanoseconds = static_cast<std::int64_t>(delaySeconds * static_cast<double>(NSEC_PER_SEC));
    __weak HuxerUIFrameScheduler* scheduler = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds), dispatch_get_main_queue(), ^{
      HuxerUIFrameScheduler* strongScheduler = scheduler;
      if (strongScheduler != nil && strongScheduler->_generation == generation) {
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

  __weak HuxerUIFrameScheduler* scheduler = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    HuxerUIFrameScheduler* strongScheduler = scheduler;
    if (strongScheduler != nil && strongScheduler->_generation == generation) {
      [strongScheduler display];
    }
  });
}

- (void)displayLinkDidFire:(CADisplayLink*)displayLink {
  displayLink.paused = YES;
  [self display];
}

- (void)display {
  HuxerUIView* view = _view;
  if (view != nil && view.window != nil) {
    [view commitHuxerUIFrame];
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

class MacPlatformAdapter final : public PlatformAdapter, public PlatformClipboard, public PlatformResources {
public:
  int Run(huxerui::Runtime& runtime, const AppOptions& options) {
    @autoreleasepool {
      runtime_ = &runtime;
      NSApplication* application = [NSApplication sharedApplication];
      [application setActivationPolicy:NSApplicationActivationPolicyRegular];

      delegate_ = [[HuxerUIApplicationDelegate alloc] init];
      delegate_->huxeruiAdapter = this;
      application.delegate = delegate_;

      const NSRect frame = NSMakeRect(0.0, 0.0, options.width, options.height);
      const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
      window_ = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
      window_.title = [NSString stringWithUTF8String:options.title.c_str()];
      window_.acceptsMouseMovedEvents = YES;
      window_.delegate = delegate_;

      view_ = [[HuxerUIView alloc] initWithFrame:frame];
      view_->huxeruiRuntime = &runtime;
      view_->huxeruiAdapter = this;
      [NSNotificationCenter.defaultCenter addObserver:view_
                                             selector:@selector(resourceConfigurationDidChange:)
                                                 name:NSCurrentLocaleDidChangeNotification
                                               object:nil];
      text_input_ = std::make_unique<MacTextInput>(runtime, view_);
      accessibility_ = std::make_unique<MacAccessibility>(runtime, view_);
      frame_scheduler_ = [[HuxerUIFrameScheduler alloc] initWithView:view_];
      window_.contentView = view_;
      [window_ center];
      [window_ makeKeyAndOrderFront:nil];
      [window_ makeFirstResponder:view_];
      runtime_->UpdateResourceConfiguration(Configuration());

      [application finishLaunching];
      [application activateIgnoringOtherApps:YES];
      Resize({
          static_cast<float>(view_.bounds.size.width),
          static_cast<float>(view_.bounds.size.height),
      });
      RequestFrameAt(Now());
      [application run];
      [NSNotificationCenter.defaultCenter removeObserver:view_ name:NSCurrentLocaleDidChangeNotification object:nil];
      view_->huxeruiRuntime = nullptr;
      view_->huxeruiAdapter = nullptr;
      delegate_->huxeruiAdapter = nullptr;
      [frame_scheduler_ shutdown];
      frame_scheduler_ = nil;
      scheduled_frame_deadline_.reset();
      committed_frame_ = nullptr;
      accessibility_.reset();
      runtime_ = nullptr;
    }
    return 0;
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled =
            frame_state_.Request(deadline, Now(), frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*scheduled);
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void Resize(Size viewport) {
    if (runtime_ != nullptr) {
      runtime_->SetWindowMetrics({
          .viewport = {std::max(0.0F, viewport.width), std::max(0.0F, viewport.height)},
      });
    }
  }

  void CommitFrameAndInvalidate() {
    scheduled_frame_deadline_.reset();
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    committed_frame_ = &commit.render_frame;
    accessibility_->Commit(commit.semantic_frame);
    static_cast<void>(InvalidateDamage(commit.render_frame.damage));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void DrawCommittedFrame(CGContextRef context, NSRect dirty_rect) {
    frame_state_.BeginPaint();
    renderer_.Draw(context, NSRectToCGRect(dirty_rect), committed_frame_);
    if (const std::optional<double> deadline = frame_state_.EndPaint(frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*deadline);
    }
  }

  void InvalidateNativeSurface() {
    if (view_ != nil) {
      [view_ setNeedsDisplay:YES];
    }
  }

  void UpdateResourceConfiguration() {
    if (runtime_ != nullptr) {
      runtime_->UpdateResourceConfiguration(Configuration());
    }
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  ) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  ) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return text_input_.get();
  }

  NSTextInputContext* InputContext() const noexcept {
    return text_input_ ? text_input_->InputContext() : nil;
  }

  bool HandleTextInputEvent(NSEvent* event) {
    return text_input_ && text_input_->HandleEvent(event);
  }

  void ApplicationActiveChanged(bool active) {
    if (text_input_) {
      text_input_->ApplicationActiveChanged(active);
    }
  }

  void InvalidateTextInputGeometry() {
    if (text_input_) {
      text_input_->InvalidateGeometry();
    }
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    mach_task_basic_info_data_t task_metrics{};
    mach_msg_type_number_t task_metrics_count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(
            mach_task_self(),
            MACH_TASK_BASIC_INFO,
            reinterpret_cast<task_info_t>(&task_metrics),
            &task_metrics_count
        ) != KERN_SUCCESS) {
      return std::nullopt;
    }
    return ProcessMetrics{
        .cpu_time_seconds = TimevalSeconds(usage.ru_utime) + TimevalSeconds(usage.ru_stime),
        .memory_usage_bytes = static_cast<std::uint64_t>(task_metrics.resident_size),
        .processor_count = static_cast<std::uint32_t>(
            std::max<NSInteger>(1, [[NSProcessInfo processInfo] processorCount])
        ),
    };
  }

  ResourceConfiguration Configuration() const override {
    @autoreleasepool {
      NSString* language = NSLocale.preferredLanguages.firstObject;
      const char* language_tag = language == nil ? nullptr : language.UTF8String;
      Locale locale = language_tag == nullptr ? Locale::Default() : Locale::FromLanguageTag(language_tag);
      NSScreen* screen = window_ != nil ? window_.screen : NSScreen.mainScreen;
      const float scale = screen == nil ? 1.0F : static_cast<float>(screen.backingScaleFactor);
      return {std::move(locale), scale};
    }
  }

  RawAsset Read(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI macOS resource path is invalid");
    }
    @autoreleasepool {
      NSString* relative = [[NSString alloc] initWithBytes:package_path.data()
                                                    length:package_path.size()
                                                  encoding:NSUTF8StringEncoding];
      if (relative == nil) {
        throw std::logic_error("HuxerUI macOS resource path is not valid UTF-8");
      }
      NSURL* root = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
      NSData* data = [NSData dataWithContentsOfURL:[root URLByAppendingPathComponent:relative]];
      if (data == nil) {
        return {};
      }
      std::vector<std::byte> bytes(data.length);
      if (!bytes.empty()) {
        std::memcpy(bytes.data(), data.bytes, bytes.size());
      }
      return RawAsset::FromBytes(std::move(bytes));
    }
  }

  std::optional<std::string> ReadText() override {
    NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (text == nil) {
      return std::nullopt;
    }
    const char* utf8 = text.UTF8String;
    return utf8 == nullptr ? std::optional<std::string>{std::string{}} : std::optional<std::string>{utf8};
  }

  bool WriteText(std::string_view text) override {
    NSString* value = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    if (value == nil) {
      return false;
    }
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard setString:value forType:NSPasteboardTypeString] == YES;
  }

  NSArray* AccessibilityRootChildren() {
    return accessibility_ ? accessibility_->RootChildren() : @[];
  }

private:
  void ScheduleFrame(double deadline) {
    if (frame_scheduler_ == nil) {
      return;
    }
    if (scheduled_frame_deadline_.has_value() && *scheduled_frame_deadline_ <= deadline) {
      return;
    }
    scheduled_frame_deadline_ = deadline;
    const double maximum_delay =
        static_cast<double>(std::numeric_limits<std::int64_t>::max()) / static_cast<double>(NSEC_PER_SEC);
    [frame_scheduler_ requestFrameAfter:std::min(std::max(0.0, deadline - Now()), maximum_delay)];
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*deadline);
    }
  }

  bool InvalidateDamage(const DamageRegion& damage) {
    if (view_ == nil) {
      return false;
    }
    if (damage.full) {
      [view_ setNeedsDisplay:YES];
      frame_state_.MarkPaintPending();
      return true;
    }

    bool invalidated = false;
    for (const Rect& rect : damage.rects) {
      if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
          !std::isfinite(rect.height)) {
        [view_ setNeedsDisplay:YES];
        frame_state_.MarkPaintPending();
        return true;
      }
      if (rect.IsEmpty()) {
        continue;
      }
      NSRect dirty_rect = NSIntersectionRect(NSMakeRect(rect.x, rect.y, rect.width, rect.height), view_.bounds);
      if (NSIsEmptyRect(dirty_rect)) {
        continue;
      }
      NSRect backing_rect = [view_ convertRectToBacking:dirty_rect];
      const CGFloat left = std::floor(NSMinX(backing_rect));
      const CGFloat top = std::floor(NSMinY(backing_rect));
      const CGFloat right = std::ceil(NSMaxX(backing_rect));
      const CGFloat bottom = std::ceil(NSMaxY(backing_rect));
      backing_rect = NSMakeRect(left, top, right - left, bottom - top);
      [view_ setNeedsDisplayInRect:[view_ convertRectFromBacking:backing_rect]];
      invalidated = true;
    }
    if (invalidated) {
      frame_state_.MarkPaintPending();
    }
    return invalidated;
  }

  AppKitRenderer renderer_;
  Runtime* runtime_ = nullptr;
  __strong NSWindow* window_ = nil;
  __strong HuxerUIView* view_ = nil;
  __strong HuxerUIApplicationDelegate* delegate_ = nil;
  __strong HuxerUIFrameScheduler* frame_scheduler_ = nil;
  std::unique_ptr<MacTextInput> text_input_;
  std::unique_ptr<MacAccessibility> accessibility_;
  PlatformFrameState frame_state_;
  std::optional<double> scheduled_frame_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
};

int RunPlatformApp(AppDefinition definition) {
  AppOptions options = definition.options;
  MacPlatformAdapter platform;
  Runtime runtime{std::move(definition), platform};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail

@implementation HuxerUIView

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)isAccessibilityElement {
  return NO;
}

- (NSArray*)accessibilityChildren {
  return huxeruiAdapter == nullptr ? @[] : huxeruiAdapter->AccessibilityRootChildren();
}

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->Resize({
        static_cast<float>(newSize.width),
        static_cast<float>(newSize.height),
    });
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)setFrameOrigin:(NSPoint)newOrigin {
  [super setFrameOrigin:newOrigin];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)setBoundsOrigin:(NSPoint)newOrigin {
  [super setBoundsOrigin:newOrigin];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)viewDidMoveToSuperview {
  [super viewDidMoveToSuperview];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
    huxeruiAdapter->InvalidateNativeSurface();
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)resourceConfigurationDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
  }
}

- (NSTextInputContext*)inputContext {
  if (huxeruiAdapter != nullptr) {
    NSTextInputContext* context = huxeruiAdapter->InputContext();
    if (context != nil) {
      return context;
    }
  }
  return [super inputContext];
}

- (void)updateTrackingAreas {
  if (huxeruiTrackingArea != nil) {
    [self removeTrackingArea:huxeruiTrackingArea];
  }
  huxeruiTrackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                     options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                                             NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                       owner:self
                                                    userInfo:nil];
  [self addTrackingArea:huxeruiTrackingArea];
  [super updateTrackingAreas];
}

- (void)commitHuxerUIFrame {
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->CommitFrameAndInvalidate();
  }
}

- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  if (huxeruiAdapter == nullptr) {
    return;
  }

  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  huxeruiAdapter->DrawCommittedFrame(context, dirtyRect);
}

- (void)sendPointerEvent:(NSEvent*)event type:(huxerui::PointerEventType)type {
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
      huxerui::PointerDeviceKind::Mouse,
      type == huxerui::PointerEventType::Down ? static_cast<std::uint32_t>(event.clickCount) : 1U,
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

- (void)sendKeyEvent:(NSEvent*)event type:(huxerui::KeyEventType)type {
  if (huxeruiRuntime == nullptr) {
    return;
  }
  huxeruiRuntime->HandleKeyEvent(huxerui::detail::MakeMacKeyEvent(event, type));
}

- (void)mouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  [self sendPointerEvent:event type:huxerui::PointerEventType::Down];
}

- (void)mouseMoved:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Move];
}

- (void)mouseDragged:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Move];
}

- (void)mouseExited:(NSEvent*)event {
  static_cast<void>(event);
  [self cancelPointer];
}

- (void)mouseUp:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Up];
}

- (void)keyDown:(NSEvent*)event {
  if (huxeruiAdapter != nullptr && huxeruiAdapter->HandleTextInputEvent(event)) {
    return;
  }
  [self sendKeyEvent:event type:huxerui::KeyEventType::Down];
}

- (void)keyUp:(NSEvent*)event {
  [self sendKeyEvent:event type:huxerui::KeyEventType::Up];
}

- (void)flagsChanged:(NSEvent*)event {
  const huxerui::KeyEvent key_event = huxerui::detail::MakeMacKeyEvent(event, huxerui::KeyEventType::Down);
  NSEventModifierFlags modifier_flag = 0;
  switch (key_event.key) {
  case huxerui::Key::Shift:
    modifier_flag = NSEventModifierFlagShift;
    break;
  case huxerui::Key::Control:
    modifier_flag = NSEventModifierFlagControl;
    break;
  case huxerui::Key::Alt:
    modifier_flag = NSEventModifierFlagOption;
    break;
  case huxerui::Key::Meta:
    modifier_flag = NSEventModifierFlagCommand;
    break;
  default:
    break;
  }

  const NSEventModifierFlags next_flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
  // HuxerUI collapses left and right modifiers, so only emit the first press and final release.
  const bool was_down = modifier_flag != 0 && (huxeruiModifierFlags & modifier_flag) != 0;
  const bool is_down = modifier_flag != 0 && (next_flags & modifier_flag) != 0;
  huxeruiModifierFlags = next_flags;
  if (was_down == is_down) {
    return;
  }
  [self sendKeyEvent:event type:is_down ? huxerui::KeyEventType::Down : huxerui::KeyEventType::Up];
}

- (void)cancelOperation:(id)sender {
  static_cast<void>(sender);
  [self cancelPointer];
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow {
  if (newWindow == nil) {
    [self cancelPointer];
    huxeruiModifierFlags = 0;
  }
  [super viewWillMoveToWindow:newWindow];
}

- (void)scrollWheel:(NSEvent*)event {
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

- (void)applicationDidBecomeActive:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->ApplicationActiveChanged(true);
  }
}

- (void)applicationDidResignActive:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->ApplicationActiveChanged(false);
  }
}

- (void)windowDidMove:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  static_cast<void>(sender);
  return YES;
}

@end
