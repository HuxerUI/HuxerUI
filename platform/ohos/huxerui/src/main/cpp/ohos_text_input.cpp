#include "ohos_text_input.h"

#include <arkui/native_node.h>
#include <inputmethod/inputmethod_types_capi.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/event.h>

namespace huxerui::detail {
namespace {

constexpr TextOffset context_limit = 4096;
std::uint64_t next_token = 0;

enum class EditCommand {
  Insert, DeleteLeft, DeleteRight, Select, Preview, FinishPreview, Enter, Move, Extend, Clipboard, KeyboardHidden,
};

enum class ContextQuery { Left, Right, Cursor };

void Check(napi_status status) {
  if (status != napi_ok) {
    throw std::runtime_error("HuxerUI could not communicate with the OHOS input method");
  }
}

class HandleScope {
public:
  explicit HandleScope(napi_env env) : env_(env) { Check(napi_open_handle_scope(env_, &scope_)); }
  ~HandleScope() { napi_close_handle_scope(env_, scope_); }

  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;

private:
  napi_env env_;
  napi_handle_scope scope_ = nullptr;
};

std::int32_t ReadInteger(napi_env env, napi_value value) {
  std::int32_t number = 0;
  Check(napi_get_value_int32(env, value, &number));
  return number;
}

InputMethod_TextInputType InputType(const TextInputConfiguration& configuration) {
  if (configuration.secure) {
    return configuration.type == TextInputType::Number ? IME_TEXT_INPUT_TYPE_NUMBER_PASSWORD
                                                       : IME_TEXT_INPUT_TYPE_VISIBLE_PASSWORD;
  }
  switch (configuration.type) {
  case TextInputType::Text: return configuration.multiline ? IME_TEXT_INPUT_TYPE_MULTILINE : IME_TEXT_INPUT_TYPE_TEXT;
  case TextInputType::Email: return IME_TEXT_INPUT_TYPE_EMAIL_ADDRESS;
  case TextInputType::Number: return IME_TEXT_INPUT_TYPE_NUMBER;
  case TextInputType::Decimal: return IME_TEXT_INPUT_TYPE_NUMBER_DECIMAL;
  case TextInputType::Phone: return IME_TEXT_INPUT_TYPE_PHONE;
  case TextInputType::Url: return IME_TEXT_INPUT_TYPE_URL;
  }
  return IME_TEXT_INPUT_TYPE_TEXT;
}

InputMethod_EnterKeyType EnterKey(const TextInputConfiguration& configuration) {
  switch (configuration.action) {
  case TextInputAction::Default: return configuration.multiline ? IME_ENTER_KEY_NEWLINE : IME_ENTER_KEY_DONE;
  case TextInputAction::Done: return IME_ENTER_KEY_DONE;
  case TextInputAction::Go: return IME_ENTER_KEY_GO;
  case TextInputAction::Next: return IME_ENTER_KEY_NEXT;
  case TextInputAction::Search: return IME_ENTER_KEY_SEARCH;
  case TextInputAction::Send: return IME_ENTER_KEY_SEND;
  case TextInputAction::Newline: return IME_ENTER_KEY_NEWLINE;
  }
  return IME_ENTER_KEY_UNSPECIFIED;
}

Key DirectionKey(std::int32_t direction) {
  switch (direction) {
  case IME_DIRECTION_UP: return Key::ArrowUp;
  case IME_DIRECTION_DOWN: return Key::ArrowDown;
  case IME_DIRECTION_LEFT: return Key::ArrowLeft;
  case IME_DIRECTION_RIGHT: return Key::ArrowRight;
  default: return Key::Unknown;
  }
}

std::optional<TextEditingAction> ClipboardAction(std::int32_t action) {
  switch (action) {
  case IME_EXTEND_ACTION_SELECT_ALL: return TextEditingAction::SelectAll;
  case IME_EXTEND_ACTION_CUT: return TextEditingAction::Cut;
  case IME_EXTEND_ACTION_COPY: return TextEditingAction::Copy;
  case IME_EXTEND_ACTION_PASTE: return TextEditingAction::Paste;
  default: return std::nullopt;
  }
}

} // namespace

OhosTextInput::OhosTextInput(UiWindow& window, ArkUI_NodeHandle node, const float& scale, napi_env env, napi_value host)
    : window_(window), node_(node), scale_(scale), env_(env) {
  Check(napi_create_reference(env_, host, 1, &host_));
}

OhosTextInput::~OhosTextInput() {
  ReleaseHost();
}

void OhosTextInput::Start(TextInputSessionId session_id, const TextInputConfiguration& configuration,
                          const TextInputState& state, const TextInputGeometry& geometry) {
  state_ = state;
  configuration_ = configuration;
  selection_sent_ = false;
  if (!focused_) return;
  const HandleScope scope(env_);
  token_ = std::to_string(++next_token);
  napi_value token;
  Check(napi_create_string_utf8(env_, token_.data(), token_.size(), &token));
  Call("connect", {token, Number(InputType(configuration)), Number(EnterKey(configuration))});
  Update(session_id, state, geometry);
}

void OhosTextInput::Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) {
  if (state_.session_id != session_id) {
    return;
  }
  const bool selection_changed = !selection_sent_ || state_.selection != state.selection ||
                                 state_.content_revision != state.content_revision;
  state_ = state;
  if (!focused_) return;
  const HandleScope scope(env_);
  if (selection_changed) {
    auto context = TextInputContext{};
    if (!configuration_.secure) {
      context = window_.QueryTextInputContext(session_id, 0, std::numeric_limits<TextOffset>::max());
    }
    // changeSelection takes document offsets and the corresponding text. Secure editors send neither.
    const auto range = context.result_code == TextInputResultCode::Ok ? state.selection.Range() : TextRange{};
    napi_value text;
    Check(napi_create_string_utf8(env_, context.text.data(), context.text.size(), &text));
    Call("changeSelection", {text, Number(range.start), Number(range.end)});
    selection_sent_ = true;
  }
  ArkUI_IntOffset origin{};
  if (geometry.result_code == TextInputResultCode::Ok &&
      OH_ArkUI_NodeUtils_GetLayoutPositionInScreen(node_, &origin) == 0) {
    const auto& caret = geometry.caret;
    Call("updateCursor", {Number(origin.x + caret.x * scale_), Number(origin.y + caret.y * scale_),
                          Number(caret.width * scale_), Number(caret.height * scale_)});
  }
}

void OhosTextInput::Restart(TextInputSessionId session_id, const TextInputConfiguration& configuration,
                            const TextInputState& state, const TextInputGeometry& geometry) {
  const bool show = show_keyboard_;
  Stop(session_id);
  Start(session_id, configuration, state, geometry);
  if (show) {
    RequestShowKeyboard(session_id);
  }
}

void OhosTextInput::Stop(TextInputSessionId session_id) {
  if (state_.session_id == session_id) {
    state_ = {};
    show_keyboard_ = false;
    if (!std::exchange(token_, {}).empty()) Call("disconnect", {});
  }
}

void OhosTextInput::RequestShowKeyboard(TextInputSessionId session_id) {
  if (state_.session_id == session_id) {
    show_keyboard_ = true;
    if (focused_) Call("show", {});
  }
}

void OhosTextInput::RequestHideKeyboard(TextInputSessionId session_id) {
  if (state_.session_id == session_id) {
    show_keyboard_ = false;
    if (focused_) Call("hide", {});
  }
}

void OhosTextInput::ShowKeyboard() {
  if (state_.session_id && !configuration_.read_only) RequestShowKeyboard(state_.session_id);
}

bool OhosTextInput::HandleBack() {
  if (!show_keyboard_) {
    return false;
  }
  RequestHideKeyboard(state_.session_id);
  return true;
}

void OhosTextInput::SetFocused(bool focused) {
  if (std::exchange(focused_, focused) == focused || !state_.session_id) return;
  if (focused) {
    const auto session = state_.session_id;
    Start(session, configuration_, state_, window_.QueryTextInputGeometry(session, state_.selection.Range()));
    if (show_keyboard_) RequestShowKeyboard(session);
  } else {
    // Revoke the window's token before disconnecting the shared IME or applying its last provisional text.
    if (!std::exchange(token_, {}).empty()) Call("disconnect", {});
    show_keyboard_ = false;
    if (state_.composition) {
      TextInputCommand command;
      command.kind = TextInputCommandKind::FinishComposition;
      window_.HandleTextInputCommands({state_.session_id, {std::move(command)}});
    }
  }
}

napi_value OhosTextInput::HandleCommand(std::span<const napi_value, 5> args) {
  if (!Matches(args[0])) {
    return nullptr;
  }
  const auto kind = static_cast<EditCommand>(ReadInteger(env_, args[1]));
  const TextOffset first = ReadInteger(env_, args[2]);
  const TextOffset second = ReadInteger(env_, args[3]);
  TextInputCommand command;
  switch (kind) {
  case EditCommand::Insert:
    command.kind = TextInputCommandKind::CommitText;
    command.text = ReadString(args[4]);
    break;
  case EditCommand::DeleteLeft:
  case EditCommand::DeleteRight:
    if (first <= 0) {
      return nullptr;
    }
    if (!state_.selection.IsCollapsed() && !state_.composition) {
      command.kind = TextInputCommandKind::CommitText;
    } else {
      command.kind = TextInputCommandKind::DeleteSurrounding;
      (kind == EditCommand::DeleteLeft ? command.delete_before : command.delete_after) = first;
    }
    break;
  case EditCommand::Select:
    command.selection_after = TextSelection{first, second};
    break;
  case EditCommand::Preview:
    command.kind = TextInputCommandKind::UpdateComposition;
    command.text = ReadString(args[4]);
    if (first >= 0 && second >= 0) {
      if (const auto& composition = state_.composition) {
        command.coordinate_space = TextInputCoordinateSpace::Composition;
        command.target = TextRange{first - composition->start, second - composition->start};
      } else {
        command.target = TextRange{first, second};
      }
    } else if (first != -1 || second != -1) {
      return nullptr;
    }
    break;
  case EditCommand::FinishPreview:
    command.kind = TextInputCommandKind::FinishComposition;
    break;
  case EditCommand::Enter:
    window_.PerformTextInputAction(state_.session_id, configuration_.action);
    return nullptr;
  case EditCommand::Move:
  case EditCommand::Extend:
    if (const auto key = DirectionKey(first); key != Key::Unknown) {
      window_.HandleKeyEvent({.key = key, .modifiers = {.shift = kind == EditCommand::Extend}});
    }
    return nullptr;
  case EditCommand::Clipboard:
    if (const auto action = ClipboardAction(first)) {
      window_.PerformTextEditingAction(*action);
    }
    return nullptr;
  case EditCommand::KeyboardHidden:
    show_keyboard_ = false;
    return nullptr;
  default: return nullptr;
  }
  auto result = window_.HandleTextInputCommands({state_.session_id, {std::move(command)}});
  if (result.result_code == TextInputResultCode::Rejected && first == 1 &&
      (kind == EditCommand::DeleteLeft || kind == EditCommand::DeleteRight)) {
    // A one-unit deletion may split a surrogate pair. Use the shared grapheme-aware key handler in that case.
    window_.HandleKeyEvent({.key = kind == EditCommand::DeleteLeft ? Key::Backspace : Key::Delete});
  }
  return nullptr;
}

napi_value OhosTextInput::ReadContext(std::span<const napi_value, 3> args) {
  const auto kind = static_cast<ContextQuery>(ReadInteger(env_, args[1]));
  const bool allowed = Matches(args[0]) && !configuration_.secure;
  const auto caret = state_.selection.active;
  if (kind == ContextQuery::Cursor) {
    return Number(allowed ? caret : 0);
  }
  const auto length = std::clamp<TextOffset>(ReadInteger(env_, args[2]), 0, context_limit);
  std::string text;
  if (allowed && length > 0) {
    const auto start = kind == ContextQuery::Left ? std::max<TextOffset>(0, caret - length) : caret;
    const auto limit = kind == ContextQuery::Left ? caret - start : length;
    auto context = window_.QueryTextInputContext(state_.session_id, start, limit);
    if (context.result_code == TextInputResultCode::Ok) {
      text = std::move(context.text);
    }
  }
  napi_value result;
  Check(napi_create_string_utf8(env_, text.data(), text.size(), &result));
  return result;
}

void OhosTextInput::ReleaseHost() noexcept {
  if (host_) {
    napi_delete_reference(env_, std::exchange(host_, nullptr));
  }
}

napi_value OhosTextInput::Number(double value) const {
  napi_value result;
  Check(napi_create_double(env_, value, &result));
  return result;
}

void OhosTextInput::Call(const char* name, std::initializer_list<napi_value> arguments) const {
  if (!host_) {
    return;
  }
  const HandleScope scope(env_);
  napi_value receiver;
  napi_value function;
  Check(napi_get_reference_value(env_, host_, &receiver));
  Check(napi_get_named_property(env_, receiver, name, &function));
  Check(napi_call_function(env_, receiver, function, arguments.size(), arguments.begin(), nullptr));
}

std::string OhosTextInput::ReadString(napi_value value) const {
  std::size_t length = 0;
  Check(napi_get_value_string_utf8(env_, value, nullptr, 0, &length));
  std::string text(length + 1, '\0');
  Check(napi_get_value_string_utf8(env_, value, text.data(), text.size(), &length));
  text.resize(length);
  return text;
}

bool OhosTextInput::Matches(napi_value token) const {
  return state_.session_id != 0 && ReadString(token) == token_;
}

} // namespace huxerui::detail
