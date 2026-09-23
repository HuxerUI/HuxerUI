#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CADisplayLink.h>
#import <UserNotifications/UserNotifications.h>
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
#include "application/platform_frame_internal.h"
#include "resources/resource_internal.h"
#include "text/text_internal.h"
#include "application/window_internal.h"

namespace huxerui::detail {
class MacUiWindow;
class MacRuntime;
}

namespace {

double TimevalSeconds(const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

huxerui::PointerButton MacPointerButton(NSInteger button_number) noexcept {
  switch (button_number) {
  case 0:
    return huxerui::PointerButton::Primary;
  case 1:
    return huxerui::PointerButton::Secondary;
  case 2:
    return huxerui::PointerButton::Middle;
  case 3:
    return huxerui::PointerButton::Back;
  case 4:
    return huxerui::PointerButton::Forward;
  default:
    return huxerui::PointerButton::None;
  }
}

huxerui::PointerButton MacPressedButtons(NSUInteger mask) noexcept {
  huxerui::PointerButton buttons = huxerui::PointerButton::None;
  for (NSInteger index = 0; index < 5; ++index) {
    if ((mask & (static_cast<NSUInteger>(1) << index)) != 0) {
      buttons |= MacPointerButton(index);
    }
  }
  return buttons;
}

NSCursor* MacPointerCursor(huxerui::PointerCursorKind kind) {
  switch (kind) {
  case huxerui::PointerCursorKind::Default:
    return NSCursor.arrowCursor;
  case huxerui::PointerCursorKind::Text:
    return NSCursor.IBeamCursor;
  case huxerui::PointerCursorKind::Hand:
    return NSCursor.pointingHandCursor;
  case huxerui::PointerCursorKind::Crosshair:
    return NSCursor.crosshairCursor;
  case huxerui::PointerCursorKind::Move:
  case huxerui::PointerCursorKind::Grab:
    return NSCursor.openHandCursor;
  case huxerui::PointerCursorKind::Grabbing:
    return NSCursor.closedHandCursor;
  case huxerui::PointerCursorKind::ResizeHorizontal:
    return NSCursor.resizeLeftRightCursor;
  case huxerui::PointerCursorKind::ResizeVertical:
    return NSCursor.resizeUpDownCursor;
  case huxerui::PointerCursorKind::ResizeNorthEastSouthWest:
  case huxerui::PointerCursorKind::ResizeNorthWestSouthEast:
    return NSCursor.crosshairCursor;
  case huxerui::PointerCursorKind::NotAllowed:
    return NSCursor.operationNotAllowedCursor;
  case huxerui::PointerCursorKind::Wait:
    return NSCursor.arrowCursor;
  }
  return NSCursor.arrowCursor;
}

} // namespace

@interface HuxerUIWindow : NSWindow {
@public
  huxerui::detail::MacUiWindow* huxeruiWindow;
}
@end

@interface HuxerUIView : NSView {
@public
  huxerui::detail::MacUiWindow* huxeruiWindow;
  NSPoint huxeruiPointerPosition;
  NSTrackingArea* huxeruiTrackingArea;
  std::uint8_t huxeruiModifierKeys;
  __strong NSCursor* huxeruiPointerCursor;
  std::uint64_t huxeruiFileDropSession;
  BOOL huxeruiFileDropHover;
}
- (void)sendPointerEvent:(NSEvent*)event type:(huxerui::PointerEventType)type;
- (BOOL)sendKeyEvent:(NSEvent*)event type:(huxerui::KeyEventType)type;
- (void)resourceConfigurationDidChange:(NSNotification*)notification;
- (void)cancelPointer;
- (void)setHuxerUIPointerCursor:(NSCursor*)cursor;
- (void)commitHuxerUIFrame;
@end

@interface HuxerUIApplicationDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate,
                                                  UNUserNotificationCenterDelegate> {
@public
  huxerui::detail::MacUiWindow* huxeruiWindow;
  huxerui::detail::MacRuntime* huxeruiApplicationRuntime;
}
- (void)resourceConfigurationDidChange:(NSNotification*)notification;
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

/// Owns AppKit application services and delegate callbacks independently of individual NSWindows.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class MacRuntime final : public Runtime, public PlatformClipboard, public PlatformResources {
public:
  MacRuntime()
      : Runtime([](std::function<void()> task) {
          dispatch_async(dispatch_get_main_queue(), ^{
            try {
              task();
            } catch (...) {
            }
          });
        }) {
    NSApplication* application = NSApplication.sharedApplication;
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    delegate_ = [[HuxerUIApplicationDelegate alloc] init];
    delegate_->huxeruiApplicationRuntime = this;
    application.delegate = delegate_;
    UNUserNotificationCenter.currentNotificationCenter.delegate = delegate_;
    [application finishLaunching];
  }

  ~MacRuntime() override {
    Retire();
    if (UNUserNotificationCenter.currentNotificationCenter.delegate == delegate_) {
      UNUserNotificationCenter.currentNotificationCenter.delegate = nil;
    }
    if (NSApplication.sharedApplication.delegate == delegate_)
      NSApplication.sharedApplication.delegate = nil;
    delegate_->huxeruiApplicationRuntime = nullptr;
    [NSNotificationCenter.defaultCenter removeObserver:delegate_];
  }

  /// Borrows the delegate coordinating AppKit callbacks and the original Runtime.
  /// @return Main-thread delegate retained by this Runtime; callers must not extend its native binding.
  HuxerUIApplicationDelegate* Delegate() const {
    return delegate_;
  }

  /// Removes the first pending non-notification activation for startup.
  /// @return That activation, or LaunchActivation when absent; notifications remain queued for later delivery.
  ApplicationActivation TakeStartupActivation() {
    const auto startup = std::find_if(pending_activations_.begin(), pending_activations_.end(), [](const auto& item) {
      return !std::holds_alternative<NotificationActivation>(item);
    });
    if (startup == pending_activations_.end())
      return LaunchActivation{};
    auto activation = std::move(*startup);
    pending_activations_.erase(startup);
    return activation;
  }

  /// Initializes application services, delivers queued native activations, and starts locale observation.
  /// @param application Declaration that outlives this Runtime; called on the AppKit main thread.
  void Start(const Application& application) {
    InitializeApplication(application, TakeStartupActivation(), LifecycleState());
    for (auto& activation : pending_activations_)
      HandleApplicationActivation(std::move(activation));
    pending_activations_.clear();
    [NSNotificationCenter.defaultCenter addObserver:delegate_
                                           selector:@selector(resourceConfigurationDidChange:)
                                               name:NSCurrentLocaleDidChangeNotification
                                             object:nil];
  }

  /// Reads AppKit process activity independently of HuxerUI window attachments.
  /// @return Background when hidden, otherwise Active or Inactive according to NSApplication activity.
  static ApplicationLifecycleState LifecycleState() {
    NSApplication* application = NSApplication.sharedApplication;
    if (application.hidden) return ApplicationLifecycleState::Background;
    return application.active ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive;
  }

  /// Publishes current AppKit activity while shared services are live, on the main thread.
  void UpdateLifecycleState() {
    if (IsInitialized() && !stopped_) Runtime::UpdateApplicationLifecycleState(LifecycleState());
  }

  /// Refreshes application resource configuration after native locale/display changes, on the main thread.
  void UpdateResourceConfiguration() {
    if (IsInitialized() && !stopped_) Runtime::UpdateResourceConfiguration(Configuration());
  }

  /// Coordinates AppKit termination with asynchronous shared Runtime shutdown.
  /// @return NSTerminateLater while shutdown is pending, NSTerminateNow when stopped, or Cancel before startup.
  NSApplicationTerminateReply ShouldTerminate() {
    if (stopped_) return NSTerminateNow;
    if (!IsInitialized()) return NSTerminateCancel;
    termination_pending_ = true;
    Runtime::RequestShutdown();
    return NSTerminateLater;
  }
  void OpenURLs(NSArray* urls) noexcept {
    try {
      std::optional<std::vector<ApplicationActivation>> activations = DecodeMacApplicationActivations(urls);
      if (!activations.has_value()) {
        return;
      }
      for (ApplicationActivation& activation : *activations) {
        if (!IsInitialized()) {
          pending_activations_.push_back(std::move(activation));
        } else {
          Runtime::HandleApplicationActivation(std::move(activation));
        }
      }
    } catch (...) {
    }
  }

  void HandleNotificationActivation(NotificationActivation activation) noexcept {
    try {
      if (!IsInitialized()) {
        pending_activations_.push_back(std::move(activation));
      } else {
        Runtime::HandleApplicationActivation(std::move(activation));
      }
    } catch (...) {
    }
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<AppDirectories> CreateAppDirectories() override {
    return CreateMacAppDirectories();
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateMacHttpTransport();
  }

  std::shared_ptr<LocalNotificationTransport> CreateLocalNotificationTransport() override {
    return CreateMacLocalNotificationTransport();
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override {
    return CreateMacPermissionTransport();
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
      NSScreen* screen = NSScreen.mainScreen;
      const float scale = screen == nil ? 1.0F : static_cast<float>(screen.backingScaleFactor);
      return {std::move(locale), scale};
    }
  }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
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
      NSURL* url = [root URLByAppendingPathComponent:relative];
      const char* path = url.fileSystemRepresentation;
      if (path == nullptr) {
        throw std::runtime_error("HuxerUI package resource path could not be resolved");
      }
      return OpenPackageFile(std::filesystem::path(path));
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

private:
  void OnRuntimeStopped() override {
    stopped_ = true;
    if (termination_pending_) {
      [NSApplication.sharedApplication replyToApplicationShouldTerminate:YES];
    } else {
      [NSApplication.sharedApplication terminate:nil];
    }
  }

  __strong HuxerUIApplicationDelegate* delegate_ = nil;
  std::vector<ApplicationActivation> pending_activations_;
  bool stopped_ = false;
  bool termination_pending_ = false;
};

/// Owns one AppKit window and its text/rendering state; shared UI state retires before native facilities.
class MacUiWindow final : public UiWindow {
public:
  ~MacUiWindow() override { Shutdown(); }

  int Run(MacRuntime& application_runtime, const WindowOptions& options) {
    @autoreleasepool {
      NSApplication* application = [NSApplication sharedApplication];
      delegate_ = application_runtime.Delegate();
      delegate_->huxeruiWindow = this;
      const Size initial_size = ResolveInitialWindowSize(options);
      const NSRect frame = NSMakeRect(0.0, 0.0, initial_size.width, initial_size.height);
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
      window_->huxeruiWindow = this;
      if (options.minimum_size.has_value()) {
        window_.contentMinSize = NSMakeSize(options.minimum_size->width, options.minimum_size->height);
      }
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

      view_ = [[HuxerUIView alloc] initWithFrame:frame];
      window_.contentView = view_;
      auto configuration = application_runtime.Configuration();
      if (window_.screen != nil) configuration.display_scale = static_cast<float>(window_.screen.backingScaleFactor);
      InitializeWindow(application_runtime, configuration);
      view_->huxeruiWindow = this;
      [view_ registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
      [NSNotificationCenter.defaultCenter addObserver:view_
                                             selector:@selector(resourceConfigurationDidChange:)
                                                 name:NSCurrentLocaleDidChangeNotification
                                               object:nil];
      text_input_ = std::make_unique<MacTextInput>(*this, view_);
      platform_views_ = std::make_unique<AppKitPlatformViews>(renderer_, PlatformRegistry(), *this, window_);
      accessibility_ = std::make_unique<MacAccessibility>(*this, view_, *platform_views_);
      frame_scheduler_ = [[HuxerUIFrameScheduler alloc] initWithView:view_];
      [window_ center];
      [window_ makeKeyAndOrderFront:nil];
      [window_ makeFirstResponder:view_];
      UiWindow::UpdateResourceConfiguration(Configuration());

      [application activateIgnoringOtherApps:YES];
      UpdateWindowLifecycleState();
      UpdateWindowMetrics({
          static_cast<float>(view_.bounds.size.width),
          static_cast<float>(view_.bounds.size.height),
      });
      RequestFrameAt(Now());
      [application run];
      Shutdown();
      if (failure_) {
        std::rethrow_exception(failure_);
      }
    }
    return 0;
  }

  void Shutdown() noexcept {
    Retire();
    [NSNotificationCenter.defaultCenter removeObserver:view_ name:NSCurrentLocaleDidChangeNotification object:nil];
    [view_ unregisterDraggedTypes];
    [view_ draggingExited:nil];
    if (view_ != nil) {
      view_->huxeruiWindow = nullptr;
    }
    if (delegate_ != nil) delegate_->huxeruiWindow = nullptr;
    if (window_ != nil) window_->huxeruiWindow = nullptr;
    [frame_scheduler_ shutdown];
    frame_scheduler_ = nil;
    scheduled_frame_deadline_.reset();
    committed_frame_ = nullptr;
    accessibility_.reset();
    if (platform_views_) platform_views_->Shutdown();
    platform_views_.reset();
    text_input_.reset();
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

  void SetPointerCursor(PointerCursorKind kind) override {
    if (view_ != nil) {
      [view_ setHuxerUIPointerCursor:MacPointerCursor(kind)];
    }
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
    UpdateWindowLifecycleState();
  }

  bool AllowWindowRequest(WindowCommand command) {
    if (command == WindowCommand::Minimize && performing_minimize_) {
      performing_minimize_ = false;
      return true;
    }
    try {
      const bool handled = !(command == WindowCommand::Close && performing_close_) &&
                           IsInitialized() && UiWindow::HandleWindowRequest(command);
      performing_close_ = false;
      if (handled) return false;
      if (command == WindowCommand::Close) {
        ApplicationRuntime().RequestShutdown();
        return false;
      }
      return true;
    } catch (...) {
      if (!failure_) {
        failure_ = std::current_exception();
      }
      [[NSApplication sharedApplication] terminate:nil];
      return false;
    }
  }

  bool BeginWindowDrag(NSEvent* event) {
    if (!custom_chrome_ || !IsInitialized() || view_ == nil || window_ == nil || event == nil) {
      return false;
    }
    const NSPoint point = [view_ convertPoint:event.locationInWindow fromView:nil];
    if (!IsWindowDragRegion({static_cast<float>(point.x), static_cast<float>(point.y)})) {
      return false;
    }
    [window_ performWindowDragWithEvent:event];
    return true;
  }

  void UpdateWindowMetrics(Size viewport) {
    if (IsInitialized()) {
      const Size constrained_viewport{
          std::max(0.0F, viewport.width),
          std::max(0.0F, viewport.height),
      };
      UiWindow::SetWindowMetrics({
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
    if (!IsInitialized() || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = UiWindow::BuildFrame();
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
    if (!IsInitialized() || platform_views_ == nullptr) {
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
    if (IsInitialized()) {
      UiWindow::UpdateResourceConfiguration(Configuration());
    }
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options = {}) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(const huxerui::AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options = {}) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return text_input_.get();
  }

  NSTextInputContext* InputContext() const noexcept {
    return text_input_ ? text_input_->InputContext() : nil;
  }

  bool IsTextInputActive() const noexcept {
    return text_input_ && text_input_->IsActive();
  }

  bool HandleTextInputEvent(NSEvent* event) {
    return text_input_ && text_input_->HandleEvent(event);
  }

  void ApplicationActiveChanged(bool active) {
    UpdateWindowLifecycleState();
    if (text_input_) {
      text_input_->ApplicationActiveChanged(active);
    }
  }

  /// Publishes this NSWindow attachment's visibility, minimization, and key-window state on the main thread.
  void UpdateWindowLifecycleState() {
    if (!IsInitialized()) return;
    const auto state = window_ == nil || !window_.visible || window_.miniaturized || NSApplication.sharedApplication.hidden
        ? WindowLifecycleState::Background
        : window_.keyWindow && NSApplication.sharedApplication.active
            ? WindowLifecycleState::Active : WindowLifecycleState::Inactive;
    UiWindow::UpdateWindowLifecycleState(state);
  }

  void InvalidateTextInputGeometry() {
    if (text_input_) {
      text_input_->InvalidateGeometry();
    }
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateMacFilePickerTransport([this] { return window_; });
  }

  ResourceConfiguration Configuration() const {
    auto configuration = ApplicationRuntime().Resources()->Configuration();
    if (window_.screen != nil) configuration.display_scale = static_cast<float>(window_.screen.backingScaleFactor);
    return configuration;
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
  const RenderFrame* committed_frame_ = nullptr;
  bool performing_minimize_ = false;
  bool performing_close_ = false;
  std::exception_ptr failure_;
};

int RunPlatformApplication(const Application& application) {
  @autoreleasepool {
    MacRuntime runtime;
    runtime.Start(application);
    MacUiWindow window;
    return window.Run(runtime, application.options.window);
  }
}

} // namespace huxerui::detail

namespace huxerui::macos::detail {

NSWindow* GetAppKitWindow(UiWindow& ui_window) {
  auto* mac_window = dynamic_cast<huxerui::detail::MacUiWindow*>(&ui_window);
  if (mac_window == nullptr || mac_window->Window() == nil) {
    throw std::logic_error("HuxerUI macOS platform module requires an owning NSWindow");
  }
  return mac_window->Window();
}

} // namespace huxerui::macos::detail

@implementation HuxerUIWindow

- (void)sendEvent:(NSEvent*)event {
  if (huxeruiWindow == nullptr || !huxeruiWindow->BeginPlatformViewFocusTraversal(event)) {
    [super sendEvent:event];
    return;
  }
  @try {
    [super sendEvent:event];
  } @finally {
    huxeruiWindow->EndPlatformViewFocusTraversal();
  }
}

@end

@implementation HuxerUIView

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  [self draggingExited:nil];
  if (huxeruiWindow == nullptr || !(sender.draggingSourceOperationMask & NSDragOperationCopy)) {
    return NSDragOperationNone;
  }
  const NSPoint position = [self convertPoint:sender.draggingLocation fromView:nil];
  huxeruiFileDropHover = YES;
  ++huxeruiFileDropSession;
  try {
    const bool accepted = huxeruiWindow->HandleFileDragEntered(
        huxeruiFileDropSession, {}, {static_cast<float>(position.x), static_cast<float>(position.y)}
    );
    return accepted ? NSDragOperationCopy : NSDragOperationNone;
  } catch (...) {
    [self draggingExited:nil];
    return NSDragOperationNone;
  }
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
  if (!(sender.draggingSourceOperationMask & NSDragOperationCopy)) {
    [self draggingExited:nil];
    return NSDragOperationNone;
  }
  if (!huxeruiFileDropHover) {
    return [self draggingEntered:sender];
  }
  const NSPoint position = [self convertPoint:sender.draggingLocation fromView:nil];
  try {
    return huxeruiWindow != nullptr && huxeruiWindow->HandleFileDragMoved(
        huxeruiFileDropSession, {}, {static_cast<float>(position.x), static_cast<float>(position.y)}
    ) ? NSDragOperationCopy : NSDragOperationNone;
  } catch (...) {
    [self draggingExited:nil];
    return NSDragOperationNone;
  }
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
  static_cast<void>(sender);
  if (huxeruiFileDropHover && huxeruiWindow != nullptr) {
    huxeruiFileDropHover = NO;
    try {
      huxeruiWindow->HandleFileDragExited(huxeruiFileDropSession);
    } catch (...) {
    }
  }
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  if (huxeruiWindow == nullptr || !huxeruiFileDropHover ||
      !(sender.draggingSourceOperationMask & NSDragOperationCopy)) {
    [self draggingExited:nil];
    return NO;
  }
  const NSPoint position = [self convertPoint:sender.draggingLocation fromView:nil];
  try {
    const bool accepted = huxeruiWindow->HandleFileDrop(
        huxeruiFileDropSession, {}, {static_cast<float>(position.x), static_cast<float>(position.y)},
        huxerui::detail::CaptureMacFileDrop(sender.draggingPasteboard)
    );
    [self draggingExited:nil];
    return accepted;
  } catch (...) {
    [self draggingExited:nil];
    return NO;
  }
}

- (void)draggingEnded:(id<NSDraggingInfo>)sender {
  [self draggingExited:sender];
}

- (BOOL)isFlipped {
  return YES;
}

- (NSView*)hitTest:(NSPoint)point {
  const NSPoint local_point = self.superview == nil ? point : [self convertPoint:point fromView:self.superview];
  if (!NSPointInRect(local_point, self.bounds)) {
    return nil;
  }
  if (huxeruiWindow != nullptr) {
    NSView* platform_view = huxeruiWindow->HitTestPlatformView({
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
  if (became_first_responder && huxeruiWindow != nullptr) {
    huxeruiWindow->SynchronizePlatformViewFocus(self);
  }
  return became_first_responder;
}

- (BOOL)isAccessibilityElement {
  return NO;
}

- (NSArray*)accessibilityChildren {
  return huxeruiWindow == nullptr ? @[] : huxeruiWindow->AccessibilityRootChildren();
}

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->WindowGeometryChanged();
    huxeruiWindow->CommitFrameAndInvalidate();
  }
}

- (void)setFrameOrigin:(NSPoint)newOrigin {
  [super setFrameOrigin:newOrigin];
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->InvalidateTextInputGeometry();
  }
}

- (void)setBoundsOrigin:(NSPoint)newOrigin {
  [super setBoundsOrigin:newOrigin];
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->InvalidateTextInputGeometry();
  }
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (void)viewDidMoveToSuperview {
  [super viewDidMoveToSuperview];
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->UpdateResourceConfiguration();
    huxeruiWindow->InvalidateAppKitSurface();
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (void)resourceConfigurationDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->UpdateResourceConfiguration();
  }
}

- (NSTextInputContext*)inputContext {
  if (huxeruiWindow != nullptr) {
    NSTextInputContext* context = huxeruiWindow->InputContext();
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

- (void)resetCursorRects {
  [super resetCursorRects];
  [self addCursorRect:self.bounds cursor:huxeruiPointerCursor ?: NSCursor.arrowCursor];
}

- (void)setHuxerUIPointerCursor:(NSCursor*)cursor {
  NSCursor* resolved = cursor ?: NSCursor.arrowCursor;
  if (huxeruiPointerCursor == resolved) {
    return;
  }
  huxeruiPointerCursor = resolved;
  [self.window invalidateCursorRectsForView:self];
}

- (void)commitHuxerUIFrame {
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->CommitFrameAndInvalidate();
  }
}

- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  if (huxeruiWindow == nullptr) {
    return;
  }

  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  huxeruiWindow->DrawCommittedFrame(context, dirtyRect);
}

- (void)sendPointerEvent:(NSEvent*)event type:(huxerui::PointerEventType)type {
  if (huxeruiWindow == nullptr) {
    return;
  }

  const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  huxeruiPointerPosition = point;
  huxeruiWindow->HandlePointerEvent({
      type,
      0,
      {
          static_cast<float>(point.x),
          static_cast<float>(point.y),
      },
      huxerui::PointerDeviceKind::Mouse,
      type == huxerui::PointerEventType::Down || type == huxerui::PointerEventType::Up
          ? MacPointerButton(event.buttonNumber)
          : huxerui::PointerButton::None,
      type == huxerui::PointerEventType::Cancel
          ? huxerui::PointerButton::None
          : MacPressedButtons(NSEvent.pressedMouseButtons),
      {(event.modifierFlags & NSEventModifierFlagShift) != 0,
       (event.modifierFlags & NSEventModifierFlagControl) != 0,
       (event.modifierFlags & NSEventModifierFlagOption) != 0,
       (event.modifierFlags & NSEventModifierFlagCommand) != 0},
  });
}

- (void)cancelPointer {
  if (huxeruiWindow == nullptr) {
    return;
  }
  huxeruiWindow->HandlePointerEvent({
      huxerui::PointerEventType::Cancel,
      0,
      {
          static_cast<float>(huxeruiPointerPosition.x),
          static_cast<float>(huxeruiPointerPosition.y),
      },
      huxerui::PointerDeviceKind::Mouse,
      huxerui::PointerButton::None,
      huxerui::PointerButton::None,
  });
}

- (BOOL)sendKeyEvent:(NSEvent*)event type:(huxerui::KeyEventType)type {
  if (huxeruiWindow == nullptr) {
    return NO;
  }
  return huxeruiWindow->HandleKeyEvent(huxerui::detail::MakeMacKeyEvent(event, type));
}

- (void)mouseDown:(NSEvent*)event {
  if (huxeruiWindow != nullptr && huxeruiWindow->BeginWindowDrag(event)) {
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

- (void)rightMouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  [self sendPointerEvent:event type:huxerui::PointerEventType::Down];
}

- (void)rightMouseDragged:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Move];
}

- (void)rightMouseUp:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Up];
}

- (void)otherMouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  [self sendPointerEvent:event type:huxerui::PointerEventType::Down];
}

- (void)otherMouseDragged:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Move];
}

- (void)otherMouseUp:(NSEvent*)event {
  [self sendPointerEvent:event type:huxerui::PointerEventType::Up];
}

- (void)keyDown:(NSEvent*)event {
  if (huxeruiWindow != nullptr && huxeruiWindow->IsTextInputActive()) {
    if (!huxeruiWindow->HandleTextInputEvent(event)) {
      [super keyDown:event];
    }
    return;
  }
  if (![self sendKeyEvent:event type:huxerui::KeyEventType::Down]) {
    [super keyDown:event];
  }
}

- (void)keyUp:(NSEvent*)event {
  if (![self sendKeyEvent:event type:huxerui::KeyEventType::Up]) {
    [super keyUp:event];
  }
}

- (void)flagsChanged:(NSEvent*)event {
  const huxerui::KeyEvent key_event = huxerui::detail::MakeMacKeyEvent(event, huxerui::KeyEventType::Down);
  NSEventModifierFlags modifier_flag = 0;
  std::uint8_t modifier_bit = 0;
  switch (key_event.key) {
  case huxerui::Key::ShiftLeft:
    modifier_bit = 1U << 0U;
    modifier_flag = NSEventModifierFlagShift;
    break;
  case huxerui::Key::ShiftRight:
    modifier_bit = 1U << 1U;
    modifier_flag = NSEventModifierFlagShift;
    break;
  case huxerui::Key::ControlLeft:
    modifier_bit = 1U << 2U;
    modifier_flag = NSEventModifierFlagControl;
    break;
  case huxerui::Key::ControlRight:
    modifier_bit = 1U << 3U;
    modifier_flag = NSEventModifierFlagControl;
    break;
  case huxerui::Key::AltLeft:
    modifier_bit = 1U << 4U;
    modifier_flag = NSEventModifierFlagOption;
    break;
  case huxerui::Key::AltRight:
    modifier_bit = 1U << 5U;
    modifier_flag = NSEventModifierFlagOption;
    break;
  case huxerui::Key::MetaLeft:
    modifier_bit = 1U << 6U;
    modifier_flag = NSEventModifierFlagCommand;
    break;
  case huxerui::Key::MetaRight:
    modifier_bit = 1U << 7U;
    modifier_flag = NSEventModifierFlagCommand;
    break;
  default:
    break;
  }

  if (key_event.key == huxerui::Key::CapsLock) {
    const huxerui::KeyEventType type = (event.modifierFlags & NSEventModifierFlagCapsLock) != 0
                                           ? huxerui::KeyEventType::Down
                                           : huxerui::KeyEventType::Up;
    if (![self sendKeyEvent:event type:type]) {
      [super flagsChanged:event];
    }
    return;
  }
  if (modifier_bit == 0) {
    [super flagsChanged:event];
    return;
  }
  const bool was_down = (huxeruiModifierKeys & modifier_bit) != 0;
  const bool aggregate_down = (event.modifierFlags & modifier_flag) != 0;
  const bool is_down = aggregate_down && !was_down;
  if (is_down) {
    huxeruiModifierKeys |= modifier_bit;
  } else {
    huxeruiModifierKeys &= static_cast<std::uint8_t>(~modifier_bit);
  }
  if (![self sendKeyEvent:event type:is_down ? huxerui::KeyEventType::Down : huxerui::KeyEventType::Up]) {
    [super flagsChanged:event];
  }
}

- (void)cancelOperation:(id)sender {
  static_cast<void>(sender);
  [self cancelPointer];
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow {
  if (newWindow == nil) {
    [self cancelPointer];
    huxeruiModifierKeys = 0;
  }
  [super viewWillMoveToWindow:newWindow];
}

- (void)scrollWheel:(NSEvent*)event {
  if (huxeruiWindow == nullptr) {
    return;
  }

  const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  const float scale = event.hasPreciseScrollingDeltas ? 1.0F : 12.0F;
  const huxerui::Point consumed = huxeruiWindow->HandleScrollInput({
      {
          static_cast<float>(point.x),
          static_cast<float>(point.y),
      },
      static_cast<float>(-event.scrollingDeltaX) * scale,
      static_cast<float>(-event.scrollingDeltaY) * scale,
      {
          (event.modifierFlags & NSEventModifierFlagShift) != 0,
          (event.modifierFlags & NSEventModifierFlagControl) != 0,
          (event.modifierFlags & NSEventModifierFlagOption) != 0,
          (event.modifierFlags & NSEventModifierFlagCommand) != 0,
      },
  });
  if (consumed.x == 0.0F && consumed.y == 0.0F) {
    [super scrollWheel:event];
  }
}

@end

@implementation HuxerUIApplicationDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler {
  static_cast<void>(center);
  completionHandler(huxerui::detail::MacLocalNotificationPresentationOptions(notification.request));
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  static_cast<void>(center);
  std::optional<huxerui::NotificationActivation> activation =
      huxerui::detail::DecodeMacLocalNotificationActivation(response.notification.request, response.actionIdentifier);
  if (!activation.has_value()) {
    completionHandler();
    return;
  }

  if ([NSThread isMainThread]) {
    if (huxeruiApplicationRuntime != nullptr) {
      huxeruiApplicationRuntime->HandleNotificationActivation(std::move(*activation));
    }
    completionHandler();
    return;
  }

  // Retain the complete snapshot across the dispatch boundary, including application data.
  const huxerui::NotificationActivation notification_activation = std::move(*activation);
  dispatch_async(dispatch_get_main_queue(), ^{
    if (huxeruiApplicationRuntime != nullptr) {
      huxeruiApplicationRuntime->HandleNotificationActivation(notification_activation);
    }
    completionHandler();
  });
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  static_cast<void>(sender);
  return huxeruiWindow == nullptr || huxeruiWindow->AllowWindowRequest(huxerui::WindowCommand::Close);
}

- (BOOL)windowShouldMiniaturize:(NSWindow*)sender {
  static_cast<void>(sender);
  return huxeruiWindow == nullptr || huxeruiWindow->AllowWindowRequest(huxerui::WindowCommand::Minimize);
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
  static_cast<void>(application);
  if (huxeruiApplicationRuntime != nullptr) {
    huxeruiApplicationRuntime->OpenURLs(urls);
  }
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiApplicationRuntime != nullptr) huxeruiApplicationRuntime->UpdateLifecycleState();
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->ApplicationActiveChanged(true);
  }
}

- (void)applicationDidResignActive:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiApplicationRuntime != nullptr) huxeruiApplicationRuntime->UpdateLifecycleState();
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->ApplicationActiveChanged(false);
  }
}

- (void)applicationDidHide:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiApplicationRuntime != nullptr) huxeruiApplicationRuntime->UpdateLifecycleState();
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->UpdateWindowLifecycleState();
  }
}

- (void)applicationDidUnhide:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiApplicationRuntime != nullptr) huxeruiApplicationRuntime->UpdateLifecycleState();
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->UpdateWindowLifecycleState();
  }
}

- (void)windowDidMove:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->InvalidateTextInputGeometry();
  }
}

- (void)windowDidUpdate:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->SynchronizePlatformViewFocus();
  }
}

- (void)windowDidResize:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->UpdateResourceConfiguration();
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (void)windowDidExitFullScreen:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) {
    huxeruiWindow->WindowGeometryChanged();
  }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  static_cast<void>(sender);
  return huxeruiApplicationRuntime == nullptr ? NSTerminateNow : huxeruiApplicationRuntime->ShouldTerminate();
}

- (void)resourceConfigurationDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiApplicationRuntime != nullptr) huxeruiApplicationRuntime->UpdateResourceConfiguration();
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) huxeruiWindow->UpdateWindowLifecycleState();
}

- (void)windowDidResignKey:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) huxeruiWindow->UpdateWindowLifecycleState();
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) huxeruiWindow->UpdateWindowLifecycleState();
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiWindow != nullptr) huxeruiWindow->UpdateWindowLifecycleState();
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  static_cast<void>(sender);
  return YES;
}

@end
