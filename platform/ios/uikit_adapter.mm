#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>
#import <mach/mach.h>
#import <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/ios/platform_registry.h>

#include "ios_application_internal.h"
#include "ios_file_internal.h"
#include "ios_http_internal.h"
#include "uikit_accessibility.h"
#include "uikit_platform_view.h"
#include "uikit_renderer.h"
#include "uikit_text_input.h"
#include "uikit_view.h"
#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {
class IosPlatformAdapter;
}

@interface HuxerUIIOSApplicationDelegate : UIResponder <UIApplicationDelegate> {
@public
  huxerui::detail::IosPlatformAdapter* huxeruiAdapter;
}
@end

@interface HuxerUIIOSViewController : UIViewController {
  UIStatusBarStyle _huxerUIStatusBarStyle;
}
- (void)setHuxerUIStatusBarStyle:(UIStatusBarStyle)style;
@end

@interface HuxerUIIOSFrameScheduler : NSObject
- (instancetype)initWithView:(HuxerUIView*)view;
- (void)requestFrameAfter:(double)delaySeconds;
- (void)shutdown;
@end

@implementation HuxerUIIOSFrameScheduler {
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
  _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayLinkDidFire:)];
  _displayLink.paused = YES;
  [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  return self;
}

- (void)requestFrameAfter:(double)delaySeconds {
  const NSUInteger generation = ++_generation;
  if (delaySeconds > 0.0) {
    const auto nanoseconds = static_cast<std::int64_t>(delaySeconds * static_cast<double>(NSEC_PER_SEC));
    __weak HuxerUIIOSFrameScheduler* scheduler = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds), dispatch_get_main_queue(), ^{
      HuxerUIIOSFrameScheduler* strongScheduler = scheduler;
      if (strongScheduler != nil && strongScheduler->_generation == generation) {
        [strongScheduler armForGeneration:generation];
      }
    });
    return;
  }
  [self armForGeneration:generation];
}

- (void)armForGeneration:(NSUInteger)generation {
  if (_generation == generation) {
    _displayLink.paused = NO;
  }
}

- (void)displayLinkDidFire:(CADisplayLink*)displayLink {
  static_cast<void>(displayLink);
  _displayLink.paused = YES;
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

namespace {

double TimevalSeconds(const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

huxerui::PointerDeviceKind PointerKind(UITouch* touch) {
  if (@available(iOS 13.4, *)) {
    if (touch.type == UITouchTypePencil) {
      return huxerui::PointerDeviceKind::Pen;
    }
    if (touch.type == UITouchTypeIndirectPointer) {
      return huxerui::PointerDeviceKind::Mouse;
    }
  }
  return huxerui::PointerDeviceKind::Touch;
}

huxerui::PointerButton UIKitPointerButton(NSInteger button_number) noexcept {
  switch (button_number) {
  case 1:
    return huxerui::PointerButton::Primary;
  case 2:
    return huxerui::PointerButton::Secondary;
  case 3:
    return huxerui::PointerButton::Middle;
  case 4:
    return huxerui::PointerButton::Back;
  case 5:
    return huxerui::PointerButton::Forward;
  default:
    return huxerui::PointerButton::None;
  }
}

huxerui::PointerButton UIKitPressedButtons(UIEvent* event) {
  if (@available(iOS 13.4, *)) {
    huxerui::PointerButton buttons = huxerui::PointerButton::None;
    for (NSInteger button_number = 1; button_number <= 5; ++button_number) {
      if ((event.buttonMask & UIEventButtonMaskForButtonNumber(button_number)) != 0) {
        buttons |= UIKitPointerButton(button_number);
      }
    }
    return buttons;
  }
  return huxerui::PointerButton::None;
}

std::int64_t PointerId(UITouch* touch) {
  return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>((__bridge void*)touch));
}

huxerui::Key TranslateKey(UIKeyboardHIDUsage code) API_AVAILABLE(ios(13.4)) {
  if (code >= UIKeyboardHIDUsageKeyboardA && code <= UIKeyboardHIDUsageKeyboardZ) {
    return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::A) + code - UIKeyboardHIDUsageKeyboardA);
  }
  if (code >= UIKeyboardHIDUsageKeyboardF1 && code <= UIKeyboardHIDUsageKeyboardF12) {
    return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::F1) + code - UIKeyboardHIDUsageKeyboardF1);
  }
  if (code >= UIKeyboardHIDUsageKeyboardF13 && code <= UIKeyboardHIDUsageKeyboardF24) {
    return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::F13) + code - UIKeyboardHIDUsageKeyboardF13);
  }
  switch (code) {
  case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
    return huxerui::Key::Backspace;
  case UIKeyboardHIDUsageKeyboardTab:
    return huxerui::Key::Tab;
  case UIKeyboardHIDUsageKeyboardReturnOrEnter:
  case UIKeyboardHIDUsageKeyboardReturn:
    return huxerui::Key::Enter;
  case UIKeyboardHIDUsageKeyboardEscape:
    return huxerui::Key::Escape;
  case UIKeyboardHIDUsageKeyboardSpacebar:
    return huxerui::Key::Space;
  case UIKeyboardHIDUsageKeyboardInsert:
    return huxerui::Key::Insert;
  case UIKeyboardHIDUsageKeyboardDeleteForward:
    return huxerui::Key::Delete;
  case UIKeyboardHIDUsageKeyboardHome:
    return huxerui::Key::Home;
  case UIKeyboardHIDUsageKeyboardEnd:
    return huxerui::Key::End;
  case UIKeyboardHIDUsageKeyboardPageUp:
    return huxerui::Key::PageUp;
  case UIKeyboardHIDUsageKeyboardPageDown:
    return huxerui::Key::PageDown;
  case UIKeyboardHIDUsageKeyboardLeftArrow:
    return huxerui::Key::ArrowLeft;
  case UIKeyboardHIDUsageKeyboardRightArrow:
    return huxerui::Key::ArrowRight;
  case UIKeyboardHIDUsageKeyboardUpArrow:
    return huxerui::Key::ArrowUp;
  case UIKeyboardHIDUsageKeyboardDownArrow:
    return huxerui::Key::ArrowDown;
  case UIKeyboardHIDUsageKeyboard0:
    return huxerui::Key::Digit0;
  case UIKeyboardHIDUsageKeyboard1:
    return huxerui::Key::Digit1;
  case UIKeyboardHIDUsageKeyboard2:
    return huxerui::Key::Digit2;
  case UIKeyboardHIDUsageKeyboard3:
    return huxerui::Key::Digit3;
  case UIKeyboardHIDUsageKeyboard4:
    return huxerui::Key::Digit4;
  case UIKeyboardHIDUsageKeyboard5:
    return huxerui::Key::Digit5;
  case UIKeyboardHIDUsageKeyboard6:
    return huxerui::Key::Digit6;
  case UIKeyboardHIDUsageKeyboard7:
    return huxerui::Key::Digit7;
  case UIKeyboardHIDUsageKeyboard8:
    return huxerui::Key::Digit8;
  case UIKeyboardHIDUsageKeyboard9:
    return huxerui::Key::Digit9;
  case UIKeyboardHIDUsageKeyboardGraveAccentAndTilde:
    return huxerui::Key::Backquote;
  case UIKeyboardHIDUsageKeyboardHyphen:
    return huxerui::Key::Minus;
  case UIKeyboardHIDUsageKeyboardEqualSign:
    return huxerui::Key::Equal;
  case UIKeyboardHIDUsageKeyboardOpenBracket:
    return huxerui::Key::BracketLeft;
  case UIKeyboardHIDUsageKeyboardCloseBracket:
    return huxerui::Key::BracketRight;
  case UIKeyboardHIDUsageKeyboardBackslash:
    return huxerui::Key::Backslash;
  case UIKeyboardHIDUsageKeyboardSemicolon:
    return huxerui::Key::Semicolon;
  case UIKeyboardHIDUsageKeyboardQuote:
    return huxerui::Key::Quote;
  case UIKeyboardHIDUsageKeyboardComma:
    return huxerui::Key::Comma;
  case UIKeyboardHIDUsageKeyboardPeriod:
    return huxerui::Key::Period;
  case UIKeyboardHIDUsageKeyboardSlash:
    return huxerui::Key::Slash;
  case UIKeyboardHIDUsageKeyboardNonUSBackslash:
    return huxerui::Key::IntlBackslash;
  case UIKeyboardHIDUsageKeyboardInternational1:
    return huxerui::Key::IntlRo;
  case UIKeyboardHIDUsageKeyboardInternational3:
    return huxerui::Key::IntlYen;
  case UIKeyboardHIDUsageKeyboardLeftShift:
    return huxerui::Key::ShiftLeft;
  case UIKeyboardHIDUsageKeyboardRightShift:
    return huxerui::Key::ShiftRight;
  case UIKeyboardHIDUsageKeyboardLeftControl:
    return huxerui::Key::ControlLeft;
  case UIKeyboardHIDUsageKeyboardRightControl:
    return huxerui::Key::ControlRight;
  case UIKeyboardHIDUsageKeyboardLeftAlt:
    return huxerui::Key::AltLeft;
  case UIKeyboardHIDUsageKeyboardRightAlt:
    return huxerui::Key::AltRight;
  case UIKeyboardHIDUsageKeyboardLeftGUI:
    return huxerui::Key::MetaLeft;
  case UIKeyboardHIDUsageKeyboardRightGUI:
    return huxerui::Key::MetaRight;
  case UIKeyboardHIDUsageKeyboardCapsLock:
    return huxerui::Key::CapsLock;
  case UIKeyboardHIDUsageKeypadNumLock:
    return huxerui::Key::NumLock;
  case UIKeyboardHIDUsageKeyboardScrollLock:
    return huxerui::Key::ScrollLock;
  case UIKeyboardHIDUsageKeyboardPrintScreen:
    return huxerui::Key::PrintScreen;
  case UIKeyboardHIDUsageKeyboardPause:
    return huxerui::Key::Pause;
  case UIKeyboardHIDUsageKeyboardApplication:
    return huxerui::Key::ContextMenu;
  case UIKeyboardHIDUsageKeyboardHelp:
    return huxerui::Key::Help;
  case UIKeyboardHIDUsageKeypad0:
    return huxerui::Key::Numpad0;
  case UIKeyboardHIDUsageKeypad1:
    return huxerui::Key::Numpad1;
  case UIKeyboardHIDUsageKeypad2:
    return huxerui::Key::Numpad2;
  case UIKeyboardHIDUsageKeypad3:
    return huxerui::Key::Numpad3;
  case UIKeyboardHIDUsageKeypad4:
    return huxerui::Key::Numpad4;
  case UIKeyboardHIDUsageKeypad5:
    return huxerui::Key::Numpad5;
  case UIKeyboardHIDUsageKeypad6:
    return huxerui::Key::Numpad6;
  case UIKeyboardHIDUsageKeypad7:
    return huxerui::Key::Numpad7;
  case UIKeyboardHIDUsageKeypad8:
    return huxerui::Key::Numpad8;
  case UIKeyboardHIDUsageKeypad9:
    return huxerui::Key::Numpad9;
  case UIKeyboardHIDUsageKeypadPeriod:
    return huxerui::Key::NumpadDecimal;
  case UIKeyboardHIDUsageKeypadSlash:
    return huxerui::Key::NumpadDivide;
  case UIKeyboardHIDUsageKeypadAsterisk:
    return huxerui::Key::NumpadMultiply;
  case UIKeyboardHIDUsageKeypadHyphen:
    return huxerui::Key::NumpadSubtract;
  case UIKeyboardHIDUsageKeypadPlus:
    return huxerui::Key::NumpadAdd;
  case UIKeyboardHIDUsageKeypadEnter:
    return huxerui::Key::NumpadEnter;
  case UIKeyboardHIDUsageKeypadEqualSign:
    return huxerui::Key::NumpadEqual;
  case UIKeyboardHIDUsageKeypadComma:
    return huxerui::Key::NumpadComma;
  case UIKeyboardHIDUsageKeyboardClear:
    return huxerui::Key::NumpadClear;
  default:
    return huxerui::Key::Unknown;
  }
}

huxerui::Key TranslateKey(UIKey* key) API_AVAILABLE(ios(13.4)) {
  NSString* characters = key.charactersIgnoringModifiers;
  if (characters.length == 1) {
    const unichar character = [characters characterAtIndex:0];
    if (character >= 'a' && character <= 'z') {
      return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::A) + character - 'a');
    }
    if (character >= 'A' && character <= 'Z') {
      return static_cast<huxerui::Key>(static_cast<int>(huxerui::Key::A) + character - 'A');
    }
  }
  return TranslateKey(key.keyCode);
}

huxerui::KeyEvent MakeKeyEvent(UIPress* press, huxerui::KeyEventType type) {
  if (@available(iOS 13.4, *)) {
    UIKey* key = press.key;
    if (key != nil) {
      const char* characters = key.characters == nil ? nullptr : key.characters.UTF8String;
      const UIKeyModifierFlags flags = key.modifierFlags;
      return {
          type,
          TranslateKey(key),
          type == huxerui::KeyEventType::Down && characters != nullptr ? std::string{characters} : std::string{},
          {
              static_cast<bool>(flags & UIKeyModifierShift),
              static_cast<bool>(flags & UIKeyModifierControl),
              static_cast<bool>(flags & UIKeyModifierAlternate),
              static_cast<bool>(flags & UIKeyModifierCommand),
          },
      };
    }
  }
  return {.type = type};
}

} // namespace

namespace huxerui::detail {

class IosPlatformAdapter final : public PlatformAdapter, public PlatformClipboard, public PlatformResources {
public:
  explicit IosPlatformAdapter(const Application& application)
      : PlatformAdapter([](std::function<void()> task) {
          dispatch_async(dispatch_get_main_queue(), ^{
            try {
              task();
            } catch (...) {
            }
          });
        }),
        application_(application) {}

  int Run() {
    if (active_adapter_ != nullptr) {
      throw std::logic_error("HuxerUI iOS application is already running");
    }
    active_adapter_ = this;
    char application_name[] = "huxerui";
    char* arguments[] = {application_name, nullptr};
    const int result = UIApplicationMain(1, arguments, nil, NSStringFromClass(HuxerUIIOSApplicationDelegate.class));
    active_adapter_ = nullptr;
    return result;
  }

  bool FinishLaunching(NSDictionary* launch_options) noexcept {
    if (runtime_) {
      return false;
    }

    if (launch_options[UIApplicationLaunchOptionsURLKey] != nil) {
      return true;
    }
    return StartRuntime(LaunchActivation{});
  }

  bool StartRuntime(ApplicationActivation startup_activation) noexcept {
    try {
      window_ = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
      view_controller_ = [[HuxerUIIOSViewController alloc] init];
      view_controller_.view.backgroundColor = UIColor.systemBackgroundColor;

      view_ = [[HuxerUIView alloc] initWithFrame:CGRectZero];
      view_.translatesAutoresizingMaskIntoConstraints = NO;
      view_->huxeruiAdapter = this;
      [view_controller_.view addSubview:view_];
      [NSLayoutConstraint activateConstraints:@[
        [view_.leadingAnchor constraintEqualToAnchor:view_controller_.view.leadingAnchor],
        [view_.trailingAnchor constraintEqualToAnchor:view_controller_.view.trailingAnchor],
        [view_.topAnchor constraintEqualToAnchor:view_controller_.view.topAnchor],
        [view_.bottomAnchor constraintEqualToAnchor:view_controller_.view.bottomAnchor],
      ]];

      window_.rootViewController = view_controller_;
      [view_controller_.view layoutIfNeeded];

      runtime_ = std::make_unique<Runtime>(application_, *this, std::move(startup_activation));
      runtime_->UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
      view_->huxeruiRuntime = runtime_.get();
      platform_views_ = std::make_unique<UIKitPlatformViews>(renderer_, PlatformRegistry(), *runtime_);

      [window_ makeKeyAndVisible];
      [view_controller_.view layoutIfNeeded];

      text_input_ = std::make_unique<UIKitTextInput>(*runtime_, view_);
      accessibility_ =
          std::make_unique<UIKitAccessibility>(*runtime_, view_, *platform_views_, text_input_->Responder());
      frame_scheduler_ = [[HuxerUIIOSFrameScheduler alloc] initWithView:view_];

      NSNotificationCenter* notifications = NSNotificationCenter.defaultCenter;
      [notifications addObserver:view_
                        selector:@selector(huxeruiKeyboardFrameDidChange:)
                            name:UIKeyboardWillChangeFrameNotification
                          object:nil];
      [notifications addObserver:view_
                        selector:@selector(huxeruiKeyboardFrameDidChange:)
                            name:UIKeyboardWillHideNotification
                          object:nil];
      [notifications addObserver:view_
                        selector:@selector(huxeruiResourceConfigurationDidChange:)
                            name:NSCurrentLocaleDidChangeNotification
                          object:nil];

      runtime_->UpdateResourceConfiguration(Configuration());
      Resize(view_.bounds.size);
      RequestFrameAt(Now());
      return true;
    } catch (...) {
      Shutdown();
      return false;
    }
  }

  void Shutdown() {
    [NSNotificationCenter.defaultCenter removeObserver:view_];
    [frame_scheduler_ shutdown];
    frame_scheduler_ = nil;
    if (accessibility_) {
      accessibility_->Shutdown();
      accessibility_.reset();
    }
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    if (view_ != nil) {
      view_->huxeruiRuntime = nullptr;
      view_->huxeruiAdapter = nullptr;
    }
    text_input_.reset();
    scheduled_frame_deadline_.reset();
    view_ = nil;
    view_controller_ = nil;
    window_ = nil;
    runtime_.reset();
  }

  static IosPlatformAdapter* Active() noexcept {
    return active_adapter_;
  }

  UIViewController* ViewController() const noexcept {
    return view_controller_;
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

  void SetSystemBarsContentBrightness(
      SystemBarContentBrightness status_bar, SystemBarContentBrightness navigation_bar
  ) override {
    static_cast<void>(navigation_bar);
    if (view_controller_ == nil) {
      return;
    }
    const UIStatusBarStyle style =
        status_bar == SystemBarContentBrightness::Light ? UIStatusBarStyleLightContent : UIStatusBarStyleDarkContent;
    [view_controller_ setHuxerUIStatusBarStyle:style];
  }

  void Resize(CGSize size) {
    viewport_size_ = size;
    ApplyViewport();
  }

  void KeyboardFrameChanged(NSNotification* notification) {
    keyboard_frame_.reset();
    NSValue* value = notification.userInfo[UIKeyboardFrameEndUserInfoKey];
    if (value != nil && view_ != nil && view_.window != nil && notification.name != UIKeyboardWillHideNotification) {
      keyboard_frame_ = [view_ convertRect:value.CGRectValue fromView:nil];
    }
    ApplyViewport();
  }

  void CommitFrameAndInvalidate() {
    scheduled_frame_deadline_.reset();
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    const bool composition_changed = platform_views_->Commit(view_, commit.render_frame);
    accessibility_->Commit(commit.semantic_frame, composition_changed);
    if (composition_changed) {
      [view_ setNeedsDisplay];
      frame_state_.MarkPaintPending();
    } else {
      static_cast<void>(InvalidateDamage(commit.render_frame.damage));
    }
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void DrawCommittedFrame(CGContextRef context, CGRect dirty_rect) {
    frame_state_.BeginPaint();
    platform_views_->DrawBase(context, dirty_rect);
    if (const std::optional<double> deadline = frame_state_.EndPaint(frame_scheduler_ != nil && view_ != nil)) {
      ScheduleFrame(*deadline);
    }
  }

  void UpdateResourceConfiguration() {
    if (runtime_ != nullptr) {
      runtime_->UpdateResourceConfiguration(Configuration());
    }
  }

  void InvalidateTextInputGeometry() {
    if (text_input_ && text_input_->IsActive()) {
      [view_ setNeedsLayout];
    }
  }

  void CancelActiveTouches() {
    [view_ cancelHuxerUITouches];
  }

  void UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state) {
    if (runtime_) {
      runtime_->UpdateApplicationLifecycleState(lifecycle_state);
    }
  }

  void ApplicationDidBecomeActive() {
    UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  }

  bool OpenURL(NSURL* url, NSDictionary<UIApplicationOpenURLOptionsKey, id>* options) noexcept {
    try {
      NSNumber* open_in_place = options[UIApplicationOpenURLOptionsOpenInPlaceKey];
      std::optional<ApplicationActivation> activation =
          DecodeIosApplicationActivation(url, open_in_place == nil || !open_in_place.boolValue);
      if (!activation.has_value()) {
        if (!runtime_) {
          StartRuntime(LaunchActivation{});
        }
        return false;
      }
      if (!runtime_) {
        return StartRuntime(std::move(*activation));
      }
      runtime_->HandleApplicationActivation(std::move(*activation));
      return true;
    } catch (...) {
      return false;
    }
  }

  UIView* HitTestPlatformView(Point point, UIEvent* event) const {
    if (runtime_ == nullptr || platform_views_ == nullptr) {
      return nil;
    }
    return platform_views_->HitTest(point, event);
  }

  void ClearPlatformViewFocus() {
    if (platform_views_ != nullptr) {
      platform_views_->ClearFocus();
    }
  }

  NSArray* AccessibilityElements() const noexcept {
    return accessibility_ ? accessibility_->Elements() : @[];
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

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return CreateIosFileSystem();
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateIosFilePickerTransport([this] { return view_controller_; });
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateIosHttpTransport();
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override {
    return CreateIosPermissionTransport();
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    mach_task_basic_info_data_t task_metrics{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task_metrics), &count) !=
        KERN_SUCCESS) {
      return std::nullopt;
    }
    return ProcessMetrics{
        .cpu_time_seconds = TimevalSeconds(usage.ru_utime) + TimevalSeconds(usage.ru_stime),
        .memory_usage_bytes = static_cast<std::uint64_t>(task_metrics.resident_size),
        .processor_count = static_cast<std::uint32_t>(std::max<NSInteger>(1, NSProcessInfo.processInfo.processorCount)),
    };
  }

  ResourceConfiguration Configuration() const override {
    NSString* language = NSLocale.preferredLanguages.firstObject;
    const char* language_tag = language == nil ? nullptr : language.UTF8String;
    Locale locale = language_tag == nullptr ? Locale::Default() : Locale::FromLanguageTag(language_tag);
    UIScreen* screen = view_.window.screen == nil ? UIScreen.mainScreen : view_.window.screen;
    const float scale = screen == nil ? 1.0F : static_cast<float>(screen.scale);
    return {std::move(locale), scale};
  }

  RawAsset Read(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI iOS resource path is invalid");
    }
    NSString* relative = [[NSString alloc] initWithBytes:package_path.data()
                                                  length:package_path.size()
                                                encoding:NSUTF8StringEncoding];
    if (relative == nil) {
      throw std::logic_error("HuxerUI iOS resource path is not valid UTF-8");
    }
    NSURL* root = [NSBundle.mainBundle.bundleURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
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

  std::optional<std::string> ReadText() override {
    NSString* text = UIPasteboard.generalPasteboard.string;
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
    UIPasteboard.generalPasteboard.string = value;
    return true;
  }

private:
  void ApplyViewport() {
    if (runtime_ == nullptr) {
      return;
    }
    float width = std::max(0.0F, static_cast<float>(viewport_size_.width));
    float height = std::max(0.0F, static_cast<float>(viewport_size_.height));
    bool keyboard_occludes_bottom = false;
    if (keyboard_frame_.has_value() && !CGRectIsEmpty(*keyboard_frame_)) {
      const float keyboard_top = std::max(0.0F, static_cast<float>(CGRectGetMinY(*keyboard_frame_)));
      keyboard_occludes_bottom = keyboard_top < height;
      height = std::min(height, keyboard_top);
    }
    const UIEdgeInsets safe_area = view_ == nil ? UIEdgeInsetsZero : view_.safeAreaInsets;
    runtime_->SetWindowMetrics({
        .viewport = {width, height},
        .safe_area = {
            .top = std::max(0.0F, static_cast<float>(safe_area.top)),
            .right = std::max(0.0F, static_cast<float>(safe_area.right)),
            .bottom = keyboard_occludes_bottom ? 0.0F : std::max(0.0F, static_cast<float>(safe_area.bottom)),
            .left = std::max(0.0F, static_cast<float>(safe_area.left)),
        },
    });
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
      [view_ setNeedsDisplay];
      frame_state_.MarkPaintPending();
      return true;
    }
    bool invalidated = false;
    const CGFloat scale = std::max<CGFloat>(1.0, view_.contentScaleFactor);
    for (const Rect& rect : damage.rects) {
      if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
          !std::isfinite(rect.height)) {
        [view_ setNeedsDisplay];
        frame_state_.MarkPaintPending();
        return true;
      }
      if (rect.IsEmpty()) {
        continue;
      }
      CGRect dirty = CGRectIntersection(CGRectMake(rect.x, rect.y, rect.width, rect.height), view_.bounds);
      if (CGRectIsEmpty(dirty)) {
        continue;
      }
      const CGFloat left = std::floor(CGRectGetMinX(dirty) * scale) / scale;
      const CGFloat top = std::floor(CGRectGetMinY(dirty) * scale) / scale;
      const CGFloat right = std::ceil(CGRectGetMaxX(dirty) * scale) / scale;
      const CGFloat bottom = std::ceil(CGRectGetMaxY(dirty) * scale) / scale;
      [view_ setNeedsDisplayInRect:CGRectMake(left, top, right - left, bottom - top)];
      invalidated = true;
    }
    if (invalidated) {
      frame_state_.MarkPaintPending();
    }
    return invalidated;
  }

  static inline IosPlatformAdapter* active_adapter_ = nullptr;
  const Application& application_;
  UIKitRenderer renderer_;
  std::unique_ptr<Runtime> runtime_;
  __strong UIWindow* window_ = nil;
  __strong HuxerUIIOSViewController* view_controller_ = nil;
  __strong HuxerUIView* view_ = nil;
  __strong HuxerUIIOSFrameScheduler* frame_scheduler_ = nil;
  std::unique_ptr<UIKitTextInput> text_input_;
  std::unique_ptr<UIKitPlatformViews> platform_views_;
  std::unique_ptr<UIKitAccessibility> accessibility_;
  PlatformFrameState frame_state_;
  CGSize viewport_size_ = CGSizeZero;
  std::optional<CGRect> keyboard_frame_;
  std::optional<double> scheduled_frame_deadline_;
};

int RunPlatformApplication(const Application& application) {
  IosPlatformAdapter platform(application);
  return platform.Run();
}

} // namespace huxerui::detail

namespace huxerui::ios::detail {

UIViewController* GetUIKitViewController(PlatformAdapter& adapter) {
  auto* ios_adapter = dynamic_cast<huxerui::detail::IosPlatformAdapter*>(&adapter);
  if (ios_adapter == nullptr || ios_adapter->ViewController() == nil) {
    throw std::logic_error("HuxerUI iOS platform module requires an owning UIViewController");
  }
  return ios_adapter->ViewController();
}

} // namespace huxerui::ios::detail

@implementation HuxerUIView

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self == nil) {
    return nil;
  }
  self.backgroundColor = UIColor.whiteColor;
  self.opaque = YES;
  self.clipsToBounds = YES;
  self.multipleTouchEnabled = YES;
  self.isAccessibilityElement = NO;
  self.contentMode = UIViewContentModeRedraw;
  huxeruiTouches = [[NSMutableSet alloc] init];
  huxeruiPointerButtons = [[NSMutableDictionary alloc] init];
  return self;
}

- (NSArray*)accessibilityElements {
  return huxeruiAdapter == nullptr ? @[] : huxeruiAdapter->AccessibilityElements();
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  if (!CGRectContainsPoint(self.bounds, point)) {
    return nil;
  }
  if (huxeruiAdapter != nullptr) {
    UIView* platform_view =
        huxeruiAdapter->HitTestPlatformView({static_cast<float>(point.x), static_cast<float>(point.y)}, event);
    if (platform_view != nil) {
      return platform_view;
    }
    if (event != nil && event.type == UIEventTypeTouches) {
      huxeruiAdapter->ClearPlatformViewFocus();
    }
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->Resize(self.bounds.size);
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)safeAreaInsetsDidChange {
  [super safeAreaInsetsDidChange];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->Resize(self.bounds.size);
  }
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
    huxeruiAdapter->InvalidateTextInputGeometry();
  }
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
  }
}

- (void)commitHuxerUIFrame {
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->CommitFrameAndInvalidate();
  }
}

- (void)drawRect:(CGRect)rect {
  [super drawRect:rect];
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->DrawCommittedFrame(UIGraphicsGetCurrentContext(), rect);
  }
}

- (void)huxeruiKeyboardFrameDidChange:(NSNotification*)notification {
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->KeyboardFrameChanged(notification);
  }
}

- (void)huxeruiResourceConfigurationDidChange:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateResourceConfiguration();
  }
}

- (void)sendTouches:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event type:(huxerui::PointerEventType)type {
  if (huxeruiRuntime == nullptr) {
    return;
  }
  for (UITouch* touch in touches) {
    const CGPoint point = [touch locationInView:self];
    const huxerui::PointerDeviceKind kind = PointerKind(touch);
    const std::int64_t pointer_id = PointerId(touch);
    NSNumber* pointer_key = @(pointer_id);
    const huxerui::PointerButton previous =
        static_cast<huxerui::PointerButton>([huxeruiPointerButtons[pointer_key] unsignedIntValue]);
    huxerui::PointerButton pressed = huxerui::PointerButton::None;
    if (kind == huxerui::PointerDeviceKind::Mouse && event != nil) {
      pressed = UIKitPressedButtons(event);
    } else if (type == huxerui::PointerEventType::Down || type == huxerui::PointerEventType::Move) {
      pressed = huxerui::PointerButton::Primary;
    }
    if (pressed == huxerui::PointerButton::None) {
      [huxeruiPointerButtons removeObjectForKey:pointer_key];
    } else {
      huxeruiPointerButtons[pointer_key] = @(static_cast<std::uint32_t>(pressed));
    }
    const auto send = [&](huxerui::PointerEventType event_type, huxerui::PointerButton changed,
                          huxerui::PointerButton buttons) {
      huxeruiRuntime->HandlePointerEvent({
          event_type,
          pointer_id,
          {static_cast<float>(point.x), static_cast<float>(point.y)},
          kind,
          changed,
          buttons,
      });
    };
    if (type == huxerui::PointerEventType::Cancel) {
      send(type, huxerui::PointerButton::None, huxerui::PointerButton::None);
      continue;
    }

    const std::uint32_t previous_mask = static_cast<std::uint32_t>(previous);
    const std::uint32_t pressed_mask = static_cast<std::uint32_t>(pressed);
    const auto emit_changes = [&](std::uint32_t changed_mask, huxerui::PointerEventType event_type,
                                  bool adding, huxerui::PointerButton& current) {
      for (NSInteger button_number = 1; button_number <= 5; ++button_number) {
        const huxerui::PointerButton button = UIKitPointerButton(button_number);
        const std::uint32_t button_mask = static_cast<std::uint32_t>(button);
        if ((changed_mask & button_mask) == 0) {
          continue;
        }
        current = adding
                      ? current | button
                      : static_cast<huxerui::PointerButton>(static_cast<std::uint32_t>(current) & ~button_mask);
        send(event_type, button, current);
      }
    };
    huxerui::PointerButton current = previous;
    const std::uint32_t removed = previous_mask & ~pressed_mask;
    const std::uint32_t added = pressed_mask & ~previous_mask;
    emit_changes(removed, huxerui::PointerEventType::Up, false, current);
    emit_changes(added, huxerui::PointerEventType::Down, true, current);
    if (removed == 0 && added == 0 && type == huxerui::PointerEventType::Move) {
      send(type, huxerui::PointerButton::None, pressed);
    }
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [huxeruiTouches unionSet:touches];
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Down];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Move];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Up];
  [huxeruiTouches minusSet:touches];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self sendTouches:touches withEvent:event type:huxerui::PointerEventType::Cancel];
  [huxeruiTouches minusSet:touches];
}

- (void)cancelHuxerUITouches {
  [self sendTouches:huxeruiTouches withEvent:nil type:huxerui::PointerEventType::Cancel];
  [huxeruiTouches removeAllObjects];
  [huxeruiPointerButtons removeAllObjects];
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  bool consumed = false;
  if (huxeruiRuntime != nullptr) {
    for (UIPress* press in presses) {
      const huxerui::KeyEvent key_event = MakeKeyEvent(press, huxerui::KeyEventType::Down);
      consumed = huxeruiRuntime->HandleKeyEvent(key_event) || consumed;
    }
  }
  if (!consumed) {
    [super pressesBegan:presses withEvent:event];
  }
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  bool consumed = false;
  if (huxeruiRuntime != nullptr) {
    for (UIPress* press in presses) {
      const huxerui::KeyEvent key_event = MakeKeyEvent(press, huxerui::KeyEventType::Up);
      consumed = huxeruiRuntime->HandleKeyEvent(key_event) || consumed;
    }
  }
  if (!consumed) {
    [super pressesEnded:presses withEvent:event];
  }
}

@end

@implementation HuxerUIIOSViewController

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _huxerUIStatusBarStyle = UIStatusBarStyleDarkContent;
  }
  return self;
}

- (BOOL)prefersStatusBarHidden {
  return NO;
}

- (UIStatusBarStyle)preferredStatusBarStyle {
  return _huxerUIStatusBarStyle;
}

- (void)setHuxerUIStatusBarStyle:(UIStatusBarStyle)style {
  if (_huxerUIStatusBarStyle == style) {
    return;
  }
  _huxerUIStatusBarStyle = style;
  [self setNeedsStatusBarAppearanceUpdate];
}

@end

@implementation HuxerUIIOSApplicationDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  static_cast<void>(application);
  huxeruiAdapter = huxerui::detail::IosPlatformAdapter::Active();
  return huxeruiAdapter != nullptr && huxeruiAdapter->FinishLaunching(launchOptions);
}

- (BOOL)application:(UIApplication*)application
            openURL:(NSURL*)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id>*)options {
  static_cast<void>(application);
  return huxeruiAdapter != nullptr && huxeruiAdapter->OpenURL(url, options);
}

- (void)applicationWillTerminate:(UIApplication*)application {
  static_cast<void>(application);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->Shutdown();
    huxeruiAdapter = nullptr;
  }
}

- (void)applicationWillResignActive:(UIApplication*)application {
  static_cast<void>(application);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState::Inactive);
    huxeruiAdapter->CancelActiveTouches();
  }
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
  static_cast<void>(application);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->ApplicationDidBecomeActive();
  }
}

- (void)applicationDidEnterBackground:(UIApplication*)application {
  static_cast<void>(application);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState::Background);
  }
}

- (void)applicationWillEnterForeground:(UIApplication*)application {
  static_cast<void>(application);
  if (huxeruiAdapter != nullptr) {
    huxeruiAdapter->UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState::Inactive);
  }
}

@end
