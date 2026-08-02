#include "linux_text_input.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/app.h>

#include "linux_text_input_internal.h"
#include "text_input_internal.h"

namespace huxerui::detail {

namespace {

// XIM style preedit/status components occupy the low and high 16 bits of a
// style; Xlib exposes no named mask, so define the preedit component mask.
constexpr XIMStyle kXimPreeditMask = 0x000F;

// Decodes one UTF-8 code point starting at *byte_offset of `text`, advances
// the offset past it, and returns the code point value. Returns std::nullopt
// for invalid UTF-8 or when the offset is out of range.
std::optional<std::uint32_t> DecodeUtf8CodePoint(std::string_view text, std::size_t& byte_offset) {
  if (byte_offset >= text.size()) {
    return std::nullopt;
  }
  const auto lead = static_cast<std::uint8_t>(text[byte_offset]);
  std::size_t length = 0;
  std::uint32_t value = 0;
  if (lead <= 0x7FU) {
    length = 1;
    value = lead;
  } else if (lead >= 0xC2U && lead <= 0xDFU) {
    length = 2;
    value = lead & 0x1FU;
  } else if (lead >= 0xE0U && lead <= 0xEFU) {
    length = 3;
    value = lead & 0x0FU;
  } else if (lead >= 0xF0U && lead <= 0xF4U) {
    length = 4;
    value = lead & 0x07U;
  } else {
    return std::nullopt;
  }
  if (byte_offset + length > text.size()) {
    return std::nullopt;
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<std::uint8_t>(text[byte_offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    value = (value << 6) | (continuation & 0x3FU);
  }
  // Reject overlong encodings, surrogate code points, and values above the
  // Unicode range.
  if ((length == 2 && value < 0x80U) || (length == 3 && value < 0x800U) || (length == 4 && value < 0x10000U) ||
      (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) {
    return std::nullopt;
  }
  byte_offset += length;
  return value;
}

// Returns the byte offset of the code_point_index-th code point of `text`.
std::optional<std::size_t> Utf8ByteOffsetOfCodePoint(std::string_view text, int code_point_index) noexcept {
  if (code_point_index < 0) {
    return std::nullopt;
  }
  std::size_t byte_offset = 0;
  for (int index = 0; index < code_point_index; ++index) {
    if (!DecodeUtf8CodePoint(text, byte_offset).has_value()) {
      return std::nullopt;
    }
  }
  return byte_offset;
}

// Appends the UTF-8 encoding of code_point; returns false for invalid input.
bool AppendUtf8(std::string& output, std::uint32_t code_point) {
  if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
    return false;
  }
  if (code_point <= 0x7FU) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0x10FFFFU) {
    output.push_back(static_cast<char>(0xF0U | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    return false;
  }
  return true;
}

// Converts a wide-character XIM string (used when encoding_is_wchar is set)
// into UTF-8. Linux wchar_t is 32-bit; UTF-16 surrogate pairs are combined for
// portability on platforms with 16-bit wchar_t.
std::string WideTextToUtf8(const wchar_t* text) {
  std::string result;
  if (text == nullptr) {
    return result;
  }
  for (const wchar_t* current = text; *current != L'\0'; ++current) {
    std::uint32_t code_point = static_cast<std::uint32_t>(*current);
    if (sizeof(wchar_t) == 2 && code_point >= 0xD800U && code_point <= 0xDBFFU) {
      if (current[1] == L'\0' || current[1] < 0xDC00 || current[1] > 0xDFFF) {
        return {};
      }
      code_point = 0x10000U + ((code_point - 0xD800U) << 10) + (static_cast<std::uint32_t>(current[1]) - 0xDC00U);
      ++current;
    }
    if (!AppendUtf8(result, code_point)) {
      return {};
    }
  }
  return result;
}

// Clamps a scaled pixel coordinate into the short range used by XPoint.
short ClampToShort(long value) noexcept {
  if (value <= std::numeric_limits<short>::min()) {
    return std::numeric_limits<short>::min();
  }
  if (value >= std::numeric_limits<short>::max()) {
    return std::numeric_limits<short>::max();
  }
  return static_cast<short>(value);
}

constexpr int kXimCommitBufferBytes = 512;

} // namespace

std::optional<int> Utf8CodePointCount(std::string_view text) noexcept {
  int count = 0;
  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    if (!DecodeUtf8CodePoint(text, byte_offset).has_value() || count == std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    ++count;
  }
  return count;
}

std::optional<TextOffset> Utf8PrefixUtf16Length(std::string_view text, int code_point_count) noexcept {
  if (code_point_count < 0) {
    return std::nullopt;
  }
  TextOffset utf16_length = 0;
  std::size_t byte_offset = 0;
  for (int index = 0; index < code_point_count; ++index) {
    const std::optional<std::uint32_t> code_point = DecodeUtf8CodePoint(text, byte_offset);
    if (!code_point.has_value()) {
      return std::nullopt;
    }
    const TextOffset width = *code_point > 0xFFFFU ? 2 : 1;
    if (utf16_length > std::numeric_limits<TextOffset>::max() - width) {
      return std::nullopt;
    }
    utf16_length += width;
  }
  return utf16_length;
}

std::optional<std::string>
ApplyXimPreeditEdit(std::string_view current, int chg_first, int chg_length, std::string_view replacement) {
  if (chg_first < 0 || chg_length < 0) {
    return std::nullopt;
  }
  const std::optional<std::size_t> range_start = Utf8ByteOffsetOfCodePoint(current, chg_first);
  const std::optional<std::size_t> range_end = Utf8ByteOffsetOfCodePoint(current, chg_first + chg_length);
  const std::optional<int> replacement_count = Utf8CodePointCount(replacement);
  if (!range_start.has_value() || !range_end.has_value() || !replacement_count.has_value()) {
    return std::nullopt;
  }
  if (*range_end < *range_start || *range_end > current.size()) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(current.size() + replacement.size());
  result.append(current.substr(0, *range_start));
  result.append(replacement);
  result.append(current.substr(*range_end));
  return result;
}

struct LinuxTextInput::State {
  // XIM callbacks are registered as XIMProc; the preedit start callback returns
  // int (non-zero continues the preedit) and the remaining callbacks receive
  // typed call_data. Casting to XIMProc is the established XIM client
  // convention used by GTK and SDL.
  static int XimStartCallback(XIM im, XPointer client_data, XPointer call_data);
  static void XimDrawCallback(XIM im, XPointer client_data, XPointer call_data);
  static void XimCaretCallback(XIM im, XPointer client_data, XPointer call_data);
  static void XimDoneCallback(XIM im, XPointer client_data, XPointer call_data);

  void SetRuntime(Runtime* value) noexcept {
    runtime = value;
  }

  void SetDisplayAndWindow(Display* value_display, Window value_window) {
    if (value_display == display && value_window == window) {
      return;
    }
    CloseInputContext();
    CloseInputMethod();
    display = value_display;
    window = value_window;
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
  }

  void SetDpiScale(float scale) noexcept {
    dpi_scale = std::isfinite(scale) && scale > 0.0F ? scale : 1.0F;
  }

  void Reset() noexcept {
    CloseInputContext();
    CloseInputMethod();
    runtime = nullptr;
    display = nullptr;
    window = 0;
    session_id = 0;
    configuration = {};
    text_input_state = {};
    composing = false;
    focused = false;
    composition_text.clear();
    composition_caret_code_points = 0;
  }

  [[nodiscard]] bool Active() const noexcept {
    return session_id != 0;
  }

  [[nodiscard]] bool Composing() const noexcept {
    return composing;
  }

  [[nodiscard]] XIC InputContext() const noexcept {
    return xic;
  }

  void SetFocus(bool value) {
    focused = value;
    if (xic != nullptr) {
      if (focused) {
        XSetICFocus(xic);
      } else {
        XUnsetICFocus(xic);
      }
    }
  }

  [[nodiscard]] bool HandleXKeyEvent(const XKeyEvent& event) {
    if (display == nullptr || window == 0 || xic == nullptr) {
      return false;
    }
    // Every key event must reach XFilterEvent: input methods switch triggers
    // (for example Shift on fcitx) are detected on key release. Commit text
    // only on presses; releases are filtered for state tracking only.
    const bool is_release = event.type != KeyPress;
    try {
      XEvent filtered{};
      filtered.xkey = event;
      const bool consumed = XFilterEvent(&filtered, window) != 0;
      if (consumed) {
        // The input method took the key. Report it as handled even when no
        // committed text follows (preedit progress, IME hotkeys): letting it
        // re-enter the plain key path would duplicate characters into the
        // editor. Commit text, when any, arrives through Xutf8LookupString.
        if (!is_release && !composing) {
          CommitCommittedText(event);
        }
        return true;
      }
      if (!Active()) {
        return false;
      }
      return is_release ? true : CommitCommittedText(event);
    } catch (...) {
      // Never let an input-method failure escape into the host event loop.
      return false;
    }
  }

  void Start(
      TextInputSessionId requested_session,
      const TextInputConfiguration& config,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    session_id = requested_session;
    configuration = config;
    text_input_state = state;
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
    EnsureInputMethod();
    UpdateSpot(geometry);
  }

  void Update(TextInputSessionId requested_session, const TextInputState& state, const TextInputGeometry& geometry) {
    if (requested_session != session_id) {
      return;
    }
    text_input_state = state;
    UpdateSpot(geometry);
  }

  void Restart(
      TextInputSessionId requested_session,
      const TextInputConfiguration& config,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    if (requested_session != session_id) {
      return;
    }
    configuration = config;
    text_input_state = state;
    ResetNativeComposition();
    UpdateSpot(geometry);
  }

  void Stop(TextInputSessionId requested_session) {
    if (requested_session != session_id) {
      return;
    }
    ResetNativeComposition();
    session_id = 0;
    configuration = {};
    text_input_state = {};
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
  }

  // ---- XIM setup ----

  bool EnsureInputMethod() {
    if (xic != nullptr) {
      return true;
    }
    if (display == nullptr || window == 0) {
      return false;
    }
    if (xim == nullptr) {
      // XSetLocaleModifiers must reflect the process locale before XOpenIM; the
      // empty list restores the environment-derived modifiers.
      XSetLocaleModifiers("");
      xim = XOpenIM(display, nullptr, nullptr, nullptr);
      if (xim == nullptr) {
        // No input method server is reachable; the host falls back to plain key
        // events. IME absence must not throw.
        return false;
      }
    }

    XIMStyles* styles = nullptr;
    if (XGetIMValues(xim, XNQueryInputStyle, &styles, nullptr) != nullptr || styles == nullptr) {
      return false;
    }

    // Choose the preedit component only among the styles the server actually
    // advertises. fcitx5 accepts an unadvertised XIMPreeditCallbacks context
    // but then never delivers preedit callbacks, leaving the client without
    // preedit text and without a candidate window; the advertised list is the
    // only trustworthy signal. Prefer on-the-spot callbacks when advertised
    // (ibus, GTK style); otherwise fall back to over-the-spot modes and
    // finally to a bare XIMPreeditNothing context, where the input method
    // draws its own candidate window and committed text arrives through
    // Xutf8LookupString.
    bool advertised_preedit[16] = {};
    for (int index = 0; index < styles->count_styles; ++index) {
      advertised_preedit[styles->supported_styles[index] & kXimPreeditMask] = true;
    }
    XFree(styles);

    constexpr std::array<XIMStyle, 4> kPreeditPriority = {
        XIMPreeditCallbacks,
        XIMPreeditPosition,
        XIMPreeditArea,
        XIMPreeditNothing,
    };

    const auto create_context = [&](XIMStyle preedit_style, bool callbacks) -> XIC {
      if (callbacks) {
        // A callbacks context still requires a status component; XCreateIC
        // rejects the bare XIMPreeditCallbacks style.
        const XIMStyle full_style = preedit_style | XIMStatusNothing;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        XIMCallback preedit_start{reinterpret_cast<XPointer>(this), reinterpret_cast<XIMProc>(XimStartCallback)};
        XIMCallback preedit_draw{reinterpret_cast<XPointer>(this), reinterpret_cast<XIMProc>(XimDrawCallback)};
        XIMCallback preedit_caret{reinterpret_cast<XPointer>(this), reinterpret_cast<XIMProc>(XimCaretCallback)};
        XIMCallback preedit_done{reinterpret_cast<XPointer>(this), reinterpret_cast<XIMProc>(XimDoneCallback)};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        XVaNestedList preedit_attributes = XVaCreateNestedList(
            0,
            XNPreeditStartCallback,
            &preedit_start,
            XNPreeditDrawCallback,
            &preedit_draw,
            XNPreeditCaretCallback,
            &preedit_caret,
            XNPreeditDoneCallback,
            &preedit_done,
            nullptr
        );
        XIC created = XCreateIC(
            xim,
            XNInputStyle,
            full_style,
            XNClientWindow,
            window,
            XNPreeditAttributes,
            preedit_attributes,
            nullptr
        );
        XFree(preedit_attributes);
        return created;
      }
      return XCreateIC(xim, XNInputStyle, preedit_style, XNClientWindow, window, nullptr);
    };

    xic = nullptr;
    for (const XIMStyle candidate : kPreeditPriority) {
      if (!advertised_preedit[candidate & kXimPreeditMask]) {
        continue;
      }
      const bool callbacks = candidate == XIMPreeditCallbacks;
      xic = create_context(candidate, callbacks);
      if (xic != nullptr) {
        break;
      }
    }
    if (xic == nullptr) {
      return false;
    }
    if (focused) {
      XSetICFocus(xic);
    }
    return true;
  }

  void CloseInputContext() noexcept {
    if (xic != nullptr) {
      XDestroyIC(xic);
      xic = nullptr;
    }
    pending_spot_valid = false;
  }

  void CloseInputMethod() noexcept {
    if (xim != nullptr) {
      XCloseIM(xim);
      xim = nullptr;
    }
  }

  // ---- Preedit callbacks ----

  int OnPreeditStart() {
    if (runtime == nullptr || session_id == 0) {
      return 0; // Reject preedit without an active text session.
    }
    composing = true;
    composition_text.clear();
    composition_caret_code_points = 0;
    return 1;
  }

  void OnPreeditDraw(const XIMPreeditDrawCallbackStruct* call_data) {
    if (runtime == nullptr || session_id == 0 || call_data == nullptr) {
      return;
    }
    const int chg_first = std::max(call_data->chg_first, 0);
    const int chg_length = std::max(call_data->chg_length, 0);
    const std::string replacement = XimTextToUtf8(call_data->text);
    const std::optional<std::string> updated =
        ApplyXimPreeditEdit(composition_text, chg_first, chg_length, replacement);
    if (!updated.has_value()) {
      return; // Out-of-range edit; keep the previous composition.
    }
    composition_text = *updated;
    composition_caret_code_points = std::max(call_data->caret, 0);
    SendCompositionUpdate(composition_text, CaretToUtf16(composition_text, composition_caret_code_points));
  }

  void OnPreeditCaret(const XIMPreeditCaretCallbackStruct* call_data) {
    if (runtime == nullptr || session_id == 0 || !composing || call_data == nullptr) {
      return;
    }
    if (call_data->direction != XIMAbsolutePosition) {
      return; // Relative caret moves cannot be resolved without more context.
    }
    composition_caret_code_points = std::max(call_data->position, 0);
    SendCompositionUpdate(composition_text, CaretToUtf16(composition_text, composition_caret_code_points));
  }

  void OnPreeditDone() {
    const bool had_pending_text = !composition_text.empty();
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
    if (runtime == nullptr || session_id == 0) {
      return;
    }
    TextOffset composition_start = 0;
    bool runtime_composing = false;
    QueryCompositionState(composition_start, runtime_composing);
    if (!runtime_composing) {
      return;
    }
    if (had_pending_text) {
      // The input method ended the preedit without clearing it through a draw
      // callback. Remove the preedit text; committed characters arrive through
      // Xutf8LookupString during the same key event and reinsert the final text
      // at the restored selection.
      TextInputCommand cancel;
      cancel.kind = TextInputCommandKind::CancelComposition;
      ApplyCommands({std::move(cancel)});
    } else {
      TextInputCommand finish;
      finish.kind = TextInputCommandKind::FinishComposition;
      ApplyCommands({std::move(finish)});
    }
  }

  // ---- Command delivery ----

  TextInputApplyResult ApplyCommands(std::vector<TextInputCommand> commands) {
    if (runtime == nullptr || session_id == 0 || commands.empty()) {
      return {};
    }
    TextInputCommandBatch batch;
    batch.session_id = session_id;
    batch.commands = std::move(commands);
    return runtime->HandleTextInputCommands(batch);
  }

  void QueryCompositionState(TextOffset& composition_start, bool& runtime_composing) const {
    composition_start = 0;
    runtime_composing = false;
    if (runtime == nullptr || session_id == 0) {
      return;
    }
    const TextInputContext context = runtime->QueryTextInputContext(session_id, 0, 0);
    if (context.result_code != TextInputResultCode::Ok) {
      return;
    }
    runtime_composing = context.composition.has_value();
    composition_start = context.composition.value_or(context.selection.Range()).start;
  }

  bool SendCompositionUpdate(std::string text, TextOffset caret_utf16) {
    if (runtime == nullptr || session_id == 0) {
      return false;
    }
    TextOffset composition_start = 0;
    bool runtime_composing = false;
    QueryCompositionState(composition_start, runtime_composing);
    const std::optional<TextOffset> text_utf16_length = Utf16Length(text);
    if (!text_utf16_length.has_value()) {
      return false;
    }
    caret_utf16 = std::clamp<TextOffset>(caret_utf16, 0, *text_utf16_length);

    TextInputCommand update;
    update.kind = TextInputCommandKind::UpdateComposition;
    update.text = std::move(text);
    update.selection_after = TextSelection{
        composition_start + caret_utf16,
        composition_start + caret_utf16,
    };
    const TextInputApplyResult result = ApplyCommands({std::move(update)});
    composing = result.result_code == TextInputResultCode::Ok;
    return composing;
  }

  bool CommitText(std::string_view text) {
    if (runtime == nullptr || session_id == 0 || text.empty() || !Utf16Length(text).has_value()) {
      return false;
    }
    TextInputCommand commit;
    commit.kind = TextInputCommandKind::CommitText;
    commit.text.assign(text);
    const TextInputApplyResult result = ApplyCommands({std::move(commit)});
    return result.result_code == TextInputResultCode::Ok;
  }

  bool CommitCommittedText(const XKeyEvent& event) {
    if (xic == nullptr) {
      return false;
    }
    char buffer[kXimCommitBufferBytes];
    KeySym keysym = NoSymbol;
    // Status is an Xlib macro that linux_internal.h undefines; use int.
    int status = XLookupNone;
    const int length =
        Xutf8LookupString(xic, const_cast<XKeyPressedEvent*>(&event), buffer, sizeof(buffer), &keysym, &status);
    if ((status != XLookupChars && status != XLookupBoth) || length <= 0) {
      return false;
    }
    return CommitText(std::string_view(buffer, static_cast<std::size_t>(length)));
  }

  // ---- Geometry ----

  void UpdateSpot(const TextInputGeometry& geometry) {
    if (geometry.result_code != TextInputResultCode::Ok || geometry.session_id != session_id || xic == nullptr) {
      return;
    }
    if (!std::isfinite(geometry.caret.x) || !std::isfinite(geometry.caret.y)) {
      return;
    }
    const double scale = std::isfinite(dpi_scale) && dpi_scale > 0.0F ? static_cast<double>(dpi_scale) : 1.0;
    const long x = std::lround(static_cast<double>(geometry.caret.x) * scale);
    const long y = std::lround(static_cast<double>(geometry.caret.y) * scale);
    pending_spot = XPoint{ClampToShort(x), ClampToShort(y)};
    pending_spot_valid = true;
    ApplyPendingSpot();
  }

  void ApplyPendingSpot() noexcept {
    if (!pending_spot_valid || xic == nullptr || in_callback) {
      return;
    }
    XSetICValues(xic, XNSpotLocation, &pending_spot, nullptr);
    pending_spot_valid = false;
  }

  void ResetNativeComposition() {
    if (xic == nullptr || !composing) {
      return;
    }
    // Forcing the preedit state off and back on makes the input method abandon
    // the in-flight preedit without destroying the input context.
    XSetICValues(xic, XNPreeditState, XIMPreeditDisable, nullptr);
    XSetICValues(xic, XNPreeditState, XIMPreeditEnable, nullptr);
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
  }

  std::string XimTextToUtf8(const XIMText* text) const {
    if (text == nullptr) {
      return {};
    }
    if (text->encoding_is_wchar != 0 && text->string.wide_char != nullptr) {
      return WideTextToUtf8(text->string.wide_char);
    }
    if (text->string.multi_byte != nullptr) {
      return std::string(text->string.multi_byte);
    }
    return {};
  }

  TextOffset CaretToUtf16(std::string_view text, int caret_code_points) const {
    const std::optional<int> count = Utf8CodePointCount(text);
    if (!count.has_value()) {
      return 0;
    }
    return Utf8PrefixUtf16Length(text, std::clamp(caret_code_points, 0, *count)).value_or(0);
  }

  Runtime* runtime = nullptr;
  Display* display = nullptr;
  Window window = 0;
  float dpi_scale = 1.0F;
  bool focused = false;
  bool composing = false;
  bool in_callback = false;

  XIM xim = nullptr;
  XIC xic = nullptr;

  TextInputSessionId session_id = 0;
  TextInputConfiguration configuration;
  TextInputState text_input_state;

  std::string composition_text;
  int composition_caret_code_points = 0;

  XPoint pending_spot{0, 0};
  bool pending_spot_valid = false;
};

int LinuxTextInput::State::XimStartCallback(XIM, XPointer client_data, XPointer) {
  State* state = reinterpret_cast<State*>(client_data);
  if (state == nullptr) {
    return 0;
  }
  try {
    return state->OnPreeditStart();
  } catch (...) {
    return 0;
  }
}

void LinuxTextInput::State::XimDrawCallback(XIM, XPointer client_data, XPointer call_data) {
  State* state = reinterpret_cast<State*>(client_data);
  if (state == nullptr) {
    return;
  }
  state->in_callback = true;
  try {
    state->OnPreeditDraw(reinterpret_cast<XIMPreeditDrawCallbackStruct*>(call_data));
  } catch (...) {
  }
  state->in_callback = false;
  state->ApplyPendingSpot();
}

void LinuxTextInput::State::XimCaretCallback(XIM, XPointer client_data, XPointer call_data) {
  State* state = reinterpret_cast<State*>(client_data);
  if (state == nullptr) {
    return;
  }
  state->in_callback = true;
  try {
    state->OnPreeditCaret(reinterpret_cast<XIMPreeditCaretCallbackStruct*>(call_data));
  } catch (...) {
  }
  state->in_callback = false;
  state->ApplyPendingSpot();
}

void LinuxTextInput::State::XimDoneCallback(XIM, XPointer client_data, XPointer) {
  State* state = reinterpret_cast<State*>(client_data);
  if (state == nullptr) {
    return;
  }
  state->in_callback = true;
  try {
    state->OnPreeditDone();
  } catch (...) {
  }
  state->in_callback = false;
  state->ApplyPendingSpot();
}

LinuxTextInput::LinuxTextInput() : state_(std::make_unique<State>()) {}

LinuxTextInput::~LinuxTextInput() {
  // The display is host-owned and is expected to outlive this adapter; the
  // input context and input method must be released before the state object
  // that the XIM callbacks reference.
  state_->CloseInputContext();
  state_->CloseInputMethod();
}

void LinuxTextInput::SetRuntime(Runtime* runtime) noexcept {
  state_->SetRuntime(runtime);
}

void LinuxTextInput::SetDisplayAndWindow(Display* display, Window window) {
  state_->SetDisplayAndWindow(display, window);
}

void LinuxTextInput::SetDpiScale(float scale) noexcept {
  state_->SetDpiScale(scale);
}

void LinuxTextInput::Reset() noexcept {
  state_->Reset();
}

bool LinuxTextInput::Active() const noexcept {
  return state_->Active();
}

bool LinuxTextInput::Composing() const noexcept {
  return state_->Composing();
}

XIC LinuxTextInput::InputContext() const noexcept {
  return state_->InputContext();
}

void LinuxTextInput::SetFocus(bool focused) {
  state_->SetFocus(focused);
}

bool LinuxTextInput::HandleXKeyEvent(const XKeyEvent& event) {
  return state_->HandleXKeyEvent(event);
}

void LinuxTextInput::Start(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Start(session_id, configuration, state, geometry);
}

void LinuxTextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  state_->Update(session_id, state, geometry);
}

void LinuxTextInput::Restart(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Restart(session_id, configuration, state, geometry);
}

void LinuxTextInput::Stop(TextInputSessionId session_id) {
  state_->Stop(session_id);
}

} // namespace huxerui::detail
