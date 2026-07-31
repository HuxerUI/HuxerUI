#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <imm.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "internal.h"
#include "text_input_internal.h"
#include "win32_damage_internal.h"

namespace huxerui::detail {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"HuxerUI.Win32.Window";
constexpr UINT kRenderMessage = WM_APP + 1;
constexpr UINT_PTR kFrameTimer = 1;
constexpr float kDipsPerInch = 96.0F;
constexpr float kFullCircle = 6.28318530717958647692F;

void ThrowIfFailed(HRESULT result, const char* message) {
  if (FAILED(result)) {
    throw std::runtime_error(message);
  }
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int input_size =
      static_cast<int>(std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int output_size = MultiByteToWideChar(CP_UTF8, 0, text.data(), input_size, nullptr, 0);
  if (output_size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(output_size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), input_size, result.data(), output_size);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int input_size =
      static_cast<int>(std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int output_size = WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size, nullptr, 0, nullptr, nullptr);
  if (output_size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(output_size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size, result.data(), output_size, nullptr, nullptr);
  return result;
}

D2D1_COLOR_F ToD2DColor(Color color) {
  return D2D1::ColorF(color.red, color.green, color.blue, color.alpha);
}

D2D1_RECT_F ToD2DRect(Rect rect) {
  return D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
}

Key TranslateKey(WPARAM virtual_key) {
  switch (virtual_key) {
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

D2D1_CAP_STYLE ToD2DCap(StrokeCap cap) {
  switch (cap) {
  case StrokeCap::Round:
    return D2D1_CAP_STYLE_ROUND;
  case StrokeCap::Square:
    return D2D1_CAP_STYLE_SQUARE;
  case StrokeCap::Butt:
  default:
    return D2D1_CAP_STYLE_FLAT;
  }
}

} // namespace

class Win32TextLayout final : public TextLayout {
public:
  Win32TextLayout(std::wstring text, ComPtr<IDWriteTextLayout> layout)
      : text_(std::move(text)), layout_(std::move(layout)) {
    UINT32 count = 0;
    layout_->GetClusterMetrics(nullptr, 0, &count);
    cluster_metrics_.resize(count);
    if (count > 0) {
      ThrowIfFailed(
          layout_->GetClusterMetrics(cluster_metrics_.data(), count, &count),
          "HuxerUI could not query DirectWrite text clusters"
      );
      cluster_metrics_.resize(count);
    }
  }

  Size Measure() const override {
    DWRITE_TEXT_METRICS metrics{};
    ThrowIfFailed(layout_->GetMetrics(&metrics), "HuxerUI could not measure a DirectWrite text layout");
    return {
        std::ceil(metrics.widthIncludingTrailingWhitespace),
        std::ceil(metrics.height),
    };
  }

  TextPosition HitTest(Point point) const override {
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    ThrowIfFailed(
        layout_->HitTestPoint(point.x, point.y, &trailing, &inside, &metrics),
        "HuxerUI could not hit test a DirectWrite text layout"
    );
    static_cast<void>(inside);
    const TextOffset offset = std::min<TextOffset>(
        static_cast<TextOffset>(text_.size()),
        static_cast<TextOffset>(metrics.textPosition) + (trailing != FALSE ? metrics.length : 0)
    );
    return {
        offset,
        trailing != FALSE ? TextAffinity::Upstream : TextAffinity::Downstream,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    UINT32 position = static_cast<UINT32>(std::clamp<TextOffset>(offset, 0, text_.size()));
    const BOOL trailing = affinity == TextAffinity::Upstream && position > 0;
    if (trailing != FALSE) {
      --position;
    }
    DWRITE_HIT_TEST_METRICS metrics{};
    float x = 0.0F;
    float y = 0.0F;
    ThrowIfFailed(
        layout_->HitTestTextPosition(position, trailing, &x, &y, &metrics),
        "HuxerUI could not locate a DirectWrite text caret"
    );
    return {
        x,
        y,
        1.0F,
        metrics.height,
    };
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const TextOffset start = std::clamp<TextOffset>(range.start, 0, text_.size());
    const TextOffset end = std::clamp<TextOffset>(range.end, start, text_.size());
    if (start == end) {
      return {};
    }

    std::vector<DWRITE_HIT_TEST_METRICS> metrics(text_.size() + 1);
    UINT32 count = 0;
    const HRESULT result = layout_->HitTestTextRange(
        static_cast<UINT32>(start),
        static_cast<UINT32>(end - start),
        0.0F,
        0.0F,
        metrics.data(),
        static_cast<UINT32>(metrics.size()),
        &count
    );
    ThrowIfFailed(result, "HuxerUI could not locate a DirectWrite text range");

    std::vector<Rect> rects;
    rects.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
      rects.push_back({
          metrics[index].left,
          metrics[index].top,
          metrics[index].width,
          metrics[index].height,
      });
    }
    return rects;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const TextOffset target = std::clamp<TextOffset>(offset, 0, text_.size());
    TextOffset position = 0;
    for (const DWRITE_CLUSTER_METRICS& cluster : cluster_metrics_) {
      const TextOffset end = position + cluster.length;
      if (target <= end) {
        return position;
      }
      position = end;
    }
    return position;
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const TextOffset target = std::clamp<TextOffset>(offset, 0, text_.size());
    TextOffset position = 0;
    for (const DWRITE_CLUSTER_METRICS& cluster : cluster_metrics_) {
      position += cluster.length;
      if (target < position) {
        return position;
      }
    }
    return position;
  }

private:
  std::wstring text_;
  ComPtr<IDWriteTextLayout> layout_;
  std::vector<DWRITE_CLUSTER_METRICS> cluster_metrics_;
};

class Win32PlatformHost final : public huxerui::PlatformHost,
                                public huxerui::PlatformTextInput,
                                public huxerui::PlatformClipboard {
public:
  int Run(huxerui::Runtime& runtime, const AppOptions& options) {
    runtime_ = &runtime;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try {
      InitializeFactories();
      RegisterWindowClass();
      CreateApplicationWindow(options);

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
    frame_build_pending_ = true;
    const double now = Now();
    if (std::isnan(deadline) || deadline <= now) {
      deadline = now;
    } else if (!std::isfinite(deadline)) {
      deadline = std::numeric_limits<double>::max();
    }
    if (paint_pending_ || paint_in_progress_ || window_ == nullptr) {
      if (!deferred_frame_deadline_.has_value() || deadline < *deferred_frame_deadline_) {
        deferred_frame_deadline_ = deadline;
      }
      return;
    }

    ScheduleFrame(deadline);
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

  Size MeasureText(std::string_view text, float font_size, float max_width) override {
    const std::wstring wide = Utf8ToWide(text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(font_size);
    const bool constrained = std::isfinite(max_width);
    if (constrained && max_width <= 0.0F) {
      return {};
    }
    format->SetWordWrapping(constrained ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

    const float layout_width = constrained ? max_width : std::numeric_limits<float>::max();
    ComPtr<IDWriteTextLayout> layout;
    ThrowIfFailed(
        write_factory_->CreateTextLayout(
            wide.data(),
            static_cast<UINT32>(wide.size()),
            format.Get(),
            layout_width,
            std::numeric_limits<float>::max(),
            layout.GetAddressOf()
        ),
        "HuxerUI could not create a DirectWrite text layout"
    );

    DWRITE_TEXT_METRICS metrics{};
    ThrowIfFailed(layout->GetMetrics(&metrics), "HuxerUI could not measure a DirectWrite text layout");
    const float width = constrained ? std::min(metrics.widthIncludingTrailingWhitespace, max_width)
                                    : metrics.widthIncludingTrailingWhitespace;
    return {
        std::ceil(width),
        std::ceil(metrics.height),
    };
  }

  std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, float font_size, float max_width) override {
    std::wstring wide = Utf8ToWide(text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(font_size);
    const bool constrained = std::isfinite(max_width);
    format->SetWordWrapping(constrained ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteTextLayout> layout;
    ThrowIfFailed(
        write_factory_->CreateTextLayout(
            wide.data(),
            static_cast<UINT32>(wide.size()),
            format.Get(),
            constrained ? std::max(1.0F, max_width) : std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            layout.GetAddressOf()
        ),
        "HuxerUI could not create an editable DirectWrite text layout"
    );
    return std::make_unique<Win32TextLayout>(std::move(wide), std::move(layout));
  }

  PlatformTextInput* TextInput() noexcept override {
    return this;
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
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

  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    static_cast<void>(configuration);
    text_input_session_id_ = session_id;
    text_input_state_ = state;
    ime_composing_ = state.composition.has_value();
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    UpdateImePosition(geometry);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    if (session_id != text_input_session_id_) {
      return;
    }
    text_input_state_ = state;
    ime_composing_ = state.composition.has_value();
    UpdateImePosition(geometry);
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    static_cast<void>(configuration);
    CancelNativeComposition();
    text_input_session_id_ = session_id;
    text_input_state_ = state;
    ime_composing_ = state.composition.has_value();
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    UpdateImePosition(geometry);
  }

  void Stop(TextInputSessionId session_id) override {
    if (session_id != text_input_session_id_) {
      return;
    }
    text_input_session_id_ = 0;
    text_input_state_ = {};
    ime_composing_ = false;
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    CancelNativeComposition();
  }

private:
  struct ClipState {
    bool uses_layer = false;
    ComPtr<ID2D1Layer> layer;
    ComPtr<ID2D1Geometry> geometry;
  };

  void InitializeFactories() {
    ThrowIfFailed(
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf()),
        "HuxerUI could not create a Direct2D factory"
    );
    ThrowIfFailed(
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf())
        ),
        "HuxerUI could not create a DirectWrite factory"
    );
  }

  void RegisterWindowClass() {
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW),
        CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
        &Win32PlatformHost::WindowProcedure,
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
    dpi_ = static_cast<float>(GetDpiForSystem());
    const float scale = DpiScale();
    RECT frame{
        0,
        0,
        std::max(1L, static_cast<LONG>(std::lround(options.width * scale))),
        std::max(1L, static_cast<LONG>(std::lround(options.height * scale))),
    };
    const DWORD style = WS_OVERLAPPEDWINDOW;
    if (!AdjustWindowRectExForDpi(&frame, style, FALSE, 0, static_cast<UINT>(dpi_))) {
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
    dpi_ = static_cast<float>(GetDpiForWindow(window_));
  }

  void Cleanup() noexcept {
    text_input_session_id_ = 0;
    ime_composing_ = false;
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    committed_frame_ = nullptr;
    if (window_ != nullptr) {
      DestroyWindow(window_);
      window_ = nullptr;
    }
    DiscardDeviceResources();
    if (class_atom_ != 0 && instance_ != nullptr) {
      UnregisterClassW(kWindowClassName, instance_);
      class_atom_ = 0;
    }
    write_factory_.Reset();
    d2d_factory_.Reset();
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
    runtime_->SetViewport({
        static_cast<float>(client.right - client.left) / scale,
        static_cast<float>(client.bottom - client.top) / scale,
    });
  }

  void FlushDeferredFrame() {
    if (paint_pending_ || paint_in_progress_ || !frame_build_pending_ || !deferred_frame_deadline_.has_value() ||
        window_ == nullptr) {
      return;
    }
    const double deadline = *deferred_frame_deadline_;
    deferred_frame_deadline_.reset();
    ScheduleFrame(deadline);
  }

  bool InvalidateFullWindow() {
    const bool invalidated = window_ != nullptr && InvalidateRect(window_, nullptr, FALSE) != FALSE;
    paint_pending_ = paint_pending_ || invalidated;
    return invalidated;
  }

  bool InvalidateDamage(const DamageRegion& damage) {
    RECT client{};
    GetClientRect(window_, &client);
    if (force_full_repaint_) {
      return InvalidateFullWindow();
    }

    const Win32DamageRegion resolved = ResolveWin32Damage(damage, DpiScale(), client);
    if (resolved.full) {
      return InvalidateFullWindow();
    }
    bool invalidated = false;
    for (const RECT& rect : resolved.rects) {
      invalidated = InvalidateRect(window_, &rect, FALSE) != FALSE || invalidated;
    }
    paint_pending_ = paint_pending_ || invalidated;
    return invalidated;
  }

  void CommitFrameAndInvalidate() {
    frame_build_pending_ = false;
    deferred_frame_deadline_.reset();
    const FrameCommit& commit = runtime_->BuildFrame();
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

  ComPtr<IDWriteTextFormat> CreateTextFormat(float font_size) const {
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale_name, static_cast<int>(std::size(locale_name))) == 0) {
      wcscpy_s(locale_name, L"en-us");
    }

    ComPtr<IDWriteTextFormat> format;
    ThrowIfFailed(
        write_factory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            std::max(font_size, 0.1F),
            locale_name,
            format.GetAddressOf()
        ),
        "HuxerUI could not create a DirectWrite text format"
    );
    return format;
  }

  bool EnsureRenderTarget() {
    if (render_target_) {
      return true;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = static_cast<UINT>(std::max(0L, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max(0L, client.bottom - client.top));
    if (width == 0 || height == 0) {
      return false;
    }

    const HRESULT target_result = d2d_factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(width, height)),
        render_target_.GetAddressOf()
    );
    if (FAILED(target_result)) {
      return false;
    }
    render_target_->SetDpi(dpi_, dpi_);
    if (FAILED(render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush_.GetAddressOf()))) {
      DiscardDeviceResources();
      return false;
    }
    force_full_repaint_ = true;
    return true;
  }

  void DiscardDeviceResources() noexcept {
    clip_stack_.clear();
    transform_stack_.clear();
    brush_.Reset();
    render_target_.Reset();
    force_full_repaint_ = true;
  }

  void ResizeRenderTarget() {
    force_full_repaint_ = true;
    if (!render_target_) {
      return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = static_cast<UINT>(std::max(0L, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max(0L, client.bottom - client.top));
    if (FAILED(render_target_->Resize(D2D1::SizeU(width, height)))) {
      DiscardDeviceResources();
    }
  }

  void Render(const RenderFrame& frame, const RECT& paint_rect) {
    if (paint_rect.left >= paint_rect.right || paint_rect.top >= paint_rect.bottom) {
      return;
    }
    if (!EnsureRenderTarget()) {
      return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const Rect paint_bounds = Win32PixelRectToDips(paint_rect, DpiScale());
    const Rect client_bounds = Win32PixelRectToDips(client, DpiScale());
    clip_stack_.clear();
    transform_stack_.clear();
    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->PushAxisAlignedClip(ToD2DRect(paint_bounds), D2D1_ANTIALIAS_MODE_ALIASED);
    SetBrushColor(Color::Rgb(247, 248, 250));
    render_target_->FillRectangle(ToD2DRect(client_bounds), brush_.Get());

    if (frame.scene.root != nullptr) {
      RenderSceneNode(*frame.scene.root);
    }
    while (!transform_stack_.empty()) {
      RenderCommand(PopTransformCommand{});
    }
    while (!clip_stack_.empty()) {
      PopClip();
    }
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->PopAxisAlignedClip();

    const HRESULT result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
      DiscardDeviceResources();
      InvalidateFullWindow();
      return;
    }
    ThrowIfFailed(result, "HuxerUI could not render the Windows frame");
    if (force_full_repaint_) {
      if (Win32RectCovers(paint_rect, client)) {
        force_full_repaint_ = false;
      } else {
        InvalidateFullWindow();
      }
    }
  }

  void RenderSequence(const PaintSequence& sequence) {
    for (const PaintCommand& command : sequence.Commands()) {
      std::visit([this](const auto& value) { RenderCommand(value); }, command);
    }
  }

  void RenderSceneNode(const RenderNode& node) {
    const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
    if (!node.visible || opacity <= 0.0F) {
      return;
    }

    Transform2D transform = node.transform;
    transform.translate_x += node.offset.x;
    transform.translate_y += node.offset.y;
    const bool transformed = !transform.IsIdentity();
    if (transformed) {
      RenderCommand(PushTransformCommand{transform});
    }

    ComPtr<ID2D1Layer> opacity_layer;
    if (opacity < 1.0F) {
      ThrowIfFailed(
          render_target_->CreateLayer(nullptr, opacity_layer.GetAddressOf()),
          "HuxerUI could not create a Direct2D opacity layer"
      );
      render_target_->PushLayer(
          D2D1::LayerParameters(
              D2D1::InfiniteRect(),
              nullptr,
              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
              D2D1::IdentityMatrix(),
              opacity
          ),
          opacity_layer.Get()
      );
    }

    RenderSequence(node.content);
    if (node.child_clip.has_value()) {
      RenderCommand(
          PushClipCommand{
              node.child_clip->rect,
              node.child_clip->corner_radius,
          }
      );
    }
    const bool children_transformed = !node.children_transform.IsIdentity();
    if (children_transformed) {
      RenderCommand(PushTransformCommand{node.children_transform});
    }
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        RenderSceneNode(*child);
      }
    }
    if (children_transformed) {
      RenderCommand(PopTransformCommand{});
    }
    if (node.child_clip.has_value()) {
      RenderCommand(PopClipCommand{});
    }
    RenderSequence(node.foreground);
    if (opacity_layer != nullptr) {
      render_target_->PopLayer();
    }
    if (transformed) {
      RenderCommand(PopTransformCommand{});
    }
  }

  void SetBrushColor(Color color) {
    brush_->SetColor(ToD2DColor(color));
  }

  void RenderCommand(const DrawRectCommand& command) {
    if (command.rect.width <= 0.0F || command.rect.height <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetBrushColor(command.color);
    if (command.corner_radius > 0.0F) {
      render_target_->FillRoundedRectangle(
          D2D1::RoundedRect(ToD2DRect(command.rect), command.corner_radius, command.corner_radius),
          brush_.Get()
      );
    } else {
      render_target_->FillRectangle(ToD2DRect(command.rect), brush_.Get());
    }
  }

  void RenderCommand(const DrawTextCommand& command) {
    if (command.rect.width <= 0.0F || command.rect.height <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    const std::wstring text = Utf8ToWide(command.text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(command.font_size);
    format->SetWordWrapping(
        command.align == TextAlign::Leading ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP
    );
    if (command.align == TextAlign::Center) {
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_factory_->CreateTextLayout(
            text.data(),
            static_cast<UINT32>(text.size()),
            format.Get(),
            command.rect.width,
            command.rect.height,
            layout.GetAddressOf()
        ))) {
      return;
    }
    SetBrushColor(command.color);
    render_target_->DrawTextLayout(
        D2D1::Point2F(command.rect.x, command.rect.y),
        layout.Get(),
        brush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP
    );
  }

  ComPtr<ID2D1StrokeStyle> CreateStrokeStyle(StrokeCap cap) const {
    ComPtr<ID2D1StrokeStyle> style;
    const D2D1_CAP_STYLE d2d_cap = ToD2DCap(cap);
    const D2D1_STROKE_STYLE_PROPERTIES properties{
        d2d_cap,
        d2d_cap,
        d2d_cap,
        D2D1_LINE_JOIN_MITER,
        10.0F,
        D2D1_DASH_STYLE_SOLID,
        0.0F,
    };
    if (FAILED(d2d_factory_->CreateStrokeStyle(properties, nullptr, 0, style.GetAddressOf()))) {
      return {};
    }
    return style;
  }

  void RenderCommand(const DrawCircleCommand& command) {
    if (command.radius <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetBrushColor(command.color);
    render_target_->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(command.center.x, command.center.y), command.radius, command.radius),
        brush_.Get()
    );
  }

  void RenderCommand(const DrawArcCommand& command) {
    if (command.radius <= 0.0F || command.width <= 0.0F || command.color.alpha <= 0.0F ||
        !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
      return;
    }

    SetBrushColor(command.color);
    ComPtr<ID2D1StrokeStyle> stroke_style = CreateStrokeStyle(command.cap);
    if (std::abs(command.sweep_angle) >= kFullCircle - 0.0001F) {
      render_target_->DrawEllipse(
          D2D1::Ellipse(D2D1::Point2F(command.center.x, command.center.y), command.radius, command.radius),
          brush_.Get(),
          command.width,
          stroke_style.Get()
      );
      return;
    }

    const D2D1_POINT_2F start{
        command.center.x + std::cos(command.start_angle) * command.radius,
        command.center.y + std::sin(command.start_angle) * command.radius,
    };
    const float end_angle = command.start_angle + command.sweep_angle;
    const D2D1_POINT_2F end{
        command.center.x + std::cos(end_angle) * command.radius,
        command.center.y + std::sin(end_angle) * command.radius,
    };

    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(d2d_factory_->CreatePathGeometry(geometry.GetAddressOf()))) {
      return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf()))) {
      return;
    }
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(
        D2D1::ArcSegment(
            end,
            D2D1::SizeF(command.radius, command.radius),
            0.0F,
            command.sweep_angle > 0.0F ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
            std::abs(command.sweep_angle) > 3.14159265358979323846F ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL
        )
    );
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
      return;
    }

    render_target_->DrawGeometry(geometry.Get(), brush_.Get(), command.width, stroke_style.Get());
  }

  void RenderCommand(const DrawBorderCommand& command) {
    if (command.width <= 0.0F || command.color.alpha <= 0.0F || command.rect.width <= 0.0F ||
        command.rect.height <= 0.0F) {
      return;
    }
    const float inset = command.width * 0.5F;
    const Rect rect{
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.width),
        std::max(0.0F, command.rect.height - command.width),
    };
    const float radius = std::max(0.0F, command.corner_radius - inset);
    SetBrushColor(command.color);
    render_target_
        ->DrawRoundedRectangle(D2D1::RoundedRect(ToD2DRect(rect), radius, radius), brush_.Get(), command.width);
  }

  void RenderCommand(const PushClipCommand& command) {
    if (command.corner_radius <= 0.0F) {
      render_target_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      clip_stack_.push_back({});
      return;
    }

    const float radius =
        std::max(0.0F, std::min(command.corner_radius, std::min(command.rect.width, command.rect.height) * 0.5F));
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (FAILED(d2d_factory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(ToD2DRect(command.rect), radius, radius),
            geometry.GetAddressOf()
        ))) {
      render_target_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      clip_stack_.push_back({});
      return;
    }
    ComPtr<ID2D1Layer> layer;
    if (FAILED(render_target_->CreateLayer(nullptr, layer.GetAddressOf()))) {
      render_target_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      clip_stack_.push_back({});
      return;
    }

    render_target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), layer.Get());
    ClipState state;
    state.uses_layer = true;
    state.layer = std::move(layer);
    state.geometry = std::move(geometry);
    clip_stack_.push_back(std::move(state));
  }

  void RenderCommand(const PopClipCommand& command) {
    static_cast<void>(command);
    PopClip();
  }

  void RenderCommand(const PushTransformCommand& command) {
    D2D1_MATRIX_3X2_F previous;
    render_target_->GetTransform(&previous);
    transform_stack_.push_back(previous);
    const D2D1::Matrix3x2F transform(
        command.transform.m11,
        command.transform.m12,
        command.transform.m21,
        command.transform.m22,
        command.transform.translate_x,
        command.transform.translate_y
    );
    render_target_->SetTransform(transform * previous);
  }

  void RenderCommand(const PopTransformCommand& command) {
    static_cast<void>(command);
    if (transform_stack_.empty()) {
      return;
    }
    render_target_->SetTransform(transform_stack_.back());
    transform_stack_.pop_back();
  }

  void PopClip() {
    if (clip_stack_.empty()) {
      return;
    }
    if (clip_stack_.back().uses_layer) {
      render_target_->PopLayer();
    } else {
      render_target_->PopAxisAlignedClip();
    }
    clip_stack_.pop_back();
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
        type == KeyEventType::Down && text_input_session_id_ == 0 ? TranslateKeyText(virtual_key, key_data)
                                                                  : std::string{},
        CurrentKeyModifiers(),
        type == KeyEventType::Down && (static_cast<std::uintptr_t>(key_data) & (1ULL << 30U)) != 0,
    });
  }

  TextInputGeometry QueryTextInputGeometry() const {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return {};
    }
    return runtime_->QueryTextInputGeometry(text_input_session_id_, text_input_state_.selection.Range());
  }

  void UpdateImePosition(const TextInputGeometry& geometry) {
    if (window_ == nullptr || geometry.result_code != TextInputResultCode::Ok ||
        geometry.session_id != text_input_session_id_) {
      return;
    }

    HIMC context = ImmGetContext(window_);
    if (context == nullptr) {
      return;
    }

    const float scale = DpiScale();
    const LONG left = static_cast<LONG>(std::lround(geometry.caret.x * scale));
    const LONG top = static_cast<LONG>(std::lround(geometry.caret.y * scale));
    const LONG right =
        static_cast<LONG>(std::lround((geometry.caret.x + std::max(geometry.caret.width, 1.0F)) * scale));
    const LONG bottom =
        static_cast<LONG>(std::lround((geometry.caret.y + std::max(geometry.caret.height, 1.0F)) * scale));

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {left, top};
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = {left, bottom};
    candidate.rcArea = {left, top, right, bottom};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window_, context);
  }

  void CancelNativeComposition() {
    if (window_ == nullptr) {
      return;
    }
    HIMC context = ImmGetContext(window_);
    if (context == nullptr) {
      return;
    }
    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    ImmReleaseContext(window_, context);
  }

  std::wstring ReadCompositionString(HIMC context, DWORD index) const {
    const LONG byte_count = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (byte_count <= 0 || byte_count % static_cast<LONG>(sizeof(wchar_t)) != 0) {
      return {};
    }
    std::wstring result(static_cast<std::size_t>(byte_count) / sizeof(wchar_t), L'\0');
    const LONG copied = ImmGetCompositionStringW(context, index, result.data(), static_cast<DWORD>(byte_count));
    if (copied != byte_count) {
      return {};
    }
    return result;
  }

  TextInputApplyResult ApplyTextInputCommands(std::vector<TextInputCommand> commands) {
    if (runtime_ == nullptr || text_input_session_id_ == 0 || commands.empty()) {
      return {};
    }
    TextInputCommandBatch batch;
    batch.session_id = text_input_session_id_;
    batch.commands = std::move(commands);
    return runtime_->HandleTextInputCommands(batch);
  }

  bool BeginImeComposition() {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return false;
    }
    ime_composing_ = true;
    UpdateImePosition(QueryTextInputGeometry());

    const TextInputContext context = runtime_->QueryTextInputContext(text_input_session_id_, 0, 0);
    if (context.result_code != TextInputResultCode::Ok || context.composition.has_value()) {
      return context.result_code == TextInputResultCode::Ok;
    }

    TextInputCommand begin;
    begin.kind = TextInputCommandKind::BeginComposition;
    begin.target = context.selection.Range();
    const TextInputApplyResult result = ApplyTextInputCommands({std::move(begin)});
    return result.result_code == TextInputResultCode::Ok;
  }

  bool UpdateImeComposition(LPARAM flags) {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return false;
    }
    HIMC context = ImmGetContext(window_);
    if (context == nullptr) {
      return false;
    }

    const bool has_result = (flags & GCS_RESULTSTR) != 0;
    const bool has_composition = (flags & GCS_COMPSTR) != 0;
    const std::wstring result_text = has_result ? ReadCompositionString(context, GCS_RESULTSTR) : std::wstring{};
    const std::wstring composition_text =
        has_composition ? ReadCompositionString(context, GCS_COMPSTR) : std::wstring{};
    LONG cursor = has_composition ? ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0) : 0;
    ImmReleaseContext(window_, context);

    if (!has_result && !has_composition) {
      return true;
    }

    const TextInputContext input_context = runtime_->QueryTextInputContext(text_input_session_id_, 0, 0);
    if (input_context.result_code != TextInputResultCode::Ok) {
      return false;
    }

    std::vector<TextInputCommand> commands;
    TextOffset composition_start = input_context.composition.value_or(input_context.selection.Range()).start;
    if (has_result) {
      TextInputCommand commit;
      commit.kind = TextInputCommandKind::CommitText;
      commit.text = WideToUtf8(result_text);
      commands.push_back(std::move(commit));
      pending_ime_result_ = result_text;
      const std::optional<TextOffset> result_length = Utf16Length(commands.back().text);
      if (!result_length.has_value()) {
        return false;
      }
      composition_start += *result_length;
    }
    if (has_composition) {
      TextInputCommand update;
      update.kind = TextInputCommandKind::UpdateComposition;
      update.text = WideToUtf8(composition_text);
      const std::optional<TextOffset> composition_length = Utf16Length(update.text);
      if (!composition_length.has_value()) {
        return false;
      }
      cursor = std::clamp<LONG>(cursor, 0, static_cast<LONG>(*composition_length));
      update.selection_after = TextSelection{
          composition_start + cursor,
          composition_start + cursor,
      };
      commands.push_back(std::move(update));
    }

    const TextInputApplyResult result = ApplyTextInputCommands(std::move(commands));
    if (result.result_code != TextInputResultCode::Ok) {
      pending_ime_result_.clear();
      return false;
    }
    ime_composing_ = has_composition;
    return true;
  }

  bool EndImeComposition() {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return false;
    }
    ime_composing_ = false;
    pending_high_surrogate_ = 0;

    const TextInputContext context = runtime_->QueryTextInputContext(text_input_session_id_, 0, 0);
    if (context.result_code != TextInputResultCode::Ok || !context.composition.has_value()) {
      return context.result_code == TextInputResultCode::Ok;
    }

    TextInputCommand finish;
    finish.kind = TextInputCommandKind::FinishComposition;
    const TextInputApplyResult result = ApplyTextInputCommands({std::move(finish)});
    return result.result_code == TextInputResultCode::Ok;
  }

  bool SuppressImeCharacter(wchar_t character) {
    if (pending_ime_result_.empty()) {
      return false;
    }
    if (pending_ime_result_.front() != character) {
      pending_ime_result_.clear();
      return false;
    }
    pending_ime_result_.erase(pending_ime_result_.begin());
    return true;
  }

  bool CommitCharacter(wchar_t character) {
    if (text_input_session_id_ == 0) {
      return false;
    }
    if (SuppressImeCharacter(character)) {
      return true;
    }
    if (character < L' ') {
      pending_high_surrogate_ = 0;
      return true;
    }

    std::wstring text;
    if (character >= 0xD800 && character <= 0xDBFF) {
      pending_high_surrogate_ = character;
      return true;
    }
    if (character >= 0xDC00 && character <= 0xDFFF) {
      if (pending_high_surrogate_ == 0) {
        return true;
      }
      text.push_back(pending_high_surrogate_);
      text.push_back(character);
      pending_high_surrogate_ = 0;
    } else {
      pending_high_surrogate_ = 0;
      text.push_back(character);
    }

    TextInputCommand commit;
    commit.kind = TextInputCommandKind::CommitText;
    commit.text = WideToUtf8(text);
    const TextInputApplyResult result = ApplyTextInputCommands({std::move(commit)});
    return result.result_code == TextInputResultCode::Ok;
  }

  LRESULT HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
      dpi_ = static_cast<float>(GetDpiForWindow(window));
      RequestFrameAt(Now());
      return 0;
    case WM_DESTROY:
      window_ = nullptr;
      committed_frame_ = nullptr;
      PostQuitMessage(0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      ResizeRenderTarget();
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      return 0;
    case WM_DPICHANGED: {
      dpi_ = static_cast<float>(HIWORD(w_param));
      if (render_target_) {
        render_target_->SetDpi(dpi_, dpi_);
      }
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
      force_full_repaint_ = true;
      UpdateRuntimeViewport();
      RequestFrameAt(Now());
      return 0;
    }
    case WM_DISPLAYCHANGE:
      DiscardDeviceResources();
      RequestFrameAt(Now());
      return 0;
    case WM_PAINT: {
      paint_in_progress_ = true;
      if (committed_frame_ == nullptr || (frame_build_pending_ && !paint_pending_)) {
        CommitFrameAndInvalidate();
      } else {
        UpdateRuntimeViewport();
      }
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      paint_pending_ = false;
      Render(*committed_frame_, paint.rcPaint);
      EndPaint(window, &paint);
      paint_in_progress_ = false;
      FlushDeferredFrame();
      return 0;
    }
    case kRenderMessage:
      render_message_posted_ = false;
      if (frame_build_pending_) {
        CommitFrameAndInvalidate();
      }
      return 0;
    case WM_TIMER:
      if (w_param == kFrameTimer) {
        KillTimer(window, kFrameTimer);
        timer_armed_ = false;
        timer_deadline_.reset();
        if (frame_build_pending_) {
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
      pending_ime_result_.clear();
      if (text_input_session_id_ != 0 && (w_param == VK_PROCESSKEY || ime_composing_ || w_param == VK_SPACE)) {
        return w_param == VK_SPACE ? 0 : DefWindowProcW(window, message, w_param, l_param);
      }
      SendKey(KeyEventType::Down, w_param, l_param);
      return 0;
    case WM_KEYUP:
      if (text_input_session_id_ != 0 && (w_param == VK_PROCESSKEY || ime_composing_ || w_param == VK_SPACE)) {
        return w_param == VK_SPACE ? 0 : DefWindowProcW(window, message, w_param, l_param);
      }
      SendKey(KeyEventType::Up, w_param, l_param);
      return 0;
    case WM_CHAR:
      return CommitCharacter(static_cast<wchar_t>(w_param)) ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_STARTCOMPOSITION:
      return BeginImeComposition() ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_COMPOSITION:
      return UpdateImeComposition(l_param) ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_ENDCOMPOSITION:
      return EndImeComposition() ? 0 : DefWindowProcW(window, message, w_param, l_param);
    case WM_IME_CHAR:
      if (text_input_session_id_ != 0) {
        SuppressImeCharacter(static_cast<wchar_t>(w_param));
        return 0;
      }
      break;
    default:
      break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    Win32PlatformHost* host = reinterpret_cast<Win32PlatformHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      host = static_cast<Win32PlatformHost*>(create->lpCreateParams);
      host->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (host == nullptr) {
      return DefWindowProcW(window, message, w_param, l_param);
    }

    try {
      return host->HandleMessage(window, message, w_param, l_param);
    } catch (...) {
      if (!host->failure_) {
        host->failure_ = std::current_exception();
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
  bool frame_build_pending_ = true;
  bool paint_pending_ = false;
  bool paint_in_progress_ = false;
  bool force_full_repaint_ = true;
  bool mouse_tracking_ = false;
  bool pointer_down_ = false;
  bool ime_composing_ = false;
  Point last_pointer_position_;
  TextInputSessionId text_input_session_id_ = 0;
  TextInputState text_input_state_;
  wchar_t pending_high_surrogate_ = 0;
  std::wstring pending_ime_result_;
  std::exception_ptr failure_;
  std::optional<double> timer_deadline_;
  std::optional<double> deferred_frame_deadline_;
  const RenderFrame* committed_frame_ = nullptr;
  ComPtr<ID2D1Factory> d2d_factory_;
  ComPtr<IDWriteFactory> write_factory_;
  ComPtr<ID2D1HwndRenderTarget> render_target_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  std::vector<ClipState> clip_stack_;
  std::vector<D2D1_MATRIX_3X2_F> transform_stack_;
};

int RunPlatformApp(AppDefinition definition) {
  AppOptions options = definition.options;
  Win32PlatformHost platform;
  Runtime runtime{std::move(definition), platform};
  return platform.Run(runtime, options);
}

} // namespace huxerui::detail
