#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "internal.h"

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

class Win32PlatformHost final : public huxerui::PlatformHost {
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

  void RequestFrame(double delay_seconds) override {
    if (window_ == nullptr) {
      return;
    }
    if (!std::isfinite(delay_seconds) || delay_seconds <= 0.0) {
      if (timer_armed_) {
        KillTimer(window_, kFrameTimer);
        timer_armed_ = false;
      }
      if (!render_message_posted_) {
        render_message_posted_ = PostMessageW(window_, kRenderMessage, 0, 0) != FALSE;
      }
      return;
    }

    if (render_message_posted_) {
      return;
    }
    if (timer_armed_) {
      KillTimer(window_, kFrameTimer);
    }
    const double milliseconds = std::ceil(delay_seconds * 1000.0);
    const double bounded = std::clamp(milliseconds, 1.0, static_cast<double>(std::numeric_limits<UINT>::max()));
    timer_armed_ = SetTimer(window_, kFrameTimer, static_cast<UINT>(bounded), nullptr) != 0;
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
    return true;
  }

  void DiscardDeviceResources() noexcept {
    clip_stack_.clear();
    transform_stack_.clear();
    brush_.Reset();
    render_target_.Reset();
  }

  void ResizeRenderTarget() {
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

  void Render(const DisplayList& display_list) {
    if (!EnsureRenderTarget()) {
      return;
    }

    clip_stack_.clear();
    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->Clear(D2D1::ColorF(247.0F / 255.0F, 248.0F / 255.0F, 250.0F / 255.0F, 1.0F));

    for (const DrawCommand& command : display_list.Commands()) {
      std::visit([this](const auto& value) { RenderCommand(value); }, command);
    }
    while (!clip_stack_.empty()) {
      PopClip();
    }

    const HRESULT result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
      DiscardDeviceResources();
      InvalidateRect(window_, nullptr, FALSE);
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
    sink->AddArc(D2D1::ArcSegment(
        end,
        D2D1::SizeF(command.radius, command.radius),
        0.0F,
        command.sweep_angle > 0.0F ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
        std::abs(command.sweep_angle) > 3.14159265358979323846F ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL
    ));
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
    const D2D1::Matrix3x2F
        transform(command.m11, command.m12, command.m21, command.m22, command.translate_x, command.translate_y);
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

  void SendPointer(PointerEventType type, Point position) {
    last_pointer_position_ = position;
    runtime_->HandlePointerEvent({
        type,
        0,
        position,
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
        type == KeyEventType::Down ? TranslateKeyText(virtual_key, key_data) : std::string{},
        CurrentKeyModifiers(),
        type == KeyEventType::Down && (static_cast<std::uintptr_t>(key_data) & (1ULL << 30U)) != 0,
    });
  }

  LRESULT HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
      dpi_ = static_cast<float>(GetDpiForWindow(window));
      return 0;
    case WM_DESTROY:
      window_ = nullptr;
      PostQuitMessage(0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      ResizeRenderTarget();
      InvalidateRect(window, nullptr, FALSE);
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
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_DISPLAYCHANGE:
      DiscardDeviceResources();
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      const float scale = DpiScale();
      runtime_->SetViewport({
          static_cast<float>(client.right - client.left) / scale,
          static_cast<float>(client.bottom - client.top) / scale,
      });
      Render(runtime_->BuildFrame());
      EndPaint(window, &paint);
      return 0;
    }
    case kRenderMessage:
      render_message_posted_ = false;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_TIMER:
      if (w_param == kFrameTimer) {
        KillTimer(window, kFrameTimer);
        timer_armed_ = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetFocus(window);
      SetCapture(window);
      pointer_down_ = true;
      SendPointer(PointerEventType::Down, ClientPoint(l_param));
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
      SendKey(KeyEventType::Down, w_param, l_param);
      return 0;
    case WM_KEYUP:
      SendKey(KeyEventType::Up, w_param, l_param);
      return 0;
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
  bool mouse_tracking_ = false;
  bool pointer_down_ = false;
  Point last_pointer_position_;
  std::exception_ptr failure_;
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
