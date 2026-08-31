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
  case kVK_ANSI_A:
    return Key::A;
  case kVK_ANSI_B:
    return Key::B;
  case kVK_ANSI_C:
    return Key::C;
  case kVK_ANSI_D:
    return Key::D;
  case kVK_ANSI_E:
    return Key::E;
  case kVK_ANSI_F:
    return Key::F;
  case kVK_ANSI_G:
    return Key::G;
  case kVK_ANSI_H:
    return Key::H;
  case kVK_ANSI_I:
    return Key::I;
  case kVK_ANSI_J:
    return Key::J;
  case kVK_ANSI_K:
    return Key::K;
  case kVK_ANSI_L:
    return Key::L;
  case kVK_ANSI_M:
    return Key::M;
  case kVK_ANSI_N:
    return Key::N;
  case kVK_ANSI_O:
    return Key::O;
  case kVK_ANSI_P:
    return Key::P;
  case kVK_ANSI_Q:
    return Key::Q;
  case kVK_ANSI_R:
    return Key::R;
  case kVK_ANSI_S:
    return Key::S;
  case kVK_ANSI_T:
    return Key::T;
  case kVK_ANSI_U:
    return Key::U;
  case kVK_ANSI_V:
    return Key::V;
  case kVK_ANSI_W:
    return Key::W;
  case kVK_ANSI_X:
    return Key::X;
  case kVK_ANSI_Y:
    return Key::Y;
  case kVK_ANSI_Z:
    return Key::Z;
  case kVK_ANSI_0:
    return Key::Digit0;
  case kVK_ANSI_1:
    return Key::Digit1;
  case kVK_ANSI_2:
    return Key::Digit2;
  case kVK_ANSI_3:
    return Key::Digit3;
  case kVK_ANSI_4:
    return Key::Digit4;
  case kVK_ANSI_5:
    return Key::Digit5;
  case kVK_ANSI_6:
    return Key::Digit6;
  case kVK_ANSI_7:
    return Key::Digit7;
  case kVK_ANSI_8:
    return Key::Digit8;
  case kVK_ANSI_9:
    return Key::Digit9;
  case kVK_ANSI_Grave:
    return Key::Backquote;
  case kVK_ANSI_Minus:
    return Key::Minus;
  case kVK_ANSI_Equal:
    return Key::Equal;
  case kVK_ANSI_LeftBracket:
    return Key::BracketLeft;
  case kVK_ANSI_RightBracket:
    return Key::BracketRight;
  case kVK_ANSI_Backslash:
    return Key::Backslash;
  case kVK_ANSI_Semicolon:
    return Key::Semicolon;
  case kVK_ANSI_Quote:
    return Key::Quote;
  case kVK_ANSI_Comma:
    return Key::Comma;
  case kVK_ANSI_Period:
    return Key::Period;
  case kVK_ANSI_Slash:
    return Key::Slash;
  case kVK_ISO_Section:
    return Key::IntlBackslash;
  case kVK_JIS_Underscore:
    return Key::IntlRo;
  case kVK_JIS_Yen:
    return Key::IntlYen;
  case kVK_Shift:
    return Key::ShiftLeft;
  case kVK_RightShift:
    return Key::ShiftRight;
  case kVK_Control:
    return Key::ControlLeft;
  case kVK_RightControl:
    return Key::ControlRight;
  case kVK_Option:
    return Key::AltLeft;
  case kVK_RightOption:
    return Key::AltRight;
  case kVK_Command:
    return Key::MetaLeft;
  case kVK_RightCommand:
    return Key::MetaRight;
  case kVK_CapsLock:
    return Key::CapsLock;
  case kVK_Delete:
    return Key::Backspace;
  case kVK_Tab:
    return Key::Tab;
  case kVK_Return:
    return Key::Enter;
  case kVK_Escape:
    return Key::Escape;
  case kVK_Space:
    return Key::Space;
  case kVK_ForwardDelete:
    return Key::Delete;
  case kVK_Home:
    return Key::Home;
  case kVK_End:
    return Key::End;
  case kVK_PageUp:
    return Key::PageUp;
  case kVK_PageDown:
    return Key::PageDown;
  case kVK_LeftArrow:
    return Key::ArrowLeft;
  case kVK_RightArrow:
    return Key::ArrowRight;
  case kVK_UpArrow:
    return Key::ArrowUp;
  case kVK_DownArrow:
    return Key::ArrowDown;
  case kVK_F1:
    return Key::F1;
  case kVK_F2:
    return Key::F2;
  case kVK_F3:
    return Key::F3;
  case kVK_F4:
    return Key::F4;
  case kVK_F5:
    return Key::F5;
  case kVK_F6:
    return Key::F6;
  case kVK_F7:
    return Key::F7;
  case kVK_F8:
    return Key::F8;
  case kVK_F9:
    return Key::F9;
  case kVK_F10:
    return Key::F10;
  case kVK_F11:
    return Key::F11;
  case kVK_F12:
    return Key::F12;
  case kVK_F13:
    return Key::F13;
  case kVK_F14:
    return Key::F14;
  case kVK_F15:
    return Key::F15;
  case kVK_F16:
    return Key::F16;
  case kVK_F17:
    return Key::F17;
  case kVK_F18:
    return Key::F18;
  case kVK_F19:
    return Key::F19;
  case kVK_F20:
    return Key::F20;
  case kVK_ContextualMenu:
    return Key::ContextMenu;
  case kVK_Help:
    return Key::Help;
  case kVK_ANSI_Keypad0:
    return Key::Numpad0;
  case kVK_ANSI_Keypad1:
    return Key::Numpad1;
  case kVK_ANSI_Keypad2:
    return Key::Numpad2;
  case kVK_ANSI_Keypad3:
    return Key::Numpad3;
  case kVK_ANSI_Keypad4:
    return Key::Numpad4;
  case kVK_ANSI_Keypad5:
    return Key::Numpad5;
  case kVK_ANSI_Keypad6:
    return Key::Numpad6;
  case kVK_ANSI_Keypad7:
    return Key::Numpad7;
  case kVK_ANSI_Keypad8:
    return Key::Numpad8;
  case kVK_ANSI_Keypad9:
    return Key::Numpad9;
  case kVK_ANSI_KeypadDecimal:
    return Key::NumpadDecimal;
  case kVK_ANSI_KeypadDivide:
    return Key::NumpadDivide;
  case kVK_ANSI_KeypadMultiply:
    return Key::NumpadMultiply;
  case kVK_ANSI_KeypadMinus:
    return Key::NumpadSubtract;
  case kVK_ANSI_KeypadPlus:
    return Key::NumpadAdd;
  case kVK_ANSI_KeypadEnter:
    return Key::NumpadEnter;
  case kVK_ANSI_KeypadEquals:
    return Key::NumpadEqual;
  case kVK_JIS_KeypadComma:
    return Key::NumpadComma;
  case kVK_ANSI_KeypadClear:
    return Key::NumpadClear;
  default:
    return Key::Unknown;
  }
}

Key TranslateKey(NSEvent* event) {
  NSString* characters = event.charactersIgnoringModifiers;
  if (characters.length == 1) {
    const unichar character = [characters characterAtIndex:0];
    if (character >= 'a' && character <= 'z') {
      return static_cast<Key>(static_cast<int>(Key::A) + character - 'a');
    }
    if (character >= 'A' && character <= 'Z') {
      return static_cast<Key>(static_cast<int>(Key::A) + character - 'A');
    }
  }
  return TranslateKey(event.keyCode);
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
      TranslateKey(event),
      type == KeyEventType::Down && characters != nullptr ? std::string(characters) : std::string{},
      {
          static_cast<bool>(flags & NSEventModifierFlagShift),
          static_cast<bool>(flags & NSEventModifierFlagControl),
          static_cast<bool>(flags & NSEventModifierFlagOption),
          static_cast<bool>(flags & NSEventModifierFlagCommand),
      },
      type == KeyEventType::Down && static_cast<bool>(event.isARepeat),
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
    const bool composing = HasMarkedText();
    if (composing) {
      const bool handled = [input_context_ handleEvent:event] == YES;
      if (handled) {
        return true;
      }
    }
    KeyEvent key_event = MakeMacKeyEvent(event, KeyEventType::Down);
    key_event.text.clear();
    if (runtime_->HandleKeyEvent(key_event)) {
      return true;
    }
    if (composing) {
      return false;
    }
    routed_event_ = key_event;
    const bool handled = [input_context_ handleEvent:event] == YES;
    routed_event_.reset();
    return handled;
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
    if (routed_event_.has_value() && routed_event_->key == key && routed_event_->modifiers == modifiers) {
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
  std::optional<KeyEvent> routed_event_;
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
