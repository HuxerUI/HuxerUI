#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <QuartzCore/CADisplayLink.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "appkit_text_input.h"
#include "internal.h"

namespace huxerui::detail {
class MacPlatformHost;
}

@interface HuxerUIHostView : NSView {
@public
  huxerui::Runtime* huxeruiRuntime;
  huxerui::detail::MacPlatformHost* huxeruiHost;
  NSPoint huxeruiPointerPosition;
  NSTrackingArea* huxeruiTrackingArea;
}
- (void)sendPointerEvent:(NSEvent*)event type:(huxerui::PointerEventType)type;
- (void)sendKeyEvent:(NSEvent*)event type:(huxerui::KeyEventType)type;
- (void)cancelPointer;
- (void)commitHuxerUIFrame;
@end

@interface HuxerUIApplicationDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
@public
  huxerui::detail::MacPlatformHost* huxeruiHost;
}
@end

@interface HuxerUIFrameScheduler : NSObject
- (instancetype)initWithView:(HuxerUIHostView*)view;
- (void)requestFrameAfter:(double)delaySeconds;
- (void)shutdown;
@end

@interface HuxerUIFrameScheduler ()
- (void)armForGeneration:(NSUInteger)generation;
- (void)display;
@end

@implementation HuxerUIFrameScheduler {
  __weak HuxerUIHostView* _view;
  __strong CADisplayLink* _displayLink;
  NSUInteger _generation;
}

- (instancetype)initWithView:(HuxerUIHostView*)view {
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
  HuxerUIHostView* view = _view;
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

namespace {

CFStringRef CreateString(std::string_view text) {
  return CFStringCreateWithBytes(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(text.data()),
      static_cast<CFIndex>(text.size()),
      kCFStringEncodingUTF8,
      false
  );
}

CTFontRef CreateFont(float font_size) {
  return CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, static_cast<CGFloat>(font_size), nullptr);
}

CFAttributedStringRef CreateAttributedString(std::string_view text, float font_size) {
  CFStringRef string = CreateString(text);
  CTFontRef font = CreateFont(font_size);
  const void* keys[] = {
      kCTFontAttributeName,
      kCTForegroundColorFromContextAttributeName,
  };
  const void* values[] = {
      font,
      kCFBooleanTrue,
  };
  CFDictionaryRef attributes = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  );
  CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
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
  CGContextSetRGBFillColor(context, color.red, color.green, color.blue, color.alpha);
}

void SetStrokeColor(CGContextRef context, Color color) {
  CGContextSetRGBStrokeColor(context, color.red, color.green, color.blue, color.alpha);
}

} // namespace

class MacTextLayout final : public TextLayout {
public:
  MacTextLayout(std::string_view text, float font_size, float max_width) {
    string_ = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    CTFontRef font = CreateFont(font_size);
    line_height_ = static_cast<float>(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    CFRelease(font);

    if (!std::isfinite(max_width)) {
      line_ = CreateLine(text, font_size);
      CGFloat ascent = 0.0;
      CGFloat descent = 0.0;
      CGFloat leading = 0.0;
      const double width = CTLineGetTypographicBounds(line_, &ascent, &descent, &leading);
      size_ = {
          std::ceil(static_cast<float>(width)),
          std::ceil(std::max(line_height_, static_cast<float>(ascent + descent + leading))),
      };
      return;
    }

    const float width = std::max(1.0F, max_width);
    std::string layout_text{text};
    if (!layout_text.empty() && layout_text.back() == '\n') {
      layout_text.append("\xE2\x80\x8B");
    }
    CFAttributedStringRef attributed = CreateAttributedString(layout_text, font_size);
    CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
    const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
        framesetter,
        CFRangeMake(0, 0),
        nullptr,
        CGSizeMake(width, CGFLOAT_MAX),
        nullptr
    );
    const float height = std::max(line_height_, std::ceil(static_cast<float>(suggested.height)));
    CGPathRef path = CGPathCreateWithRect(CGRectMake(0.0, 0.0, width, height), nullptr);
    frame_ = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);
    size_ = {std::min(width, std::ceil(static_cast<float>(suggested.width))), height};

    CFArrayRef lines = CTFrameGetLines(frame_);
    const CFIndex count = CFArrayGetCount(lines);
    std::vector<CGPoint> origins(static_cast<std::size_t>(count));
    if (count > 0) {
      CTFrameGetLineOrigins(frame_, CFRangeMake(0, count), origins.data());
    }
    line_records_.reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
      CTLineRef line = static_cast<CTLineRef>(const_cast<void*>(CFArrayGetValueAtIndex(lines, index)));
      CGFloat ascent = 0.0;
      CGFloat descent = 0.0;
      CGFloat leading = 0.0;
      static_cast<void>(CTLineGetTypographicBounds(line, &ascent, &descent, &leading));
      line_records_.push_back({
          line,
          origins[static_cast<std::size_t>(index)],
          CTLineGetStringRange(line),
          height - static_cast<float>(origins[static_cast<std::size_t>(index)].y) - static_cast<float>(ascent),
          std::max(line_height_, static_cast<float>(ascent + descent + leading)),
      });
    }
    if (line_records_.empty()) {
      line_ = CreateLine(layout_text, font_size);
    }

    CGPathRelease(path);
    CFRelease(framesetter);
    CFRelease(attributed);
  }

  ~MacTextLayout() override {
    if (line_ != nullptr) {
      CFRelease(line_);
    }
    if (frame_ != nullptr) {
      CFRelease(frame_);
    }
  }

  Size Measure() const override {
    return size_;
  }

  TextPosition HitTest(Point point) const override {
    CTLineRef line = line_;
    CGPoint origin{};
    CFRange range = CFRangeMake(0, static_cast<CFIndex>(string_.length));
    std::size_t selected_line = 0;
    if (!line_records_.empty()) {
      const LineRecord* selected = &line_records_.front();
      float distance = std::numeric_limits<float>::infinity();
      for (std::size_t index = 0; index < line_records_.size(); ++index) {
        const LineRecord& candidate = line_records_[index];
        const float bottom = candidate.top + candidate.height;
        const float candidate_distance =
            point.y < candidate.top ? candidate.top - point.y : (point.y > bottom ? point.y - bottom : 0.0F);
        if (candidate_distance < distance) {
          selected = &candidate;
          selected_line = index;
          distance = candidate_distance;
        }
      }
      line = selected->line;
      origin = selected->origin;
      range = selected->range;
    }
    CFIndex index = CTLineGetStringIndexForPosition(line, CGPointMake(point.x - origin.x, 0.0));
    if (index == kCFNotFound) {
      index = point.x <= origin.x ? range.location : range.location + range.length;
    }
    const CFIndex line_end = range.location + range.length;
    if (index == line_end && line_end <= static_cast<CFIndex>(string_.length) && range.length > 0 &&
        [string_ characterAtIndex:static_cast<NSUInteger>(line_end - 1)] == '\n') {
      --index;
    }
    const bool upstream = index == line_end && selected_line + 1 < line_records_.size() &&
                          line_records_[selected_line + 1].range.location == line_end;
    return {
        static_cast<TextOffset>(std::clamp<CFIndex>(index, 0, static_cast<CFIndex>(string_.length))),
        upstream ? TextAffinity::Upstream : TextAffinity::Downstream,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const CFIndex index = std::clamp<CFIndex>(static_cast<CFIndex>(offset), 0, static_cast<CFIndex>(string_.length));
    CTLineRef line = line_;
    CGPoint origin{};
    float top = 0.0F;
    float height = line_height_;
    if (!line_records_.empty()) {
      const LineRecord* selected = &line_records_.back();
      for (std::size_t line_index = 0; line_index < line_records_.size(); ++line_index) {
        const LineRecord& candidate = line_records_[line_index];
        const CFIndex start = candidate.range.location;
        const CFIndex end = start + candidate.range.length;
        if (index < end ||
            (index == end && (affinity == TextAffinity::Upstream || line_index + 1 == line_records_.size()))) {
          selected = &candidate;
          break;
        }
      }
      line = selected->line;
      origin = selected->origin;
      top = selected->top;
      height = selected->height;
    }
    CGFloat secondary = 0.0;
    const CGFloat primary = CTLineGetOffsetForStringIndex(line, index, &secondary);
    const CGFloat x = affinity == TextAffinity::Upstream && secondary != primary ? secondary : primary;
    return {
        static_cast<float>(origin.x + x),
        top,
        1.0F,
        std::ceil(height),
    };
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const CFIndex start =
        std::clamp<CFIndex>(static_cast<CFIndex>(range.start), 0, static_cast<CFIndex>(string_.length));
    const CFIndex end =
        std::clamp<CFIndex>(static_cast<CFIndex>(range.end), start, static_cast<CFIndex>(string_.length));
    if (start == end) {
      return {};
    }

    std::vector<Rect> rects;
    auto append_line = [&](CTLineRef line, CFRange line_range, CGPoint origin, float top, float height) {
      const CFIndex line_start = std::max(start, line_range.location);
      const CFIndex line_end = std::min(end, line_range.location + line_range.length);
      if (line_start >= line_end) {
        return;
      }
      CFArrayRef runs = CTLineGetGlyphRuns(line);
      const CFIndex count = CFArrayGetCount(runs);
      for (CFIndex index = 0; index < count; ++index) {
        CTRunRef run = static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs, index)));
        const CFRange run_range = CTRunGetStringRange(run);
        const CFIndex run_start = std::max(line_start, run_range.location);
        const CFIndex run_end = std::min(line_end, run_range.location + run_range.length);
        if (run_start >= run_end) {
          continue;
        }
        const CGFloat first = CTLineGetOffsetForStringIndex(line, run_start, nullptr);
        const CGFloat last = CTLineGetOffsetForStringIndex(line, run_end, nullptr);
        rects.push_back({
            static_cast<float>(origin.x + std::min(first, last)),
            top,
            static_cast<float>(std::abs(last - first)),
            std::ceil(height),
        });
      }
    };
    if (line_records_.empty()) {
      append_line(line_, CFRangeMake(0, static_cast<CFIndex>(string_.length)), {}, 0.0F, line_height_);
    } else {
      for (const LineRecord& line : line_records_) {
        append_line(line.line, line.range, line.origin, line.top, line.height);
      }
    }
    return rects;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const NSUInteger length = string_.length;
    const NSUInteger target = static_cast<NSUInteger>(std::clamp<TextOffset>(offset, 0, length));
    if (target == 0) {
      return 0;
    }
    return static_cast<TextOffset>([string_ rangeOfComposedCharacterSequenceAtIndex:target - 1].location);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const NSUInteger length = string_.length;
    const NSUInteger target = static_cast<NSUInteger>(std::clamp<TextOffset>(offset, 0, length));
    if (target >= length) {
      return static_cast<TextOffset>(length);
    }
    return static_cast<TextOffset>(NSMaxRange([string_ rangeOfComposedCharacterSequenceAtIndex:target]));
  }

private:
  struct LineRecord {
    CTLineRef line = nullptr;
    CGPoint origin;
    CFRange range;
    float top = 0.0F;
    float height = 0.0F;
  };

  __strong NSString* string_ = nil;
  CTLineRef line_ = nullptr;
  CTFrameRef frame_ = nullptr;
  std::vector<LineRecord> line_records_;
  Size size_;
  float line_height_ = 0.0F;
};

class MacPlatformHost final : public PlatformHost, public PlatformClipboard {
public:
  int Run(huxerui::Runtime& runtime, const AppOptions& options) {
    @autoreleasepool {
      runtime_ = &runtime;
      NSApplication* application = [NSApplication sharedApplication];
      [application setActivationPolicy:NSApplicationActivationPolicyRegular];

      delegate_ = [[HuxerUIApplicationDelegate alloc] init];
      delegate_->huxeruiHost = this;
      application.delegate = delegate_;

      const NSRect frame = NSMakeRect(0.0, 0.0, options.width, options.height);
      const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
      window_ = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
      window_.title = [NSString stringWithUTF8String:options.title.c_str()];
      window_.acceptsMouseMovedEvents = YES;
      window_.delegate = delegate_;

      view_ = [[HuxerUIHostView alloc] initWithFrame:frame];
      view_->huxeruiRuntime = &runtime;
      view_->huxeruiHost = this;
      text_input_ = std::make_unique<MacTextInput>(runtime, view_);
      frame_scheduler_ = [[HuxerUIFrameScheduler alloc] initWithView:view_];
      window_.contentView = view_;
      [window_ center];
      [window_ makeKeyAndOrderFront:nil];
      [window_ makeFirstResponder:view_];

      [application finishLaunching];
      [application activateIgnoringOtherApps:YES];
      Resize({
          static_cast<float>(view_.bounds.size.width),
          static_cast<float>(view_.bounds.size.height),
      });
      RequestFrameAt(Now());
      [application run];
      view_->huxeruiRuntime = nullptr;
      view_->huxeruiHost = nullptr;
      delegate_->huxeruiHost = nullptr;
      [frame_scheduler_ shutdown];
      frame_scheduler_ = nil;
      scheduled_frame_deadline_.reset();
      committed_frame_ = nullptr;
      runtime_ = nullptr;
    }
    return 0;
  }

  void RequestFrameAt(double deadline) override {
    frame_build_pending_ = true;
    const double now = Now();
    if (std::isnan(deadline) || deadline <= now) {
      deadline = now;
    } else if (!std::isfinite(deadline)) {
      deadline = std::numeric_limits<double>::max();
    }
    if (paint_pending_ || paint_in_progress_ || frame_scheduler_ == nil || view_ == nil) {
      if (!deferred_frame_deadline_.has_value() || deadline < *deferred_frame_deadline_) {
        deferred_frame_deadline_ = deadline;
      }
      return;
    }
    ScheduleFrame(deadline);
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void Resize(Size viewport) {
    if (runtime_ != nullptr) {
      runtime_->SetViewport({
          std::max(0.0F, viewport.width),
          std::max(0.0F, viewport.height),
      });
    }
  }

  void CommitFrameAndInvalidate() {
    scheduled_frame_deadline_.reset();
    if (!frame_build_pending_ || runtime_ == nullptr) {
      return;
    }
    frame_build_pending_ = false;
    deferred_frame_deadline_.reset();
    const FrameCommit& commit = runtime_->BuildFrame();
    committed_frame_ = &commit.render_frame;
    static_cast<void>(InvalidateDamage(commit.render_frame.damage));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void DrawCommittedFrame(CGContextRef context, NSRect dirty_rect) {
    paint_in_progress_ = true;
    CGContextSaveGState(context);
    CGContextClipToRect(context, NSRectToCGRect(dirty_rect));
    SetFillColor(context, Color::Rgb(247, 248, 250));
    CGContextFillRect(context, NSRectToCGRect(dirty_rect));
    if (committed_frame_ != nullptr && committed_frame_->scene.root != nullptr) {
      RenderSceneNode(*committed_frame_->scene.root, context);
    }
    CGContextRestoreGState(context);
    paint_in_progress_ = false;
    paint_pending_ = false;
    FlushDeferredFrame();
  }

  void InvalidateNativeSurface() {
    if (view_ != nil) {
      [view_ setNeedsDisplay:YES];
    }
  }

  Size MeasureText(std::string_view text, float font_size, float max_width) override {
    if (std::isfinite(max_width)) {
      if (max_width <= 0.0F) {
        return {};
      }
      CFAttributedStringRef attributed = CreateAttributedString(text, font_size);
      CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
      const CGSize suggested = CTFramesetterSuggestFrameSizeWithConstraints(
          framesetter,
          CFRangeMake(0, 0),
          nullptr,
          CGSizeMake(max_width, CGFLOAT_MAX),
          nullptr
      );
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
    const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CFRelease(line);

    return {
        std::ceil(static_cast<float>(width)),
        std::ceil(static_cast<float>(ascent + descent + leading)),
    };
  }

  std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, float font_size, float max_width) override {
    return std::make_unique<MacTextLayout>(text, font_size, max_width);
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

  void RenderSequence(const PaintSequence& sequence, CGContextRef context) {
    for (const PaintCommand& command : sequence.Commands()) {
      std::visit([this, context](const auto& value) { RenderCommand(context, value); }, command);
    }
  }

  void RenderSceneNode(const RenderNode& node, CGContextRef context) {
    const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
    if (!node.visible || opacity <= 0.0F) {
      return;
    }

    Transform2D transform = node.transform;
    transform.translate_x += node.offset.x;
    transform.translate_y += node.offset.y;
    const bool transformed = !transform.IsIdentity();
    if (transformed) {
      RenderCommand(context, PushTransformCommand{transform});
    }

    const bool translucent = opacity < 1.0F;
    if (translucent) {
      CGContextSaveGState(context);
      CGContextSetAlpha(context, opacity);
      CGContextBeginTransparencyLayer(context, nullptr);
    }

    RenderSequence(node.content, context);
    if (node.child_clip.has_value()) {
      RenderCommand(
          context,
          PushClipCommand{
              node.child_clip->rect,
              node.child_clip->corner_radius,
          }
      );
    }
    const bool children_transformed = !node.children_transform.IsIdentity();
    if (children_transformed) {
      RenderCommand(context, PushTransformCommand{node.children_transform});
    }
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        RenderSceneNode(*child, context);
      }
    }
    if (children_transformed) {
      RenderCommand(context, PopTransformCommand{});
    }
    if (node.child_clip.has_value()) {
      RenderCommand(context, PopClipCommand{});
    }
    RenderSequence(node.foreground, context);
    if (translucent) {
      CGContextEndTransparencyLayer(context);
      CGContextRestoreGState(context);
    }
    if (transformed) {
      RenderCommand(context, PopTransformCommand{});
    }
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
    if (paint_pending_ || paint_in_progress_ || !frame_build_pending_ || !deferred_frame_deadline_.has_value() ||
        frame_scheduler_ == nil || view_ == nil) {
      return;
    }
    const double deadline = *deferred_frame_deadline_;
    deferred_frame_deadline_.reset();
    ScheduleFrame(deadline);
  }

  bool InvalidateDamage(const DamageRegion& damage) {
    if (view_ == nil) {
      return false;
    }
    if (damage.full) {
      [view_ setNeedsDisplay:YES];
      paint_pending_ = true;
      return true;
    }

    bool invalidated = false;
    for (const Rect& rect : damage.rects) {
      if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
          !std::isfinite(rect.height)) {
        [view_ setNeedsDisplay:YES];
        paint_pending_ = true;
        return true;
      }
      if (rect.IsEmpty()) {
        continue;
      }
      NSRect dirty_rect =
          NSIntersectionRect(NSMakeRect(rect.x, rect.y, rect.width, rect.height), view_.bounds);
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
    paint_pending_ = paint_pending_ || invalidated;
    return invalidated;
  }

  void RenderCommand(CGContextRef context, const DrawRectCommand& command) {
    SetFillColor(context, command.color);
    const CGRect rect = CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height);
    if (command.corner_radius > 0.0F) {
      CGPathRef path = CGPathCreateWithRoundedRect(rect, command.corner_radius, command.corner_radius, nullptr);
      CGContextAddPath(context, path);
      CGContextFillPath(context);
      CGPathRelease(path);
    } else {
      CGContextFillRect(context, rect);
    }
  }

  void RenderCommand(CGContextRef context, const DrawTextCommand& command) {
    if (command.rect.width <= 0.0F || command.rect.height <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    if (command.align == TextAlign::Leading) {
      CFAttributedStringRef attributed = CreateAttributedString(command.text, command.font_size);
      CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(attributed);
      CGPathRef path = CGPathCreateWithRect(CGRectMake(0.0, 0.0, command.rect.width, command.rect.height), nullptr);
      CTFrameRef frame = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);

      CGContextSaveGState(context);
      CGContextTranslateCTM(context, command.rect.x, command.rect.y + command.rect.height);
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
    const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    const CGFloat text_height = ascent + descent + leading;

    CGFloat x = command.rect.x;
    if (command.align == TextAlign::Center) {
      x += std::max(0.0F, (command.rect.width - static_cast<float>(width)) * 0.5F);
    }

    const CGFloat bottom_padding = std::max(0.0F, (command.rect.height - static_cast<float>(text_height)) * 0.5F);
    const CGFloat baseline_from_bottom = bottom_padding + descent;

    CGContextSaveGState(context);
    CGContextClipToRect(
        context,
        CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height)
    );
    CGContextTranslateCTM(context, 0.0, command.rect.y + command.rect.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    SetFillColor(context, command.color);
    CGContextSetTextPosition(context, x, baseline_from_bottom);
    CTLineDraw(line, context);
    CGContextRestoreGState(context);
    CFRelease(line);
  }

  void RenderCommand(CGContextRef context, const DrawCircleCommand& command) {
    if (command.radius <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetFillColor(context, command.color);
    const float diameter = command.radius * 2.0F;
    CGContextFillEllipseInRect(
        context,
        CGRectMake(command.center.x - command.radius, command.center.y - command.radius, diameter, diameter)
    );
  }

  void RenderCommand(CGContextRef context, const DrawArcCommand& command) {
    if (command.radius <= 0.0F || command.width <= 0.0F || command.color.alpha <= 0.0F ||
        !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
      return;
    }

    CGLineCap cap = kCGLineCapButt;
    if (command.cap == StrokeCap::Round) {
      cap = kCGLineCapRound;
    } else if (command.cap == StrokeCap::Square) {
      cap = kCGLineCapSquare;
    }

    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRelativeArc(
        path,
        nullptr,
        command.center.x,
        command.center.y,
        command.radius,
        command.start_angle,
        command.sweep_angle
    );
    CGContextSaveGState(context);
    SetStrokeColor(context, command.color);
    CGContextSetLineWidth(context, command.width);
    CGContextSetLineCap(context, cap);
    CGContextAddPath(context, path);
    CGContextStrokePath(context);
    CGContextRestoreGState(context);
    CGPathRelease(path);
  }

  void RenderCommand(CGContextRef context, const DrawBorderCommand& command) {
    if (command.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    const float inset = command.width * 0.5F;
    const CGRect rect = CGRectMake(
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.width),
        std::max(0.0F, command.rect.height - command.width)
    );
    const float radius = std::max(0.0F, command.corner_radius - inset);
    CGPathRef path = CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
    CGContextSaveGState(context);
    SetStrokeColor(context, command.color);
    CGContextSetLineWidth(context, command.width);
    CGContextAddPath(context, path);
    CGContextStrokePath(context);
    CGContextRestoreGState(context);
    CGPathRelease(path);
  }

  void RenderCommand(CGContextRef context, const PushClipCommand& command) {
    CGContextSaveGState(context);
    const CGRect rect = CGRectMake(command.rect.x, command.rect.y, command.rect.width, command.rect.height);
    if (command.corner_radius <= 0.0F) {
      CGContextClipToRect(context, rect);
      return;
    }
    const float radius = std::min(command.corner_radius, std::min(command.rect.width, command.rect.height) * 0.5F);
    CGPathRef path = CGPathCreateWithRoundedRect(rect, radius, radius, nullptr);
    CGContextAddPath(context, path);
    CGContextClip(context);
    CGPathRelease(path);
  }

  void RenderCommand(CGContextRef context, const PopClipCommand& command) {
    static_cast<void>(command);
    CGContextRestoreGState(context);
  }

  void RenderCommand(CGContextRef context, const PushTransformCommand& command) {
    CGContextSaveGState(context);
    CGContextConcatCTM(
        context,
        CGAffineTransformMake(
            command.transform.m11,
            command.transform.m12,
            command.transform.m21,
            command.transform.m22,
            command.transform.translate_x,
            command.transform.translate_y
        )
    );
  }

  void RenderCommand(CGContextRef context, const PopTransformCommand& command) {
    static_cast<void>(command);
    CGContextRestoreGState(context);
  }

  Runtime* runtime_ = nullptr;
  __strong NSWindow* window_ = nil;
  __strong HuxerUIHostView* view_ = nil;
  __strong HuxerUIApplicationDelegate* delegate_ = nil;
  __strong HuxerUIFrameScheduler* frame_scheduler_ = nil;
  std::unique_ptr<MacTextInput> text_input_;
  bool frame_build_pending_ = false;
  bool paint_pending_ = false;
  bool paint_in_progress_ = false;
  std::optional<double> scheduled_frame_deadline_;
  std::optional<double> deferred_frame_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
};

int RunPlatformApp(AppDefinition definition) {
  AppOptions options = definition.options;
  MacPlatformHost platform;
  Runtime runtime{std::move(definition), platform};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail

@implementation HuxerUIHostView

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  if (huxeruiHost != nullptr) {
    huxeruiHost->Resize({
        static_cast<float>(newSize.width),
        static_cast<float>(newSize.height),
    });
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (void)setFrameOrigin:(NSPoint)newOrigin {
  [super setFrameOrigin:newOrigin];
  if (huxeruiHost != nullptr) {
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (void)setBoundsOrigin:(NSPoint)newOrigin {
  [super setBoundsOrigin:newOrigin];
  if (huxeruiHost != nullptr) {
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (huxeruiHost != nullptr) {
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (void)viewDidMoveToSuperview {
  [super viewDidMoveToSuperview];
  if (huxeruiHost != nullptr) {
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (huxeruiHost != nullptr) {
    huxeruiHost->InvalidateNativeSurface();
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (NSTextInputContext*)inputContext {
  if (huxeruiHost != nullptr) {
    NSTextInputContext* context = huxeruiHost->InputContext();
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
  if (huxeruiHost != nullptr) {
    huxeruiHost->CommitFrameAndInvalidate();
  }
}

- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  if (huxeruiHost == nullptr) {
    return;
  }

  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  huxeruiHost->DrawCommittedFrame(context, dirtyRect);
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
  if (huxeruiHost != nullptr && huxeruiHost->HandleTextInputEvent(event)) {
    return;
  }
  [self sendKeyEvent:event type:huxerui::KeyEventType::Down];
}

- (void)keyUp:(NSEvent*)event {
  [self sendKeyEvent:event type:huxerui::KeyEventType::Up];
}

- (void)cancelOperation:(id)sender {
  static_cast<void>(sender);
  [self cancelPointer];
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow {
  if (newWindow == nil) {
    [self cancelPointer];
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
  if (huxeruiHost != nullptr) {
    huxeruiHost->ApplicationActiveChanged(true);
  }
}

- (void)applicationDidResignActive:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiHost != nullptr) {
    huxeruiHost->ApplicationActiveChanged(false);
  }
}

- (void)windowDidMove:(NSNotification*)notification {
  static_cast<void>(notification);
  if (huxeruiHost != nullptr) {
    huxeruiHost->InvalidateTextInputGeometry();
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  static_cast<void>(sender);
  return YES;
}

@end
