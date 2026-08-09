#import <UIKit/UIKit.h>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/app.h>

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

std::int64_t PointerId(UITouch* touch) {
  return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>((__bridge void*)touch));
}

huxerui::Key TranslateKey(UIKeyboardHIDUsage code) API_AVAILABLE(ios(13.4)) {
  switch (code) {
  case UIKeyboardHIDUsageKeyboardTab:
    return huxerui::Key::Tab;
  case UIKeyboardHIDUsageKeyboardReturnOrEnter:
  case UIKeyboardHIDUsageKeyboardReturn:
    return huxerui::Key::Enter;
  case UIKeyboardHIDUsageKeyboardSpacebar:
    return huxerui::Key::Space;
  case UIKeyboardHIDUsageKeyboardEscape:
    return huxerui::Key::Escape;
  case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
    return huxerui::Key::Backspace;
  case UIKeyboardHIDUsageKeyboardDeleteForward:
    return huxerui::Key::Delete;
  case UIKeyboardHIDUsageKeyboardLeftArrow:
    return huxerui::Key::ArrowLeft;
  case UIKeyboardHIDUsageKeyboardRightArrow:
    return huxerui::Key::ArrowRight;
  case UIKeyboardHIDUsageKeyboardUpArrow:
    return huxerui::Key::ArrowUp;
  case UIKeyboardHIDUsageKeyboardDownArrow:
    return huxerui::Key::ArrowDown;
  case UIKeyboardHIDUsageKeyboardHome:
    return huxerui::Key::Home;
  case UIKeyboardHIDUsageKeyboardEnd:
    return huxerui::Key::End;
  case UIKeyboardHIDUsageKeyboardPageUp:
    return huxerui::Key::PageUp;
  case UIKeyboardHIDUsageKeyboardPageDown:
    return huxerui::Key::PageDown;
  case UIKeyboardHIDUsageKeyboardA:
    return huxerui::Key::A;
  case UIKeyboardHIDUsageKeyboardC:
    return huxerui::Key::C;
  case UIKeyboardHIDUsageKeyboardV:
    return huxerui::Key::V;
  case UIKeyboardHIDUsageKeyboardX:
    return huxerui::Key::X;
  case UIKeyboardHIDUsageKeyboardY:
    return huxerui::Key::Y;
  case UIKeyboardHIDUsageKeyboardZ:
    return huxerui::Key::Z;
  case UIKeyboardHIDUsageKeyboardLeftShift:
  case UIKeyboardHIDUsageKeyboardRightShift:
    return huxerui::Key::Shift;
  case UIKeyboardHIDUsageKeyboardLeftControl:
  case UIKeyboardHIDUsageKeyboardRightControl:
    return huxerui::Key::Control;
  case UIKeyboardHIDUsageKeyboardLeftAlt:
  case UIKeyboardHIDUsageKeyboardRightAlt:
    return huxerui::Key::Alt;
  case UIKeyboardHIDUsageKeyboardLeftGUI:
  case UIKeyboardHIDUsageKeyboardRightGUI:
    return huxerui::Key::Meta;
  default:
    return huxerui::Key::Unknown;
  }
}

huxerui::KeyEvent MakeKeyEvent(UIPress* press, huxerui::KeyEventType type) {
  if (@available(iOS 13.4, *)) {
    UIKey* key = press.key;
    if (key != nil) {
      const char* characters = key.characters == nil ? nullptr : key.characters.UTF8String;
      const UIKeyModifierFlags flags = key.modifierFlags;
      return {
          type,
          TranslateKey(key.keyCode),
          characters == nullptr ? std::string{} : std::string{characters},
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

bool TextInputConsumesKey(huxerui::Key key) noexcept {
  switch (key) {
  case huxerui::Key::Tab:
  case huxerui::Key::Enter:
  case huxerui::Key::Escape:
  case huxerui::Key::Backspace:
  case huxerui::Key::Delete:
  case huxerui::Key::ArrowLeft:
  case huxerui::Key::ArrowRight:
  case huxerui::Key::ArrowUp:
  case huxerui::Key::ArrowDown:
  case huxerui::Key::Home:
  case huxerui::Key::End:
  case huxerui::Key::PageUp:
  case huxerui::Key::PageDown:
    return true;
  default:
    return false;
  }
}

} // namespace

namespace huxerui::detail {

class IosPlatformAdapter final : public PlatformAdapter, public PlatformClipboard, public PlatformResources {
public:
  int Run(Runtime& runtime, const AppOptions& options) {
    static_cast<void>(options);
    if (active_adapter_ != nullptr) {
      throw std::logic_error("HuxerUI iOS application is already running");
    }
    runtime_ = &runtime;
    active_adapter_ = this;
    char application_name[] = "huxerui";
    char* arguments[] = {application_name, nullptr};
    const int result = UIApplicationMain(1, arguments, nil, NSStringFromClass(HuxerUIIOSApplicationDelegate.class));
    active_adapter_ = nullptr;
    runtime_ = nullptr;
    return result;
  }

  bool Launch(UIApplication* application) {
    static_cast<void>(application);
    if (runtime_ == nullptr) {
      return false;
    }

    window_ = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    view_controller_ = [[HuxerUIIOSViewController alloc] init];
    view_controller_.view.backgroundColor = UIColor.systemBackgroundColor;

    view_ = [[HuxerUIView alloc] initWithFrame:CGRectZero];
    view_.translatesAutoresizingMaskIntoConstraints = NO;
    view_->huxeruiRuntime = runtime_;
    view_->huxeruiAdapter = this;
    [view_controller_.view addSubview:view_];
    [NSLayoutConstraint activateConstraints:@[
      [view_.leadingAnchor constraintEqualToAnchor:view_controller_.view.leadingAnchor],
      [view_.trailingAnchor constraintEqualToAnchor:view_controller_.view.trailingAnchor],
      [view_.topAnchor constraintEqualToAnchor:view_controller_.view.topAnchor],
      [view_.bottomAnchor constraintEqualToAnchor:view_controller_.view.bottomAnchor],
    ]];

    window_.rootViewController = view_controller_;
    [window_ makeKeyAndVisible];
    [view_controller_.view layoutIfNeeded];

    text_input_ = std::make_unique<UIKitTextInput>(*runtime_, view_);
    view_->huxeruiTextInput = text_input_.get();
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
  }

  void Shutdown() {
    [NSNotificationCenter.defaultCenter removeObserver:view_];
    [frame_scheduler_ shutdown];
    frame_scheduler_ = nil;
    if (view_ != nil) {
      view_->huxeruiTextInput = nullptr;
      view_->huxeruiRuntime = nullptr;
      view_->huxeruiAdapter = nullptr;
    }
    text_input_.reset();
    committed_frame_ = nullptr;
    scheduled_frame_deadline_.reset();
    view_ = nil;
    view_controller_ = nil;
    window_ = nil;
  }

  static IosPlatformAdapter* Active() noexcept {
    return active_adapter_;
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
    committed_frame_ = &commit.render_frame;
    static_cast<void>(InvalidateDamage(commit.render_frame.damage));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void DrawCommittedFrame(CGContextRef context, CGRect dirty_rect) {
    frame_state_.BeginPaint();
    renderer_.Draw(context, dirty_rect, committed_frame_);
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

  bool TextInputActive() const noexcept {
    return text_input_ && text_input_->IsActive();
  }

  void CancelActiveTouches() {
    [view_ cancelHuxerUITouches];
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
  UIKitRenderer renderer_;
  Runtime* runtime_ = nullptr;
  __strong UIWindow* window_ = nil;
  __strong HuxerUIIOSViewController* view_controller_ = nil;
  __strong HuxerUIView* view_ = nil;
  __strong HuxerUIIOSFrameScheduler* frame_scheduler_ = nil;
  std::unique_ptr<UIKitTextInput> text_input_;
  PlatformFrameState frame_state_;
  CGSize viewport_size_ = CGSizeZero;
  std::optional<CGRect> keyboard_frame_;
  std::optional<double> scheduled_frame_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
};

int RunPlatformApp(AppDefinition definition) {
  AppOptions options = definition.options;
  IosPlatformAdapter platform;
  Runtime runtime{std::move(definition), platform};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail

@implementation HuxerUIView

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self == nil) {
    return nil;
  }
  self.backgroundColor = UIColor.whiteColor;
  self.opaque = YES;
  self.multipleTouchEnabled = YES;
  self.contentMode = UIViewContentModeRedraw;
  huxeruiTouches = [[NSMutableSet alloc] init];
  return self;
}

- (BOOL)canBecomeFirstResponder {
  return YES;
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

- (void)sendTouches:(NSSet<UITouch*>*)touches type:(huxerui::PointerEventType)type {
  if (huxeruiRuntime == nullptr) {
    return;
  }
  for (UITouch* touch in touches) {
    const CGPoint point = [touch locationInView:self];
    huxeruiRuntime->HandlePointerEvent({
        type,
        PointerId(touch),
        {static_cast<float>(point.x), static_cast<float>(point.y)},
        PointerKind(touch),
        static_cast<std::uint32_t>(std::max<NSUInteger>(1, touch.tapCount)),
    });
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  static_cast<void>(event);
  [huxeruiTouches unionSet:touches];
  [self sendTouches:touches type:huxerui::PointerEventType::Down];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  static_cast<void>(event);
  [self sendTouches:touches type:huxerui::PointerEventType::Move];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  static_cast<void>(event);
  [self sendTouches:touches type:huxerui::PointerEventType::Up];
  [huxeruiTouches minusSet:touches];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  static_cast<void>(event);
  [self sendTouches:touches type:huxerui::PointerEventType::Cancel];
  [huxeruiTouches minusSet:touches];
}

- (void)cancelHuxerUITouches {
  [self sendTouches:huxeruiTouches type:huxerui::PointerEventType::Cancel];
  [huxeruiTouches removeAllObjects];
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  bool consumed = false;
  if (huxeruiRuntime != nullptr) {
    for (UIPress* press in presses) {
      const huxerui::KeyEvent key_event = MakeKeyEvent(press, huxerui::KeyEventType::Down);
      if (key_event.key != huxerui::Key::Unknown) {
        huxeruiRuntime->HandleKeyEvent(key_event);
        consumed = consumed || (huxeruiAdapter != nullptr && huxeruiAdapter->TextInputActive() &&
                                (TextInputConsumesKey(key_event.key) || !key_event.text.empty()));
      }
    }
  }
  if (!consumed) {
    [super pressesBegan:presses withEvent:event];
  }
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
  if (huxeruiRuntime != nullptr) {
    for (UIPress* press in presses) {
      const huxerui::KeyEvent key_event = MakeKeyEvent(press, huxerui::KeyEventType::Up);
      if (key_event.key != huxerui::Key::Unknown) {
        huxeruiRuntime->HandleKeyEvent(key_event);
      }
    }
  }
  [super pressesEnded:presses withEvent:event];
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
  static_cast<void>(launchOptions);
  huxeruiAdapter = huxerui::detail::IosPlatformAdapter::Active();
  return huxeruiAdapter != nullptr && huxeruiAdapter->Launch(application);
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
    huxeruiAdapter->CancelActiveTouches();
  }
}

@end
