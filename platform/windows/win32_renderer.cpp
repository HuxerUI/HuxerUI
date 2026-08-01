#include "win32_renderer.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "shadow_internal.h"
#include "text_layout_internal.h"
#include "win32_internal.h"

namespace huxerui::detail {

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kDipsPerInch = 96.0F;
constexpr float kFullCircle = 6.28318530717958647692F;

void ThrowIfFailed(HRESULT result, const char* message) {
  if (FAILED(result)) {
    throw std::runtime_error(message);
  }
}

D2D1_COLOR_F ToD2DColor(Color color) {
  return D2D1::ColorF(color.red, color.green, color.blue, color.alpha);
}

D2D1_RECT_F ToD2DRect(Rect rect) {
  return D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
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

struct Win32Renderer::State {
  struct ClipState {
    bool uses_layer = false;
    ComPtr<ID2D1Layer> layer;
    ComPtr<ID2D1Geometry> geometry;
  };

  struct ShadowMask {
    Size size;
    float corner_radius = 0.0F;
    float blur_radius = 0.0F;
    ComPtr<ID2D1CommandList> commands;
    // The exterior clip removes the caster from the blurred mask so offsets never form a solid duplicate shape.
    ComPtr<ID2D1Geometry> exterior_clip;
  };

  Size MeasureText(std::string_view text, float font_size, float max_width) {
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

  std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, float font_size, float max_width) {
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

  void InitializeFactories() {
    D2D1_FACTORY_OPTIONS factory_options{};
    ThrowIfFailed(
        D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &factory_options,
            reinterpret_cast<void**>(d2d_factory_.GetAddressOf())
        ),
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

  [[nodiscard]] float DpiScale() const noexcept {
    return std::max(dpi_, 1.0F) / kDipsPerInch;
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

  bool EnsureDeviceResources() {
    if (device_context_) {
      return true;
    }

    constexpr D3D_FEATURE_LEVEL feature_levels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT device_result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        d3d_device_.GetAddressOf(),
        &feature_level,
        nullptr
    );
    if (FAILED(device_result)) {
      device_result = D3D11CreateDevice(
          nullptr,
          D3D_DRIVER_TYPE_WARP,
          nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
          feature_levels,
          static_cast<UINT>(std::size(feature_levels)),
          D3D11_SDK_VERSION,
          d3d_device_.GetAddressOf(),
          &feature_level,
          nullptr
      );
    }
    if (FAILED(device_result)) {
      DiscardDeviceResources();
      return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(d3d_device_.As(&dxgi_device)) ||
        FAILED(d2d_factory_->CreateDevice(dxgi_device.Get(), d2d_device_.GetAddressOf())) ||
        FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, device_context_.GetAddressOf())) ||
        FAILED(device_context_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush_.GetAddressOf()))) {
      DiscardDeviceResources();
      return false;
    }
    device_context_->SetDpi(dpi_, dpi_);
    return true;
  }

  bool EnsureShadowResources() {
    if (shadow_context_ && shadow_brush_ && shadow_effect_ && shadow_clip_layer_) {
      return true;
    }
    DiscardShadowResources();

    ComPtr<ID2D1DeviceContext> shadow_context;
    ComPtr<ID2D1SolidColorBrush> shadow_brush;
    ComPtr<ID2D1Effect> shadow_effect;
    ComPtr<ID2D1Layer> shadow_clip_layer;
    if (FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, shadow_context.GetAddressOf())) ||
        FAILED(shadow_context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), shadow_brush.GetAddressOf())) ||
        FAILED(device_context_->CreateEffect(CLSID_D2D1Shadow, shadow_effect.GetAddressOf())) ||
        FAILED(device_context_->CreateLayer(nullptr, shadow_clip_layer.GetAddressOf()))) {
      return false;
    }

    shadow_context->SetDpi(dpi_, dpi_);
    shadow_context_ = std::move(shadow_context);
    shadow_brush_ = std::move(shadow_brush);
    shadow_effect_ = std::move(shadow_effect);
    shadow_clip_layer_ = std::move(shadow_clip_layer);
    return true;
  }

  HRESULT CreateSwapChain(IDXGIFactory2& factory, DXGI_SWAP_EFFECT effect, UINT buffer_count) {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = buffer_count;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = effect;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    return factory
        .CreateSwapChainForHwnd(d3d_device_.Get(), window_, &description, nullptr, nullptr, swap_chain_.GetAddressOf());
  }

  bool EnsureSizeDependentResources() {
    if (swap_chain_target_ && scene_target_) {
      return true;
    }
    if (!EnsureDeviceResources()) {
      return false;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = static_cast<UINT>(std::max(0L, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max(0L, client.bottom - client.top));
    if (width == 0 || height == 0) {
      return false;
    }

    if (!swap_chain_) {
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      ComPtr<IDXGIFactory2> factory;
      if (FAILED(d3d_device_.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) ||
          FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        DiscardDeviceResources();
        return false;
      }

#if defined(HUXERUI_WINDOWS_7_COMPAT)
      HRESULT swap_chain_result = CreateSwapChain(*factory.Get(), DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2);
      supports_dirty_present_ = SUCCEEDED(swap_chain_result);
      if (FAILED(swap_chain_result)) {
        swap_chain_.Reset();
        swap_chain_result = CreateSwapChain(*factory.Get(), DXGI_SWAP_EFFECT_SEQUENTIAL, 1);
        supports_dirty_present_ = false;
      }
#else
      const HRESULT swap_chain_result = CreateSwapChain(*factory.Get(), DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2);
      supports_dirty_present_ = SUCCEEDED(swap_chain_result);
#endif
      if (FAILED(swap_chain_result)) {
        DiscardDeviceResources();
        return false;
      }
      static_cast<void>(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER));
    }

    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf())))) {
      DiscardDeviceResources();
      return false;
    }

    const D2D1_BITMAP_PROPERTIES1 swap_chain_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        dpi_,
        dpi_
    );
    const D2D1_BITMAP_PROPERTIES1 scene_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi_,
        dpi_
    );
    if (FAILED(device_context_->CreateBitmapFromDxgiSurface(
            surface.Get(),
            &swap_chain_properties,
            swap_chain_target_.GetAddressOf()
        )) ||
        FAILED(
            device_context_
                ->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &scene_properties, scene_target_.GetAddressOf())
        )) {
      DiscardDeviceResources();
      return false;
    }
    force_full_repaint_ = true;
    return true;
  }

  void DiscardSizeDependentResources() noexcept {
    if (device_context_) {
      device_context_->SetTarget(nullptr);
    }
    scene_target_.Reset();
    swap_chain_target_.Reset();
  }

  void DiscardShadowResources() noexcept {
    shadow_masks_.clear();
    shadow_effect_.Reset();
    shadow_brush_.Reset();
    shadow_clip_layer_.Reset();
    shadow_context_.Reset();
  }

  void DiscardDeviceResources() noexcept {
    clip_stack_.clear();
    transform_stack_.clear();
    DiscardSizeDependentResources();
    DiscardShadowResources();
    brush_.Reset();
    swap_chain_.Reset();
    device_context_.Reset();
    d2d_device_.Reset();
    d3d_device_.Reset();
    supports_dirty_present_ = false;
    force_full_repaint_ = true;
  }

  void ResizeRenderTarget() {
    force_full_repaint_ = true;
    if (!swap_chain_) {
      return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = static_cast<UINT>(std::max(0L, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max(0L, client.bottom - client.top));
    DiscardSizeDependentResources();
    if (width == 0 || height == 0) {
      return;
    }
    if (FAILED(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
      DiscardDeviceResources();
    }
  }

  Win32RenderResult Render(const RenderFrame& frame, const RECT& paint_rect) {
    if (paint_rect.left >= paint_rect.right || paint_rect.top >= paint_rect.bottom) {
      return Win32RenderResult::Skipped;
    }
    if (!EnsureSizeDependentResources()) {
      return Win32RenderResult::Skipped;
    }

    RECT client{};
    GetClientRect(window_, &client);
    RECT render_rect = force_full_repaint_ ? client : paint_rect;
    const Rect paint_bounds = Win32PixelRectToDips(render_rect, DpiScale());
    const Rect client_bounds = Win32PixelRectToDips(client, DpiScale());
    clip_stack_.clear();
    transform_stack_.clear();
    device_context_->SetTarget(scene_target_.Get());
    device_context_->BeginDraw();
    device_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    device_context_->PushAxisAlignedClip(ToD2DRect(paint_bounds), D2D1_ANTIALIAS_MODE_ALIASED);
    SetBrushColor(Color::Rgb(247, 248, 250));
    device_context_->FillRectangle(ToD2DRect(client_bounds), brush_.Get());

    if (frame.scene.root != nullptr) {
      RenderSceneNode(*frame.scene.root);
    }
    while (!transform_stack_.empty()) {
      RenderCommand(PopTransformCommand{});
    }
    while (!clip_stack_.empty()) {
      PopClip();
    }
    device_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    device_context_->PopAxisAlignedClip();

    HRESULT result = device_context_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
      DiscardDeviceResources();
      return Win32RenderResult::Recreate;
    }
    ThrowIfFailed(result, "HuxerUI could not render the Windows frame");

    device_context_->SetTarget(swap_chain_target_.Get());
    device_context_->BeginDraw();
    device_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    // Every rotating back buffer receives the complete retained scene; Present1 still limits native damage.
    device_context_->DrawBitmap(scene_target_.Get());
    result = device_context_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
      DiscardDeviceResources();
      return Win32RenderResult::Recreate;
    }
    ThrowIfFailed(result, "HuxerUI could not copy the Windows frame to the swap chain");

    if (supports_dirty_present_) {
      DXGI_PRESENT_PARAMETERS present_parameters{};
      present_parameters.DirtyRectsCount = 1;
      present_parameters.pDirtyRects = &render_rect;
      result = swap_chain_->Present1(1, 0, &present_parameters);
    } else {
      result = swap_chain_->Present(1, 0);
    }
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
      DiscardDeviceResources();
      return Win32RenderResult::Recreate;
    }
    ThrowIfFailed(result, "HuxerUI could not present the Windows frame");
    force_full_repaint_ = false;
    return Win32RenderResult::Presented;
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
          device_context_->CreateLayer(nullptr, opacity_layer.GetAddressOf()),
          "HuxerUI could not create a Direct2D opacity layer"
      );
      device_context_->PushLayer(
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
      device_context_->PopLayer();
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
      device_context_->FillRoundedRectangle(
          D2D1::RoundedRect(ToD2DRect(command.rect), command.corner_radius, command.corner_radius),
          brush_.Get()
      );
    } else {
      device_context_->FillRectangle(ToD2DRect(command.rect), brush_.Get());
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
    device_context_->DrawTextLayout(
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
    device_context_->FillEllipse(
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
      device_context_->DrawEllipse(
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

    device_context_->DrawGeometry(geometry.Get(), brush_.Get(), command.width, stroke_style.Get());
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
    device_context_
        ->DrawRoundedRectangle(D2D1::RoundedRect(ToD2DRect(rect), radius, radius), brush_.Get(), command.width);
  }

  void RenderCommand(const DrawShadowCommand& command) {
    const ResolvedShadow resolved = ResolveShadow(command);
    if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    if (command.blur_radius <= 0.0F) {
      RenderCommand(DrawRectCommand{resolved.caster, command.color, resolved.corner_radius});
      return;
    }
    if (!EnsureShadowResources()) {
      return;
    }

    const ShadowMask* mask = ShadowMaskFor(resolved, command.blur_radius);
    if (mask == nullptr) {
      return;
    }
    shadow_effect_->SetInput(0, mask->commands.Get());
    if (FAILED(shadow_effect_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, resolved.standard_deviation)) ||
        FAILED(shadow_effect_->SetValue(
            D2D1_SHADOW_PROP_COLOR,
            D2D1_VECTOR_4F{command.color.red, command.color.green, command.color.blue, command.color.alpha}
        )) ||
        FAILED(shadow_effect_->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_BALANCED))) {
      shadow_effect_->SetInput(0, nullptr);
      return;
    }

    D2D1_MATRIX_3X2_F previous;
    device_context_->GetTransform(&previous);
    device_context_->SetTransform(D2D1::Matrix3x2F::Translation(resolved.caster.x, resolved.caster.y) * previous);

    device_context_->PushLayer(
        D2D1::LayerParameters1(
            D2D1::InfiniteRect(),
            mask->exterior_clip.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Identity(),
            1.0F,
            nullptr,
            D2D1_LAYER_OPTIONS1_NONE
        ),
        shadow_clip_layer_.Get()
    );
    device_context_->DrawImage(shadow_effect_.Get());
    device_context_->PopLayer();
    device_context_->SetTransform(previous);
    shadow_effect_->SetInput(0, nullptr);
  }

  ShadowMask* ShadowMaskFor(const ResolvedShadow& shadow, float blur_radius) {
    const Size size{shadow.caster.width, shadow.caster.height};
    const auto existing = std::find_if(shadow_masks_.begin(), shadow_masks_.end(), [&](const ShadowMask& mask) {
      return mask.size == size && mask.corner_radius == shadow.corner_radius && mask.blur_radius == blur_radius;
    });
    if (existing != shadow_masks_.end()) {
      return &*existing;
    }

    ComPtr<ID2D1CommandList> commands;
    if (FAILED(shadow_context_->CreateCommandList(commands.GetAddressOf()))) {
      return nullptr;
    }
    shadow_context_->SetTarget(commands.Get());
    shadow_context_->BeginDraw();
    shadow_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (shadow.corner_radius > 0.0F) {
      shadow_context_->FillRoundedRectangle(
          D2D1::RoundedRect(
              D2D1::RectF(0.0F, 0.0F, size.width, size.height),
              shadow.corner_radius,
              shadow.corner_radius
          ),
          shadow_brush_.Get()
      );
    } else {
      shadow_context_->FillRectangle(D2D1::RectF(0.0F, 0.0F, size.width, size.height), shadow_brush_.Get());
    }
    const HRESULT result = shadow_context_->EndDraw();
    shadow_context_->SetTarget(nullptr);
    if (FAILED(result) || FAILED(commands->Close())) {
      return nullptr;
    }

    ComPtr<ID2D1RectangleGeometry> outer;
    ComPtr<ID2D1PathGeometry> outside_path;
    ComPtr<ID2D1GeometrySink> outside_sink;
    const D2D1_RECT_F outer_rect =
        D2D1::RectF(-blur_radius, -blur_radius, size.width + blur_radius, size.height + blur_radius);
    if (FAILED(d2d_factory_->CreateRectangleGeometry(outer_rect, outer.GetAddressOf())) ||
        FAILED(d2d_factory_->CreatePathGeometry(outside_path.GetAddressOf())) ||
        FAILED(outside_path->Open(outside_sink.GetAddressOf()))) {
      return nullptr;
    }

    HRESULT combine_result = E_FAIL;
    if (shadow.corner_radius > 0.0F) {
      ComPtr<ID2D1RoundedRectangleGeometry> caster;
      if (FAILED(d2d_factory_->CreateRoundedRectangleGeometry(
              D2D1::RoundedRect(
                  D2D1::RectF(0.0F, 0.0F, size.width, size.height),
                  shadow.corner_radius,
                  shadow.corner_radius
              ),
              caster.GetAddressOf()
          ))) {
        return nullptr;
      }
      combine_result = outer->CombineWithGeometry(
          caster.Get(),
          D2D1_COMBINE_MODE_EXCLUDE,
          nullptr,
          D2D1_DEFAULT_FLATTENING_TOLERANCE,
          outside_sink.Get()
      );
    } else {
      ComPtr<ID2D1RectangleGeometry> caster;
      if (FAILED(d2d_factory_->CreateRectangleGeometry(
              D2D1::RectF(0.0F, 0.0F, size.width, size.height),
              caster.GetAddressOf()
          ))) {
        return nullptr;
      }
      combine_result = outer->CombineWithGeometry(
          caster.Get(),
          D2D1_COMBINE_MODE_EXCLUDE,
          nullptr,
          D2D1_DEFAULT_FLATTENING_TOLERANCE,
          outside_sink.Get()
      );
    }
    if (FAILED(combine_result) || FAILED(outside_sink->Close())) {
      return nullptr;
    }

    constexpr std::size_t maximum_shadow_masks = 64;
    if (shadow_masks_.size() >= maximum_shadow_masks) {
      shadow_masks_.erase(shadow_masks_.begin());
    }
    shadow_masks_.push_back({size, shadow.corner_radius, blur_radius, commands, outside_path});
    return &shadow_masks_.back();
  }

  void RenderCommand(const PushClipCommand& command) {
    if (command.corner_radius <= 0.0F) {
      device_context_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
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
      device_context_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      clip_stack_.push_back({});
      return;
    }
    ComPtr<ID2D1Layer> layer;
    if (FAILED(device_context_->CreateLayer(nullptr, layer.GetAddressOf()))) {
      device_context_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      clip_stack_.push_back({});
      return;
    }

    device_context_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), layer.Get());
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
    device_context_->GetTransform(&previous);
    transform_stack_.push_back(previous);
    const D2D1::Matrix3x2F transform(
        command.transform.m11,
        command.transform.m12,
        command.transform.m21,
        command.transform.m22,
        command.transform.translate_x,
        command.transform.translate_y
    );
    device_context_->SetTransform(transform * previous);
  }

  void RenderCommand(const PopTransformCommand& command) {
    static_cast<void>(command);
    if (transform_stack_.empty()) {
      return;
    }
    device_context_->SetTransform(transform_stack_.back());
    transform_stack_.pop_back();
  }

  void PopClip() {
    if (clip_stack_.empty()) {
      return;
    }
    if (clip_stack_.back().uses_layer) {
      device_context_->PopLayer();
    } else {
      device_context_->PopAxisAlignedClip();
    }
    clip_stack_.pop_back();
  }

  HWND window_ = nullptr;
  float dpi_ = kDipsPerInch;
  bool force_full_repaint_ = true;
  bool supports_dirty_present_ = false;
  ComPtr<ID2D1Factory1> d2d_factory_;
  ComPtr<IDWriteFactory> write_factory_;
  ComPtr<ID3D11Device> d3d_device_;
  ComPtr<ID2D1Device> d2d_device_;
  ComPtr<ID2D1DeviceContext> device_context_;
  ComPtr<ID2D1DeviceContext> shadow_context_;
  ComPtr<IDXGISwapChain1> swap_chain_;
  ComPtr<ID2D1Bitmap1> swap_chain_target_;
  ComPtr<ID2D1Bitmap1> scene_target_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<ID2D1SolidColorBrush> shadow_brush_;
  ComPtr<ID2D1Effect> shadow_effect_;
  ComPtr<ID2D1Layer> shadow_clip_layer_;
  std::vector<ShadowMask> shadow_masks_;
  std::vector<ClipState> clip_stack_;
  std::vector<D2D1_MATRIX_3X2_F> transform_stack_;
};

Win32Renderer::Win32Renderer() : state_(std::make_unique<State>()) {}

Win32Renderer::~Win32Renderer() = default;

void Win32Renderer::Initialize() {
  state_->InitializeFactories();
}

void Win32Renderer::Discard() noexcept {
  state_->DiscardDeviceResources();
  state_->write_factory_.Reset();
  state_->d2d_factory_.Reset();
  state_->window_ = nullptr;
}

void Win32Renderer::ResetDeviceResources() noexcept {
  state_->DiscardDeviceResources();
}

void Win32Renderer::Resize(HWND window, float dpi) {
  state_->window_ = window;
  state_->dpi_ = dpi;
  state_->ResizeRenderTarget();
}

void Win32Renderer::DpiChanged(HWND window, float dpi) {
  state_->window_ = window;
  state_->dpi_ = dpi;
  if (state_->device_context_) {
    state_->device_context_->SetDpi(dpi, dpi);
  }
  if (state_->shadow_context_) {
    state_->shadow_context_->SetDpi(dpi, dpi);
    state_->shadow_masks_.clear();
  }
  state_->DiscardSizeDependentResources();
  state_->force_full_repaint_ = true;
}

Size Win32Renderer::MeasureText(std::string_view text, float font_size, float max_width) {
  return state_->MeasureText(text, font_size, max_width);
}

std::unique_ptr<TextLayout> Win32Renderer::CreateTextLayout(std::string_view text, float font_size, float max_width) {
  return state_->CreateTextLayout(text, font_size, max_width);
}

Win32RenderResult Win32Renderer::Render(HWND window, float dpi, const RenderFrame& frame, const RECT& paint_rect) {
  state_->window_ = window;
  state_->dpi_ = dpi;
  return state_->Render(frame, paint_rect);
}

} // namespace huxerui::detail
