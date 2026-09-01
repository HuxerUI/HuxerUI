#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <huxerui/app.h>

#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"
#include "win32_accessibility.h"
#include "win32_application_internal.h"
#include "win32_file_internal.h"
#include "win32_http_internal.h"
#include "win32_internal.h"
#include "win32_platform_view.h"
#include "win32_renderer.h"
#include "win32_system_tray.h"
#include "win32_text_input.h"
#include "win32_ui_dispatcher.h"
#include "window_internal.h"

namespace huxerui::detail {

namespace {

constexpr UINT kRenderMessage = WM_APP + 1;
constexpr UINT kWindowCommandMessage = WM_APP + 3;
constexpr UINT_PTR kFrameTimer = 1;
constexpr float kDipsPerInch = 96.0F;

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
      static_cast<void>(set_process_dpi_awareness_context_(reinterpret_cast<HANDLE>(per_monitor_aware_v2)));
      return;
    }
    static_cast<void>(SetProcessDPIAware());
#else
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
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
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
      throw std::runtime_error("HuxerUI could not initialize Windows COM services");
    }
    initialized_ = SUCCEEDED(result);
  }

  ~Win32COMApartment() {
    if (initialized_) {
      CoUninitialize();
    }
  }

  Win32COMApartment(const Win32COMApartment&) = delete;
  Win32COMApartment& operator=(const Win32COMApartment&) = delete;

private:
  bool initialized_ = false;
};

} // namespace

class Win32PlatformAdapter final : public huxerui::PlatformAdapter,
                                   public huxerui::PlatformClipboard,
                                   public huxerui::PlatformResources {
public:
  Win32PlatformAdapter(Win32UIThreadDispatcher& ui_dispatcher, std::wstring window_class_name)
      : PlatformAdapter(ui_dispatcher.Bind()), ui_dispatcher_(ui_dispatcher),
        window_class_name_(std::move(window_class_name)) {
    win32_api_.ConfigureProcessDpiAwareness();
  }

  int Run(huxerui::Runtime& runtime, const WindowOptions& options) {
    runtime_ = &runtime;
    accessibility_.SetRuntime(runtime_);
    text_input_.SetRuntime(runtime_);

    try {
      renderer_.Initialize();
      RegisterWindowClass();
      CreateApplicationWindow(options);
      runtime_->UpdateResourceConfiguration(Configuration());

      ShowWindow(window_, SW_SHOW);
      UpdateApplicationLifecycleState(
          GetActiveWindow() == window_ ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive
      );
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      UpdateWindow(window_);

      MSG message{};
      int message_result = 0;
      while ((message_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (platform_views_ && platform_views_->HandleFocusTraversal(message)) {
          platform_views_->SynchronizeFocus(GetFocus());
          continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (platform_views_) {
          platform_views_->SynchronizeFocus(GetFocus());
        }
      }
      if (message_result < 0 && !failure_) {
        failure_ = std::make_exception_ptr(std::runtime_error("HuxerUI Windows message loop failed"));
      }

      const int exit_code = static_cast<int>(message.wParam);
      Cleanup();
      runtime_ = nullptr;
      if (failure_) {
        std::rethrow_exception(failure_);
      }
      return exit_code;
    } catch (...) {
      Cleanup();
      runtime_ = nullptr;
      throw;
    }
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), window_ != nullptr)) {
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

  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return CreateWin32FileSystem();
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateWin32FilePickerTransport([this] { return window_; }, ui_dispatcher_.Bind());
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateWin32HttpTransport();
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override {
    return CreateWin32PermissionTransport();
  }

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

  void RequestWindowCommand(WindowCommand command) override {
    if (window_ == nullptr) {
      return;
    }
    PostMessageW(window_, kWindowCommandMessage, static_cast<WPARAM>(command), 0);
  }

  void RequestApplicationQuit() override {
    if (window_ != nullptr) {
      performing_close_ = true;
      SendMessageW(window_, WM_CLOSE, 0, 0);
    }
  }

  ResourceConfiguration Configuration() const override {
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    Locale locale = Locale::Default();
    if (GetUserDefaultLocaleName(locale_name, static_cast<int>(std::size(locale_name))) > 0) {
      locale = Locale::FromLanguageTag(WideToUtf8(locale_name));
    }
    const UINT dpi = window_ != nullptr ? win32_api_.WindowDpi(window_) : win32_api_.SystemDpi();
    return {std::move(locale), static_cast<float>(dpi) / kDipsPerInch};
  }

  RawAsset Read(std::string_view package_path) override {
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
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return {};
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
      throw std::logic_error("HuxerUI Windows resource size is invalid: " + WideToUtf8(path.native()));
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
      throw std::logic_error("HuxerUI Windows resource could not be read: " + WideToUtf8(path.native()));
    }
    return RawAsset::FromBytes(std::move(bytes));
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

private:
  void UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state) {
    if (runtime_ != nullptr) {
      runtime_->UpdateApplicationLifecycleState(lifecycle_state);
    }
  }

  void RegisterWindowClass() {
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW),
        CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
        &Win32PlatformAdapter::WindowProcedure,
        0,
        0,
        instance_,
        nullptr,
        LoadCursorW(nullptr, IDC_ARROW),
        nullptr,
        nullptr,
        window_class_name_.c_str(),
        nullptr,
    };
    class_atom_ = RegisterClassExW(&window_class);
    if (class_atom_ == 0) {
      throw std::runtime_error("HuxerUI could not register its Windows window class");
    }
  }

  void CreateApplicationWindow(const WindowOptions& options) {
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
    if (window_ == nullptr) {
      throw std::runtime_error("HuxerUI could not create its Windows application window");
    }
    platform_views_ =
        std::make_unique<Win32PlatformViews>(instance_, window_, PlatformRegistry(), *runtime_,
                                             [this](HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
                                               return HandleOverlayMessage(source, message, w_param, l_param);
                                             });
    ui_dispatcher_.Attach(window_);
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
    }
    dpi_ = static_cast<float>(win32_api_.WindowDpi(window_));
    accessibility_.SetDpiScale(DpiScale());
  }

  void Cleanup() noexcept {
    ui_dispatcher_.Shutdown();
    text_input_.Reset();
    committed_frame_ = nullptr;
    accessibility_.Reset();
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    if (window_ != nullptr) {
      static_cast<void>(DestroyWindow(window_));
      window_ = nullptr;
    }
    renderer_.Discard();
    if (class_atom_ != 0 && instance_ != nullptr) {
      UnregisterClassW(window_class_name_.c_str(), instance_);
      class_atom_ = 0;
    }
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
    if (runtime_ == nullptr || window_ == nullptr) {
      return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const float scale = DpiScale();
    const Size viewport{
        static_cast<float>(client.right - client.left) / scale,
        static_cast<float>(client.bottom - client.top) / scale,
    };
    runtime_->SetWindowMetrics({
        .viewport = viewport,
        .title_bar = QueryTitleBarMetrics(viewport),
    });
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(window_ != nullptr)) {
      ScheduleFrame(*deadline);
    }
  }

  bool InvalidateFullWindow() {
    const bool invalidated = window_ != nullptr && InvalidateRect(window_, nullptr, FALSE) != FALSE;
    if (invalidated) {
      frame_state_.MarkPaintPending();
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
      frame_state_.MarkPaintPending();
    }
    return invalidated;
  }

  void CommitFrameAndInvalidate() {
    if (!frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    committed_frame_ = &commit.render_frame;
    const bool has_platform_views = platform_views_ && platform_views_->Commit(*committed_frame_, DpiScale());
    const bool composition_changed = has_platform_views && renderer_.EnablePlatformComposition(window_);
    accessibility_.Commit(commit.semantic_frame, has_platform_views ? platform_views_.get() : nullptr);
    if (composition_changed) {
      static_cast<void>(InvalidateFullWindow());
    }
    static_cast<void>(InvalidateDamage(committed_frame_->damage));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
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

  void SendPointer(PointerEventType type, Point position, PointerButton changed_button = PointerButton::None,
                   PointerButton pressed_buttons = PointerButton::None) {
    last_pointer_position_ = position;
    runtime_->HandlePointerEvent({
        type,
        0,
        position,
        PointerDeviceKind::Mouse,
        changed_button,
        pressed_buttons,
    });
  }

  void CancelPointer() {
    if (runtime_ == nullptr) {
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
      static_cast<void>(TrackMouseEvent(&cancellation));
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
      runtime_->HandleScrollEvent({ScreenPoint(l_param), 0.0F, -delta});
      return 0;
    }
    case WM_MOUSEHWHEEL: {
      const float delta =
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA) * 120.0F;
      runtime_->HandleScrollEvent({ScreenPoint(l_param), delta, 0.0F});
      return 0;
    }
    default:
      return std::nullopt;
    }
  }

  LRESULT HandleOverlayMessage(HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
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
      return runtime_ != nullptr && runtime_->IsWindowDragRegion(ScreenPoint(l_param)) ? HTCAPTION : HTCLIENT;
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
    return runtime_->HandleKeyEvent({
        type,
        TranslateKey(virtual_key, key_data),
        type == KeyEventType::Down && !text_input_.Active() ? TranslateKeyText(virtual_key, key_data) : std::string{},
        CurrentKeyModifiers(),
        type == KeyEventType::Down && (static_cast<std::uintptr_t>(key_data) & (1ULL << 30U)) != 0,
    });
  }

  LRESULT HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (system_tray_) {
      if (const std::optional<LRESULT> handled = system_tray_->HandleMessage(message, w_param, l_param)) {
        return *handled;
      }
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
        return runtime_ != nullptr && runtime_->IsWindowDragRegion(position) ? HTCAPTION : HTCLIENT;
      }
      break;
    case WM_CREATE:
      dpi_ = static_cast<float>(win32_api_.WindowDpi(window));
      accessibility_.SetDpiScale(DpiScale());
      text_input_.SetWindow(window);
      text_input_.SetDpiScale(DpiScale());
      return 0;
    case WM_DESTROY:
      if (system_tray_) {
        system_tray_->SetWindow(nullptr);
      }
      ui_dispatcher_.Shutdown();
      text_input_.SetWindow(nullptr);
      accessibility_.Reset();
      if (platform_views_) {
        platform_views_->Shutdown();
      }
      window_ = nullptr;
      committed_frame_ = nullptr;
      PostQuitMessage(0);
      return 0;
    case WM_CLOSE:
      if (performing_close_) {
        performing_close_ = false;
        break;
      }
      if (runtime_ != nullptr && runtime_->HandleWindowRequest(WindowCommand::Close)) {
        return 0;
      }
      break;
    case WM_SYSCOMMAND:
      if ((w_param & 0xFFF0U) == SC_MINIMIZE && runtime_ != nullptr &&
          runtime_->HandleWindowRequest(WindowCommand::Minimize)) {
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
      UpdateRuntimeViewport();
      runtime_->UpdateResourceConfiguration(Configuration());
      RequestFrameAt(Now());
      return 0;
    }
    case WM_THEMECHANGED:
      if (custom_chrome_) {
        UpdateRuntimeViewport();
        RequestFrameAt(Now());
        return 0;
      }
      break;
    case WM_SETTINGCHANGE:
      runtime_->UpdateResourceConfiguration(Configuration());
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
    case WM_COPYDATA: {
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(l_param);
      if (data == nullptr || data->dwData != win32_application_activation_data_id || data->lpData == nullptr ||
          data->cbData % sizeof(wchar_t) != 0 ||
          data->cbData / sizeof(wchar_t) > win32_application_activation_max_characters) {
        break;
      }
      const auto* characters = static_cast<const wchar_t*>(data->lpData);
      const std::span<const wchar_t> payload(characters, data->cbData / sizeof(wchar_t));
      std::optional<ApplicationActivation> activation = DecodeWin32ApplicationActivation(payload);
      if (!activation.has_value()) {
        return FALSE;
      }
      runtime_->HandleApplicationActivation(std::move(*activation));
      return TRUE;
    }
    case Win32Accessibility::action_message:
      return accessibility_.HandleActionMessage(l_param);
    case WM_PAINT: {
      frame_state_.BeginPaint();
      if (committed_frame_ == nullptr || (frame_state_.FrameBuildPending() && !frame_state_.PaintPending())) {
        CommitFrameAndInvalidate();
      } else {
        UpdateRuntimeViewport();
      }
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      const Win32RenderResult render_result = renderer_.Render(window_, dpi_, *committed_frame_, paint.rcPaint);
      if (render_result == Win32RenderResult::Recreate) {
        InvalidateFullWindow();
      }
      EndPaint(window, &paint);
      if (render_result == Win32RenderResult::Presented && platform_views_) {
        platform_views_->DidPresent();
      }
      if (const std::optional<double> deadline = frame_state_.EndPaint(window_ != nullptr)) {
        ScheduleFrame(*deadline);
      }
      return 0;
    }
    case kRenderMessage:
      render_message_posted_ = false;
      if (frame_state_.FrameBuildPending()) {
        CommitFrameAndInvalidate();
      }
      return 0;
    case Win32UIThreadDispatcher::task_message:
      ui_dispatcher_.RunPending();
      return 0;
    case kWindowCommandMessage:
      ExecuteWindowCommand(static_cast<WindowCommand>(w_param));
      return 0;
    case WM_TIMER:
      if (w_param == kFrameTimer) {
        KillTimer(window, kFrameTimer);
        timer_armed_ = false;
        timer_deadline_.reset();
        if (frame_state_.FrameBuildPending()) {
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
        static_cast<void>(text_input_.SuppressCharacter(static_cast<wchar_t>(w_param)));
        return 0;
      }
      break;
    default:
      break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    Win32PlatformAdapter* adapter = reinterpret_cast<Win32PlatformAdapter*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      adapter = static_cast<Win32PlatformAdapter*>(create->lpCreateParams);
      adapter->window_ = window;
      if (adapter->system_tray_) {
        adapter->system_tray_->SetWindow(window);
      }
      adapter->accessibility_.SetWindow(window);
      adapter->text_input_.SetWindow(window);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(adapter));
    }
    if (adapter == nullptr) {
      return DefWindowProcW(window, message, w_param, l_param);
    }

    try {
      return adapter->HandleMessage(window, message, w_param, l_param);
    } catch (...) {
      if (!adapter->failure_) {
        adapter->failure_ = std::current_exception();
      }
      PostQuitMessage(1);
      return 0;
    }
  }

  void ApplyPointerCursor() const {
    SetCursor(LoadCursorW(nullptr, Win32PointerCursorResource(pointer_cursor_kind_)));
  }

  huxerui::Runtime* runtime_ = nullptr;
  HINSTANCE instance_ = nullptr;
  ATOM class_atom_ = 0;
  HWND window_ = nullptr;
  float dpi_ = kDipsPerInch;
  bool custom_chrome_ = false;
  float custom_title_bar_height_ = 0.0F;
  std::optional<Size> minimum_size_;
  bool first_nc_calc_ = true;
  bool render_message_posted_ = false;
  bool timer_armed_ = false;
  PlatformFrameState frame_state_;
  MouseTrackingArea mouse_tracking_area_ = MouseTrackingArea::None;
  PointerCursorKind pointer_cursor_kind_ = PointerCursorKind::Default;
  HWND mouse_tracking_window_ = nullptr;
  bool pointer_down_ = false;
  bool performing_close_ = false;
  Point last_pointer_position_;
  Win32Accessibility accessibility_;
  Win32TextInput text_input_;
  std::exception_ptr failure_;
  std::optional<double> timer_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
  Win32UIThreadDispatcher& ui_dispatcher_;
  std::wstring window_class_name_;
  Win32Api win32_api_;
  Win32Renderer renderer_;
  std::unique_ptr<Win32PlatformViews> platform_views_;
  std::shared_ptr<Win32SystemTrayTransport> system_tray_;
};

int RunPlatformApplication(const Application& application) {
  WindowOptions options = application.options.window;
  Win32StartupInput startup = CurrentWin32StartupInput();
  const std::wstring window_class_name = Win32ApplicationWindowClassName();
  // Ordinary launches remain independent; only externally supplied URL and file payloads reuse an existing window.
  if (!std::holds_alternative<LaunchActivation>(startup.activation) &&
      TryForwardWin32ApplicationActivation(window_class_name, startup.arguments)) {
    return 0;
  }
  Win32COMApartment com_apartment;
  Win32UIThreadDispatcher ui_dispatcher;
  Win32PlatformAdapter platform(ui_dispatcher, window_class_name);
  Runtime runtime{application, platform, std::move(startup.activation)};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail
