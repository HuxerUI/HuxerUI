#include "linux_text_input.h"

#include <X11/keysym.h>

#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
#include <fcitx-gclient/fcitxgclient.h>
#include <glib.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iconv.h>
#include <langinfo.h>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <strings.h>
#include <utility>
#include <vector>

#include <huxerui/app.h>

#include "linux_text_input_internal.h"
#include "text_input_internal.h"

namespace huxerui::detail {

namespace {

constexpr XIMStyle kXimPreeditMask = 0x000F;

#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
constexpr std::uint64_t kFcitxCapabilityPreedit = 1ULL << 1;
constexpr std::uint64_t kFcitxCapabilityFormattedPreedit = 1ULL << 4;
constexpr std::uint64_t kFcitxCapabilitySurroundingText = 1ULL << 6;
constexpr std::uint64_t kFcitxCapabilityEmail = 1ULL << 7;
constexpr std::uint64_t kFcitxCapabilityDigit = 1ULL << 8;
constexpr std::uint64_t kFcitxCapabilityUrl = 1ULL << 12;
constexpr std::uint64_t kFcitxCapabilityDialable = 1ULL << 13;
constexpr std::uint64_t kFcitxCapabilityNumber = 1ULL << 14;
constexpr std::uint64_t kFcitxCapabilityMultiline = 1ULL << 35;
constexpr std::uint64_t kFcitxCapabilitySensitive = 1ULL << 36;
constexpr std::uint64_t kFcitxCapabilityKeyEventOrderFix = 1ULL << 37;
constexpr int kFcitxKeyTimeoutMs = 250;
constexpr std::size_t kFcitxMaxPendingKeys = 64;
constexpr TextOffset kFcitxSurroundingLimit = 4096;
#endif

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
  if ((length == 2 && value < 0x80U) || (length == 3 && value < 0x800U) || (length == 4 && value < 0x10000U) ||
      (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) {
    return std::nullopt;
  }
  byte_offset += length;
  return value;
}

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

bool ContainsFcitx(std::string_view value) noexcept {
  constexpr std::string_view needle = "fcitx";
  if (value.size() < needle.size()) {
    return false;
  }
  for (std::size_t offset = 0; offset <= value.size() - needle.size(); ++offset) {
    bool matches = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      const unsigned char character = static_cast<unsigned char>(value[offset + index]);
      if (static_cast<char>(std::tolower(character)) != needle[index]) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

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

std::string LocaleBytesToUtf8(const char* input) {
  if (input == nullptr) {
    return {};
  }
  const char* codeset = nl_langinfo(CODESET);
  if (codeset == nullptr || codeset[0] == '\0' || strcasecmp(codeset, "UTF-8") == 0 ||
      strcasecmp(codeset, "UTF8") == 0) {
    return input;
  }
  const std::size_t input_length = std::strlen(input);
  std::string output(input_length * 4 + 16, '\0');
  iconv_t converter = iconv_open("UTF-8", codeset);
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return input;
  }
  char* in_ptr = const_cast<char*>(input);
  std::size_t in_left = input_length;
  char* out_ptr = output.data();
  std::size_t out_left = output.size();
  while (in_left > 0) {
    const std::size_t result = iconv(converter, &in_ptr, &in_left, &out_ptr, &out_left);
    if (result == static_cast<std::size_t>(-1)) {
      if (errno == E2BIG) {
        const std::size_t consumed = static_cast<std::size_t>(out_ptr - output.data());
        output.resize(output.size() * 2);
        out_ptr = output.data() + consumed;
        out_left = output.size() - consumed;
        continue;
      }
      break;
    }
    break;
  }
  iconv_close(converter);
  output.resize(static_cast<std::size_t>(out_ptr - output.data()));
  return output;
}

} // namespace

bool ShouldBypassXimLookup(KeySym keysym, unsigned int state) noexcept {
  switch (keysym) {
  case XK_a:
  case XK_A:
  case XK_c:
  case XK_C:
  case XK_v:
  case XK_V:
  case XK_x:
  case XK_X:
  case XK_y:
  case XK_Y:
  case XK_z:
  case XK_Z:
    return (state & (ControlMask | Mod4Mask)) != 0;
  case XK_space:
    return (state & (ControlMask | Mod1Mask | Mod4Mask)) != 0;
  case XK_Shift_L:
  case XK_Shift_R:
  case XK_Control_L:
  case XK_Control_R:
  case XK_Alt_L:
  case XK_Alt_R:
  case XK_Meta_L:
  case XK_Meta_R:
  case XK_Super_L:
  case XK_Super_R:
  case XK_Hyper_L:
  case XK_Hyper_R:
    return true;
  }
  return false;
}

bool ShouldFilterXimEvent(int event_type, bool input_context_available, bool active, bool secure) noexcept {
  if (!input_context_available) {
    return false;
  }
  const bool key_event = event_type == KeyPress || event_type == KeyRelease;
  return !key_event || (active && !secure);
}

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

std::optional<TextOffset> Utf8BytePrefixUtf16Length(std::string_view text, int byte_count) noexcept {
  if (byte_count < 0 || static_cast<std::size_t>(byte_count) > text.size()) {
    return std::nullopt;
  }
  TextOffset utf16_length = 0;
  std::size_t byte_offset = 0;
  const std::size_t requested_bytes = static_cast<std::size_t>(byte_count);
  while (byte_offset < requested_bytes) {
    const std::optional<std::uint32_t> code_point = DecodeUtf8CodePoint(text, byte_offset);
    if (!code_point.has_value() || byte_offset > requested_bytes) {
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

std::optional<std::size_t> Utf16OffsetToUtf8Byte(std::string_view text, TextOffset offset) noexcept {
  if (offset < 0) {
    return std::nullopt;
  }
  TextOffset utf16_offset = 0;
  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    if (utf16_offset == offset) {
      return byte_offset;
    }
    const std::optional<std::uint32_t> code_point = DecodeUtf8CodePoint(text, byte_offset);
    if (!code_point.has_value()) {
      return std::nullopt;
    }
    const TextOffset width = *code_point > 0xFFFFU ? 2 : 1;
    if (utf16_offset > std::numeric_limits<TextOffset>::max() - width) {
      return std::nullopt;
    }
    utf16_offset += width;
    if (utf16_offset > offset) {
      return std::nullopt;
    }
  }
  return utf16_offset == offset ? std::optional<std::size_t>(byte_offset) : std::nullopt;
}

bool ShouldUseFcitxFrontend(const char* xmodifiers, const char* gtk_im_module, const char* qt_im_module) noexcept {
  return (xmodifiers != nullptr && ContainsFcitx(xmodifiers)) ||
         (gtk_im_module != nullptr && ContainsFcitx(gtk_im_module)) ||
         (qt_im_module != nullptr && ContainsFcitx(qt_im_module));
}

bool ShouldFocusXim(bool focused, bool fcitx_available, bool active, bool secure) noexcept {
  return focused && active && !secure && !fcitx_available;
}

std::optional<std::string>
ApplyXimPreeditEdit(std::string_view current, int chg_first, int chg_length, std::string_view replacement) {
  if (chg_first < 0 || chg_length < 0) {
    return std::nullopt;
  }
  if (chg_first > std::numeric_limits<int>::max() - chg_length) {
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
  result.append(current, 0, *range_start);
  result.append(replacement);
  result.append(current, *range_end, std::string_view::npos);
  return result;
}

struct LinuxTextInput::State {
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
  struct AsyncLifetime {
    State* state = nullptr;
  };

  struct AsyncKeyCallbackData {
    std::weak_ptr<AsyncLifetime> lifetime;
    std::uint64_t generation = 0;
    XEvent event{};
    GCancellable* cancellable = nullptr;
  };
#endif

  State() {
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    fcitx_async_lifetime = std::make_shared<AsyncLifetime>();
    fcitx_async_lifetime->state = this;
#endif
  }

  ~State() {
    Reset();
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    fcitx_async_lifetime->state = nullptr;
#endif
  }

  static int XimStartCallback(XIC ic, XPointer client_data, XPointer call_data);
  static void XimDrawCallback(XIC ic, XPointer client_data, XPointer call_data);
  static void XimCaretCallback(XIC ic, XPointer client_data, XPointer call_data);
  static void XimDoneCallback(XIC ic, XPointer client_data, XPointer call_data);
  static void XimInstantiateCallback(Display* display, XPointer client_data, XPointer call_data);
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
  static void FcitxConnectedCallback(FcitxGClient* client, gpointer user_data);
  static void FcitxForwardCallback(FcitxGClient* client, guint keyval, gint state, gint is_release, gpointer user_data);
  static void FcitxCommitCallback(FcitxGClient* client, gchar* text, gpointer user_data);
  static void FcitxPreeditCallback(FcitxGClient* client, GPtrArray* items, gint cursor, gpointer user_data);
  static void FcitxDeleteSurroundingCallback(FcitxGClient* client, gint offset, guint count, gpointer user_data);
  static void FcitxNotifyFocusOutCallback(FcitxGClient* client, gpointer user_data);
  static void FcitxKeyCallback(GObject* source, GAsyncResult* result, gpointer user_data);
#endif

  void SetRuntime(Runtime* value) noexcept {
    runtime = value;
  }

  void SetDisplayAndWindow(Display* value_display, Window value_window) {
    if (value_display == display && value_window == window) {
      return;
    }
    CloseInputContext();
    CloseInputMethod();
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    CloseFcitx();
#endif
    display = value_display;
    window = value_window;
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
    XRegisterIMInstantiateCallback(
        display,
        nullptr,
        nullptr,
        nullptr,
        reinterpret_cast<XIDProc>(XimInstantiateCallback),
        reinterpret_cast<XPointer>(this)
    );
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    InitializeFcitx();
#endif
  }

  void SetDpiScale(float scale) noexcept {
    dpi_scale = std::isfinite(scale) && scale > 0.0F ? scale : 1.0F;
  }

  void Reset() noexcept {
    CloseInputContext();
    CloseInputMethod();
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    CloseFcitx();
#endif
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
    if (!Active() || secure) {
      return nullptr;
    }
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    if (FcitxAvailable()) {
      return nullptr;
    }
#endif
    return xic;
  }

  [[nodiscard]] bool FilterEvent(XEvent& event) noexcept {
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    if ((event.type == KeyPress || event.type == KeyRelease) && FcitxAvailable() && Active() && !secure) {
      return false;
    }
#endif
    if (!ShouldFilterXimEvent(event.type, xic != nullptr, Active(), secure)) {
      return false;
    }
    return XFilterEvent(&event, 0) != 0;
  }

  void SetFocus(bool value) {
    focused = value;
    UpdateInputContextFocus();
    if (!focused) {
      composing = false;
      composition_text.clear();
      composition_caret_code_points = 0;
    }
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    UpdateFcitxFocus();
#endif
  }

  [[nodiscard]] XimKeyEventResult HandleXKeyEvent(XEvent& xevent) {
    XKeyEvent& event = xevent.xkey;
    if (display == nullptr || window == 0) {
      return XimKeyEventResult::Unhandled;
    }
    const bool is_release = event.type != KeyPress;
    const KeySym keysym = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    try {
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
      UpdateInputContextFocus();
      UpdateFcitxFocus();
      if (!secure && Active() && FcitxAvailable()) {
        return ProcessFcitxKey(xevent) ? XimKeyEventResult::Consumed : XimKeyEventResult::Unhandled;
      }
#endif
      if (secure || !Active()) {
        return XimKeyEventResult::Unhandled;
      }
      if (xic == nullptr) {
        return XimKeyEventResult::Unhandled;
      }
      if (is_release) {
        return XimKeyEventResult::Unhandled;
      }
      if (ShouldBypassXimLookup(keysym, event.state)) {
        return XimKeyEventResult::DispatchWithoutText;
      }
      return CommitCommittedText(event) ? XimKeyEventResult::Consumed : XimKeyEventResult::DispatchWithoutText;
    } catch (...) {
      return XimKeyEventResult::Unhandled;
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
    secure = config.secure;
    text_input_state = state;
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
    EnsureInputMethod();
    UpdateSpot(geometry);
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    ConfigureFcitx();
    UpdateInputContextFocus();
    UpdateFcitxFocus();
    UpdateFcitxSurroundingText();
#endif
  }

  void Update(TextInputSessionId requested_session, const TextInputState& state, const TextInputGeometry& geometry) {
    if (requested_session != session_id) {
      return;
    }
    text_input_state = state;
    UpdateSpot(geometry);
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    ConfigureFcitx();
    UpdateFcitxSurroundingText();
#endif
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
    ResetSystemComposition();
    configuration = config;
    secure = config.secure;
    text_input_state = state;
    UpdateSpot(geometry);
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    ConfigureFcitx();
    UpdateInputContextFocus();
    UpdateFcitxFocus();
    UpdateFcitxSurroundingText();
#endif
  }

  void Stop(TextInputSessionId requested_session) {
    if (requested_session != session_id) {
      return;
    }
    ResetSystemComposition();
    session_id = 0;
    configuration = {};
    secure = false;
    text_input_state = {};
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
    UpdateInputContextFocus();
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    UpdateFcitxFocus();
#endif
  }

#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
  [[nodiscard]] bool FcitxAvailable() const noexcept {
    return fcitx_client != nullptr && fcitx_g_client_is_valid(fcitx_client) != FALSE;
  }

  [[nodiscard]] bool ProcessFcitxKey(const XEvent& event) noexcept {
    if (fcitx_pending_key_count >= kFcitxMaxPendingKeys) {
      return false;
    }
    GCancellable* cancellable = g_cancellable_new();
    if (cancellable == nullptr) {
      return false;
    }
    auto* callback_data =
        new (std::nothrow) AsyncKeyCallbackData{fcitx_async_lifetime, fcitx_key_generation, event, cancellable};
    if (callback_data == nullptr) {
      g_object_unref(cancellable);
      return false;
    }
    const XKeyEvent& key_event = event.xkey;
    const KeySym keysym = XLookupKeysym(const_cast<XKeyEvent*>(&key_event), 0);
    ++fcitx_pending_key_count;
    fcitx_g_client_process_key(
        fcitx_client,
        static_cast<guint32>(keysym),
        static_cast<guint32>(key_event.keycode),
        static_cast<guint32>(key_event.state),
        key_event.type == KeyRelease ? TRUE : FALSE,
        static_cast<guint32>(key_event.time),
        kFcitxKeyTimeoutMs,
        cancellable,
        FcitxKeyCallback,
        callback_data
    );
    return true;
  }

  void CompleteFcitxKey(const XEvent& event, bool handled, std::uint64_t generation) {
    if (fcitx_pending_key_count > 0) {
      --fcitx_pending_key_count;
    }
    if (generation != fcitx_key_generation || handled) {
      return;
    }
    deferred_key_events.push_back({event, XimKeyEventResult::Unhandled});
  }

  void OnFcitxForward(guint keyval, guint state, bool release) {
    if (display == nullptr || window == 0) {
      return;
    }
    const KeyCode keycode = XKeysymToKeycode(display, keyval);
    if (keycode == 0) {
      return;
    }
    XEvent event{};
    event.type = release ? KeyRelease : KeyPress;
    event.xkey.type = event.type;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = DefaultRootWindow(display);
    event.xkey.subwindow = 0;
    event.xkey.time = CurrentTime;
    event.xkey.x = 0;
    event.xkey.y = 0;
    event.xkey.x_root = 0;
    event.xkey.y_root = 0;
    event.xkey.state = state;
    event.xkey.keycode = keycode;
    event.xkey.same_screen = 1;
    deferred_key_events.push_back({event, XimKeyEventResult::Unhandled});
  }

  void InitializeFcitx() {
    if (display == nullptr || window == 0 ||
        !ShouldUseFcitxFrontend(std::getenv("XMODIFIERS"), std::getenv("GTK_IM_MODULE"), std::getenv("QT_IM_MODULE"))) {
      return;
    }
    fcitx_context = g_main_context_ref(g_main_context_default());
    fcitx_client = fcitx_g_client_new();
    if (fcitx_client == nullptr) {
      g_main_context_unref(fcitx_context);
      fcitx_context = nullptr;
      return;
    }
    fcitx_g_client_set_use_batch_process_key_event(fcitx_client, FALSE);
    fcitx_g_client_set_display(fcitx_client, "x11:");
    if (const char* program = g_get_prgname(); program != nullptr) {
      fcitx_g_client_set_program(fcitx_client, program);
    }
    g_signal_connect(fcitx_client, "connected", G_CALLBACK(FcitxConnectedCallback), this);
    g_signal_connect(fcitx_client, "forward-key", G_CALLBACK(FcitxForwardCallback), this);
    g_signal_connect(fcitx_client, "commit-string", G_CALLBACK(FcitxCommitCallback), this);
    g_signal_connect(fcitx_client, "update-formatted-preedit", G_CALLBACK(FcitxPreeditCallback), this);
    g_signal_connect(fcitx_client, "delete-surrounding-text", G_CALLBACK(FcitxDeleteSurroundingCallback), this);
    g_signal_connect(fcitx_client, "notify-focus-out", G_CALLBACK(FcitxNotifyFocusOutCallback), this);
  }

  void CloseFcitx() noexcept {
    ++fcitx_key_generation;
    deferred_key_events.clear();
    if (fcitx_poll_prepared && fcitx_context != nullptr) {
      g_main_context_release(fcitx_context);
    }
    fcitx_poll_prepared = false;
    fcitx_poll_fds.clear();
    if (fcitx_client != nullptr) {
      if (fcitx_focused && FcitxAvailable()) {
        fcitx_g_client_focus_out(fcitx_client);
      }
      g_signal_handlers_disconnect_by_data(fcitx_client, this);
      g_object_unref(fcitx_client);
      fcitx_client = nullptr;
    }
    if (fcitx_context != nullptr) {
      g_main_context_unref(fcitx_context);
      fcitx_context = nullptr;
    }
    fcitx_focused = false;
  }

  void OnFcitxConnected() {
    if (!FcitxAvailable()) {
      return;
    }
    fcitx_capabilities = std::numeric_limits<std::uint64_t>::max();
    fcitx_focused = false;
    ConfigureFcitx();
    SendFcitxUuidToX11();
    UpdateInputContextFocus();
    UpdateFcitxFocus();
    UpdateFcitxCursor();
    UpdateFcitxSurroundingText();
  }

  void ConfigureFcitx() {
    if (!FcitxAvailable()) {
      return;
    }
    std::uint64_t capabilities = kFcitxCapabilityKeyEventOrderFix;
    if (!secure && Active()) {
      capabilities |= kFcitxCapabilityPreedit | kFcitxCapabilityFormattedPreedit | kFcitxCapabilitySurroundingText;
      if (configuration.multiline) {
        capabilities |= kFcitxCapabilityMultiline;
      }
      switch (configuration.type) {
      case TextInputType::Email:
        capabilities |= kFcitxCapabilityEmail;
        break;
      case TextInputType::Number:
      case TextInputType::Decimal:
        capabilities |= kFcitxCapabilityNumber | kFcitxCapabilityDigit;
        break;
      case TextInputType::Phone:
        capabilities |= kFcitxCapabilityDialable;
        break;
      case TextInputType::Url:
        capabilities |= kFcitxCapabilityUrl;
        break;
      case TextInputType::Text:
        break;
      }
    } else {
      capabilities |= kFcitxCapabilitySensitive;
    }
    if (capabilities != fcitx_capabilities) {
      fcitx_capabilities = capabilities;
      fcitx_g_client_set_capability(fcitx_client, capabilities);
    }
  }

  void UpdateFcitxFocus() {
    const bool should_focus = focused && Active() && !secure && FcitxAvailable();
    if (should_focus == fcitx_focused) {
      return;
    }
    if (FcitxAvailable()) {
      if (should_focus) {
        fcitx_g_client_focus_in(fcitx_client);
      } else {
        fcitx_g_client_focus_out(fcitx_client);
      }
    }
    fcitx_focused = should_focus;
  }

  void UpdateFcitxSurroundingText() {
    if (!FcitxAvailable() || runtime == nullptr || !Active() || secure) {
      return;
    }
    const TextOffset requested_cursor = std::max<TextOffset>(text_input_state.selection.active, 0);
    const TextOffset requested_start =
        requested_cursor > kFcitxSurroundingLimit / 2 ? requested_cursor - kFcitxSurroundingLimit / 2 : 0;
    const TextInputContext context =
        runtime->QueryTextInputContext(session_id, requested_start, kFcitxSurroundingLimit);
    const std::optional<TextOffset> context_length = Utf16Length(context.text);
    if (context.result_code != TextInputResultCode::Ok || !context_length.has_value() || context.slice_start < 0 ||
        *context_length > std::numeric_limits<TextOffset>::max() - context.slice_start) {
      return;
    }
    const TextOffset context_end = context.slice_start + *context_length;
    const TextOffset cursor_offset = context.selection.active;
    if (cursor_offset < context.slice_start || cursor_offset > context_end) {
      return;
    }

    TextOffset surrounding_end =
        std::min(context_end, cursor_offset + std::min(kFcitxSurroundingLimit / 2, context_end - cursor_offset));
    TextOffset surrounding_start =
        surrounding_end > kFcitxSurroundingLimit ? surrounding_end - kFcitxSurroundingLimit : 0;
    surrounding_start = std::max(surrounding_start, context.slice_start);
    surrounding_end = surrounding_start + std::min(kFcitxSurroundingLimit, context_end - surrounding_start);
    const TextOffset anchor_offset = std::clamp(context.selection.anchor, surrounding_start, surrounding_end);

    const std::optional<std::size_t> start_byte =
        Utf16OffsetToUtf8Byte(context.text, surrounding_start - context.slice_start);
    const std::optional<std::size_t> end_byte =
        Utf16OffsetToUtf8Byte(context.text, surrounding_end - context.slice_start);
    const std::optional<std::size_t> cursor_byte =
        Utf16OffsetToUtf8Byte(context.text, cursor_offset - context.slice_start);
    const std::optional<std::size_t> anchor_byte =
        Utf16OffsetToUtf8Byte(context.text, anchor_offset - context.slice_start);
    if (!start_byte.has_value() || !end_byte.has_value() || !cursor_byte.has_value() || !anchor_byte.has_value() ||
        *end_byte < *start_byte || *cursor_byte < *start_byte || *anchor_byte < *start_byte) {
      return;
    }
    std::string surrounding = context.text.substr(*start_byte, *end_byte - *start_byte);
    const std::size_t cursor = *cursor_byte - *start_byte;
    const std::size_t anchor = *anchor_byte - *start_byte;
    if (cursor > std::numeric_limits<guint>::max() || anchor > std::numeric_limits<guint>::max()) {
      return;
    }
    fcitx_g_client_set_surrounding_text(
        fcitx_client,
        surrounding.data(),
        static_cast<guint>(cursor),
        static_cast<guint>(anchor)
    );
  }

  void UpdateFcitxCursor() {
    const TextInputGeometry& geometry = latest_geometry;
    if (!FcitxAvailable() || geometry.result_code != TextInputResultCode::Ok || geometry.session_id != session_id ||
        display == nullptr || window == 0) {
      return;
    }
    Window child = 0;
    int root_x = 0;
    int root_y = 0;
    if (XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &root_x, &root_y, &child) == 0) {
      return;
    }
    const double scale = std::isfinite(dpi_scale) && dpi_scale > 0.0F ? static_cast<double>(dpi_scale) : 1.0;
    const int x = root_x + static_cast<int>(std::lround(static_cast<double>(geometry.caret.x) * scale));
    const int y = root_y + static_cast<int>(std::lround(static_cast<double>(geometry.caret.y) * scale));
    const int width = std::max(1, static_cast<int>(std::lround(static_cast<double>(geometry.caret.width) * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(static_cast<double>(geometry.caret.height) * scale)));
    fcitx_g_client_set_cursor_rect(fcitx_client, x, y, width, height);
  }

  void SendFcitxUuidToX11() {
    if (!FcitxAvailable() || display == nullptr) {
      return;
    }
    const Atom server_atom = XInternAtom(display, "_FCITX_SERVER", 0);
    const Window server_window = XGetSelectionOwner(display, server_atom);
    const guint8* uuid = fcitx_g_client_get_uuid(fcitx_client);
    if (server_atom == 0 || server_window == 0 || uuid == nullptr) {
      return;
    }
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = server_window;
    event.xclient.message_type = server_atom;
    event.xclient.format = 8;
    std::memcpy(event.xclient.data.b, uuid, 16);
    XSendEvent(display, server_window, 0, NoEventMask, &event);
    XFlush(display);
  }

  void OnFcitxPreedit(GPtrArray* items, int cursor_bytes) {
    if (!Active() || secure) {
      return;
    }
    std::string next;
    if (items != nullptr) {
      for (guint index = 0; index < items->len; ++index) {
        const auto* item = static_cast<const FcitxGPreeditItem*>(g_ptr_array_index(items, index));
        if (item != nullptr && item->string != nullptr) {
          next.append(item->string);
        }
      }
    }
    if (next.empty()) {
      const bool had_composition = composing || text_input_state.composition.has_value();
      composing = false;
      composition_text.clear();
      composition_caret_code_points = 0;
      if (had_composition) {
        TextInputCommand cancel;
        cancel.kind = TextInputCommandKind::CancelComposition;
        ApplyCommands({std::move(cancel)});
      }
      return;
    }
    const std::optional<TextOffset> caret = Utf8BytePrefixUtf16Length(next, cursor_bytes);
    if (!caret.has_value()) {
      return;
    }
    if (SendCompositionUpdate(next, *caret)) {
      composition_text = std::move(next);
      composition_caret_code_points = Utf8CodePointCount(composition_text).value_or(0);
    }
  }

  void OnFcitxCommit(std::string_view text) {
    if (!Active() || secure || text.empty()) {
      return;
    }
    if (CommitText(text)) {
      composing = false;
      composition_text.clear();
      composition_caret_code_points = 0;
      UpdateFcitxSurroundingText();
    }
  }

  void OnFcitxDeleteSurrounding(int offset, unsigned int count) {
    if (!Active() || secure || count == 0) {
      return;
    }
    const std::int64_t start = static_cast<std::int64_t>(offset);
    const std::int64_t end = start + static_cast<std::int64_t>(count);
    if (start > 0 || end < 0) {
      return;
    }
    TextInputCommand command;
    command.kind = TextInputCommandKind::DeleteSurrounding;
    command.delete_unit = TextInputUnit::UnicodeCodePoint;
    command.delete_before =
        static_cast<TextOffset>(std::clamp<std::int64_t>(-start, 0, std::numeric_limits<TextOffset>::max()));
    command.delete_after =
        static_cast<TextOffset>(std::clamp<std::int64_t>(end, 0, std::numeric_limits<TextOffset>::max()));
    const TextInputApplyResult result = ApplyCommands({std::move(command)});
    if (result.result_code == TextInputResultCode::Ok) {
      UpdateFcitxSurroundingText();
    }
  }

  void OnFcitxNotifyFocusOut() {
    fcitx_focused = false;
    const bool had_composition = composing || text_input_state.composition.has_value();
    composing = false;
    composition_text.clear();
    composition_caret_code_points = 0;
    if (had_composition) {
      TextInputCommand cancel;
      cancel.kind = TextInputCommandKind::CancelComposition;
      ApplyCommands({std::move(cancel)});
    }
  }

  int PreparePoll(std::vector<pollfd>& descriptors, int timeout_ms) {
    if (fcitx_context == nullptr || fcitx_client == nullptr || fcitx_poll_prepared) {
      return timeout_ms;
    }
    for (int iteration = 0; iteration < 16 && g_main_context_iteration(fcitx_context, FALSE) != FALSE; ++iteration) {
    }
    if (!g_main_context_acquire(fcitx_context)) {
      return timeout_ms;
    }
    try {
      fcitx_poll_prepared = true;
      fcitx_poll_offset = descriptors.size();
      fcitx_poll_ready = g_main_context_prepare(fcitx_context, &fcitx_poll_priority) != FALSE;
      gint glib_timeout = -1;
      gint count = g_main_context_query(fcitx_context, fcitx_poll_priority, &glib_timeout, nullptr, 0);
      if (count < 0) {
        count = 0;
      }
      while (count > 0) {
        fcitx_poll_fds.resize(static_cast<std::size_t>(count));
        const gint actual =
            g_main_context_query(fcitx_context, fcitx_poll_priority, &glib_timeout, fcitx_poll_fds.data(), count);
        if (actual <= count) {
          count = std::max(actual, 0);
          break;
        }
        count = actual;
      }
      fcitx_poll_fds.resize(static_cast<std::size_t>(count));
      for (const GPollFD& descriptor : fcitx_poll_fds) {
        descriptors.push_back({descriptor.fd, static_cast<short>(descriptor.events), 0});
      }
      if (fcitx_poll_ready) {
        return 0;
      }
      if (glib_timeout < 0) {
        return timeout_ms;
      }
      return timeout_ms < 0 ? glib_timeout : std::min(timeout_ms, glib_timeout);
    } catch (...) {
      g_main_context_release(fcitx_context);
      fcitx_poll_prepared = false;
      fcitx_poll_fds.clear();
      throw;
    }
  }

  void DispatchPoll(const std::vector<pollfd>& descriptors, bool poll_succeeded) noexcept {
    if (!fcitx_poll_prepared || fcitx_context == nullptr) {
      return;
    }
    if (poll_succeeded && descriptors.size() >= fcitx_poll_offset + fcitx_poll_fds.size()) {
      for (std::size_t index = 0; index < fcitx_poll_fds.size(); ++index) {
        fcitx_poll_fds[index].revents = static_cast<gushort>(descriptors[fcitx_poll_offset + index].revents);
      }
      if (fcitx_poll_ready || g_main_context_check(
                                  fcitx_context,
                                  fcitx_poll_priority,
                                  fcitx_poll_fds.empty() ? nullptr : fcitx_poll_fds.data(),
                                  static_cast<gint>(fcitx_poll_fds.size())
                              )) {
        g_main_context_dispatch(fcitx_context);
      }
    }
    g_main_context_release(fcitx_context);
    fcitx_poll_prepared = false;
    fcitx_poll_fds.clear();
  }
#else
  int PreparePoll(std::vector<pollfd>&, int timeout_ms) {
    return timeout_ms;
  }

  void DispatchPoll(const std::vector<pollfd>&, bool) noexcept {}
#endif

  void TakeDeferredKeyEvents(std::vector<LinuxDeferredKeyEvent>& events) {
    events.clear();
    events.swap(deferred_key_events);
  }

  bool EnsureInputMethod() {
    if (xic != nullptr) {
      return true;
    }
    if (display == nullptr || window == 0) {
      return false;
    }
    if (xim == nullptr) {
      XSetLocaleModifiers("");
      xim = XOpenIM(display, nullptr, nullptr, nullptr);
      if (xim == nullptr) {
        return false;
      }
    }

    XIMStyles* styles = nullptr;
    if (XGetIMValues(xim, XNQueryInputStyle, &styles, nullptr) != nullptr || styles == nullptr) {
      return false;
    }

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
            XNFocusWindow,
            window,
            XNPreeditAttributes,
            preedit_attributes,
            nullptr
        );
        XFree(preedit_attributes);
        return created;
      }
      return XCreateIC(xim, XNInputStyle, preedit_style, XNClientWindow, window, XNFocusWindow, window, nullptr);
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
    UpdateInputContextFocus();
    return true;
  }

  void OnInputMethodInstantiated() {
    if (display == nullptr || window == 0) {
      return;
    }
    CloseInputContext();
    CloseInputMethod();
    static_cast<void>(EnsureInputMethod());
  }

  void UpdateInputContextFocus() noexcept {
    if (xic == nullptr) {
      xic_focused = false;
      return;
    }
    bool should_focus = focused && Active() && !secure;
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    should_focus = ShouldFocusXim(focused, FcitxAvailable(), Active(), secure);
#endif
    if (should_focus == xic_focused) {
      return;
    }
    if (should_focus) {
      XSetICFocus(xic);
    } else {
      XUnsetICFocus(xic);
    }
    xic_focused = should_focus;
  }

  void CloseInputContext() noexcept {
    if (xic != nullptr) {
      XDestroyIC(xic);
      xic = nullptr;
    }
    xic_focused = false;
    pending_spot_valid = false;
  }

  void CloseInputMethod() noexcept {
    if (display != nullptr) {
      XUnregisterIMInstantiateCallback(
          display,
          nullptr,
          nullptr,
          nullptr,
          reinterpret_cast<XIDProc>(XimInstantiateCallback),
          reinterpret_cast<XPointer>(this)
      );
    }
    if (xim != nullptr) {
      XCloseIM(xim);
      xim = nullptr;
    }
  }

  int OnPreeditStart() {
    if (runtime == nullptr || session_id == 0 || secure) {
      return 0;
    }
    composing = true;
    composition_text.clear();
    composition_caret_code_points = 0;
    // XIM interprets a positive return as a byte limit, while -1 allows the
    // input method to maintain an unrestricted preedit string.
    return -1;
  }

  void OnPreeditDraw(const XIMPreeditDrawCallbackStruct* call_data) {
    if (runtime == nullptr || session_id == 0 || call_data == nullptr) {
      return;
    }
    const int chg_first = std::max(call_data->chg_first, 0);
    const int chg_length = std::max(call_data->chg_length, 0);
    const std::string replacement = XimTextToUtf8(call_data->text);
    std::optional<std::string> updated = ApplyXimPreeditEdit(composition_text, chg_first, chg_length, replacement);
    if (!updated.has_value()) {
      return;
    }
    const int caret_code_points = std::max(call_data->caret, 0);
    const TextOffset caret_offset = CaretToUtf16(*updated, caret_code_points);
    std::string next = std::move(*updated);
    if (SendCompositionUpdate(next, caret_offset)) {
      composition_text = std::move(next);
      composition_caret_code_points = caret_code_points;
    }
  }

  void OnPreeditCaret(const XIMPreeditCaretCallbackStruct* call_data) {
    if (runtime == nullptr || session_id == 0 || !composing || call_data == nullptr) {
      return;
    }
    if (call_data->direction != XIMAbsolutePosition) {
      return;
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
      TextInputCommand cancel;
      cancel.kind = TextInputCommandKind::CancelComposition;
      ApplyCommands({std::move(cancel)});
    } else {
      TextInputCommand finish;
      finish.kind = TextInputCommandKind::FinishComposition;
      ApplyCommands({std::move(finish)});
    }
  }

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

  bool SendCompositionUpdate(const std::string& text, TextOffset caret_utf16) {
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
    update.text = text;
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
    const auto commit_lookup_text = [this](const char* data, int length) {
      if (data == nullptr || length <= 0) {
        return false;
      }
      std::string text(data, static_cast<std::size_t>(length));
      std::erase_if(text, [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7F;
      });
      if (text.empty()) {
        return false;
      }
      return CommitText(text);
    };

    char buffer[kXimCommitBufferBytes];
    KeySym keysym = NoSymbol;
    int status = XLookupNone;
    int length =
        Xutf8LookupString(xic, const_cast<XKeyPressedEvent*>(&event), buffer, sizeof(buffer), &keysym, &status);
    if (status == XBufferOverflow) {
      const std::size_t needed = static_cast<std::size_t>(std::abs(length)) + 1;
      if (needed > sizeof(buffer)) {
        std::vector<char> grown(needed);
        length =
            Xutf8LookupString(xic, const_cast<XKeyPressedEvent*>(&event), grown.data(), grown.size(), &keysym, &status);
        if ((status == XLookupChars || status == XLookupBoth) && length > 0) {
          return commit_lookup_text(grown.data(), length);
        }
        return false;
      }
    }
    if ((status != XLookupChars && status != XLookupBoth) || length <= 0) {
      return false;
    }
    return commit_lookup_text(buffer, length);
  }

  void UpdateSpot(const TextInputGeometry& geometry) {
    latest_geometry = geometry;
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    UpdateFcitxCursor();
#endif
    if (geometry.result_code != TextInputResultCode::Ok || geometry.session_id != session_id || xic == nullptr) {
      return;
    }
    if (!std::isfinite(geometry.caret.x) || !std::isfinite(geometry.caret.y) || !std::isfinite(geometry.caret.height)) {
      return;
    }
    const double scale = std::isfinite(dpi_scale) && dpi_scale > 0.0F ? static_cast<double>(dpi_scale) : 1.0;
    const long x = std::lround(static_cast<double>(geometry.caret.x) * scale);
    // TextInputGeometry describes the caret rectangle from its top edge; XIM
    // expects a baseline-like spot, so use the bottom edge available here.
    const long y = std::lround(static_cast<double>(geometry.caret.y + std::max(geometry.caret.height, 0.0F)) * scale);
    pending_spot = XPoint{ClampToShort(x), ClampToShort(y)};
    pending_spot_valid = true;
    ApplyPendingSpot();
  }

  void ApplyPendingSpot() noexcept {
    if (!pending_spot_valid || xic == nullptr || in_callback) {
      return;
    }
    XVaNestedList preedit_attributes = XVaCreateNestedList(0, XNSpotLocation, &pending_spot, nullptr);
    if (preedit_attributes == nullptr) {
      return;
    }
    const char* failed_attribute = XSetICValues(xic, XNPreeditAttributes, preedit_attributes, nullptr);
    XFree(preedit_attributes);
    pending_spot_valid = failed_attribute != nullptr;
  }

  void ResetSystemComposition() {
#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
    if (FcitxAvailable() && Active() && !secure) {
      fcitx_g_client_reset(fcitx_client);
    }
#endif
    if (xic == nullptr || !composing) {
      return;
    }
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
      return LocaleBytesToUtf8(text->string.multi_byte);
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
  bool secure = false;
  bool in_callback = false;

  XIM xim = nullptr;
  XIC xic = nullptr;
  bool xic_focused = false;

  TextInputSessionId session_id = 0;
  TextInputConfiguration configuration;
  TextInputState text_input_state;
  TextInputGeometry latest_geometry;

  std::string composition_text;
  int composition_caret_code_points = 0;

  XPoint pending_spot{0, 0};
  bool pending_spot_valid = false;
  std::vector<LinuxDeferredKeyEvent> deferred_key_events;

#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
  std::shared_ptr<AsyncLifetime> fcitx_async_lifetime;
  FcitxGClient* fcitx_client = nullptr;
  GMainContext* fcitx_context = nullptr;
  std::uint64_t fcitx_capabilities = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t fcitx_key_generation = 1;
  std::size_t fcitx_pending_key_count = 0;
  bool fcitx_focused = false;
  bool fcitx_poll_prepared = false;
  bool fcitx_poll_ready = false;
  gint fcitx_poll_priority = G_PRIORITY_DEFAULT;
  std::size_t fcitx_poll_offset = 0;
  std::vector<GPollFD> fcitx_poll_fds;
#endif
};

int LinuxTextInput::State::XimStartCallback(XIC, XPointer client_data, XPointer) {
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

void LinuxTextInput::State::XimDrawCallback(XIC, XPointer client_data, XPointer call_data) {
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

void LinuxTextInput::State::XimCaretCallback(XIC, XPointer client_data, XPointer call_data) {
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

void LinuxTextInput::State::XimDoneCallback(XIC, XPointer client_data, XPointer) {
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

void LinuxTextInput::State::XimInstantiateCallback(Display*, XPointer client_data, XPointer) {
  State* state = reinterpret_cast<State*>(client_data);
  if (state == nullptr) {
    return;
  }
  try {
    state->OnInputMethodInstantiated();
  } catch (...) {
  }
}

#if defined(HUXERUI_HAS_FCITX5_GCLIENT)
void LinuxTextInput::State::FcitxConnectedCallback(FcitxGClient*, gpointer user_data) {
  State* state = static_cast<State*>(user_data);
  if (state == nullptr) {
    return;
  }
  try {
    state->OnFcitxConnected();
  } catch (...) {
  }
}

void LinuxTextInput::State::FcitxForwardCallback(
    FcitxGClient*, guint keyval, gint modifiers, gint is_release, gpointer user_data
) {
  State* owner = static_cast<State*>(user_data);
  if (owner != nullptr) {
    try {
      owner->OnFcitxForward(keyval, static_cast<guint>(modifiers), is_release != 0);
    } catch (...) {
    }
  }
}

void LinuxTextInput::State::FcitxCommitCallback(FcitxGClient*, gchar* text, gpointer user_data) {
  State* state = static_cast<State*>(user_data);
  if (state == nullptr || text == nullptr) {
    return;
  }
  try {
    state->OnFcitxCommit(text);
  } catch (...) {
  }
}

void LinuxTextInput::State::FcitxPreeditCallback(FcitxGClient*, GPtrArray* items, gint cursor, gpointer user_data) {
  State* state = static_cast<State*>(user_data);
  if (state == nullptr) {
    return;
  }
  try {
    state->OnFcitxPreedit(items, cursor);
  } catch (...) {
  }
}

void LinuxTextInput::State::FcitxDeleteSurroundingCallback(
    FcitxGClient*, gint offset, guint count, gpointer user_data
) {
  State* state = static_cast<State*>(user_data);
  if (state == nullptr) {
    return;
  }
  try {
    state->OnFcitxDeleteSurrounding(offset, count);
  } catch (...) {
  }
}

void LinuxTextInput::State::FcitxNotifyFocusOutCallback(FcitxGClient*, gpointer user_data) {
  State* state = static_cast<State*>(user_data);
  if (state != nullptr) {
    try {
      state->OnFcitxNotifyFocusOut();
    } catch (...) {
    }
  }
}

void LinuxTextInput::State::FcitxKeyCallback(GObject* source, GAsyncResult* result, gpointer user_data) {
  std::unique_ptr<AsyncKeyCallbackData> callback_data(static_cast<AsyncKeyCallbackData*>(user_data));
  const bool handled = fcitx_g_client_process_key_finish(FCITX_G_CLIENT(source), result) != FALSE;
  g_object_unref(callback_data->cancellable);
  callback_data->cancellable = nullptr;
  const std::shared_ptr<AsyncLifetime> lifetime = callback_data->lifetime.lock();
  if (lifetime == nullptr || lifetime->state == nullptr) {
    return;
  }
  try {
    lifetime->state->CompleteFcitxKey(callback_data->event, handled, callback_data->generation);
  } catch (...) {
  }
}
#endif

LinuxTextInput::LinuxTextInput() : state_(std::make_unique<State>()) {}

LinuxTextInput::~LinuxTextInput() = default;

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

bool LinuxTextInput::FilterEvent(XEvent& event) noexcept {
  return state_->FilterEvent(event);
}

void LinuxTextInput::SetFocus(bool focused) {
  state_->SetFocus(focused);
}

XimKeyEventResult LinuxTextInput::HandleXKeyEvent(XEvent& event) {
  return state_->HandleXKeyEvent(event);
}

int LinuxTextInput::PreparePoll(std::vector<pollfd>& descriptors, int timeout_ms) {
  return state_->PreparePoll(descriptors, timeout_ms);
}

void LinuxTextInput::DispatchPoll(const std::vector<pollfd>& descriptors, bool poll_succeeded) noexcept {
  state_->DispatchPoll(descriptors, poll_succeeded);
}

void LinuxTextInput::TakeDeferredKeyEvents(std::vector<LinuxDeferredKeyEvent>& events) {
  state_->TakeDeferredKeyEvents(events);
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
