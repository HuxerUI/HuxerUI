#import "uikit_text_input.h"

#import <UIKit/UIKit.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "text_input_internal.h"
#include "uikit_view.h"

@interface HuxerUITextPosition : UITextPosition {
  NSInteger huxeruiOffset;
}
@property(nonatomic, readonly) NSInteger offset;
+ (instancetype)positionWithOffset:(NSInteger)offset;
@end

@implementation HuxerUITextPosition

+ (instancetype)positionWithOffset:(NSInteger)offset {
  HuxerUITextPosition* position = [[self alloc] init];
  position->huxeruiOffset = offset;
  return position;
}

- (NSInteger)offset {
  return huxeruiOffset;
}

@end

@interface HuxerUITextRange : UITextRange {
  __strong HuxerUITextPosition* huxeruiStart;
  __strong HuxerUITextPosition* huxeruiEnd;
}
@property(nonatomic, readonly) HuxerUITextPosition* start;
@property(nonatomic, readonly) HuxerUITextPosition* end;
+ (instancetype)rangeWithStart:(NSInteger)start end:(NSInteger)end;
@end

@implementation HuxerUITextRange

+ (instancetype)rangeWithStart:(NSInteger)start end:(NSInteger)end {
  HuxerUITextRange* range = [[self alloc] init];
  range->huxeruiStart = [HuxerUITextPosition positionWithOffset:start];
  range->huxeruiEnd = [HuxerUITextPosition positionWithOffset:end];
  return range;
}

- (HuxerUITextPosition*)start {
  return huxeruiStart;
}

- (HuxerUITextPosition*)end {
  return huxeruiEnd;
}

- (BOOL)isEmpty {
  return self.start.offset == self.end.offset;
}

@end

@interface HuxerUIView (HuxerUITextInput) <UITextInput>
@end

namespace huxerui::detail {
namespace {

HuxerUITextPosition* ToPosition(UITextPosition* position) {
  return [position isKindOfClass:HuxerUITextPosition.class] ? static_cast<HuxerUITextPosition*>(position) : nil;
}

HuxerUITextRange* ToRange(UITextRange* range) {
  return [range isKindOfClass:HuxerUITextRange.class] ? static_cast<HuxerUITextRange*>(range) : nil;
}

std::optional<TextRange> ToTextRange(UITextRange* range) {
  HuxerUITextRange* value = ToRange(range);
  if (value == nil || value.start.offset < 0 || value.end.offset < value.start.offset) {
    return std::nullopt;
  }
  return TextRange{value.start.offset, value.end.offset};
}

HuxerUITextRange* ToUIKitRange(TextRange range) {
  if (!range.IsValid() || range.end > std::numeric_limits<NSInteger>::max()) {
    return nil;
  }
  return [HuxerUITextRange rangeWithStart:static_cast<NSInteger>(range.start) end:static_cast<NSInteger>(range.end)];
}

std::optional<std::string> ToUtf8(NSString* value) {
  if (value == nil) {
    return std::nullopt;
  }
  const char* utf8 = value.UTF8String;
  return utf8 == nullptr ? std::nullopt : std::optional<std::string>{utf8};
}

NSString* ToNSString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

UIKeyboardType KeyboardType(const TextInputConfiguration& configuration) {
  switch (configuration.type) {
  case TextInputType::Email:
    return UIKeyboardTypeEmailAddress;
  case TextInputType::Number:
    return UIKeyboardTypeNumberPad;
  case TextInputType::Decimal:
    return UIKeyboardTypeDecimalPad;
  case TextInputType::Phone:
    return UIKeyboardTypePhonePad;
  case TextInputType::Url:
    return UIKeyboardTypeURL;
  case TextInputType::Text:
    return UIKeyboardTypeDefault;
  }
  return UIKeyboardTypeDefault;
}

UITextAutocapitalizationType Capitalization(const TextInputConfiguration& configuration) {
  switch (configuration.capitalization) {
  case TextCapitalization::Characters:
    return UITextAutocapitalizationTypeAllCharacters;
  case TextCapitalization::Words:
    return UITextAutocapitalizationTypeWords;
  case TextCapitalization::Sentences:
    return UITextAutocapitalizationTypeSentences;
  case TextCapitalization::None:
    return UITextAutocapitalizationTypeNone;
  }
  return UITextAutocapitalizationTypeNone;
}

UIReturnKeyType ReturnKeyType(const TextInputConfiguration& configuration) {
  switch (configuration.action) {
  case TextInputAction::Go:
    return UIReturnKeyGo;
  case TextInputAction::Next:
    return UIReturnKeyNext;
  case TextInputAction::Search:
    return UIReturnKeySearch;
  case TextInputAction::Send:
    return UIReturnKeySend;
  case TextInputAction::Default:
  case TextInputAction::Done:
    return UIReturnKeyDone;
  case TextInputAction::Newline:
    return UIReturnKeyDefault;
  }
  return UIReturnKeyDefault;
}

TextInputAction ResolvedAction(const TextInputConfiguration& configuration) {
  if (configuration.action != TextInputAction::Default) {
    return configuration.action;
  }
  return configuration.multiline ? TextInputAction::Newline : TextInputAction::Done;
}

void ApplyCommittedText(
    UIKitTextInput& text_input, std::string text, std::optional<TextRange> replacement = std::nullopt
) {
  const TextInputContext context = text_input.QueryContext();
  if (context.result_code != TextInputResultCode::Ok) {
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::CommitText;
  command.text = std::move(text);
  if (!context.composition.has_value() && replacement.has_value()) {
    if (replacement->end > context.total_length) {
      return;
    }
    command.target = replacement;
  }
  text_input.Apply(std::move(command));
}

} // namespace

class UIKitTextInputState {
public:
  UIKitTextInputState(Runtime& runtime, HuxerUIView* view) : runtime_(&runtime), view_(view) {}

  ~UIKitTextInputState() {
    HuxerUIView* view = view_;
    if (view != nil && view->huxeruiTextInput != nullptr) {
      view->huxeruiTextInput = nullptr;
    }
  }

  bool IsActive() const noexcept {
    return session_id_ != 0;
  }

  const TextInputConfiguration& Configuration() const noexcept {
    return configuration_;
  }

  TextInputContext QueryContext(TextOffset start, TextOffset length) const {
    if (!IsActive()) {
      TextInputContext result;
      result.result_code = TextInputResultCode::SessionMismatch;
      return result;
    }
    return runtime_->QueryTextInputContext(session_id_, start, length);
  }

  TextInputGeometry QueryGeometry(TextRange range) const {
    if (!IsActive()) {
      TextInputGeometry result;
      result.result_code = TextInputResultCode::SessionMismatch;
      return result;
    }
    return runtime_->QueryTextInputGeometry(session_id_, range);
  }

  TextInputPositionResult QueryPosition(Point point) const {
    if (!IsActive()) {
      TextInputPositionResult result;
      result.result_code = TextInputResultCode::SessionMismatch;
      return result;
    }
    return runtime_->QueryTextInputPosition(session_id_, point);
  }

  TextInputApplyResult Apply(TextInputCommand command) {
    if (!IsActive()) {
      return {.result_code = TextInputResultCode::SessionMismatch};
    }
    applying_command_ = true;
    try {
      TextInputApplyResult result = runtime_->HandleTextInputCommands({session_id_, {std::move(command)}});
      applying_command_ = false;
      return result;
    } catch (...) {
      applying_command_ = false;
      throw;
    }
  }

  bool PerformAction(TextInputAction action) {
    return IsActive() && runtime_->PerformTextInputAction(session_id_, action);
  }

  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    static_cast<void>(geometry);
    session_id_ = session_id;
    configuration_ = configuration;
    input_state_ = state;
    HuxerUIView* view = view_;
    if (view != nil) {
      [view reloadInputViews];
      [view becomeFirstResponder];
    }
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) {
    static_cast<void>(geometry);
    if (session_id != session_id_) {
      return;
    }
    if (applying_command_) {
      input_state_ = state;
      return;
    }
    HuxerUIView* view = view_;
    id<UITextInputDelegate> delegate = view == nil ? nil : view->huxeruiInputDelegate;
    const bool text_changed = state.content_revision != input_state_.content_revision;
    const bool selection_changed =
        state.selection != input_state_.selection || state.composition != input_state_.composition;
    if (text_changed) {
      [delegate textWillChange:view];
    }
    if (selection_changed) {
      [delegate selectionWillChange:view];
    }
    input_state_ = state;
    if (selection_changed) {
      [delegate selectionDidChange:view];
    }
    if (text_changed) {
      [delegate textDidChange:view];
    }
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    if (session_id != session_id_) {
      return;
    }
    configuration_ = configuration;
    Update(session_id, state, geometry);
    HuxerUIView* view = view_;
    if (view != nil) {
      [view reloadInputViews];
    }
  }

  void Stop(TextInputSessionId session_id) {
    if (session_id != session_id_) {
      return;
    }
    session_id_ = 0;
    configuration_ = {};
    input_state_ = {};
    HuxerUIView* view = view_;
    if (view != nil && view.isFirstResponder) {
      [view resignFirstResponder];
    }
  }

  void RequestShow(TextInputSessionId session_id) {
    if (session_id != session_id_) {
      return;
    }
    HuxerUIView* view = view_;
    if (view != nil) {
      [view reloadInputViews];
      [view becomeFirstResponder];
    }
  }

private:
  Runtime* runtime_ = nullptr;
  __weak HuxerUIView* view_ = nil;
  TextInputSessionId session_id_ = 0;
  TextInputConfiguration configuration_;
  TextInputState input_state_;
  bool applying_command_ = false;
};

UIKitTextInput::UIKitTextInput(Runtime& runtime, HuxerUIView* view)
    : state_(std::make_unique<UIKitTextInputState>(runtime, view)) {}

UIKitTextInput::~UIKitTextInput() = default;

bool UIKitTextInput::IsActive() const noexcept {
  return state_->IsActive();
}

const TextInputConfiguration& UIKitTextInput::Configuration() const noexcept {
  return state_->Configuration();
}

TextInputContext UIKitTextInput::QueryContext(TextOffset start, TextOffset length) const {
  return state_->QueryContext(start, length);
}

TextInputGeometry UIKitTextInput::QueryGeometry(TextRange range) const {
  return state_->QueryGeometry(range);
}

TextInputPositionResult UIKitTextInput::QueryPosition(Point point) const {
  return state_->QueryPosition(point);
}

TextInputApplyResult UIKitTextInput::Apply(TextInputCommand command) {
  return state_->Apply(std::move(command));
}

bool UIKitTextInput::PerformAction(TextInputAction action) {
  return state_->PerformAction(action);
}

void UIKitTextInput::Start(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Start(session_id, configuration, state, geometry);
}

void UIKitTextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  state_->Update(session_id, state, geometry);
}

void UIKitTextInput::Restart(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Restart(session_id, configuration, state, geometry);
}

void UIKitTextInput::Stop(TextInputSessionId session_id) {
  state_->Stop(session_id);
}

void UIKitTextInput::RequestShow(TextInputSessionId session_id) {
  state_->RequestShow(session_id);
}

} // namespace huxerui::detail

@implementation HuxerUIView (HuxerUITextInput)

- (BOOL)hasText {
  if (huxeruiTextInput == nullptr) {
    return NO;
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  return context.result_code == huxerui::TextInputResultCode::Ok && context.total_length > 0;
}

- (void)insertText:(NSString*)text {
  if (huxeruiTextInput == nullptr) {
    return;
  }
  const std::optional<std::string> utf8 = huxerui::detail::ToUtf8(text);
  if (!utf8.has_value()) {
    return;
  }
  const huxerui::TextInputConfiguration& configuration = huxeruiTextInput->Configuration();
  const huxerui::TextInputAction action = huxerui::detail::ResolvedAction(configuration);
  if (([text isEqualToString:@"\n"] || [text isEqualToString:@"\r"]) && action != huxerui::TextInputAction::Newline) {
    huxeruiTextInput->PerformAction(action);
    return;
  }
  huxerui::detail::ApplyCommittedText(*huxeruiTextInput, *utf8);
}

- (void)deleteBackward {
  if (huxeruiTextInput == nullptr) {
    return;
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  if (context.result_code != huxerui::TextInputResultCode::Ok) {
    return;
  }
  const huxerui::TextRange selection = context.selection.Range();
  if (!selection.IsCollapsed()) {
    huxerui::detail::ApplyCommittedText(*huxeruiTextInput, {}, selection);
    return;
  }
  huxerui::TextInputCommand command;
  command.kind = huxerui::TextInputCommandKind::DeleteSurrounding;
  command.delete_before = 1;
  command.delete_unit = huxerui::TextInputUnit::UnicodeCodePoint;
  huxeruiTextInput->Apply(std::move(command));
}

- (NSString*)textInRange:(UITextRange*)range {
  if (huxeruiTextInput == nullptr || huxeruiTextInput->Configuration().secure) {
    return nil;
  }
  const std::optional<huxerui::TextRange> requested = huxerui::detail::ToTextRange(range);
  if (!requested.has_value()) {
    return nil;
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext(requested->start, requested->Length());
  if (context.result_code != huxerui::TextInputResultCode::Ok) {
    return nil;
  }
  const std::optional<huxerui::TextOffset> slice_length = huxerui::detail::Utf16Length(context.text);
  if (!slice_length.has_value()) {
    return nil;
  }
  const huxerui::TextRange available{
      context.slice_start,
      context.slice_start + *slice_length,
  };
  if (requested->start < available.start || requested->end > available.end || requested->end > context.total_length) {
    return nil;
  }
  const std::optional<std::string> result = huxerui::detail::Utf8TextInRange(
      context.text,
      {
          requested->start - context.slice_start,
          requested->end - context.slice_start,
      }
  );
  return result.has_value() ? huxerui::detail::ToNSString(*result) : nil;
}

- (void)replaceRange:(UITextRange*)range withText:(NSString*)text {
  if (huxeruiTextInput == nullptr) {
    return;
  }
  const std::optional<huxerui::TextRange> target = huxerui::detail::ToTextRange(range);
  const std::optional<std::string> utf8 = huxerui::detail::ToUtf8(text);
  if (!target.has_value() || !utf8.has_value()) {
    return;
  }
  huxerui::detail::ApplyCommittedText(*huxeruiTextInput, *utf8, target);
}

- (UITextRange*)selectedTextRange {
  if (huxeruiTextInput == nullptr) {
    return nil;
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  return context.result_code == huxerui::TextInputResultCode::Ok
             ? huxerui::detail::ToUIKitRange(context.selection.Range())
             : nil;
}

- (void)setSelectedTextRange:(UITextRange*)selectedTextRange {
  if (huxeruiTextInput == nullptr) {
    return;
  }
  const std::optional<huxerui::TextRange> range = huxerui::detail::ToTextRange(selectedTextRange);
  if (!range.has_value()) {
    return;
  }
  huxerui::TextInputCommand command;
  command.kind = huxerui::TextInputCommandKind::SetSelection;
  command.selection_after = huxerui::TextSelection{range->start, range->end};
  huxeruiTextInput->Apply(std::move(command));
}

- (UITextRange*)markedTextRange {
  if (huxeruiTextInput == nullptr) {
    return nil;
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  return context.result_code == huxerui::TextInputResultCode::Ok && context.composition.has_value()
             ? huxerui::detail::ToUIKitRange(*context.composition)
             : nil;
}

- (NSDictionary<NSAttributedStringKey, id>*)markedTextStyle {
  return huxeruiMarkedTextStyle;
}

- (void)setMarkedTextStyle:(NSDictionary<NSAttributedStringKey, id>*)markedTextStyle {
  huxeruiMarkedTextStyle = [markedTextStyle copy];
}

- (void)setMarkedText:(NSString*)markedText selectedRange:(NSRange)selectedRange {
  if (huxeruiTextInput == nullptr || markedText == nil || selectedRange.location == NSNotFound) {
    return;
  }
  const std::optional<std::string> utf8 = huxerui::detail::ToUtf8(markedText);
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  if (!utf8.has_value() || context.result_code != huxerui::TextInputResultCode::Ok ||
      selectedRange.location > markedText.length || selectedRange.length > markedText.length - selectedRange.location) {
    return;
  }
  huxerui::TextOffset insertion_start = context.selection.Range().start;
  if (context.composition.has_value()) {
    insertion_start = context.composition->start;
  }
  huxerui::TextInputCommand command;
  command.kind = huxerui::TextInputCommandKind::UpdateComposition;
  command.text = *utf8;
  if (!context.composition.has_value()) {
    command.target = context.selection.Range();
  }
  command.selection_after = huxerui::TextSelection{
      insertion_start + static_cast<huxerui::TextOffset>(selectedRange.location),
      insertion_start + static_cast<huxerui::TextOffset>(NSMaxRange(selectedRange)),
  };
  huxeruiTextInput->Apply(std::move(command));
}

- (void)unmarkText {
  if (huxeruiTextInput == nullptr) {
    return;
  }
  huxerui::TextInputCommand command;
  command.kind = huxerui::TextInputCommandKind::FinishComposition;
  huxeruiTextInput->Apply(std::move(command));
}

- (UITextPosition*)beginningOfDocument {
  return [HuxerUITextPosition positionWithOffset:0];
}

- (UITextPosition*)endOfDocument {
  if (huxeruiTextInput == nullptr) {
    return [HuxerUITextPosition positionWithOffset:0];
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  const huxerui::TextOffset end =
      std::clamp<huxerui::TextOffset>(context.total_length, 0, std::numeric_limits<NSInteger>::max());
  return [HuxerUITextPosition positionWithOffset:static_cast<NSInteger>(end)];
}

- (UITextRange*)textRangeFromPosition:(UITextPosition*)fromPosition toPosition:(UITextPosition*)toPosition {
  HuxerUITextPosition* from = huxerui::detail::ToPosition(fromPosition);
  HuxerUITextPosition* to = huxerui::detail::ToPosition(toPosition);
  return from == nil || to == nil
             ? nil
             : [HuxerUITextRange rangeWithStart:std::min(from.offset, to.offset) end:std::max(from.offset, to.offset)];
}

- (UITextPosition*)positionFromPosition:(UITextPosition*)position offset:(NSInteger)offset {
  HuxerUITextPosition* value = huxerui::detail::ToPosition(position);
  if (value == nil || huxeruiTextInput == nullptr) {
    return nil;
  }
  const huxerui::TextInputContext context = huxeruiTextInput->QueryContext();
  const NSInteger end = static_cast<NSInteger>(
      std::clamp<huxerui::TextOffset>(context.total_length, 0, std::numeric_limits<NSInteger>::max())
  );
  if (value.offset < 0 || value.offset > end || offset < -value.offset || offset > end - value.offset) {
    return nil;
  }
  return [HuxerUITextPosition positionWithOffset:value.offset + offset];
}

- (UITextPosition*)positionFromPosition:(UITextPosition*)position
                            inDirection:(UITextLayoutDirection)direction
                                 offset:(NSInteger)offset {
  if ((direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp) &&
      offset == std::numeric_limits<NSInteger>::min()) {
    return nil;
  }
  const NSInteger signed_offset =
      direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp ? -offset : offset;
  return [self positionFromPosition:position offset:signed_offset];
}

- (NSComparisonResult)comparePosition:(UITextPosition*)position toPosition:(UITextPosition*)other {
  HuxerUITextPosition* left = huxerui::detail::ToPosition(position);
  HuxerUITextPosition* right = huxerui::detail::ToPosition(other);
  if (left == nil || right == nil) {
    return NSOrderedSame;
  }
  if (left.offset < right.offset) {
    return NSOrderedAscending;
  }
  if (left.offset > right.offset) {
    return NSOrderedDescending;
  }
  return NSOrderedSame;
}

- (NSInteger)offsetFromPosition:(UITextPosition*)from toPosition:(UITextPosition*)toPosition {
  HuxerUITextPosition* left = huxerui::detail::ToPosition(from);
  HuxerUITextPosition* right = huxerui::detail::ToPosition(toPosition);
  return left == nil || right == nil ? 0 : right.offset - left.offset;
}

- (id<UITextInputDelegate>)inputDelegate {
  return huxeruiInputDelegate;
}

- (void)setInputDelegate:(id<UITextInputDelegate>)inputDelegate {
  huxeruiInputDelegate = inputDelegate;
}

- (id<UITextInputTokenizer>)tokenizer {
  if (huxeruiTokenizer == nil) {
    huxeruiTokenizer = [[UITextInputStringTokenizer alloc] initWithTextInput:self];
  }
  return huxeruiTokenizer;
}

- (UITextPosition*)positionWithinRange:(UITextRange*)range farthestInDirection:(UITextLayoutDirection)direction {
  HuxerUITextRange* value = huxerui::detail::ToRange(range);
  if (value == nil) {
    return nil;
  }
  return direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp ? value.start : value.end;
}

- (UITextRange*)characterRangeByExtendingPosition:(UITextPosition*)position
                                      inDirection:(UITextLayoutDirection)direction {
  HuxerUITextPosition* value = huxerui::detail::ToPosition(position);
  if (value == nil) {
    return nil;
  }
  UITextPosition* other = [self positionFromPosition:position inDirection:direction offset:1];
  HuxerUITextPosition* endpoint = huxerui::detail::ToPosition(other);
  if (endpoint == nil) {
    return nil;
  }
  return [HuxerUITextRange rangeWithStart:std::min(value.offset, endpoint.offset)
                                      end:std::max(value.offset, endpoint.offset)];
}

- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition*)position
                                          inDirection:(UITextStorageDirection)direction {
  static_cast<void>(position);
  static_cast<void>(direction);
  return NSWritingDirectionNatural;
}

- (void)setBaseWritingDirection:(NSWritingDirection)writingDirection forRange:(UITextRange*)range {
  static_cast<void>(writingDirection);
  static_cast<void>(range);
}

- (CGRect)firstRectForRange:(UITextRange*)range {
  if (huxeruiTextInput == nullptr) {
    return CGRectZero;
  }
  const std::optional<huxerui::TextRange> requested = huxerui::detail::ToTextRange(range);
  if (!requested.has_value()) {
    return CGRectZero;
  }
  const huxerui::TextInputGeometry geometry = huxeruiTextInput->QueryGeometry(*requested);
  if (geometry.result_code != huxerui::TextInputResultCode::Ok) {
    return CGRectZero;
  }
  const huxerui::Rect rect = geometry.range_rects.empty() ? geometry.caret : geometry.range_rects.front();
  return CGRectMake(rect.x, rect.y, rect.width, rect.height);
}

- (CGRect)caretRectForPosition:(UITextPosition*)position {
  if (huxeruiTextInput == nullptr) {
    return CGRectZero;
  }
  HuxerUITextPosition* value = huxerui::detail::ToPosition(position);
  if (value == nil) {
    return CGRectZero;
  }
  const huxerui::TextInputGeometry geometry = huxeruiTextInput->QueryGeometry({value.offset, value.offset});
  return geometry.result_code == huxerui::TextInputResultCode::Ok
             ? CGRectMake(geometry.caret.x, geometry.caret.y, geometry.caret.width, geometry.caret.height)
             : CGRectZero;
}

- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range {
  static_cast<void>(range);
  return @[];
}

- (UITextPosition*)closestPositionToPoint:(CGPoint)point {
  if (huxeruiTextInput == nullptr) {
    return nil;
  }
  const huxerui::TextInputPositionResult result =
      huxeruiTextInput->QueryPosition({static_cast<float>(point.x), static_cast<float>(point.y)});
  return result.result_code == huxerui::TextInputResultCode::Ok
             ? [HuxerUITextPosition positionWithOffset:static_cast<NSInteger>(result.position.offset)]
             : nil;
}

- (UITextPosition*)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange*)range {
  HuxerUITextPosition* position = static_cast<HuxerUITextPosition*>([self closestPositionToPoint:point]);
  HuxerUITextRange* limit = huxerui::detail::ToRange(range);
  if (position == nil || limit == nil) {
    return nil;
  }
  return [HuxerUITextPosition positionWithOffset:std::clamp(position.offset, limit.start.offset, limit.end.offset)];
}

- (UITextRange*)characterRangeAtPoint:(CGPoint)point {
  HuxerUITextPosition* position = static_cast<HuxerUITextPosition*>([self closestPositionToPoint:point]);
  if (position == nil) {
    return nil;
  }
  UITextPosition* end = [self positionFromPosition:position offset:1];
  HuxerUITextPosition* end_position = huxerui::detail::ToPosition(end);
  return [HuxerUITextRange rangeWithStart:position.offset
                                      end:end_position == nil ? position.offset : end_position.offset];
}

- (UIView*)textInputView {
  return self;
}

- (UITextStorageDirection)selectionAffinity {
  return huxeruiSelectionAffinity;
}

- (void)setSelectionAffinity:(UITextStorageDirection)selectionAffinity {
  huxeruiSelectionAffinity = selectionAffinity;
}

- (UITextAutocapitalizationType)autocapitalizationType {
  return huxeruiTextInput == nullptr ? UITextAutocapitalizationTypeNone
                                     : huxerui::detail::Capitalization(huxeruiTextInput->Configuration());
}

- (UITextAutocorrectionType)autocorrectionType {
  return huxeruiTextInput != nullptr && huxeruiTextInput->Configuration().autocorrect ? UITextAutocorrectionTypeYes
                                                                                      : UITextAutocorrectionTypeNo;
}

- (UITextSpellCheckingType)spellCheckingType {
  return [self autocorrectionType] == UITextAutocorrectionTypeYes ? UITextSpellCheckingTypeYes
                                                                  : UITextSpellCheckingTypeNo;
}

- (UIKeyboardType)keyboardType {
  return huxeruiTextInput == nullptr ? UIKeyboardTypeDefault
                                     : huxerui::detail::KeyboardType(huxeruiTextInput->Configuration());
}

- (UIKeyboardAppearance)keyboardAppearance {
  return UIKeyboardAppearanceDefault;
}

- (UIReturnKeyType)returnKeyType {
  return huxeruiTextInput == nullptr ? UIReturnKeyDefault
                                     : huxerui::detail::ReturnKeyType(huxeruiTextInput->Configuration());
}

- (BOOL)enablesReturnKeyAutomatically {
  return NO;
}

- (BOOL)isSecureTextEntry {
  return huxeruiTextInput != nullptr && huxeruiTextInput->Configuration().secure;
}

- (void)setAutocapitalizationType:(UITextAutocapitalizationType)value {
  static_cast<void>(value);
}

- (void)setAutocorrectionType:(UITextAutocorrectionType)value {
  static_cast<void>(value);
}

- (void)setSpellCheckingType:(UITextSpellCheckingType)value {
  static_cast<void>(value);
}

- (void)setKeyboardType:(UIKeyboardType)value {
  static_cast<void>(value);
}

- (void)setKeyboardAppearance:(UIKeyboardAppearance)value {
  static_cast<void>(value);
}

- (void)setReturnKeyType:(UIReturnKeyType)value {
  static_cast<void>(value);
}

- (void)setEnablesReturnKeyAutomatically:(BOOL)value {
  static_cast<void>(value);
}

- (void)setSecureTextEntry:(BOOL)value {
  static_cast<void>(value);
}

@end
