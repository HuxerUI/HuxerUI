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
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/gesture.h>
#include <huxerui/macos/platform_registry.h>

#include "appkit_accessibility.h"
#include "appkit_platform_view.h"
#include "appkit_renderer.h"
#include "appkit_system_tray.h"
#include "appkit_text_input.h"
#include "appkit_window_chrome.h"
#include "macos_application_internal.h"
#include "macos_file_internal.h"
#include "macos_http_internal.h"
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

@interface HuxerUIWindow : NSWindow {
@public
  huxerui::detail::MacPlatformAdapter* huxeruiAdapter;
}
@end

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

GestureSettings MacGestureDefaults() noexcept {
  GestureSettings settings;
  const double double_click_interval = [NSEvent doubleClickInterval];
  if (std::isfinite(double_click_interval) && double_click_interval > 0.0) {
    settings.multi_tap_interval = std::chrono::duration<double>{double_click_interval};
  }
  return settings;
}

class MacPlatformAdapter final : public PlatformAdapter, public PlatformClipboard, public PlatformResources {
public:
  MacPlatformAdapter()
      : PlatformAdapter([](std::function<void()> task) {
          dispatch_async(dispatch_get_main_queue(), ^{
            try {
              task();
            } catch (...) {
            }
          });
        }) {}

  int Run(const Application& application_definition, const WindowOptions& options) {
    @autoreleasepool {
      NSApplication* application = [NSApplication sharedApplication];
      [application setActivationPolicy:NSApplicationActivationPolicyRegular];

      delegate_ = [[HuxerUIApplicationDelegate alloc] init];
      delegate_->huxeruiAdapter = this;
      application.delegate = delegate_;

      [application finishLaunching];
      ApplicationActivation startup_activation = LaunchActivation{};
      if (!pending_activations_.empty()) {
        startup_activation = std::move(pending_activations_.front());
        pending_activations_.erase(pending_activations_.begin());
      }
      const NSRect frame = NSMakeRect(0.0, 0.0, options.initial_size.width, options.initial_size.height);
      custom_chrome_ = options.chrome_mode == WindowChromeMode::Custom;
      custom_title_bar_height_ = options.title_bar_height;
      NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                NSWindowStyleMaskResizable;
      if (custom_chrome_) {
        style |= NSWindowStyleMaskFullSizeContentView;
      }
      window_ = [[HuxerUIWindow alloc] initWithContentRect:frame
                                                 styleMask:style
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
      window_->huxeruiAdapter = this;
      window_.title = [NSString stringWithUTF8String:options.title.c_str()];
      window_.acceptsMouseMovedEvents = YES;
      window_.delegate = delegate_;
      if (custom_chrome_) {
        window_.titleVisibility = NSWindowTitleHidden;
        window_.titlebarAppearsTransparent = YES;
        window_.movableByWindowBackground = NO;
        if (@available(macOS 11.0, *)) {
          window_.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        }
      }

      Runtime runtime{application_definition, *this, std::move(startup_activation)};
      runtime_ = &runtime;
      for (ApplicationActivation& activation : pending_activations_) {
        runtime.HandleApplicationActivation(std::move(activation));
      }
      pending_activations_.clear();

      view_ = [[HuxerUIView alloc] initWithFrame:frame];
      view_->huxeruiRuntime = runtime_;
      view_->huxeruiAdapter = this;
      [NSNotificationCenter.defaultCenter addObserver:view_
                                             selector:@selector(resourceConfigurationDidChange:)
                                                 name:NSCurrentLocaleDidChangeNotification
                                               object:nil];
      text_input_ = std::make_unique<MacTextInput>(runtime, view_);
      platform_views_ = std::make_unique<AppKitPlatformViews>(renderer_, PlatformRegistry(), runtime, window_);
      accessibility_ = std::make_unique<MacAccessibility>(runtime, view_, *platform_views_);
      frame_scheduler_ = [[HuxerUIFrameScheduler alloc] initWithView:view_];
      window_.contentView = view_;
      [window_ center];
      [window_ makeKeyAndOrderFront:nil];
      [window_ makeFirstResponder:view_];
      runtime_->UpdateResourceConfiguration(Configuration());

      [application activateIgnoringOtherApps:YES];
      UpdateWindowMetrics({
          static_cast<float>(view_.bounds.size.width),
          static_cast<float>(view_.bounds.size.height),
      });
      RequestFrameAt(Now());
      [application run];
      [NSNotificationCenter.defaultCenter removeObserver:view_ name:NSCurrentLocaleDidChangeNotification object:nil];
      view_->huxeruiRuntime = nullptr;
      view_->huxeruiAdapter = nullptr;
      delegate_->huxeruiAdapter = nullptr;
      window_->huxeruiAdapter = nullptr;
      [frame_scheduler_ shutdown];
      frame_scheduler_ = nil;
      scheduled_frame_deadline_.reset();
      committed_frame_ = nullptr;
      accessibility_.reset();
      platform_views_->Shutdown();
      platform_views_.reset();
      runtime_ = nullptr;
      if (failure_) {
        std::rethrow_exception(failure_);
      }
    }
    return 0;
  }

  NSWindow* Window() const noexcept {
    return window_;
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

  GestureSettings GestureDefaults() const noexcept override {
    return MacGestureDefaults();
  }

  void RequestWindowCommand(WindowCommand command) override {
    if (window_ == nil) {
      return;
    }
    switch (command) {
    case WindowCommand::Minimize:
      performing_minimize_ = true;
      [window_ miniaturize:nil];
      break;
    case WindowCommand::Maximize:
      if (![window_ isZoomed]) {
        [window_ zoom:nil];
      }
      break;
    case WindowCommand::Restore:
      if ([window_ isZoomed]) {
        [window_ zoom:nil];
      }
      break;
    case WindowCommand::ToggleMaximize:
      [window_ zoom:nil];
      break;
    case WindowCommand::Close:
      performing_close_ = true;
      [window_ performClose:nil];
      break;
    case WindowCommand::Show:
      [window_ orderFront:nil];
      break;
    case WindowCommand::Hide:
      [window_ orderOut:nil];
      break;
    case WindowCommand::Activate:
      [window_ deminiaturize:nil];
      [window_ makeKeyAndOrderFront:nil];
      [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
      break;
    }
  }

  void RequestApplicationQuit() override {
    [[NSApplication sharedApplication] terminate:nil];
  }

  bool AllowWindowRequest(WindowCommand command) {
    if (command == WindowCommand::Minimize && performing_minimize_) {
      performing_minimize_ = false;
      return true;
    }
    if (command == WindowCommand::Close && performing_close_) {
      performing_close_ = false;
      return true;
    }
    try {
      return runtime_ == nullptr || !runtime_->HandleWindowRequest(command);
    } catch (...) {
      if (!failure_) {
        failure_ = std::current_exception();
      }
      [[NSApplication sharedApplication] terminate:nil];
      return false;
    }
  }

  bool BeginWindowDrag(NSEvent* event) {
    if (!custom_chrome_ || runtime_ == nullptr || view_ == nil || window_ == nil || event == nil) {
      return false;
    }
    const NSPoint point = [view_ convertPoint:event.locationInWindow fromView:nil];
    if (!runtime_->IsWindowDragRegion({static_cast<float>(point.x), static_cast<float>(point.y)})) {
      return false;
    }
    [window_ performWindowDragWithEvent:event];
    return true;
  }

  void UpdateWindowMetrics(Size viewport) {
    if (runtime_ != nullptr) {
      const Size constrained_viewport{
          std::max(0.0F, viewport.width),
          std::max(0.0F, viewport.height),
      };
      runtime_->SetWindowMetrics({
          .viewport = constrained_viewport,
          .title_bar = UpdateTitleBarLayout(constrained_viewport),
      });
    }
  }

  void WindowGeometryChanged() {
    if (view_ != nil) {
      UpdateWindowMetrics({static_cast<float>(view_.bounds.size.width), static_cast<float>(view_.bounds.size.height)});
    }
    InvalidateTextInputGeometry();
  }

  void CommitFrameAndInvalidate() {
    scheduled_frame_deadline_.reset();
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    const bool composition_changed = platform_views_->Commit(view_, commit.render_frame);
    committed_frame_ = &commit.render_frame;
    accessibility_->Commit(commit.semantic_frame);
    if (composition_changed) {
      [view_ setNeedsDisplay:YES];
      frame_state_.MarkPaintPending();
    } else {
      static_cast<void>(InvalidateDamage(commit.render_frame.damage));
    }
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void DrawCommittedFrame(CGContextRef context, NSRect dirty_rect) {
    frame_state_.BeginPaint();
    platform_views_->DrawBase(context, NSRectToCGRect(dirty_rect));
    if (const std::optional<double> deadline = frame_state_.EndPaint(frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*deadline);
    }
  }

  NSView* HitTestPlatformView(Point point) const {
    if (runtime_ == nullptr || platform_views_ == nullptr) {
      return nil;
    }
    return platform_views_->HitTest(point);
  }

  void SynchronizePlatformViewFocus() {
    if (platform_views_ != nullptr && window_ != nil) {
      platform_views_->SynchronizeFocus(window_.firstResponder);
    }
  }

  void SynchronizePlatformViewFocus(NSResponder* responder) {
    if (platform_views_ != nullptr) {
      platform_views_->SynchronizeFocus(responder);
    }
  }

  bool BeginPlatformViewFocusTraversal(NSEvent* event) {
    if (platform_views_ == nullptr || window_ == nil || event == nil || event.type != NSEventTypeKeyDown ||
        event.isARepeat) {
      return false;
    }
    const KeyEvent key_event = MakeMacKeyEvent(event, KeyEventType::Down);
    if (key_event.key != Key::Tab) {
      return false;
    }
    return platform_views_->BeginFocusTraversal(window_.firstResponder, key_event.modifiers.shift);
  }

  void EndPlatformViewFocusTraversal() {
    if (platform_views_ != nullptr) {
      platform_views_->EndFocusTraversal();
    }
  }

  void InvalidateAppKitSurface() {
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
    if (runtime_ != nullptr) {
      runtime_->UpdateApplicationLifecycleState(
          active ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive
      );
    }
    if (text_input_) {
      text_input_->ApplicationActiveChanged(active);
    }
  }

  void ApplicationHiddenChanged(bool hidden) {
    if (runtime_ == nullptr) {
      return;
    }
    runtime_->UpdateApplicationLifecycleState(
        hidden ? ApplicationLifecycleState::Background
               : [NSApplication.sharedApplication isActive] ? ApplicationLifecycleState::Active
                                                            : ApplicationLifecycleState::Inactive
    );
  }

  void OpenURLs(NSArray* urls) noexcept {
    try {
      std::optional<std::vector<ApplicationActivation>> activations = DecodeMacApplicationActivations(urls);
      if (!activations.has_value()) {
        return;
      }
      for (ApplicationActivation& activation : *activations) {
        if (runtime_ == nullptr) {
          pending_activations_.push_back(std::move(activation));
        } else {
          runtime_->HandleApplicationActivation(std::move(activation));
        }
      }
    } catch (...) {
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

  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return CreateMacFileSystem();
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateMacFilePickerTransport([this] { return window_; });
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateMacHttpTransport();
  }

  std::shared_ptr<SystemTrayTransport> CreateSystemTrayTransport() override {
    return std::make_shared<AppKitSystemTrayTransport>();
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
  float SystemTitleBarHeight(Size viewport) const noexcept {
    if (window_ == nil || view_ == nil) {
      return 0.0F;
    }
    const NSRect content_layout = [view_ convertRect:window_.contentLayoutRect fromView:nil];
    const float height = static_cast<float>(NSMinY(content_layout));
    return std::isfinite(height) ? std::clamp(height, 0.0F, viewport.height) : 0.0F;
  }

  std::optional<Rect> SystemTitleBarControlBounds() const noexcept {
    if (window_ == nil || view_ == nil) {
      return std::nullopt;
    }
    NSRect bounds = NSZeroRect;
    bool has_bounds = false;
    const NSWindowButton button_types[] = {
        NSWindowCloseButton,
        NSWindowMiniaturizeButton,
        NSWindowZoomButton,
    };
    for (NSWindowButton button_type : button_types) {
      NSButton* button = [window_ standardWindowButton:button_type];
      if (button == nil || button.superview == nil || [button isHiddenOrHasHiddenAncestor]) {
        continue;
      }
      const NSRect converted = [view_ convertRect:button.bounds fromView:button];
      bounds = has_bounds ? NSUnionRect(bounds, converted) : converted;
      has_bounds = true;
    }
    if (!has_bounds) {
      return std::nullopt;
    }
    return Rect{
        static_cast<float>(bounds.origin.x),
        static_cast<float>(bounds.origin.y),
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height),
    };
  }

  void AlignSystemTitleBarControls(const Rect& control_bounds, float title_bar_height) {
    const float target_y = ResolveMacTitleBarControlOriginY(title_bar_height, control_bounds.height);
    const float delta_y = target_y - control_bounds.y;
    if (std::abs(delta_y) <= 0.01F) {
      return;
    }
    const NSWindowButton button_types[] = {
        NSWindowCloseButton,
        NSWindowMiniaturizeButton,
        NSWindowZoomButton,
    };
    for (NSWindowButton button_type : button_types) {
      NSButton* button = [window_ standardWindowButton:button_type];
      if (button == nil || button.superview == nil || [button isHiddenOrHasHiddenAncestor]) {
        continue;
      }
      NSPoint origin = [view_ convertPoint:button.frame.origin fromView:button.superview];
      origin.y += static_cast<CGFloat>(delta_y);
      [button setFrameOrigin:[button.superview convertPoint:origin fromView:view_]];
    }
  }

  std::optional<WindowTitleBarMetrics> UpdateTitleBarLayout(Size viewport) noexcept {
    if (!custom_chrome_) {
      return std::nullopt;
    }
    const std::optional<Rect> system_controls = SystemTitleBarControlBounds();
    const WindowTitleBarMetrics metrics = ResolveMacTitleBarMetrics(
        custom_title_bar_height_,
        SystemTitleBarHeight(viewport),
        viewport,
        system_controls,
        window_ != nil && [window_ isZoomed]
    );
    if (system_controls.has_value()) {
      AlignSystemTitleBarControls(*system_controls, metrics.height);
    }
    return metrics;
  }

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
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  __strong HuxerUIWindow* window_ = nil;
  __strong HuxerUIView* view_ = nil;
  __strong HuxerUIApplicationDelegate* delegate_ = nil;
  __strong HuxerUIFrameScheduler* frame_scheduler_ = nil;
  std::unique_ptr<MacTextInput> text_input_;
  std::unique_ptr<MacAccessibility> accessibility_;
  std::unique_ptr<AppKitPlatformViews> platform_views_;
  PlatformFrameState frame_state_;
  std::optional<double> scheduled_frame_deadline_;
  std::vector<ApplicationActivation> pending_activations_;
  const RenderFrame* committed_frame_ = nullptr;
  bool performing_minimize_ = false;
  bool performing_close_ = false;
  std::exception_ptr failure_;
};

int RunPlatformApplication(const Application& application) {
  WindowOptions options = application.options.window;
  MacPlatformAdapter platform;
  return platform.Run(application, options);
}

} // namespace huxerui::detail

namespace huxerui::macos::detail {

NSWindow* GetAppKitWindow(PlatformAdapter& adapter) {
  auto* mac_adapter = dynamic_cast<huxerui::detail::MacPlatformAdapter*>(&adapter);
  if (mac_adapter == nullptr || mac_adapter->Window() == nil) {
    throw std::logic_error("HuxerUI macOS platform module requires an owning NSWindow");
  }
  return mac_adapter->Window();
}

} // namespace huxerui::macos::detail

@implementation HuxerUIWindow

- (void)sendEvent:(NSEvent*)event {
  if (huxeruiAdapter == nullptr || !huxeruiAdapter->BeginPlatformViewFocusTraversal(event)) {
    [super sendEvent:event];
    return;
  }
  @try {
    [super sendEvent:event];
  } @finally {
    huxeruiAdapter->EndPlatformViewFocusTraversal();
  }
}

@end

@implementation HuxerUIView

- (BOOL)isFlipped {
  return YES;
}

- (NSView*)hitTest:(NSPoint)point {
  const NSPoint local_point = self.superview == nil ? point : [self convertPoint:point fromView:self.superview];
  if (!NSPointInRect(local_point, self.bounds)) {
    return nil;
  }
  if (huxeruiAdapter != nullptr) {
    NSView* platform_view = huxeruiAdapter->HitTestPlatformView({
        static_cast<float>(local_point.x),
        static_cast<float>(local_point.y),
    });
    if (platform_view != nil) {
      return platform_view;
    }
  }
  return self;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)becomeFirstResponder {
  const BOOL became_first_responder = [super becomeFirstResponder];
  if (became_first_responder && huxeruiAdapter != nullptr) {
    huxeruiAdapter->SynchronizePlatformViewFocus(self);
  }
  return became_first_responder;
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
    huxeruiAdapter->WindowGeometryChanged();
    huxeruiAdapter->CommitFrameAndInvalidate();
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
    huxeruiAdapter->WindowGeometryChanged();
  }
}

- (void)viewDidMoveToSuperview {
  [super viewDidMoveToSuperview];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->WindowGeometryChanged();
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
    huxeruiAdapter->InvalidateAppKitSurface();
    huxeruiAdapter->WindowGeometryChanged();
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
  if (huxeruiAdapter != nullptr && huxeruiAdapter->BeginWindowDrag(event)) {
    return;
  }
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

- (BOOL)windowShouldClose:(NSWindow*)sender {
  static_cast<void>(sender);
  return huxeruiAdapter == nullptr || huxeruiAdapter->AllowWindowRequest(huxerui::WindowCommand::Close);
}

- (BOOL)windowShouldMiniaturize:(NSWindow*)sender {
  static_cast<void>(sender);
  return huxeruiAdapter == nullptr || huxeruiAdapter->AllowWindowRequest(huxerui::WindowCommand::Minimize);
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
  static_cast<void>(application);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->OpenURLs(urls);
  }
}

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

- (void)applicationDidHide:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->ApplicationHiddenChanged(true);
  }
}

- (void)applicationDidUnhide:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->ApplicationHiddenChanged(false);
  }
}

- (void)windowDidMove:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)windowDidUpdate:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->SynchronizePlatformViewFocus();
  }
}

- (void)windowDidResize:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->WindowGeometryChanged();
  }
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
    huxeruiAdapter->WindowGeometryChanged();
  }
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->WindowGeometryChanged();
  }
}

- (void)windowDidExitFullScreen:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->WindowGeometryChanged();
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  static_cast<void>(sender);
  return YES;
}

@end
