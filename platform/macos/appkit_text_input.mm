#import "appkit_text_input.h"
#import <Carbon/Carbon.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "text_input_internal.h"

namespace huxerui::detail {
class MacTextInputState;
}

@interface HuxerUITextInputClient : NSObject <NSTextInputClient> {
@public
  huxerui::detail::MacTextInputState* huxeruiState;
}
- (instancetype)initWithState:(huxerui::detail::MacTextInputState*)state;
@end

namespace huxerui::detail {
namespace {

Key TranslateKey(unsigned short key_code) {
  switch (key_code) {
  case 56:
  case 60:
    return Key::Shift;
  case 59:
  case 62:
    return Key::Control;
  case 58:
  case 61:
    return Key::Alt;
  case 54:
  case 55:
    return Key::Meta;
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
  case 109:
    return Key::F10;
  case 110:
    return Key::ContextMenu;
  case 0:
    return Key::A;
  case 8:
    return Key::C;
  case 9:
    return Key::V;
  case 7:
    return Key::X;
  case 16:
    return Key::Y;
  case 6:
    return Key::Z;
  default:
    return Key::Unknown;
  }
}

std::optional<TextRange> ToTextRange(NSRange range) {
  if (range.location == NSNotFound) {
    return std::nullopt;
  }
  constexpr NSUInteger maximum = static_cast<NSUInteger>(std::numeric_limits<TextOffset>::max());
  if (range.location > maximum || range.length > maximum - range.location) {
    return std::nullopt;
  }
  return TextRange{
      static_cast<TextOffset>(range.location),
      static_cast<TextOffset>(range.location + range.length),
  };
}

NSRange ToNSRange(TextRange range) {
  if (!range.IsValid()) {
    return NSMakeRange(NSNotFound, 0);
  }
  if (static_cast<std::uint64_t>(range.end) > std::numeric_limits<NSUInteger>::max()) {
    return NSMakeRange(NSNotFound, 0);
  }
  return NSMakeRange(static_cast<NSUInteger>(range.start), static_cast<NSUInteger>(range.Length()));
}

NSString* PlainString(id value) {
  if ([value isKindOfClass:[NSAttributedString class]]) {
    return static_cast<NSAttributedString*>(value).string;
  }
  return [value isKindOfClass:[NSString class]] ? static_cast<NSString*>(value) : nil;
}

std::optional<std::string> ToUtf8(id value) {
  NSString* string = PlainString(value);
  if (string == nil) {
    return std::nullopt;
  }
  const char* utf8 = string.UTF8String;
  if (utf8 == nullptr) {
    return std::nullopt;
  }
  return std::string{utf8};
}

} // namespace

KeyEvent MakeMacKeyEvent(NSEvent* event, KeyEventType type) {
  const NSEventModifierFlags flags = event.modifierFlags;
  const char* characters = event.characters == nil ? nullptr : event.characters.UTF8String;
  return {
      type,
      TranslateKey(event.keyCode),
      characters == nullptr ? std::string{} : std::string(characters),
      {
          static_cast<bool>(flags & NSEventModifierFlagShift),
          static_cast<bool>(flags & NSEventModifierFlagControl),
          static_cast<bool>(flags & NSEventModifierFlagOption),
          static_cast<bool>(flags & NSEventModifierFlagCommand),
      },
      static_cast<bool>(event.isARepeat),
  };
}

class MacTextInputState {
public:
  MacTextInputState(Runtime& runtime, NSView* view) : runtime_(&runtime), view_(view) {
    client_ = [[HuxerUITextInputClient alloc] initWithState:this];
    input_context_ = [[NSTextInputContext alloc] initWithClient:client_];
  }

  ~MacTextInputState() {
    UpdateSecureEventInput(false);
    client_->huxeruiState = nullptr;
  }

  NSTextInputContext* InputContext() const noexcept {
    return input_context_;
  }

  bool IsActive() const noexcept {
    return session_id_ != 0;
  }

  void InvalidateGeometry() {
    if (IsActive()) {
      [input_context_ invalidateCharacterCoordinates];
    }
  }

  void ApplicationActiveChanged(bool active) {
    application_active_ = active;
    UpdateSecureEventInput(configuration_.secure);
  }

  bool HandleEvent(NSEvent* event) {
    if (!IsActive() || event == nil) {
      return false;
    }
    const KeyEvent key_event = MakeMacKeyEvent(event, KeyEventType::Down);
    if ((key_event.modifiers.control || key_event.modifiers.meta) && !key_event.modifiers.alt &&
        (key_event.key == Key::A || key_event.key == Key::C || key_event.key == Key::V ||
         key_event.key == Key::X || key_event.key == Key::Y || key_event.key == Key::Z)) {
      runtime_->HandleKeyEvent(key_event);
      return true;
    }
    return [input_context_ handleEvent:event] == YES;
  }

  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    static_cast<void>(state);
    static_cast<void>(geometry);
    session_id_ = session_id;
    configuration_ = configuration;
    UpdateSecureEventInput(configuration_.secure);
    [input_context_ activate];
    [input_context_ invalidateCharacterCoordinates];
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) {
    static_cast<void>(state);
    static_cast<void>(geometry);
    if (session_id != session_id_) {
      return;
    }
    [input_context_ invalidateCharacterCoordinates];
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    static_cast<void>(state);
    static_cast<void>(geometry);
    if (session_id != session_id_) {
      return;
    }
    suppress_callbacks_ = true;
    [input_context_ discardMarkedText];
    suppress_callbacks_ = false;
    configuration_ = configuration;
    UpdateSecureEventInput(configuration_.secure);
    [input_context_ invalidateCharacterCoordinates];
  }

  void Stop(TextInputSessionId session_id) {
    if (session_id != session_id_) {
      return;
    }
    suppress_callbacks_ = true;
    [input_context_ discardMarkedText];
    [input_context_ deactivate];
    suppress_callbacks_ = false;
    UpdateSecureEventInput(false);
    session_id_ = 0;
    configuration_ = {};
  }

  bool HasMarkedText() const {
    return QueryContext().composition.has_value();
  }

  NSRange MarkedRange() const {
    const TextInputContext context = QueryContext();
    return context.result_code == TextInputResultCode::Ok && context.composition.has_value()
               ? ToNSRange(*context.composition)
               : NSMakeRange(NSNotFound, 0);
  }

  NSRange SelectedRange() const {
    const TextInputContext context = QueryContext();
    return context.result_code == TextInputResultCode::Ok ? ToNSRange(context.selection.Range())
                                                          : NSMakeRange(NSNotFound, 0);
  }

  void SetMarkedText(id value, NSRange selected_range, NSRange replacement_range) {
    if (suppress_callbacks_ || !IsActive()) {
      return;
    }
    NSString* string = PlainString(value);
    const std::optional<std::string> text = ToUtf8(value);
    const std::optional<TextRange> relative_selection = ToTextRange(selected_range);
    const TextInputContext context = QueryContext();
    if (string == nil || !text.has_value() || !relative_selection.has_value() ||
        context.result_code != TextInputResultCode::Ok ||
        relative_selection->end > static_cast<TextOffset>(string.length)) {
      return;
    }

    TextInputCommand command;
    command.kind = TextInputCommandKind::UpdateComposition;
    command.text = *text;
    TextOffset insertion_start = context.selection.Range().start;
    if (context.composition.has_value()) {
      insertion_start = context.composition->start;
    } else if (const std::optional<TextRange> replacement = ToTextRange(replacement_range);
               replacement.has_value() && replacement->end <= context.total_length) {
      command.target = replacement;
      insertion_start = replacement->start;
    }
    command.selection_after = TextSelection{
        insertion_start + relative_selection->start,
        insertion_start + relative_selection->end,
        TextAffinity::Downstream,
    };
    Apply(command);
  }

  void UnmarkText() {
    if (suppress_callbacks_ || !HasMarkedText()) {
      return;
    }
    TextInputCommand command;
    command.kind = TextInputCommandKind::FinishComposition;
    Apply(command);
  }

  void InsertText(id value, NSRange replacement_range) {
    if (suppress_callbacks_ || !IsActive()) {
      return;
    }
    const std::optional<std::string> text = ToUtf8(value);
    const TextInputContext context = QueryContext();
    if (!text.has_value() || context.result_code != TextInputResultCode::Ok) {
      return;
    }

    TextInputCommand command;
    command.kind = TextInputCommandKind::CommitText;
    command.text = *text;
    if (!context.composition.has_value()) {
      const std::optional<TextRange> replacement = ToTextRange(replacement_range);
      if (replacement.has_value() && replacement->end <= context.total_length) {
        command.target = replacement;
      }
    }
    Apply(command);
  }

  NSAttributedString* AttributedSubstring(NSRange proposed_range, NSRangePointer actual_range) const {
    const std::optional<TextRange> requested = ToTextRange(proposed_range);
    if (!requested.has_value() || !IsActive() || configuration_.secure) {
      SetActualRange(actual_range, std::nullopt);
      return nil;
    }
    const TextInputContext context =
        runtime_->QueryTextInputContext(session_id_, requested->start, requested->Length());
    if (context.result_code != TextInputResultCode::Ok || requested->start > context.total_length) {
      SetActualRange(actual_range, std::nullopt);
      return nil;
    }

    const std::optional<TextOffset> slice_length = Utf16Length(context.text);
    if (!slice_length.has_value()) {
      SetActualRange(actual_range, std::nullopt);
      return nil;
    }
    const TextRange available{
        context.slice_start,
        context.slice_start + *slice_length,
    };
    const TextRange actual{
        std::max(requested->start, available.start),
        std::min({requested->end, available.end, context.total_length}),
    };
    if (!actual.IsValid()) {
      SetActualRange(actual_range, std::nullopt);
      return nil;
    }
    const std::optional<std::string> text = Utf8TextInRange(
        context.text,
        {
            actual.start - context.slice_start,
            actual.end - context.slice_start,
        }
    );
    if (!text.has_value()) {
      SetActualRange(actual_range, std::nullopt);
      return nil;
    }
    NSString* string = [[NSString alloc] initWithBytes:text->data() length:text->size() encoding:NSUTF8StringEncoding];
    if (string == nil) {
      SetActualRange(actual_range, std::nullopt);
      return nil;
    }
    SetActualRange(actual_range, actual);
    return [[NSAttributedString alloc] initWithString:string];
  }

  NSRect FirstRect(NSRange character_range, NSRangePointer actual_range) const {
    std::optional<TextRange> requested = ToTextRange(character_range);
    if (!requested.has_value()) {
      const TextInputContext context = QueryContext();
      if (context.result_code == TextInputResultCode::Ok) {
        requested = context.selection.Range();
      }
    }
    if (!requested.has_value() || !IsActive()) {
      SetActualRange(actual_range, std::nullopt);
      return NSZeroRect;
    }

    const TextInputGeometry geometry = runtime_->QueryTextInputGeometry(session_id_, *requested);
    NSView* view = view_;
    if (geometry.result_code != TextInputResultCode::Ok || view == nil || view.window == nil) {
      SetActualRange(actual_range, std::nullopt);
      return NSZeroRect;
    }
    const Rect source = geometry.range_rects.empty() ? geometry.caret : geometry.range_rects.front();
    const NSRect view_rect = NSMakeRect(source.x, source.y, source.width, source.height);
    const NSRect window_rect = [view convertRect:view_rect toView:nil];
    SetActualRange(actual_range, requested);
    return [view.window convertRectToScreen:window_rect];
  }

  NSUInteger CharacterIndex(NSPoint screen_point) const {
    NSView* view = view_;
    if (!IsActive() || view == nil || view.window == nil) {
      return NSNotFound;
    }
    const NSPoint window_point = [view.window convertPointFromScreen:screen_point];
    const NSPoint view_point = [view convertPoint:window_point fromView:nil];
    const TextInputPositionResult position = runtime_->QueryTextInputPosition(
        session_id_,
        {
            static_cast<float>(view_point.x),
            static_cast<float>(view_point.y),
        }
    );
    if (position.result_code != TextInputResultCode::Ok || position.position.offset < 0 ||
        static_cast<std::uint64_t>(position.position.offset) > std::numeric_limits<NSUInteger>::max()) {
      return NSNotFound;
    }
    return static_cast<NSUInteger>(position.position.offset);
  }

  void DoCommand(SEL selector) {
    if (!IsActive()) {
      return;
    }

    Key key = Key::Unknown;
    KeyModifiers modifiers;
    if (selector == @selector(moveLeft:) || selector == @selector(moveBackward:)) {
      key = Key::ArrowLeft;
    } else if (selector == @selector(moveWordLeft:) || selector == @selector(moveWordBackward:)) {
      key = Key::ArrowLeft;
      modifiers.alt = true;
    } else if (selector == @selector(moveRight:) || selector == @selector(moveForward:)) {
      key = Key::ArrowRight;
    } else if (selector == @selector(moveWordRight:) || selector == @selector(moveWordForward:)) {
      key = Key::ArrowRight;
      modifiers.alt = true;
    } else if (selector == @selector(moveLeftAndModifySelection:) ||
               selector == @selector(moveBackwardAndModifySelection:)) {
      key = Key::ArrowLeft;
      modifiers.shift = true;
    } else if (selector == @selector(moveWordLeftAndModifySelection:) ||
               selector == @selector(moveWordBackwardAndModifySelection:)) {
      key = Key::ArrowLeft;
      modifiers.shift = true;
      modifiers.alt = true;
    } else if (selector == @selector(moveRightAndModifySelection:) ||
               selector == @selector(moveForwardAndModifySelection:)) {
      key = Key::ArrowRight;
      modifiers.shift = true;
    } else if (selector == @selector(moveWordRightAndModifySelection:) ||
               selector == @selector(moveWordForwardAndModifySelection:)) {
      key = Key::ArrowRight;
      modifiers.shift = true;
      modifiers.alt = true;
    } else if (selector == @selector(moveUp:)) {
      key = Key::ArrowUp;
    } else if (selector == @selector(moveDown:)) {
      key = Key::ArrowDown;
    } else if (selector == @selector(moveUpAndModifySelection:)) {
      key = Key::ArrowUp;
      modifiers.shift = true;
    } else if (selector == @selector(moveDownAndModifySelection:)) {
      key = Key::ArrowDown;
      modifiers.shift = true;
    } else if (selector == @selector(moveToBeginningOfLine:)) {
      key = Key::Home;
    } else if (selector == @selector(moveToEndOfLine:)) {
      key = Key::End;
    } else if (selector == @selector(moveToBeginningOfDocument:)) {
      key = Key::Home;
      modifiers.meta = true;
    } else if (selector == @selector(moveToEndOfDocument:)) {
      key = Key::End;
      modifiers.meta = true;
    } else if (selector == @selector(moveToBeginningOfLineAndModifySelection:)) {
      key = Key::Home;
      modifiers.shift = true;
    } else if (selector == @selector(moveToEndOfLineAndModifySelection:)) {
      key = Key::End;
      modifiers.shift = true;
    } else if (selector == @selector(moveToBeginningOfDocumentAndModifySelection:)) {
      key = Key::Home;
      modifiers.shift = true;
      modifiers.meta = true;
    } else if (selector == @selector(moveToEndOfDocumentAndModifySelection:)) {
      key = Key::End;
      modifiers.shift = true;
      modifiers.meta = true;
    } else if (selector == @selector(pageUp:)) {
      key = Key::PageUp;
    } else if (selector == @selector(pageDown:)) {
      key = Key::PageDown;
    } else if (selector == @selector(pageUpAndModifySelection:)) {
      key = Key::PageUp;
      modifiers.shift = true;
    } else if (selector == @selector(pageDownAndModifySelection:)) {
      key = Key::PageDown;
      modifiers.shift = true;
    } else if (selector == @selector(deleteBackward:)) {
      key = Key::Backspace;
    } else if (selector == @selector(deleteForward:)) {
      key = Key::Delete;
    } else if (selector == @selector(deleteWordBackward:)) {
      key = Key::Backspace;
      modifiers.alt = true;
    } else if (selector == @selector(deleteWordForward:)) {
      key = Key::Delete;
      modifiers.alt = true;
    } else if (selector == @selector(deleteToBeginningOfLine:)) {
      key = Key::Backspace;
      modifiers.meta = true;
    } else if (selector == @selector(deleteToEndOfLine:)) {
      key = Key::Delete;
      modifiers.meta = true;
    } else if (selector == @selector(insertNewline:) || selector == @selector(insertLineBreak:)) {
      key = Key::Enter;
    } else if (selector == @selector(insertTab:) || selector == @selector(insertTabIgnoringFieldEditor:)) {
      key = Key::Tab;
    } else if (selector == @selector(insertBacktab:)) {
      key = Key::Tab;
      modifiers.shift = true;
    } else if (selector == @selector(cancelOperation:)) {
      key = Key::Escape;
    }
    if (key == Key::Unknown) {
      return;
    }
    runtime_->HandleKeyEvent({
        KeyEventType::Down,
        key,
        {},
        modifiers,
    });
  }

private:
  void UpdateSecureEventInput(bool secure) {
    const bool enable = secure && application_active_ && IsActive();
    if (enable == secure_event_input_enabled_) {
      return;
    }
    const OSStatus status = enable ? EnableSecureEventInput() : DisableSecureEventInput();
    if (status == noErr) {
      secure_event_input_enabled_ = enable;
    }
  }

  TextInputContext QueryContext() const {
    if (!IsActive()) {
      TextInputContext result;
      result.result_code = TextInputResultCode::SessionMismatch;
      return result;
    }
    return runtime_->QueryTextInputContext(session_id_, 0, 0);
  }

  void Apply(TextInputCommand command) {
    runtime_->HandleTextInputCommands({
        session_id_,
        {std::move(command)},
    });
  }

  static void SetActualRange(NSRangePointer output, std::optional<TextRange> range) {
    if (output != nullptr) {
      *output = range.has_value() ? ToNSRange(*range) : NSMakeRange(NSNotFound, 0);
    }
  }

  Runtime* runtime_ = nullptr;
  __weak NSView* view_ = nil;
  __strong HuxerUITextInputClient* client_ = nil;
  __strong NSTextInputContext* input_context_ = nil;
  TextInputSessionId session_id_ = 0;
  TextInputConfiguration configuration_;
  bool suppress_callbacks_ = false;
  bool application_active_ = true;
  bool secure_event_input_enabled_ = false;
};

MacTextInput::MacTextInput(Runtime& runtime, NSView* view) : state_(std::make_unique<MacTextInputState>(runtime, view)) {}

MacTextInput::~MacTextInput() = default;

NSTextInputContext* MacTextInput::InputContext() const noexcept {
  return state_->InputContext();
}

bool MacTextInput::HandleEvent(NSEvent* event) {
  return state_->HandleEvent(event);
}

bool MacTextInput::IsActive() const noexcept {
  return state_->IsActive();
}

void MacTextInput::InvalidateGeometry() {
  state_->InvalidateGeometry();
}

void MacTextInput::ApplicationActiveChanged(bool active) {
  state_->ApplicationActiveChanged(active);
}

void MacTextInput::Start(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Start(session_id, configuration, state, geometry);
}

void MacTextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  state_->Update(session_id, state, geometry);
}

void MacTextInput::Restart(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Restart(session_id, configuration, state, geometry);
}

void MacTextInput::Stop(TextInputSessionId session_id) {
  state_->Stop(session_id);
}

} // namespace huxerui::detail

@implementation HuxerUITextInputClient

- (instancetype)initWithState:(huxerui::detail::MacTextInputState*)state {
  self = [super init];
  if (self != nil) {
    huxeruiState = state;
  }
  return self;
}

- (BOOL)hasMarkedText {
  return huxeruiState != nullptr && huxeruiState->HasMarkedText();
}

- (NSRange)markedRange {
  return huxeruiState == nullptr ? NSMakeRange(NSNotFound, 0) : huxeruiState->MarkedRange();
}

- (NSRange)selectedRange {
  return huxeruiState == nullptr ? NSMakeRange(NSNotFound, 0) : huxeruiState->SelectedRange();
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
  if (huxeruiState != nullptr) {
    huxeruiState->SetMarkedText(string, selectedRange, replacementRange);
  }
}

- (void)unmarkText {
  if (huxeruiState != nullptr) {
    huxeruiState->UnmarkText();
  }
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  return huxeruiState == nullptr ? nil : huxeruiState->AttributedSubstring(range, actualRange);
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
  if (huxeruiState != nullptr) {
    huxeruiState->InsertText(string, replacementRange);
  }
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return huxeruiState == nullptr ? NSNotFound : huxeruiState->CharacterIndex(point);
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  return huxeruiState == nullptr ? NSZeroRect : huxeruiState->FirstRect(range, actualRange);
}

- (void)doCommandBySelector:(SEL)selector {
  if (huxeruiState != nullptr) {
    huxeruiState->DoCommand(selector);
  }
}

@end
