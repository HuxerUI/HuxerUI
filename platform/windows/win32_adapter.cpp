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
#include "win32_internal.h"
#include "win32_renderer.h"
#include "win32_text_input.h"

namespace huxerui::detail {

namespace {

constexpr wchar_t kWindowClassName[] = L"HuxerUI.Win32.Window";
constexpr UINT kRenderMessage = WM_APP + 1;
constexpr UINT_PTR kFrameTimer = 1;
constexpr float kDipsPerInch = 96.0F;

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

private:
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  using SetProcessDpiAwarenessContextFunction = BOOL(WINAPI*)(HANDLE);
  using GetDpiForSystemFunction = UINT(WINAPI*)();
  using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
  using AdjustWindowRectExForDpiFunction = BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT);

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
#endif
};
Key TranslateKey(WPARAM virtual_key) {
  switch (virtual_key) {
  case VK_SHIFT:
  case VK_LSHIFT:
  case VK_RSHIFT:
    return Key::Shift;
  case VK_CONTROL:
  case VK_LCONTROL:
  case VK_RCONTROL:
    return Key::Control;
  case VK_MENU:
  case VK_LMENU:
  case VK_RMENU:
    return Key::Alt;
  case VK_LWIN:
  case VK_RWIN:
    return Key::Meta;
  case VK_TAB:
    return Key::Tab;
  case VK_RETURN:
    return Key::Enter;
  case VK_SPACE:
    return Key::Space;
  case VK_ESCAPE:
    return Key::Escape;
  case VK_BACK:
    return Key::Backspace;
  case VK_DELETE:
    return Key::Delete;
  case VK_LEFT:
    return Key::ArrowLeft;
  case VK_RIGHT:
    return Key::ArrowRight;
  case VK_UP:
    return Key::ArrowUp;
  case VK_DOWN:
    return Key::ArrowDown;
  case VK_HOME:
    return Key::Home;
  case VK_END:
    return Key::End;
  case VK_PRIOR:
    return Key::PageUp;
  case VK_NEXT:
    return Key::PageDown;
  case 'A':
    return Key::A;
  case 'C':
    return Key::C;
  case 'V':
    return Key::V;
  case 'X':
    return Key::X;
  case 'Y':
    return Key::Y;
  case 'Z':
    return Key::Z;
  default:
    return Key::Unknown;
  }
}

KeyModifiers CurrentKeyModifiers() {
  return {
      (GetKeyState(VK_SHIFT) & 0x8000) != 0,
      (GetKeyState(VK_CONTROL) & 0x8000) != 0,
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
} // namespace
class Win32PlatformAdapter final : public huxerui::PlatformAdapter,
                                   public huxerui::PlatformClipboard,
                                   public huxerui::PlatformResources {
public:
  int Run(huxerui::Runtime& runtime, const AppOptions& options) {
    runtime_ = &runtime;
    accessibility_.SetRuntime(runtime_);
    text_input_.SetRuntime(runtime_);
    win32_api_.ConfigureProcessDpiAwareness();

    try {
      const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("HuxerUI could not initialize Windows COM services");
      }
      com_initialized_ = SUCCEEDED(com_result);
      renderer_.Initialize();
      RegisterWindowClass();
      CreateApplicationWindow(options);
      runtime_->UpdateResourceConfiguration(Configuration());

      ShowWindow(window_, SW_SHOW);
      UpdateWindow(window_);

      MSG message{};
      int message_result = 0;
      while ((message_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
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
        .processor_count =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(system_info.dwNumberOfProcessors)),
    };
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
        kWindowClassName,
        nullptr,
    };
    class_atom_ = RegisterClassExW(&window_class);
    if (class_atom_ == 0) {
      throw std::runtime_error("HuxerUI could not register its Windows window class");
    }
  }

  void CreateApplicationWindow(const AppOptions& options) {
    dpi_ = static_cast<float>(win32_api_.SystemDpi());
    const float scale = DpiScale();
    RECT frame{
        0,
        0,
        std::max(1L, static_cast<LONG>(std::lround(options.width * scale))),
        std::max(1L, static_cast<LONG>(std::lround(options.height * scale))),
    };
    const DWORD style = WS_OVERLAPPEDWINDOW;
    if (!win32_api_.AdjustWindowRectForDpi(&frame, style, FALSE, 0, static_cast<UINT>(dpi_))) {
      throw std::runtime_error("HuxerUI could not calculate the Windows window size");
    }

    const std::wstring title = Utf8ToWide(options.title);
    window_ = CreateWindowExW(
        0,
        kWindowClassName,
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
    dpi_ = static_cast<float>(win32_api_.WindowDpi(window_));
    accessibility_.SetDpiScale(DpiScale());
  }

  void Cleanup() noexcept {
    text_input_.Reset();
    committed_frame_ = nullptr;
    if (window_ != nullptr) {
      if (!DestroyWindow(window_)) {
        accessibility_.Reset();
      }
      window_ = nullptr;
    }
    renderer_.Discard();
    if (com_initialized_) {
      CoUninitialize();
      com_initialized_ = false;
    }
    if (class_atom_ != 0 && instance_ != nullptr) {
      UnregisterClassW(kWindowClassName, instance_);
      class_atom_ = 0;
    }
    instance_ = nullptr;
  }

  float DpiScale() const noexcept {
    return std::max(dpi_, 1.0F) / kDipsPerInch;
  }

  void UpdateRuntimeViewport() {
    if (runtime_ == nullptr || window_ == nullptr) {
      return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const float scale = DpiScale();
    runtime_->SetWindowMetrics({
        .viewport = {static_cast<float>(client.right - client.left) / scale,
                     static_cast<float>(client.bottom - client.top) / scale},
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
    accessibility_.Commit(commit.semantic_frame);
    committed_frame_ = &commit.render_frame;
    static_cast<void>(InvalidateDamage(committed_frame_->damage));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  Point ClientPoint(LPARAM position) const noexcept {
    const float scale = DpiScale();
    return {
        static_cast<float>(GET_X_LPARAM(position)) / scale,
        static_cast<float>(GET_Y_LPARAM(position)) / scale,
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

  void SendPointer(PointerEventType type, Point position, std::uint32_t click_count = 1) {
    last_pointer_position_ = position;
    runtime_->HandlePointerEvent({
        type,
        0,
        position,
        PointerDeviceKind::Mouse,
        click_count,
    });
  }

  void CancelPointer() {
    if (runtime_ == nullptr) {
      return;
    }
    SendPointer(PointerEventType::Cancel, last_pointer_position_);
    pointer_down_ = false;
    if (GetCapture() == window_) {
      ReleaseCapture();
    }
  }

  void TrackMouse() {
    if (mouse_tracking_) {
      return;
    }
    TRACKMOUSEEVENT tracking{
        sizeof(TRACKMOUSEEVENT),
        TME_LEAVE,
        window_,
        HOVER_DEFAULT,
    };
    mouse_tracking_ = TrackMouseEvent(&tracking) != FALSE;
  }

  void SendKey(KeyEventType type, WPARAM virtual_key, LPARAM key_data) {
    runtime_->HandleKeyEvent({
        type,
        TranslateKey(virtual_key),
        type == KeyEventType::Down && !text_input_.Active() ? TranslateKeyText(virtual_key, key_data) : std::string{},
        CurrentKeyModifiers(),
        type == KeyEventType::Down && (static_cast<std::uintptr_t>(key_data) & (1ULL << 30U)) != 0,
    });
  }

  LRESULT HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
      dpi_ = static_cast<float>(win32_api_.WindowDpi(window));
      accessibility_.SetDpiScale(DpiScale());
      text_input_.SetWindow(window);
      text_input_.SetDpiScale(DpiScale());
      RequestFrameAt(Now());
      return 0;
    case WM_DESTROY:
      accessibility_.Reset();
      window_ = nullptr;
      text_input_.SetWindow(nullptr);
      committed_frame_ = nullptr;
      PostQuitMessage(0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      renderer_.Resize(window_, dpi_);
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
      if (renderer_.Render(window_, dpi_, *committed_frame_, paint.rcPaint) == Win32RenderResult::Recreate) {
        InvalidateFullWindow();
      }
      EndPaint(window, &paint);
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
    case WM_LBUTTONDOWN:
      SetFocus(window);
      SetCapture(window);
      pointer_down_ = true;
      SendPointer(PointerEventType::Down, ClientPoint(l_param));
      return 0;
    case WM_LBUTTONDBLCLK:
      SetFocus(window);
      SetCapture(window);
      pointer_down_ = true;
      SendPointer(PointerEventType::Down, ClientPoint(l_param), 2);
      return 0;
    case WM_MOUSEMOVE:
      TrackMouse();
      SendPointer(PointerEventType::Move, ClientPoint(l_param));
      return 0;
    case WM_LBUTTONUP:
      SendPointer(PointerEventType::Up, ClientPoint(l_param));
      pointer_down_ = false;
      if (GetCapture() == window) {
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSELEAVE:
      mouse_tracking_ = false;
      if (!pointer_down_) {
        SendPointer(PointerEventType::Cancel, last_pointer_position_);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (pointer_down_ && reinterpret_cast<HWND>(l_param) != window) {
        SendPointer(PointerEventType::Cancel, last_pointer_position_);
        pointer_down_ = false;
      }
      return 0;
    case WM_CANCELMODE:
      CancelPointer();
      return 0;
    case WM_MOUSEWHEEL: {
      const float delta =
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA) * 120.0F;
      runtime_->HandleScrollEvent({
          ScreenPoint(l_param),
          0.0F,
          -delta,
      });
      return 0;
    }
    case WM_MOUSEHWHEEL: {
      const float delta =
          static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / static_cast<float>(WHEEL_DELTA) * 120.0F;
      runtime_->HandleScrollEvent({
          ScreenPoint(l_param),
          delta,
          0.0F,
      });
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      text_input_.ClearPendingResult();
      if (message == WM_KEYDOWN && text_input_.Active() &&
          (w_param == VK_PROCESSKEY || text_input_.Composing() || w_param == VK_SPACE)) {
        return w_param == VK_SPACE ? 0 : DefWindowProcW(window, message, w_param, l_param);
      }
      SendKey(KeyEventType::Down, w_param, l_param);
      return message == WM_SYSKEYDOWN ? DefWindowProcW(window, message, w_param, l_param) : 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (message == WM_KEYUP && text_input_.Active() &&
          (w_param == VK_PROCESSKEY || text_input_.Composing() || w_param == VK_SPACE)) {
        return w_param == VK_SPACE ? 0 : DefWindowProcW(window, message, w_param, l_param);
      }
      SendKey(KeyEventType::Up, w_param, l_param);
      return message == WM_SYSKEYUP ? DefWindowProcW(window, message, w_param, l_param) : 0;
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

  huxerui::Runtime* runtime_ = nullptr;
  HINSTANCE instance_ = nullptr;
  ATOM class_atom_ = 0;
  HWND window_ = nullptr;
  float dpi_ = kDipsPerInch;
  bool render_message_posted_ = false;
  bool timer_armed_ = false;
  PlatformFrameState frame_state_;
  bool mouse_tracking_ = false;
  bool pointer_down_ = false;
  bool com_initialized_ = false;
  Point last_pointer_position_;
  Win32Accessibility accessibility_;
  Win32TextInput text_input_;
  std::exception_ptr failure_;
  std::optional<double> timer_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
  Win32Api win32_api_;
  Win32Renderer renderer_;
};

int RunPlatformApp(AppDefinition definition) {
  AppOptions options = definition.options;
  Win32PlatformAdapter platform;
  Runtime runtime{std::move(definition), platform};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail
