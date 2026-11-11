#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <ole2.h>
#include <psapi.h>
#include <roapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <winstring.h>
#include <windows.ui.viewmanagement.h>
#include <wrl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <huxerui/app.h>

#include "internal_access.h"
#include "application/platform_frame_internal.h"
#include "resources/resource_internal.h"
#include "text/text_internal.h"
#include "win32_accessibility.h"
#include "win32_application_internal.h"
#include "application/application_internal.h"
#include "win32_application_runner.h"
#include "win32_file_internal.h"
#include "win32_http_internal.h"
#include "win32_internal.h"
#include "win32_platform_view.h"
#include "win32_renderer.h"
#include "win32_system_tray.h"
#include "win32_text_input.h"
#include "win32_ui_dispatcher.h"
#include "application/window_internal.h"
#include "macros_internal.h"

namespace huxerui::detail {

namespace {

constexpr UINT kRenderMessage = WM_APP + 1;
constexpr UINT kWindowCommandMessage = WM_APP + 3;
constexpr UINT_PTR kFrameTimer = 1;
constexpr float kDipsPerInch = 96.0F;

// The Win7-compatible binary probes WinRT dynamically instead of importing newer OS entry points.
class Win32SystemTheme final {
public:
  Win32SystemTheme() {
    module_ = LoadLibraryW(L"combase.dll");
    if (!module_) return;
    const auto initialize = reinterpret_cast<decltype(&RoInitialize)>(GetProcAddress(module_, "RoInitialize"));
    uninitialize_ = reinterpret_cast<decltype(&RoUninitialize)>(GetProcAddress(module_, "RoUninitialize"));
    const auto activate = reinterpret_cast<decltype(&RoActivateInstance)>(
        GetProcAddress(module_, "RoActivateInstance"));
    const auto create_string = reinterpret_cast<decltype(&WindowsCreateString)>(
        GetProcAddress(module_, "WindowsCreateString"));
    const auto delete_string = reinterpret_cast<decltype(&WindowsDeleteString)>(
        GetProcAddress(module_, "WindowsDeleteString"));
    if (!initialize || !uninitialize_ || !activate || !create_string || !delete_string) return;
    const HRESULT initialized = initialize(RO_INIT_SINGLETHREADED);
    apartment_initialized_ = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return;
    constexpr wchar_t name[] = L"Windows.UI.ViewManagement.UISettings";
    HSTRING class_name = nullptr;
    if (FAILED(create_string(name, static_cast<UINT32>(std::size(name) - 1), &class_name))) return;
    Microsoft::WRL::ComPtr<IInspectable> instance;
    const HRESULT result = activate(class_name, &instance);
    delete_string(class_name);
    if (SUCCEEDED(result)) HUXERUI_DISCARD(instance.As(&settings_));
  }

  ~Win32SystemTheme() {
    Stop();
    settings_.Reset();
    if (apartment_initialized_) uninitialize_();
    if (module_) FreeLibrary(module_);
  }

  Win32SystemTheme(const Win32SystemTheme&) = delete;
  Win32SystemTheme& operator=(const Win32SystemTheme&) = delete;

  ThemeBrightness Query() const {
    ABI::Windows::UI::Color foreground{};
    if (!settings_ || FAILED(settings_->GetColorValue(
        ABI::Windows::UI::ViewManagement::UIColorType_Foreground, &foreground))) {
      return ThemeBrightness::Light;
    }
    return 5 * foreground.G + 2 * foreground.R + foreground.B > 8 * 128
        ? ThemeBrightness::Dark : ThemeBrightness::Light;
  }

  void Observe(std::function<void()> changed) {
    if (!settings_) return;
    using Handler = ABI::Windows::Foundation::ITypedEventHandler<
        ABI::Windows::UI::ViewManagement::UISettings*, IInspectable*>;
    auto handler = Microsoft::WRL::Callback<Handler>(
        [changed = std::move(changed)](ABI::Windows::UI::ViewManagement::IUISettings*, IInspectable*) -> HRESULT {
          try { changed(); } catch (...) { return E_FAIL; }
          return S_OK;
        });
    observing_ = SUCCEEDED(settings_->add_ColorValuesChanged(handler.Get(), &token_));
  }

  void Stop() noexcept {
    if (observing_) {
      HUXERUI_DISCARD(settings_->remove_ColorValuesChanged(token_));
      observing_ = false;
    }
  }

private:
  HMODULE module_ = nullptr;
  decltype(&RoUninitialize) uninitialize_ = nullptr;
  Microsoft::WRL::ComPtr<ABI::Windows::UI::ViewManagement::IUISettings3> settings_;
  EventRegistrationToken token_{};
  bool apartment_initialized_ = false;
  bool observing_ = false;
};

enum class MouseTrackingArea {
  None,
  Client,
  NonClient,
};

LPCWSTR Win32PointerCursorResource(PointerCursorKind kind) noexcept {
  switch (kind) {
  case PointerCursorKind::Default:
    return IDC_ARROW;
  case PointerCursorKind::Text:
    return IDC_IBEAM;
  case PointerCursorKind::Hand:
    return IDC_HAND;
  case PointerCursorKind::Crosshair:
    return IDC_CROSS;
  case PointerCursorKind::Move:
    return IDC_SIZEALL;
  case PointerCursorKind::Grab:
    return IDC_HAND;
  case PointerCursorKind::Grabbing:
    return IDC_SIZEALL;
  case PointerCursorKind::ResizeHorizontal:
    return IDC_SIZEWE;
  case PointerCursorKind::ResizeVertical:
    return IDC_SIZENS;
  case PointerCursorKind::ResizeNorthEastSouthWest:
    return IDC_SIZENESW;
  case PointerCursorKind::ResizeNorthWestSouthEast:
    return IDC_SIZENWSE;
  case PointerCursorKind::NotAllowed:
    return IDC_NO;
  case PointerCursorKind::Wait:
    return IDC_WAIT;
  }
  return IDC_ARROW;
}

double FileTimeSeconds(const FILETIME& time) noexcept {
  ULARGE_INTEGER value{};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return static_cast<double>(value.QuadPart) / 10'000'000.0;
}

class Win32Api {
public:
  Win32Api() {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
      return;
    }
    set_process_dpi_awareness_context_ = reinterpret_cast<SetProcessDpiAwarenessContextFunction>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext")
    );
    get_dpi_for_system_ = reinterpret_cast<GetDpiForSystemFunction>(GetProcAddress(user32, "GetDpiForSystem"));
    get_dpi_for_window_ = reinterpret_cast<GetDpiForWindowFunction>(GetProcAddress(user32, "GetDpiForWindow"));
    adjust_window_rect_for_dpi_ =
        reinterpret_cast<AdjustWindowRectExForDpiFunction>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    get_system_metrics_for_dpi_ =
        reinterpret_cast<GetSystemMetricsForDpiFunction>(GetProcAddress(user32, "GetSystemMetricsForDpi"));
#endif
  }

  void ConfigureProcessDpiAwareness() const noexcept {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    if (set_process_dpi_awareness_context_ != nullptr) {
      constexpr std::intptr_t per_monitor_aware_v2 = -4;
      HUXERUI_DISCARD(set_process_dpi_awareness_context_(reinterpret_cast<HANDLE>(per_monitor_aware_v2)));
      return;
    }
    HUXERUI_DISCARD(SetProcessDPIAware());
#else
    HUXERUI_DISCARD(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
#endif
  }

  UINT SystemDpi() const noexcept {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    if (get_dpi_for_system_ != nullptr) {
      return get_dpi_for_system_();
    }
    return LegacySystemDpi();
#else
    return GetDpiForSystem();
#endif
  }

  UINT WindowDpi(HWND window) const noexcept {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    if (get_dpi_for_window_ != nullptr) {
      return get_dpi_for_window_(window);
    }
    return LegacySystemDpi();
#else
    return GetDpiForWindow(window);
#endif
  }

  BOOL AdjustWindowRectForDpi(RECT* rect, DWORD style, BOOL menu, DWORD extended_style, UINT dpi) const noexcept {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    if (adjust_window_rect_for_dpi_ != nullptr) {
      return adjust_window_rect_for_dpi_(rect, style, menu, extended_style, dpi);
    }
    return AdjustWindowRectEx(rect, style, menu, extended_style);
#else
    return AdjustWindowRectExForDpi(rect, style, menu, extended_style, dpi);
#endif
  }

  int SystemMetricForDpi(int index, UINT dpi) const noexcept {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    if (get_system_metrics_for_dpi_ != nullptr) {
      return get_system_metrics_for_dpi_(index, dpi);
    }
    const int value = GetSystemMetrics(index);
    return static_cast<int>(std::lround(static_cast<double>(value) * dpi / LegacySystemDpi()));
#else
    return GetSystemMetricsForDpi(index, dpi);
#endif
  }

private:
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  using SetProcessDpiAwarenessContextFunction = BOOL(WINAPI*)(HANDLE);
  using GetDpiForSystemFunction = UINT(WINAPI*)();
  using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
  using AdjustWindowRectExForDpiFunction = BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT);
  using GetSystemMetricsForDpiFunction = int(WINAPI*)(int, UINT);

  static UINT LegacySystemDpi() noexcept {
    HDC context = GetDC(nullptr);
    if (context == nullptr) {
      return static_cast<UINT>(kDipsPerInch);
    }
    const int dpi = GetDeviceCaps(context, LOGPIXELSX);
    ReleaseDC(nullptr, context);
    return dpi > 0 ? static_cast<UINT>(dpi) : static_cast<UINT>(kDipsPerInch);
  }

  SetProcessDpiAwarenessContextFunction set_process_dpi_awareness_context_ = nullptr;
  GetDpiForSystemFunction get_dpi_for_system_ = nullptr;
  GetDpiForWindowFunction get_dpi_for_window_ = nullptr;
  AdjustWindowRectExForDpiFunction adjust_window_rect_for_dpi_ = nullptr;
  GetSystemMetricsForDpiFunction get_system_metrics_for_dpi_ = nullptr;
#endif
};
Key TranslateKey(WPARAM virtual_key, LPARAM key_data) {
  if (virtual_key >= 'A' && virtual_key <= 'Z') {
    return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(virtual_key - 'A'));
  }
  if (virtual_key >= '0' && virtual_key <= '9') {
    return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(virtual_key - '0'));
  }
  if (virtual_key >= VK_F1 && virtual_key <= VK_F24) {
    return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(virtual_key - VK_F1));
  }
  if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9) {
    return static_cast<Key>(static_cast<int>(Key::Numpad0) + static_cast<int>(virtual_key - VK_NUMPAD0));
  }
  const bool extended = (static_cast<std::uintptr_t>(key_data) & (1ULL << 24U)) != 0;
  switch (virtual_key) {
  case VK_LSHIFT:
    return Key::ShiftLeft;
  case VK_RSHIFT:
    return Key::ShiftRight;
  case VK_SHIFT: {
    const UINT scan_code = (static_cast<UINT>(key_data) >> 16U) & 0xFFU;
    return MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? Key::ShiftRight : Key::ShiftLeft;
  }
  case VK_LCONTROL:
    return Key::ControlLeft;
  case VK_RCONTROL:
    return Key::ControlRight;
  case VK_CONTROL:
    return extended ? Key::ControlRight : Key::ControlLeft;
  case VK_LMENU:
    return Key::AltLeft;
  case VK_RMENU:
    return Key::AltRight;
  case VK_MENU:
    return extended ? Key::AltRight : Key::AltLeft;
  case VK_LWIN:
    return Key::MetaLeft;
  case VK_RWIN:
    return Key::MetaRight;
  case VK_BACK:
    return Key::Backspace;
  case VK_TAB:
    return Key::Tab;
  case VK_RETURN:
    return extended ? Key::NumpadEnter : Key::Enter;
  case VK_ESCAPE:
    return Key::Escape;
  case VK_SPACE:
    return Key::Space;
  case VK_INSERT:
    return extended ? Key::Insert : Key::Numpad0;
  case VK_DELETE:
    return extended ? Key::Delete : Key::NumpadDecimal;
  case VK_HOME:
    return extended ? Key::Home : Key::Numpad7;
  case VK_END:
    return extended ? Key::End : Key::Numpad1;
  case VK_PRIOR:
    return extended ? Key::PageUp : Key::Numpad9;
  case VK_NEXT:
    return extended ? Key::PageDown : Key::Numpad3;
  case VK_LEFT:
    return extended ? Key::ArrowLeft : Key::Numpad4;
  case VK_RIGHT:
    return extended ? Key::ArrowRight : Key::Numpad6;
  case VK_UP:
    return extended ? Key::ArrowUp : Key::Numpad8;
  case VK_DOWN:
    return extended ? Key::ArrowDown : Key::Numpad2;
  case VK_OEM_3:
    return Key::Backquote;
  case VK_OEM_MINUS:
    return Key::Minus;
  case VK_OEM_PLUS:
    return Key::Equal;
  case VK_OEM_4:
    return Key::BracketLeft;
  case VK_OEM_6:
    return Key::BracketRight;
  case VK_OEM_5:
    return Key::Backslash;
  case VK_OEM_1:
    return Key::Semicolon;
  case VK_OEM_7:
    return Key::Quote;
  case VK_OEM_COMMA:
    return Key::Comma;
  case VK_OEM_PERIOD:
    return Key::Period;
  case VK_OEM_2:
    return Key::Slash;
  case VK_OEM_102:
    return Key::IntlBackslash;
  case VK_CAPITAL:
    return Key::CapsLock;
  case VK_NUMLOCK:
    return Key::NumLock;
  case VK_SCROLL:
    return Key::ScrollLock;
  case VK_SNAPSHOT:
    return Key::PrintScreen;
  case VK_PAUSE:
    return Key::Pause;
  case VK_APPS:
    return Key::ContextMenu;
  case VK_HELP:
    return Key::Help;
  case VK_DECIMAL:
    return Key::NumpadDecimal;
  case VK_DIVIDE:
    return Key::NumpadDivide;
  case VK_MULTIPLY:
    return Key::NumpadMultiply;
  case VK_SUBTRACT:
    return Key::NumpadSubtract;
  case VK_ADD:
    return Key::NumpadAdd;
  case VK_SEPARATOR:
    return Key::NumpadComma;
  case VK_CLEAR:
    return Key::Numpad5;
  default:
    return Key::Unknown;
  }
}

PointerButton SemanticMouseButton(bool left) noexcept {
  const bool primary = left != (GetSystemMetrics(SM_SWAPBUTTON) != 0);
  return primary ? PointerButton::Primary : PointerButton::Secondary;
}

PointerButton MouseButtons(WPARAM state) noexcept {
  PointerButton buttons = PointerButton::None;
  if ((state & MK_LBUTTON) != 0) {
    buttons |= SemanticMouseButton(true);
  }
  if ((state & MK_RBUTTON) != 0) {
    buttons |= SemanticMouseButton(false);
  }
  if ((state & MK_MBUTTON) != 0) {
    buttons |= PointerButton::Middle;
  }
  if ((state & MK_XBUTTON1) != 0) {
    buttons |= PointerButton::Back;
  }
  if ((state & MK_XBUTTON2) != 0) {
    buttons |= PointerButton::Forward;
  }
  return buttons;
}

KeyModifiers CurrentKeyModifiers() {
  const bool alt_graph = (GetKeyState(VK_RMENU) & 0x8000) != 0 && (GetKeyState(VK_LCONTROL) & 0x8000) != 0 &&
                         (GetKeyState(VK_RCONTROL) & 0x8000) == 0;
  return {
      (GetKeyState(VK_SHIFT) & 0x8000) != 0,
      !alt_graph && (GetKeyState(VK_CONTROL) & 0x8000) != 0,
      (GetKeyState(VK_MENU) & 0x8000) != 0,
      (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0,
  };
}

std::string TranslateKeyText(WPARAM virtual_key, LPARAM key_data) {
  BYTE keyboard_state[256]{};
  if (!GetKeyboardState(keyboard_state)) {
    return {};
  }

  wchar_t characters[8]{};
  const UINT scan_code = static_cast<UINT>((static_cast<std::uintptr_t>(key_data) >> 16U) & 0xFFU);
  const int length = ToUnicodeEx(
      static_cast<UINT>(virtual_key),
      scan_code,
      keyboard_state,
      characters,
      static_cast<int>(std::size(characters)),
      0,
      GetKeyboardLayout(0)
  );
  if (length <= 0) {
    return {};
  }
  return WideToUtf8(std::wstring_view(characters, static_cast<std::size_t>(length)));
}

class Win32COMApartment final {
public:
  Win32COMApartment() {
    const HRESULT result = OleInitialize(nullptr);
    if (FAILED(result)) {
      throw std::runtime_error("HuxerUI could not initialize Windows OLE services on its UI thread");
    }
    initialized_ = SUCCEEDED(result);
  }

  ~Win32COMApartment() {
    if (initialized_) {
      OleUninitialize();
    }
  }

  Win32COMApartment(const Win32COMApartment&) = delete;
  Win32COMApartment& operator=(const Win32COMApartment&) = delete;

private:
  bool initialized_ = false;
};

using Microsoft::WRL::ComPtr;

UINT HtmlFormat() { return RegisterClipboardFormatW(L"HTML Format"); }
UINT PngFormat() { return RegisterClipboardFormatW(L"PNG"); }

class ClipboardAccess {
public:
  explicit ClipboardAccess(HWND window) : opened_(window && OpenClipboard(window)) {}
  ~ClipboardAccess() { if (opened_) CloseClipboard(); }
  explicit operator bool() const noexcept { return opened_; }

private:
  bool opened_;
};

struct GlobalFreeDeleter {
  void operator()(HGLOBAL block) const noexcept { GlobalFree(block); }
};
using GlobalBlock = std::unique_ptr<void, GlobalFreeDeleter>;
using ClipboardFormats = std::vector<std::pair<UINT, GlobalBlock>>;

bool AddFormat(ClipboardFormats& formats, UINT format, std::span<const std::byte> bytes) {
  if (!format || bytes.empty()) return false;
  GlobalBlock block(GlobalAlloc(GMEM_MOVEABLE, bytes.size()));
  if (!block) return false;
  void* destination = GlobalLock(block.get());
  if (!destination) return false;
  std::memcpy(destination, bytes.data(), bytes.size());
  GlobalUnlock(block.get());
  formats.emplace_back(format, std::move(block));
  return true;
}

bool AddText(ClipboardFormats& formats, std::string_view text) {
  auto wide = StrictUtf8ToWide(text);
  return wide && AddFormat(formats, CF_UNICODETEXT, std::as_bytes(std::span(wide->c_str(), wide->size() + 1)));
}

IoResult<Bytes> CopyFormat(UINT format, std::size_t limit) {
  const HANDLE handle = GetClipboardData(format);
  const SIZE_T size = handle ? GlobalSize(handle) : 0;
  if (size > limit) return IoResult<Bytes>::Failure({IoErrorCode::TooLarge, "HuxerUI clipboard format is too large"});
  if (size == 0) return IoResult<Bytes>::Failure({IoErrorCode::Io, "HuxerUI clipboard format has no storage"});
  Bytes bytes(size);
  const void* source = GlobalLock(handle);
  if (!source) return IoResult<Bytes>::Failure({IoErrorCode::Io, "HuxerUI could not access clipboard storage"});
  std::memcpy(bytes.data(), source, size);
  GlobalUnlock(handle);
  return IoResult<Bytes>::Success(std::move(bytes));
}

IoResult<ClipboardContent> ReadUnicodeText() {
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return ClipboardFailure(IoErrorCode::NotFound);
  auto bytes = CopyFormat(CF_UNICODETEXT, (clipboard_max_text_bytes + 1) * sizeof(wchar_t));
  if (!bytes.Succeeded()) return IoResult<ClipboardContent>::Failure(std::move(bytes).Error());
  if (bytes.Value().size() % sizeof(wchar_t) != 0) return ClipboardFailure(IoErrorCode::InvalidEncoding);
  std::wstring wide(bytes.Value().size() / sizeof(wchar_t), L'\0');
  std::memcpy(wide.data(), bytes.Value().data(), bytes.Value().size());
  const auto end = wide.find(L'\0');
  if (end == wide.npos) return ClipboardFailure(IoErrorCode::InvalidEncoding);
  auto text = StrictWideToUtf8(std::wstring_view(wide).substr(0, end));
  if (!text) return ClipboardFailure(IoErrorCode::InvalidEncoding);
  return IoResult<ClipboardContent>::Success(ClipboardContent{std::move(*text)});
}

std::string Win32Html(const AttributedText& text) {
  constexpr std::string_view prefix = "<html><body><!--StartFragment-->";
  constexpr std::string_view suffix = "<!--EndFragment--></body></html>";
  std::string result = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                       "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
  const auto fragment = ClipboardHtml(text);
  if (fragment.empty() && !text.PlainText().empty()) return {};
  const auto html_start = result.size();
  const auto fragment_start = html_start + prefix.size();
  const auto fragment_end = fragment_start + fragment.size();
  const auto set = [&](std::string_view name, std::size_t offset) {
    const auto digits = std::to_string(offset);
    result.replace(result.find(name) + name.size() + 10 - digits.size(), digits.size(), digits);
  };
  set("StartHTML:", html_start);
  set("EndHTML:", fragment_end + suffix.size());
  set("StartFragment:", fragment_start);
  set("EndFragment:", fragment_end);
  result += prefix;
  result += fragment;
  result += suffix;
  return result;
}

IoResult<ClipboardContent> ReadWin32Html(UINT format) {
  auto bytes = CopyFormat(format, clipboard_max_text_bytes + 1024);
  if (!bytes.Succeeded()) return IoResult<ClipboardContent>::Failure(std::move(bytes).Error());
  std::string_view html(reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size());
  html = html.substr(0, html.find('\0'));
  const auto offset = [&](std::string_view field) -> std::optional<std::size_t> {
    const auto header = html.substr(0, 1024);
    const auto start = header.find(field);
    if (start == header.npos) return std::nullopt;
    std::size_t value = 0;
    const auto parsed = std::from_chars(header.data() + start + field.size(), header.data() + header.size(), value);
    return parsed.ec == std::errc{} ? std::optional(value) : std::nullopt;
  };
  const auto start = offset("StartFragment:");
  const auto end = offset("EndFragment:");
  if (start && end && *start <= *end && *end <= html.size()) {
    return ReadClipboardHtml(html.substr(*start, *end - *start));
  }
  constexpr std::string_view marker = "<!--StartFragment-->";
  const auto marker_start = html.find(marker);
  const auto marker_end = html.find("<!--EndFragment-->");
  if (marker_start == html.npos || marker_end == html.npos || marker_end < marker_start + marker.size()) {
    return ClipboardFailure(IoErrorCode::InvalidEncoding);
  }
  return ReadClipboardHtml(html.substr(marker_start + marker.size(), marker_end - marker_start - marker.size()));
}

IoResult<std::vector<std::wstring>> CapturePaths() {
  using Paths = IoResult<std::vector<std::wstring>>;
  if (!IsClipboardFormatAvailable(CF_HDROP)) {
    const UINT promise = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    const auto code = promise && IsClipboardFormatAvailable(promise) ? IoErrorCode::Unsupported : IoErrorCode::NotFound;
    return Paths::Failure({code, "HuxerUI clipboard has no supported Windows file list"});
  }
  const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (!drop) return Paths::Failure({IoErrorCode::Io, "HuxerUI could not read the Windows clipboard file list"});
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  if (count == 0 || count > clipboard_max_files) {
    return Paths::Failure({count ? IoErrorCode::TooLarge : IoErrorCode::NotFound,
                           "HuxerUI clipboard has no supported Windows file list"});
  }
  std::vector<std::wstring> paths;
  paths.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
    if (length == 0 || length > 32767) {
      return Paths::Failure({IoErrorCode::Io, "HuxerUI clipboard contains an invalid Windows file path"});
    }
    std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
    if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
      return Paths::Failure({IoErrorCode::Io, "HuxerUI could not read a Windows clipboard file path"});
    }
    path.resize(length);
    paths.push_back(std::move(path));
  }
  return Paths::Success(std::move(paths));
}

Task<IoResult<ClipboardContent>> RetainPaths(std::vector<std::wstring> paths) {
  co_return co_await RunWorker([paths = std::move(paths)] {
    std::vector<FileReference> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
      if (!std::filesystem::path(path).is_absolute()) return ClipboardFailure(IoErrorCode::InvalidEncoding);
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError() == ERROR_ACCESS_DENIED ? IoErrorCode::PermissionDenied : IoErrorCode::Io;
        return ClipboardFailure(code);
      }
      auto file = MakeWin32FileReference(path, false, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
      if (!file) return ClipboardFailure(IoErrorCode::Io);
      files.push_back(std::move(*file));
    }
    return IoResult<ClipboardContent>::Success(ClipboardContent{std::move(files)});
  });
}

bool AddFiles(ClipboardFormats& formats, const ClipboardContent& content) {
  const auto files = ClipboardFilePaths(content);
  if (!files) return false;
  std::wstring paths;
  for (const auto& file : *files) {
    auto path = StrictUtf8ToWide(file.Path());
    if (!path || path->empty()) return false;
    std::ranges::replace(*path, L'/', L'\\');
    paths += *path;
    paths += L'\0';
  }
  paths += L'\0';
  DROPFILES header{};
  header.pFiles = sizeof(DROPFILES);
  header.fWide = TRUE;
  Bytes data(sizeof(header) + paths.size() * sizeof(wchar_t));
  std::memcpy(data.data(), &header, sizeof(header));
  std::memcpy(data.data() + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
  const DWORD effect = DROPEFFECT_COPY;
  return AddFormat(formats, CF_HDROP, data) &&
      AddFormat(formats, RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), std::as_bytes(std::span(&effect, 1)));
}

ComPtr<IWICBitmapFrameDecode> DecodeImage(IWICImagingFactory& factory, std::span<const std::byte> bytes) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (bytes.empty() || bytes.size() > MAXDWORD || FAILED(factory.CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(
          reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())), static_cast<DWORD>(bytes.size()))) ||
      FAILED(factory.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) return {};
  return frame;
}

ComPtr<IWICImagingFactory> ImagingFactory() {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return {};
  }
  return factory;
}

bool AddImage(ClipboardFormats& formats, const ImageAsset& image) {
  auto factory = ImagingFactory();
  if (!factory) return false;
  auto frame = DecodeImage(*factory.Get(), image.EncodedBytes());
  ComPtr<IWICFormatConverter> pixels;
  if (!frame || FAILED(factory->CreateFormatConverter(&pixels)) ||
      FAILED(pixels->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = static_cast<LONG>(image.PixelWidth());
  header.bV5Height = -static_cast<LONG>(image.PixelHeight());
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5SizeImage = image.PixelWidth() * image.PixelHeight() * 4;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  header.bV5CSType = LCS_sRGB;
  header.bV5Intent = LCS_GM_IMAGES;
  Bytes dib(sizeof(header) + header.bV5SizeImage);
  std::memcpy(dib.data(), &header, sizeof(header));
  if (FAILED(pixels->CopyPixels(nullptr, image.PixelWidth() * 4, header.bV5SizeImage,
                                reinterpret_cast<BYTE*>(dib.data() + sizeof(header))))) return false;
  return AddFormat(formats, CF_DIBV5, dib) &&
      (image.Format() != ImageFormat::Png || AddFormat(formats, PngFormat(), image.EncodedBytes()));
}

IoResult<ClipboardContent> DecodeDib(Bytes dib) {
  BITMAPINFOHEADER info{};
  if (dib.size() < sizeof(info)) return ClipboardFailure(IoErrorCode::InvalidEncoding);
  std::memcpy(&info, dib.data(), sizeof(info));
  if (info.biSize < sizeof(info) || info.biSize > dib.size() || info.biWidth <= 0 || info.biHeight == 0 ||
      info.biPlanes != 1 || (info.biCompression != BI_RGB && info.biCompression != BI_BITFIELDS) ||
      (info.biBitCount != 1 && info.biBitCount != 4 && info.biBitCount != 8 && info.biBitCount != 16 &&
       info.biBitCount != 24 && info.biBitCount != 32)) {
    return ClipboardFailure(IoErrorCode::Unsupported);
  }
  const auto height = static_cast<std::size_t>(info.biHeight < 0 ? -std::int64_t(info.biHeight) : info.biHeight);
  if (!ClipboardImageSizeAllowed(info.biWidth, height)) return ClipboardFailure(IoErrorCode::TooLarge);
  const std::size_t masks = info.biSize == sizeof(info) && info.biCompression == BI_BITFIELDS ? 12 : 0;
  const std::size_t palette = info.biClrUsed ? info.biClrUsed : info.biBitCount <= 8 ? 1U << info.biBitCount : 0U;
  if (dib.size() - info.biSize < masks || palette > (dib.size() - info.biSize - masks) / 4) {
    return ClipboardFailure(IoErrorCode::InvalidEncoding);
  }
  const std::size_t pixels = info.biSize + masks + palette * 4;
  const auto stride = (static_cast<std::size_t>(info.biWidth) * info.biBitCount + 31) / 32 * 4;
  if (stride > (dib.size() - pixels) / height) return ClipboardFailure(IoErrorCode::InvalidEncoding);

  BITMAPFILEHEADER file{};
  file.bfType = 0x4D42;
  file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
  file.bfOffBits = static_cast<DWORD>(sizeof(file) + pixels);
  dib.insert(dib.begin(), reinterpret_cast<const std::byte*>(&file),
             reinterpret_cast<const std::byte*>(&file) + sizeof(file));
  auto factory = ImagingFactory();
  if (!factory) return ClipboardFailure(IoErrorCode::Io);
  auto decoded = DecodeImage(*factory.Get(), dib);
  if (!decoded) return ClipboardFailure(IoErrorCode::InvalidEncoding);
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->WriteSource(decoded.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return ClipboardFailure(IoErrorCode::Io);
  }
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return ClipboardFailure(IoErrorCode::Io);
  if (stat.cbSize.QuadPart > clipboard_max_image_bytes) return ClipboardFailure(IoErrorCode::TooLarge);
  Bytes png(static_cast<std::size_t>(stat.cbSize.QuadPart));
  ULONG read = 0;
  if (FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)) ||
      FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read != png.size()) {
    return ClipboardFailure(IoErrorCode::Io);
  }
  return ClipboardImage(std::move(png));
}

Task<IoResult<ClipboardContent>> DecodeDibAsync(Bytes bytes) {
  co_return co_await RunWorker([bytes = std::move(bytes)]() mutable {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return ClipboardFailure(IoErrorCode::Io);
    struct Apartment {
      HRESULT initialized;
      ~Apartment() { if (SUCCEEDED(initialized)) CoUninitialize(); }
    } apartment{initialized};
    return DecodeDib(std::move(bytes));
  });
}

bool Win32ClipboardMayHave(ClipboardKind kind) noexcept {
  const auto advertised = [](UINT format) { return format && IsClipboardFormatAvailable(format); };
  switch (kind) {
  case ClipboardKind::Text: return advertised(CF_UNICODETEXT);
  case ClipboardKind::RichText: return advertised(HtmlFormat()) || advertised(CF_UNICODETEXT);
  case ClipboardKind::Image: return advertised(PngFormat()) || advertised(CF_DIBV5) || advertised(CF_DIB);
  case ClipboardKind::Files: return advertised(CF_HDROP);
  }
  return false;
}

bool WriteWin32Clipboard(HWND window, const ClipboardContent& content) {
  ClipboardFormats formats;
  const bool prepared = std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, std::string>) {
      return AddText(formats, value);
    } else if constexpr (std::is_same_v<T, AttributedText>) {
      const auto html = Win32Html(value);
      return !html.empty() && AddText(formats, value.PlainText()) &&
          AddFormat(formats, HtmlFormat(), std::as_bytes(std::span(html.c_str(), html.size() + 1)));
    } else if constexpr (std::is_same_v<T, ImageAsset>) {
      return AddImage(formats, value);
    } else {
      return AddFiles(formats, content);
    }
  }, content);
  if (!prepared) return false;
  ClipboardAccess access(window);
  if (!access || !EmptyClipboard()) return false;
  for (auto& [format, block] : formats) {
    if (!SetClipboardData(format, block.get())) {
      EmptyClipboard();
      return false;
    }
    block.release();
  }
  return true;
}

Task<IoResult<ClipboardContent>> ReadWin32ClipboardAsync(HWND window, ClipboardKind kind) {
  ClipboardAccess access(window);
  if (!access) return ClipboardResult(ClipboardFailure(IoErrorCode::Io));
  switch (kind) {
  case ClipboardKind::Text:
    return ClipboardResult(ReadUnicodeText());
  case ClipboardKind::RichText: {
    const UINT html = HtmlFormat();
    return ClipboardResult(html && IsClipboardFormatAvailable(html) ? ReadWin32Html(html) : ReadUnicodeText());
  }
  case ClipboardKind::Files: {
    auto paths = CapturePaths();
    if (!paths.Succeeded()) return ClipboardResult(IoResult<ClipboardContent>::Failure(std::move(paths).Error()));
    return RetainPaths(std::move(paths).Value());
  }
  case ClipboardKind::Image: {
    const UINT png = PngFormat();
    if (png && IsClipboardFormatAvailable(png)) {
      auto bytes = CopyFormat(png, clipboard_max_image_bytes);
      if (!bytes.Succeeded()) return ClipboardResult(IoResult<ClipboardContent>::Failure(std::move(bytes).Error()));
      return ClipboardResult(ClipboardImage(std::move(bytes).Value()));
    }
    const UINT format = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
    if (!IsClipboardFormatAvailable(format)) return ClipboardResult(ClipboardFailure(IoErrorCode::NotFound));
    auto bytes = CopyFormat(format, clipboard_max_image_pixels * 4 + 64 * 1024);
    if (!bytes.Succeeded()) return ClipboardResult(IoResult<ClipboardContent>::Failure(std::move(bytes).Error()));
    return DecodeDibAsync(std::move(bytes).Value());
  }
  }
  return ClipboardResult(ClipboardFailure(IoErrorCode::Unsupported));
}

} // namespace

/// Owns process services and a hidden facilities HWND independently of visible WindowsUiWindow attachments.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class WindowsRuntime final : public huxerui::Runtime, public PlatformResources, public PlatformClipboard {
public:
  WindowsRuntime(Win32UiThreadDispatcher& dispatcher, std::shared_ptr<LocalNotificationTransport> notifications,
                 Win32WindowReady on_window_ready)
      : Runtime(dispatcher.Bind()), dispatcher_(dispatcher), on_window_ready_(std::move(on_window_ready)),
        notifications_(std::move(notifications)) {
    win32_api_.ConfigureProcessDpiAwareness();
    window_class_ = Win32ApplicationWindowClassName();
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = window_class_.c_str();
    if (!RegisterClassW(&window_class)) {
      throw std::runtime_error("HuxerUI could not register its application facilities window");
    }
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, window_class_.c_str(), L"", WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, window_class.hInstance, this);
    if (!window_) {
      UnregisterClassW(window_class_.c_str(), window_class.hInstance);
      throw std::runtime_error("HuxerUI could not create its application facilities window");
    }
  }

  ~WindowsRuntime() override {
    Retire();
    if (system_tray_) {
      system_tray_->SetWindow(nullptr);
    }
    DestroyWindow(window_);
    if (ui_class_registered_) UnregisterClassW(ui_window_class_.c_str(), GetModuleHandleW(nullptr));
    UnregisterClassW(window_class_.c_str(), GetModuleHandleW(nullptr));
  }

  bool SupportsMultipleWindows() const noexcept override { return true; }
  std::unique_ptr<UiWindow> CreateUiWindow(ViewFactory content, const WindowOptions& options) override;
  int Run();

  void ReportFailure(std::exception_ptr failure) {
    if (!failure_) failure_ = std::move(failure);
    RequestShutdown();
  }

  /// Starts shared services after the facilities HWND and dispatcher are available.
  /// @param application Declaration retained by the application runner for this Runtime's lifetime.
  /// @param activation Initial native activation delivered after application hooks, on the application thread.
  void Start(const Application& application, ApplicationActivation activation) {
    InitializeApplication(application, std::move(activation));
  }
  /// Aggregates visible, non-minimized process windows, including windows created outside HuxerUI.
  /// The facilities tool window is excluded; call on the Win32 application thread after native state changes.
  void UpdateLifecycleState() {
    if (!IsInitialized()) return;
    ApplicationLifecycleState lifecycle = ApplicationLifecycleState::Background;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
      DWORD process = 0;
      GetWindowThreadProcessId(window, &process);
      if (process != GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window) ||
          (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
      auto& state = *reinterpret_cast<ApplicationLifecycleState*>(parameter);
      if (window == GetForegroundWindow()) {
        state = ApplicationLifecycleState::Active;
        return FALSE;
      }
      state = ApplicationLifecycleState::Inactive;
      return TRUE;
    }, reinterpret_cast<LPARAM>(&lifecycle));
    Runtime::UpdateApplicationLifecycleState(lifecycle);
  }
  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<AppDirectories> CreateAppDirectories() override {
    return CreateWin32AppDirectories();
  }


  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateWin32HttpTransport();
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override {
    return CreateWin32PermissionTransport();
  }

  std::shared_ptr<LocalNotificationTransport> CreateLocalNotificationTransport() override { return notifications_; }

  std::shared_ptr<SystemTrayTransport> CreateSystemTrayTransport() override {
    if (!system_tray_) {
      system_tray_ = std::make_shared<Win32SystemTrayTransport>();
      system_tray_->SetWindow(window_);
    }
    return system_tray_;
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE) {
      return std::nullopt;
    }
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == FALSE) {
      return std::nullopt;
    }
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    return ProcessMetrics{
        .cpu_time_seconds = FileTimeSeconds(kernel) + FileTimeSeconds(user),
        .memory_usage_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize),
        .processor_count = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(system_info.dwNumberOfProcessors)),
    };
  }

  ResourceConfiguration Configuration() const override {
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    Locale locale = Locale::Default();
    if (GetUserDefaultLocaleName(locale_name, static_cast<int>(std::size(locale_name))) > 0) {
      locale = Locale::FromLanguageTag(WideToUtf8(locale_name));
    }
    const UINT dpi = win32_api_.SystemDpi();
    return {std::move(locale), static_cast<float>(dpi) / kDipsPerInch};
  }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Windows resource path is invalid");
    }
    std::wstring executable_path(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (length == 0 || length >= executable_path.size()) {
      throw std::logic_error("HuxerUI Windows executable path could not be resolved");
    }
    executable_path.resize(length);
    std::filesystem::path resource_root(executable_path);
    resource_root.replace_extension(L".resources");
    const std::wstring wide_package_path = Utf8ToWide(package_path);
    if (wide_package_path.empty()) {
      throw std::logic_error("HuxerUI Windows resource path is not valid UTF-8");
    }
    const std::filesystem::path path = resource_root / std::filesystem::path(wide_package_path);
    return OpenPackageFile(path);
  }

  std::optional<std::string> ReadText() override {
    if (window_ == nullptr || !OpenClipboard(window_)) {
      return std::nullopt;
    }
    struct ClipboardCloser {
      ~ClipboardCloser() {
        CloseClipboard();
      }
    } closer;

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
      return std::nullopt;
    }
    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (text == nullptr) {
      return std::nullopt;
    }
    const std::string result = WideToUtf8(text);
    GlobalUnlock(handle);
    return result;
  }

  bool WriteText(std::string_view text) override {
    if (window_ == nullptr || !OpenClipboard(window_)) {
      return false;
    }
    struct ClipboardCloser {
      ~ClipboardCloser() {
        CloseClipboard();
      }
    } closer;

    const std::wstring wide = Utf8ToWide(text);
    const SIZE_T size = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory == nullptr) {
      return false;
    }
    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
      GlobalFree(memory);
      return false;
    }
    std::memcpy(destination, wide.c_str(), size);
    GlobalUnlock(memory);
    if (!EmptyClipboard() || SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
      GlobalFree(memory);
      return false;
    }
    return true;
  }

  bool WriteContent(ClipboardContent content) override {
    if (!WriteWin32Clipboard(window_, content)) return false;
    if (auto* references = std::get_if<std::vector<FileReference>>(&content)) {
      clipboard_references_ = std::move(*references);
    }
    return true;
  }

  Task<IoResult<ClipboardContent>> ReadContentAsync(ClipboardKind kind) override {
    return ReadWin32ClipboardAsync(window_, kind);
  }

  bool MayHaveContent(ClipboardKind kind) const noexcept override {
    return window_ && Win32ClipboardMayHave(kind);
  }

protected:
  void OnRuntimeStopped() override { PostQuitMessage(0); }

private:
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* runtime = reinterpret_cast<WindowsRuntime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      runtime = static_cast<WindowsRuntime*>(reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(runtime));
    }
    if (runtime) {
      if (message == WM_DESTROYCLIPBOARD) {
        runtime->clipboard_references_.clear();
        return 0;
      }
      if (message == WM_COPYDATA && runtime->IsInitialized()) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(l_param);
        if (data == nullptr || data->dwData != win32_application_activation_data_id || data->lpData == nullptr ||
            data->cbData % sizeof(wchar_t) != 0 ||
            data->cbData / sizeof(wchar_t) > win32_application_activation_max_characters) {
          return FALSE;
        }
        const auto* characters = static_cast<const wchar_t*>(data->lpData);
        const std::span<const wchar_t> payload(characters, data->cbData / sizeof(wchar_t));
        std::optional<ApplicationActivation> activation = DecodeWin32ApplicationActivation(payload);
        if (!activation.has_value()) return FALSE;
        try {
          runtime->HandleApplicationActivation(std::move(*activation));
          return TRUE;
        } catch (...) {
          runtime->ReportFailure(std::current_exception());
          return FALSE;
        }
      }
      if (runtime->system_tray_) {
        if (const auto handled = runtime->system_tray_->HandleMessage(message, w_param, l_param)) {
          return *handled;
        }
      }
      if (message == WM_SETTINGCHANGE && runtime->IsInitialized()) {
        runtime->UpdateResourceConfiguration(runtime->Configuration());
      }
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  Win32UiThreadDispatcher& dispatcher_;
  Win32WindowReady on_window_ready_;
  std::exception_ptr failure_;
  const std::wstring ui_window_class_ = Win32ApplicationWindowClassName() + L".Ui";
  bool ui_class_registered_ = false;
  HWND window_ = nullptr;
  std::vector<FileReference> clipboard_references_;
  std::wstring window_class_;
  Win32Api win32_api_;
  std::shared_ptr<Win32SystemTrayTransport> system_tray_;
  std::shared_ptr<LocalNotificationTransport> notifications_;
};

/// Owns one visible Win32 window, its rendering/text facilities, and the shared UiWindow state.
class WindowsUiWindow final : public huxerui::UiWindow {
public:
  WindowsUiWindow(Win32UiThreadDispatcher& ui_dispatcher, std::wstring window_class_name)
      : ui_dispatcher_(ui_dispatcher), window_class_name_(std::move(window_class_name)) {
    win32_api_.ConfigureProcessDpiAwareness();
  }

  ~WindowsUiWindow() override { Retire(); Cleanup(); }

  void Initialize(Runtime& application, ViewFactory content, const WindowOptions& options) {
    renderer_.Initialize();
    instance_ = GetModuleHandleW(nullptr);
    CreateApplicationWindow(application, std::move(content), options);
    const auto execution = InternalAccess::WindowExecution(*this);
    system_theme_.Observe([dispatch = ui_dispatcher_.Bind(), ui = execution->ui] {
      dispatch([ui] {
        if (const auto state = ui.lock(); state && state->ui_window) {
          auto* window = static_cast<WindowsUiWindow*>(state->ui_window);
          if (window->IsInitialized()) {
            window->UpdateSystemThemeBrightness(window->QuerySystemThemeBrightness());
          }
        }
      });
    });
    UpdateSystemThemeBrightness(QuerySystemThemeBrightness());
    UiWindow::UpdateResourceConfiguration(Configuration());
    UpdateRuntimeViewport();
    RequestFrameAt(Now());
  }

  void FinishInitialization() {
    if (initialization_failure_) std::rethrow_exception(initialization_failure_);
    initializing_ = false;
  }

  ThemeBrightness QuerySystemThemeBrightness() const override { return system_theme_.Query(); }

  HWND Window() const noexcept { return window_; }

  static void Dispatch(MSG& message, const std::wstring& window_class) {
    HWND root = GetAncestor(message.hwnd, GA_ROOT);
    wchar_t name[256]{};
    GetClassNameW(root, name, static_cast<int>(std::size(name)));
    auto* ui = window_class == name ?
        reinterpret_cast<WindowsUiWindow*>(GetWindowLongPtrW(root, GWLP_USERDATA)) : nullptr;
    ExecutionGuard guard(ui ? InternalAccess::WindowExecution(*ui) : nullptr);
    if (ui && ui->IsInitialized() && ui->platform_views_->HandleFocusTraversal(message)) {
      ui->platform_views_->SynchronizeFocus(GetFocus());
      return;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
    if (ui && ui->IsInitialized()) ui->platform_views_->SynchronizeFocus(GetFocus());
  }

  static void RegisterWindowClass(const std::wstring& name) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = name.c_str();
    if (!RegisterClassExW(&window_class)) {
      throw std::runtime_error("HuxerUI could not register its Windows window class");
    }
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_coordinator_.Request(deadline, Now(), window_ != nullptr)) {
      ScheduleFrame(*scheduled);
    }
  }

  void ScheduleFrame(double deadline) {
    const double delay_seconds = std::max(0.0, deadline - Now());
    if (delay_seconds <= 0.0) {
      if (timer_armed_) {
        KillTimer(window_, kFrameTimer);
        timer_armed_ = false;
        timer_deadline_.reset();
      }
      if (!render_message_posted_) {
        render_message_posted_ = PostMessageW(window_, kRenderMessage, 0, 0) != FALSE;
      }
      return;
    }

    if (render_message_posted_) {
      return;
    }
    if (timer_armed_ && timer_deadline_.has_value() && *timer_deadline_ <= deadline) {
      return;
    }
    if (timer_armed_) {
      KillTimer(window_, kFrameTimer);
      timer_armed_ = false;
      timer_deadline_.reset();
    }
    const double milliseconds = std::ceil(delay_seconds * 1000.0);
    const double bounded = std::clamp(milliseconds, 1.0, static_cast<double>(std::numeric_limits<UINT>::max()));
    timer_armed_ = SetTimer(window_, kFrameTimer, static_cast<UINT>(bounded), nullptr) != 0;
    if (timer_armed_) {
      timer_deadline_ = deadline;
    } else if (!render_message_posted_) {
      render_message_posted_ = PostMessageW(window_, kRenderMessage, 0, 0) != FALSE;
    }
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void SetPointerCursor(PointerCursorKind kind) override {
    pointer_cursor_kind_ = kind;
    if (mouse_tracking_area_ == MouseTrackingArea::Client) {
      ApplyPointerCursor();
    }
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayoutBackend> CreatePlatformTextLayout(
      const huxerui::AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options) override {
    return renderer_.CreatePlatformTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateWin32FilePickerTransport([this] { return window_; }, ui_dispatcher_.Bind());
  }
  void RequestWindowCommand(WindowCommand command) override {
    if (window_ == nullptr) {
      return;
    }
    PostMessageW(window_, kWindowCommandMessage, static_cast<WPARAM>(command), 0);
  }



  ResourceConfiguration Configuration() const {
    ResourceConfiguration configuration = static_cast<WindowsRuntime&>(ApplicationRuntime()).Configuration();
    configuration.display_scale = static_cast<float>(window_ != nullptr ? win32_api_.WindowDpi(window_) : win32_api_.SystemDpi()) / kDipsPerInch;
    return configuration;
  }
private:
  void UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state) {
    if (IsInitialized()) {
      if (lifecycle_state != ApplicationLifecycleState::Background) {
        UpdateSystemThemeBrightness(QuerySystemThemeBrightness());
      }
      switch (lifecycle_state) {
      case ApplicationLifecycleState::Active:
        UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Active);
        break;
      case ApplicationLifecycleState::Inactive:
        UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Inactive);
        break;
      case ApplicationLifecycleState::Background:
        UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Background);
        break;
      }
      static_cast<WindowsRuntime&>(ApplicationRuntime()).UpdateLifecycleState();
    }
  }

  void CreateApplicationWindow(Runtime& application, ViewFactory content, const WindowOptions& options) {
    dpi_ = static_cast<float>(win32_api_.SystemDpi());
    const float scale = DpiScale();
    custom_chrome_ = options.chrome_mode == WindowChromeMode::Custom;
    custom_title_bar_height_ = options.title_bar_height;
    minimum_size_ = options.minimum_size;
    const Size initial_size = ResolveInitialWindowSize(options);
    RECT frame{
        0,
        0,
        std::max(1L, static_cast<LONG>(std::lround(initial_size.width * scale))),
        std::max(1L, static_cast<LONG>(std::lround(initial_size.height * scale))),
    };
    const DWORD style = WS_OVERLAPPEDWINDOW;
    if (!custom_chrome_ && !win32_api_.AdjustWindowRectForDpi(&frame, style, FALSE, 0, static_cast<UINT>(dpi_))) {
      throw std::runtime_error("HuxerUI could not calculate the Windows window size");
    }

    const std::wstring title = Utf8ToWide(options.title);
    window_ = CreateWindowExW(
        0,
        window_class_name_.c_str(),
        title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        frame.right - frame.left,
        frame.bottom - frame.top,
        nullptr,
        nullptr,
        instance_,
        this
    );
    if (initialization_failure_) std::rethrow_exception(initialization_failure_);
    if (window_ == nullptr) {
      throw std::runtime_error("HuxerUI could not create its Windows application window");
    }
    dpi_ = static_cast<float>(win32_api_.WindowDpi(window_));
    auto configuration = application.Resources()->Configuration();
    configuration.display_scale = DpiScale();
    accessibility_.SetUiWindow(this);
    text_input_.SetUiWindow(this);
    platform_views_ =
        std::make_unique<Win32PlatformViews>(instance_, window_, InternalAccess::ApplicationRegistry(application), *this,
                                             [this](HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
                                               return HandleOverlayMessage(source, message, w_param, l_param);
                                             });
    file_drop_ = std::make_unique<Win32FileDrop>(window_, *this, [this] { return DpiScale(); });
    InitializeWindow(application, std::move(content), options, configuration);
    if (custom_chrome_) {
      first_nc_calc_ = false;
      if (!SetWindowPos(
              window_,
              nullptr,
              0,
              0,
              0,
              0,
              SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
          )) {
        throw std::runtime_error("HuxerUI could not apply its custom Windows frame");
      }
      const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
      HUXERUI_DISCARD(DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference)));
    }
    dpi_ = static_cast<float>(win32_api_.WindowDpi(window_));
    accessibility_.SetDpiScale(DpiScale());
  }

  void Cleanup() noexcept {
    system_theme_.Stop();
    Retire();
    file_drop_.reset();
    text_input_.Reset();
    committed_frame_ = nullptr;
    accessibility_.Reset();
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    if (window_ != nullptr) {
      HUXERUI_DISCARD(DestroyWindow(window_));
      window_ = nullptr;
    }
    renderer_.Discard();
    instance_ = nullptr;
  }

  float DpiScale() const noexcept {
    return std::max(dpi_, 1.0F) / kDipsPerInch;
  }

  int SystemMetric(int index) const noexcept {
    return win32_api_.SystemMetricForDpi(index, static_cast<UINT>(std::max(dpi_, 1.0F)));
  }

  int ResizeBorderX() const noexcept {
    return std::max(1, SystemMetric(SM_CXSIZEFRAME) + SystemMetric(SM_CXPADDEDBORDER));
  }

  int ResizeBorderY() const noexcept {
    return std::max(1, SystemMetric(SM_CYSIZEFRAME) + SystemMetric(SM_CXPADDEDBORDER));
  }

  std::optional<WindowTitleBarMetrics> QueryTitleBarMetrics(Size viewport) const noexcept {
    if (!custom_chrome_) {
      return std::nullopt;
    }
    const float scale = DpiScale();
    const float button_width = ResolveWin32CaptionButtonWidth(static_cast<float>(SystemMetric(SM_CXSIZE)) / scale);
    return ConstrainWin32TitleBarMetrics(
        WindowTitleBarMetrics{
            .height = std::max(custom_title_bar_height_, static_cast<float>(SystemMetric(SM_CYSIZE)) / scale),
            .right_inset = button_width * 3.0F,
            .maximized = IsZoomed(window_) != FALSE,
        },
        viewport
    );
  }

  void UpdateRuntimeViewport() {
    if (!IsInitialized() || window_ == nullptr) {
      return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const float scale = DpiScale();
    const Size viewport{
        static_cast<float>(client.right - client.left) / scale,
        static_cast<float>(client.bottom - client.top) / scale,
    };
    UiWindow::SetWindowMetrics({
        .viewport = viewport,
        .title_bar = QueryTitleBarMetrics(viewport),
    });
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_coordinator_.TakeDeferred(window_ != nullptr)) {
      ScheduleFrame(*deadline);
    }
  }

  bool InvalidateFullWindow() {
    const bool invalidated = window_ != nullptr && InvalidateRect(window_, nullptr, FALSE) != FALSE;
    if (invalidated) {
      frame_coordinator_.MarkPaintPending();
    }
    return invalidated;
  }

  bool InvalidateDamage(const DamageRegion& damage) {
    RECT client{};
    GetClientRect(window_, &client);
    const Win32DamageRegion resolved = ResolveWin32Damage(damage, DpiScale(), client);
    if (resolved.full) {
      return InvalidateFullWindow();
    }
    bool invalidated = false;
    for (const RECT& rect : resolved.rects) {
      invalidated = InvalidateRect(window_, &rect, FALSE) != FALSE || invalidated;
    }
    if (invalidated) {
      frame_coordinator_.MarkPaintPending();
    }
    return invalidated;
  }

  void CommitFrameAndInvalidate() {
    frame_coordinator_.Commit([&] {
      const FrameCommit& commit = BuildFrameOrErrorPage([](const char* message) noexcept {
        try {
          const std::wstring line = Utf8ToWide(message) + L'\n';
          OutputDebugStringW(line.c_str());
        } catch (...) {
          OutputDebugStringW(L"HuxerUI could not log the composition diagnostic\n");
        }
      });
      committed_frame_ = &commit.render_frame;
      const bool has_platform_views =
          platform_views_ && platform_views_->Commit(*committed_frame_, DpiScale(), PlatformViewInputBounds());
      const bool composition_changed = has_platform_views && renderer_.EnablePlatformComposition(window_);
      accessibility_.Commit(commit.semantic_frame, has_platform_views ? platform_views_.get() : nullptr);
      if (composition_changed || frame_coordinator_.PaintInterrupted()) {
        HUXERUI_DISCARD(InvalidateFullWindow());
      } else {
        HUXERUI_DISCARD(InvalidateDamage(committed_frame_->damage));
      }
      if (commit.next_frame_deadline.has_value()) {
        RequestFrameAt(*commit.next_frame_deadline);
      }
    });
    FlushDeferredFrame();
  }

  Point ClientPoint(HWND source, LPARAM position) const noexcept {
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (source != window_) {
      MapWindowPoints(source, window_, &point, 1);
    }
    const float scale = DpiScale();
    return {
        static_cast<float>(point.x) / scale,
        static_cast<float>(point.y) / scale,
    };
  }

  Point ScreenPoint(LPARAM position) const noexcept {
    POINT point{
        GET_X_LPARAM(position),
        GET_Y_LPARAM(position),
    };
    ScreenToClient(window_, &point);
    const float scale = DpiScale();
    return {
        static_cast<float>(point.x) / scale,
        static_cast<float>(point.y) / scale,
    };
  }

  LRESULT ResizeHitTest(LPARAM position) const noexcept {
    if (window_ == nullptr || IsZoomed(window_)) {
      return HTCLIENT;
    }
    RECT bounds{};
    if (!GetWindowRect(window_, &bounds)) {
      return HTCLIENT;
    }
    const LONG x = GET_X_LPARAM(position);
    const LONG y = GET_Y_LPARAM(position);
    const bool left = x < bounds.left + ResizeBorderX();
    const bool right = x >= bounds.right - ResizeBorderX();
    const bool top = y < bounds.top + ResizeBorderY();
    const bool bottom = y >= bounds.bottom - ResizeBorderY();
    if (top && left) {
      return HTTOPLEFT;
    }
    if (top && right) {
      return HTTOPRIGHT;
    }
    if (bottom && left) {
      return HTBOTTOMLEFT;
    }
    if (bottom && right) {
      return HTBOTTOMRIGHT;
    }
    if (left) {
      return HTLEFT;
    }
    if (right) {
      return HTRIGHT;
    }
    if (top) {
      return HTTOP;
    }
    if (bottom) {
      return HTBOTTOM;
    }
    return HTCLIENT;
  }

  std::optional<LRESULT> CaptionControlHitTest(LPARAM position) const noexcept {
    RECT client{};
    if (!GetClientRect(window_, &client)) {
      return std::nullopt;
    }
    const float scale = DpiScale();
    const Size viewport{
        static_cast<float>(client.right - client.left) / scale,
        static_cast<float>(client.bottom - client.top) / scale,
    };
    const std::optional<WindowTitleBarMetrics> metrics = QueryTitleBarMetrics(viewport);
    if (!metrics.has_value()) {
      return std::nullopt;
    }
    const Point point = ScreenPoint(position);
    const float client_width = viewport.width;
    const float button_width = metrics->right_inset / 3.0F;
    const float controls_left = client_width - metrics->right_inset;
    if (point.y < 0.0F || point.y >= metrics->height || point.x < controls_left || point.x >= client_width) {
      return std::nullopt;
    }
    const float maximize_left = controls_left + button_width;
    const float maximize_right = maximize_left + button_width;
    return point.x >= maximize_left && point.x < maximize_right ? HTMAXBUTTON : HTCLIENT;
  }

  std::optional<RECT> PlatformViewInputBounds() const noexcept {
    if (!custom_chrome_) return std::nullopt;
    RECT client{};
    if (!GetClientRect(window_, &client)) return client;
    const float scale = DpiScale();
    const auto title_bar = QueryTitleBarMetrics(
        {static_cast<float>(client.right) / scale, static_cast<float>(client.bottom) / scale});
    if (!IsZoomed(window_)) {
      const int horizontal = ResizeBorderX();
      const int vertical = ResizeBorderY();
      client.left += horizontal;
      client.right -= horizontal;
      client.top += vertical;
      client.bottom -= vertical;
    }
    if (title_bar) {
      client.top = std::max(client.top, static_cast<LONG>(std::ceil(title_bar->height * scale)));
    }
    return client;
  }

  void SendPointer(PointerEventType type, Point position, PointerButton changed_button = PointerButton::None,
                   PointerButton pressed_buttons = PointerButton::None) {
    last_pointer_position_ = position;
    UiWindow::HandlePointerEvent({
        type,
        0,
        position,
        PointerDeviceKind::Mouse,
        changed_button,
        pressed_buttons,
        CurrentKeyModifiers(),
    });
  }

  void CancelPointer() {
    if (!IsInitialized()) {
      return;
    }
    SendPointer(PointerEventType::Cancel, last_pointer_position_);
    pointer_down_ = false;
    if (GetCapture() != nullptr) {
      ReleaseCapture();
    }
  }

  void TrackMouse(MouseTrackingArea area, HWND tracked_window) {
    if (mouse_tracking_area_ == area && mouse_tracking_window_ == tracked_window) {
      return;
    }
    if (mouse_tracking_area_ != MouseTrackingArea::None) {
      TRACKMOUSEEVENT cancellation{
          sizeof(TRACKMOUSEEVENT),
          static_cast<DWORD>(TME_CANCEL | (mouse_tracking_area_ == MouseTrackingArea::NonClient ? TME_NONCLIENT : 0U)),
          mouse_tracking_window_,
          HOVER_DEFAULT,
      };
      HUXERUI_DISCARD(TrackMouseEvent(&cancellation));
    }
    TRACKMOUSEEVENT tracking{
        sizeof(TRACKMOUSEEVENT),
        static_cast<DWORD>(TME_LEAVE | (area == MouseTrackingArea::NonClient ? TME_NONCLIENT : 0U)),
        tracked_window,
        HOVER_DEFAULT,
    };
    mouse_tracking_area_ = TrackMouseEvent(&tracking) != FALSE ? area : MouseTrackingArea::None;
    mouse_tracking_window_ = mouse_tracking_area_ == MouseTrackingArea::None ? nullptr : tracked_window;
  }

  std::optional<LRESULT> HandleClientPointerMessage(HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CANCELMODE:
      CancelPointer();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetFocus(window_);
      SetCapture(source);
      pointer_down_ = true;
      SendPointer(PointerEventType::Down, ClientPoint(source, l_param), SemanticMouseButton(true),
                  MouseButtons(w_param));
      return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
      SetFocus(window_);
      SetCapture(source);
      pointer_down_ = true;
      SendPointer(PointerEventType::Down, ClientPoint(source, l_param), SemanticMouseButton(false),
                  MouseButtons(w_param));
      return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
      SetFocus(window_);
      SetCapture(source);
      pointer_down_ = true;
      SendPointer(PointerEventType::Down, ClientPoint(source, l_param), PointerButton::Middle,
                  MouseButtons(w_param));
      return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK: {
      SetFocus(window_);
      SetCapture(source);
      pointer_down_ = true;
      const PointerButton button = GET_XBUTTON_WPARAM(w_param) == XBUTTON1 ? PointerButton::Back
                                                                           : PointerButton::Forward;
      SendPointer(PointerEventType::Down, ClientPoint(source, l_param), button,
                  MouseButtons(GET_KEYSTATE_WPARAM(w_param)));
      return 0;
    }
    case WM_MOUSEMOVE:
      TrackMouse(MouseTrackingArea::Client, source);
      SendPointer(PointerEventType::Move, ClientPoint(source, l_param), PointerButton::None, MouseButtons(w_param));
      return 0;
    case WM_LBUTTONUP:
      SendPointer(PointerEventType::Up, ClientPoint(source, l_param), SemanticMouseButton(true),
                  MouseButtons(w_param));
      pointer_down_ = MouseButtons(w_param) != PointerButton::None;
      if (!pointer_down_ && GetCapture() == source) {
        ReleaseCapture();
      }
      return 0;
    case WM_RBUTTONUP:
      SendPointer(PointerEventType::Up, ClientPoint(source, l_param), SemanticMouseButton(false),
                  MouseButtons(w_param));
      pointer_down_ = MouseButtons(w_param) != PointerButton::None;
      if (!pointer_down_ && GetCapture() == source) {
        ReleaseCapture();
      }
      return 0;
    case WM_MBUTTONUP:
      SendPointer(PointerEventType::Up, ClientPoint(source, l_param), PointerButton::Middle,
                  MouseButtons(w_param));
      pointer_down_ = MouseButtons(w_param) != PointerButton::None;
      if (!pointer_down_ && GetCapture() == source) {
        ReleaseCapture();
      }
      return 0;
    case WM_XBUTTONUP: {
      const PointerButton button = GET_XBUTTON_WPARAM(w_param) == XBUTTON1 ? PointerButton::Back
                                                                           : PointerButton::Forward;
      const PointerButton buttons = MouseButtons(GET_KEYSTATE_WPARAM(w_param));
      SendPointer(PointerEventType::Up, ClientPoint(source, l_param), button, buttons);
      pointer_down_ = buttons != PointerButton::None;
      if (!pointer_down_ && GetCapture() == source) {
        ReleaseCapture();
      }
      return TRUE;
    }
    case WM_MOUSELEAVE:
      mouse_tracking_area_ = MouseTrackingArea::None;
      mouse_tracking_window_ = nullptr;
      if (!pointer_down_) {
        SendPointer(PointerEventType::Cancel, last_pointer_position_);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (pointer_down_ && reinterpret_cast<HWND>(l_param) != source) {
        SendPointer(PointerEventType::Cancel, last_pointer_position_);
        pointer_down_ = false;
      }
      return 0;
    case WM_MOUSEWHEEL: {
      const float delta =
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA) * 120.0F;
      const Point consumed = UiWindow::HandleScrollInput({ScreenPoint(l_param), 0.0F, -delta, CurrentKeyModifiers()});
      return consumed.x != 0.0F || consumed.y != 0.0F ? std::optional<LRESULT>{0} : std::nullopt;
    }
    case WM_MOUSEHWHEEL: {
      const float delta =
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA) * 120.0F;
      const Point consumed = UiWindow::HandleScrollInput({ScreenPoint(l_param), delta, 0.0F, CurrentKeyModifiers()});
      return consumed.x != 0.0F || consumed.y != 0.0F ? std::optional<LRESULT>{0} : std::nullopt;
    }
    default:
      return std::nullopt;
    }
  }

  LRESULT HandleOverlayMessage(HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
    ExecutionGuard guard(InternalAccess::WindowExecution(*this));
    if (message == WM_NCHITTEST) {
      if (!custom_chrome_) {
        return HTCLIENT;
      }
      if (const std::optional<LRESULT> caption_control = CaptionControlHitTest(l_param)) {
        return *caption_control;
      }
      if (const LRESULT resize = ResizeHitTest(l_param); resize != HTCLIENT) {
        return resize;
      }
      return IsInitialized() && UiWindow::IsWindowDragRegion(ScreenPoint(l_param)) ? HTCAPTION : HTCLIENT;
    }
    if (message == WM_SETCURSOR && LOWORD(l_param) == HTCLIENT) {
      ApplyPointerCursor();
      return TRUE;
    }
    if (const std::optional<LRESULT> handled = HandleClientPointerMessage(source, message, w_param, l_param)) {
      return *handled;
    }
    return DefWindowProcW(source, message, w_param, l_param);
  }

  void ExecuteWindowCommand(WindowCommand command) {
    switch (command) {
    case WindowCommand::Minimize:
      ShowWindow(window_, SW_MINIMIZE);
      break;
    case WindowCommand::Maximize:
      ShowWindow(window_, SW_MAXIMIZE);
      break;
    case WindowCommand::Restore:
      ShowWindow(window_, SW_RESTORE);
      break;
    case WindowCommand::ToggleMaximize:
      ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
      break;
    case WindowCommand::Close:
      performing_close_ = true;
      SendMessageW(window_, WM_CLOSE, 0, 0);
      break;
    case WindowCommand::Show:
      ShowWindow(window_, SW_SHOWNA);
      break;
    case WindowCommand::Hide:
      ShowWindow(window_, SW_HIDE);
      break;
    case WindowCommand::Activate:
      ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(window_);
      break;
    }
  }

  bool SendKey(KeyEventType type, WPARAM virtual_key, LPARAM key_data) {
    return UiWindow::HandleKeyEvent({
        type,
        TranslateKey(virtual_key, key_data),
        type == KeyEventType::Down && !text_input_.Active() ? TranslateKeyText(virtual_key, key_data) : std::string{},
        CurrentKeyModifiers(),
        type == KeyEventType::Down && (static_cast<std::uintptr_t>(key_data) & (1ULL << 30U)) != 0,
    });
  }

  LRESULT HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (!IsInitialized() && message != WM_NCCREATE && message != WM_CREATE && message != WM_DESTROY &&
        message != WM_NCCALCSIZE && message != WM_GETMINMAXINFO && message != WM_NCDESTROY) {
      return message == WM_CLOSE ? 0 : DefWindowProcW(window, message, w_param, l_param);
    }
    if (custom_chrome_ && message == WM_NCCALCSIZE) {
      // User32 must observe the first captioned-window calculation before the client takes ownership of the frame.
      if (first_nc_calc_) {
        first_nc_calc_ = false;
        return DefWindowProcW(window, message, w_param, l_param);
      }
      if (IsZoomed(window)) {
        RECT* client = w_param == FALSE ? reinterpret_cast<RECT*>(l_param)
                                        : &reinterpret_cast<NCCALCSIZE_PARAMS*>(l_param)->rgrc[0];
        *client = InsetWin32MaximizedClientRect(*client, ResizeBorderX(), ResizeBorderY());
      }
      return 0;
    }
    if (const std::optional<LRESULT> handled = HandleClientPointerMessage(window, message, w_param, l_param)) {
      return *handled;
    }
    switch (message) {
    case WM_SETCURSOR:
      if (LOWORD(l_param) == HTCLIENT) {
        ApplyPointerCursor();
        return TRUE;
      }
      break;
    case WM_GETMINMAXINFO: {
      const LRESULT result = DefWindowProcW(window, message, w_param, l_param);
      if (!minimum_size_.has_value()) {
        return result;
      }
      const UINT dpi = std::max(1U, win32_api_.WindowDpi(window));
      SIZE frame_extent{};
      if (!custom_chrome_) {
        RECT frame{};
        if (win32_api_.AdjustWindowRectForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
          frame_extent = {frame.right - frame.left, frame.bottom - frame.top};
        }
      }
      auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
      limits->ptMinTrackSize = ResolveWin32MinimumTrackSize(
          *minimum_size_, static_cast<float>(dpi) / kDipsPerInch, frame_extent, limits->ptMinTrackSize
      );
      return result;
    }
    case WM_NCHITTEST:
      if (custom_chrome_) {
        if (const std::optional<LRESULT> caption_control = CaptionControlHitTest(l_param)) {
          return *caption_control;
        }
        const LRESULT resize = ResizeHitTest(l_param);
        if (resize != HTCLIENT) {
          return resize;
        }
        const Point position = ScreenPoint(l_param);
        return IsInitialized() && UiWindow::IsWindowDragRegion(position) ? HTCAPTION : HTCLIENT;
      }
      break;
    case WM_CREATE:
      dpi_ = static_cast<float>(win32_api_.WindowDpi(window));
      accessibility_.SetDpiScale(DpiScale());
      text_input_.SetWindow(window);
      text_input_.SetDpiScale(DpiScale());
      return 0;
    case WM_DESTROY:
      file_drop_.reset();
      text_input_.SetWindow(nullptr);
      accessibility_.Reset();
      if (platform_views_) {
        platform_views_->Shutdown();
      }
      window_ = nullptr;
      committed_frame_ = nullptr;
      if (IsInitialized()) CloseWindow();
      return 0;
    case WM_CLOSE:
      if (!performing_close_ && IsInitialized() && UiWindow::HandleWindowRequest(WindowCommand::Close)) {
        return 0;
      }
      performing_close_ = false;
      if (IsInitialized()) CloseWindow();
      return 0;
    case WM_SYSCOMMAND:
      if ((w_param & 0xFFF0U) == SC_MINIMIZE && IsInitialized() &&
          UiWindow::HandleWindowRequest(WindowCommand::Minimize)) {
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_ACTIVATE:
      UpdateApplicationLifecycleState(
          IsIconic(window)                 ? ApplicationLifecycleState::Background
          : LOWORD(w_param) == WA_INACTIVE ? ApplicationLifecycleState::Inactive
                                           : ApplicationLifecycleState::Active
      );
      break;
    case WM_SIZE:
      renderer_.Resize(window_, dpi_);
      if (platform_views_) {
        platform_views_->Resize();
      }
      if (w_param == SIZE_MINIMIZED) {
        UpdateApplicationLifecycleState(ApplicationLifecycleState::Background);
        return 0;
      }
      UpdateApplicationLifecycleState(
          GetActiveWindow() == window ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive
      );
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      return 0;
    case WM_DPICHANGED: {
      dpi_ = static_cast<float>(HIWORD(w_param));
      accessibility_.SetDpiScale(DpiScale());
      text_input_.SetDpiScale(DpiScale());
      renderer_.DpiChanged(window_, dpi_);
      const auto* suggested = reinterpret_cast<const RECT*>(l_param);
      SetWindowPos(
          window,
          nullptr,
          suggested->left,
          suggested->top,
          suggested->right - suggested->left,
          suggested->bottom - suggested->top,
          SWP_NOACTIVATE | SWP_NOZORDER
      );
      if (platform_views_) {
        platform_views_->Resize();
      }
      UpdateRuntimeViewport();
      UiWindow::UpdateResourceConfiguration(Configuration());
      RequestFrameAt(Now());
      return 0;
    }
    case WM_THEMECHANGED:
      if (IsInitialized()) UpdateSystemThemeBrightness(QuerySystemThemeBrightness());
      if (custom_chrome_) {
        UpdateRuntimeViewport();
        RequestFrameAt(Now());
        return 0;
      }
      break;
    case WM_SETTINGCHANGE:
      if (IsInitialized()) UpdateSystemThemeBrightness(QuerySystemThemeBrightness());
      UiWindow::UpdateResourceConfiguration(Configuration());
      return 0;
    case WM_DISPLAYCHANGE:
      renderer_.ResetDeviceResources();
      RequestFrameAt(Now());
      return 0;
    case WM_GETOBJECT:
      if (static_cast<LONG>(l_param) == UiaRootObjectId) {
        return accessibility_.HandleGetObject(w_param, l_param);
      }
      break;
    case Win32Accessibility::action_message:
      return accessibility_.HandleActionMessage(l_param);
    case WM_PAINT: {
      if (frame_coordinator_.DeferPaintIfCommitting()) {
        ValidateRect(window, nullptr);
        return 0;
      }
      frame_coordinator_.BeginPaint();
      if (committed_frame_ == nullptr ||
          (frame_coordinator_.FrameBuildPending() && !frame_coordinator_.PaintPending())) {
        CommitFrameAndInvalidate();
      } else {
        UpdateRuntimeViewport();
      }
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      const auto render_result = committed_frame_ != nullptr
          ? renderer_.Render(window_, dpi_, *committed_frame_, paint.rcPaint) : Win32RenderResult::Skipped;
      if (render_result == Win32RenderResult::Retry || render_result == Win32RenderResult::Recreate) {
        InvalidateFullWindow();
      }
      EndPaint(window, &paint);
      if (render_result == Win32RenderResult::Presented && platform_views_) {
        platform_views_->DidPresent();
      }
      if (const std::optional<double> deadline = frame_coordinator_.EndPaint(window_ != nullptr)) {
        ScheduleFrame(*deadline);
      }
      return 0;
    }
    case kRenderMessage:
      render_message_posted_ = false;
      if (frame_coordinator_.FrameBuildPending()) {
        CommitFrameAndInvalidate();
      }
      return 0;
    case kWindowCommandMessage:
      ExecuteWindowCommand(static_cast<WindowCommand>(w_param));
      return 0;
    case WM_TIMER:
      if (w_param == kFrameTimer) {
        KillTimer(window, kFrameTimer);
        timer_armed_ = false;
        timer_deadline_.reset();
        if (frame_coordinator_.FrameBuildPending()) {
          CommitFrameAndInvalidate();
        }
        return 0;
      }
      break;
    case WM_NCMOUSEMOVE:
      if (custom_chrome_) {
        TrackMouse(MouseTrackingArea::NonClient, window);
        SendPointer(PointerEventType::Move, ScreenPoint(l_param));
      }
      return DefWindowProcW(window, message, w_param, l_param);
    case WM_NCMOUSELEAVE:
      if (mouse_tracking_area_ == MouseTrackingArea::NonClient) {
        mouse_tracking_area_ = MouseTrackingArea::None;
        mouse_tracking_window_ = nullptr;
        if (!pointer_down_) {
          SendPointer(PointerEventType::Cancel, last_pointer_position_);
        }
      }
      return DefWindowProcW(window, message, w_param, l_param);
    case WM_NCLBUTTONDOWN:
      if (custom_chrome_ && w_param == HTMAXBUTTON) {
        SetFocus(window);
        SetCapture(window);
        pointer_down_ = true;
        SendPointer(PointerEventType::Down, ScreenPoint(l_param));
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (custom_chrome_ && w_param == HTMAXBUTTON && pointer_down_) {
        SendPointer(PointerEventType::Up, ScreenPoint(l_param));
        pointer_down_ = false;
        if (GetCapture() == window) {
          ReleaseCapture();
        }
        return 0;
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      text_input_.ClearPendingResult();
      if (message == WM_KEYDOWN && text_input_.Active() && (w_param == VK_PROCESSKEY || text_input_.Composing())) {
        return DefWindowProcW(window, message, w_param, l_param);
      }
      if (SendKey(KeyEventType::Down, w_param, l_param)) {
        return 0;
      }
      return DefWindowProcW(window, message, w_param, l_param);
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (message == WM_KEYUP && text_input_.Active() && (w_param == VK_PROCESSKEY || text_input_.Composing())) {
        return DefWindowProcW(window, message, w_param, l_param);
      }
      if (SendKey(KeyEventType::Up, w_param, l_param)) {
        return 0;
      }
      return DefWindowProcW(window, message, w_param, l_param);
    case WM_CHAR:
      return text_input_.CommitCharacter(static_cast<wchar_t>(w_param))
                 ? 0
                 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_STARTCOMPOSITION:
      return text_input_.BeginComposition() ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_COMPOSITION:
      return text_input_.UpdateComposition(l_param) ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_ENDCOMPOSITION:
      return text_input_.EndComposition() ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_CHAR:
      if (text_input_.Active()) {
        HUXERUI_DISCARD(text_input_.SuppressCharacter(static_cast<wchar_t>(w_param)));
        return 0;
      }
      break;
    default:
      break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    WindowsUiWindow* ui_window = reinterpret_cast<WindowsUiWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      ui_window = static_cast<WindowsUiWindow*>(create->lpCreateParams);
      ui_window->window_ = window;
      ui_window->accessibility_.SetWindow(window);
      ui_window->text_input_.SetWindow(window);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui_window));
    }
    if (ui_window == nullptr) {
      return DefWindowProcW(window, message, w_param, l_param);
    }

    ExecutionGuard guard(InternalAccess::WindowExecution(*ui_window));
    try {
      return ui_window->HandleMessage(window, message, w_param, l_param);
    } catch (...) {
      if (ui_window->IsInitialized() && !ui_window->initializing_) {
        static_cast<WindowsRuntime&>(ui_window->ApplicationRuntime()).ReportFailure(std::current_exception());
      } else {
        if (!ui_window->initialization_failure_) ui_window->initialization_failure_ = std::current_exception();
        if (message == WM_CREATE) return -1;
      }
      return 0;
    }
  }

  void ApplyPointerCursor() const {
    SetCursor(LoadCursorW(nullptr, Win32PointerCursorResource(pointer_cursor_kind_)));
  }

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  float dpi_ = kDipsPerInch;
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  std::optional<Size> minimum_size_;
  bool first_nc_calc_ = true;
  bool render_message_posted_ = false;
  bool timer_armed_ = false;
  FrameCoordinator frame_coordinator_;
  MouseTrackingArea mouse_tracking_area_ = MouseTrackingArea::None;
  PointerCursorKind pointer_cursor_kind_ = PointerCursorKind::Default;
  HWND mouse_tracking_window_ = nullptr;
  bool pointer_down_ = false;
  bool performing_close_ = false;
  Point last_pointer_position_;
  Win32Accessibility accessibility_;
  Win32TextInput text_input_;
  std::exception_ptr initialization_failure_;
  bool initializing_ = true;
  std::optional<double> timer_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
  Win32UiThreadDispatcher& ui_dispatcher_;
  std::wstring window_class_name_;
  Win32Api win32_api_;
  Win32SystemTheme system_theme_;
  Win32Renderer renderer_;
  std::unique_ptr<Win32PlatformViews> platform_views_;
  std::unique_ptr<Win32FileDrop> file_drop_;
};

std::unique_ptr<UiWindow> WindowsRuntime::CreateUiWindow(ViewFactory content, const WindowOptions& options) {
  if (!ui_class_registered_) {
    WindowsUiWindow::RegisterWindowClass(ui_window_class_);
    ui_class_registered_ = true;
  }
  auto window = std::make_unique<WindowsUiWindow>(dispatcher_, ui_window_class_);
  window->Initialize(*this, std::move(content), options);
  if (on_window_ready_) {
    auto ready = std::move(on_window_ready_);
    ready(dispatcher_.Bind(), window->Window());
  }
  window->FinishInitialization();
  return window;
}

int WindowsRuntime::Run() {
  MSG message{};
  int result = 0;
  while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    WindowsUiWindow::Dispatch(message, ui_window_class_);
  }
  if (failure_) std::rethrow_exception(failure_);
  if (result < 0) throw std::runtime_error("HuxerUI Windows message loop failed");
  return static_cast<int>(message.wParam);
}

int RunWin32PlatformApplication(const Application& application, Win32WindowReady on_window_ready) {
  WindowOptions options = application.options.window;
  Win32StartupInput startup = CurrentWin32StartupInput();
  const std::wstring window_class_name = Win32ApplicationWindowClassName();
  // Ordinary launches remain independent; only externally supplied URL and file payloads reuse an existing application.
  if (!std::holds_alternative<LaunchActivation>(startup.activation) &&
      TryForwardWin32ApplicationActivation(window_class_name, startup.arguments)) {
    return 0;
  }
  Win32COMApartment com_apartment;
  Win32UiThreadDispatcher ui_dispatcher;
  Win32LocalNotificationHost notifications(ui_dispatcher.Bind());
  std::optional<NotificationActivation> notification;
  if (startup.notification_server) {
    notification = notifications.WaitForActivation();
    if (!notification) {
      return 1;
    }
    const ResolvedLocalNotification resolved{.identifier = notification->identifier,
                                             .data = EncodeLocalNotificationData(notification->data)};
    const std::vector<std::wstring> arguments{std::wstring(win32_notification_payload_flag),
                                              EncodeWin32NotificationActivation(resolved)};
    if (TryForwardWin32ApplicationActivation(window_class_name, arguments)) {
      return 0;
    }
    startup.activation = LaunchActivation{};
  }
  WindowsRuntime application_runtime(ui_dispatcher, notifications.Transport(), std::move(on_window_ready));
  application_runtime.Start(application, std::move(startup.activation));
  notifications.SetActivationHandler(
      [&application_runtime](NotificationActivation activation) { application_runtime.HandleApplicationActivation(std::move(activation)); });
  if (notification) {
    application_runtime.HandleApplicationActivation(std::move(*notification));
  }
  try {
    ui_dispatcher.Start();
    UseApplication().OpenWindow(application.root_factory, options);
    const int result = application_runtime.Run();
    notifications.SetActivationHandler({});
    return result;
  } catch (...) {
    notifications.SetActivationHandler({});
    throw;
  }
}

int RunPlatformApplication(const Application& application) {
  return RunWin32PlatformApplication(application);
}

} // namespace huxerui::detail
