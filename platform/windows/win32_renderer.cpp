#include "win32_renderer.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "path_internal.h"
#include "paint_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_layout_internal.h"
#include "win32_external_texture_internal.h"
#include "win32_internal.h"

namespace huxerui::detail {

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kDipsPerInch = 96.0F;
constexpr float kFullCircle = 6.28318530717958647692F;
// The effect property stores the enum as UINT32; some MinGW headers expose the property but omit its enum.
constexpr UINT32 kShadowOptimizationBalanced = 1U;

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

D2D1_MATRIX_3X2_F ToD2DTransform(Transform2D transform) {
  return D2D1::Matrix3x2F(transform.m11, transform.m12, transform.m21, transform.m22, transform.translate_x,
                         transform.translate_y);
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

D2D1_LINE_JOIN ToD2DJoin(StrokeJoin join) {
  switch (join) {
  case StrokeJoin::Round:
    return D2D1_LINE_JOIN_ROUND;
  case StrokeJoin::Bevel:
    return D2D1_LINE_JOIN_BEVEL;
  case StrokeJoin::Miter:
  default:
    return D2D1_LINE_JOIN_MITER;
  }
}

void CombineHash(std::size_t& seed, std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::size_t HashFont(const Font& font) noexcept {
  std::size_t result = std::hash<FontFamilyKind>{}(font.FamilyKind());
  CombineHash(result, std::hash<std::string_view>{}(font.FamilyName()));
  CombineHash(result, std::hash<float>{}(font.Size()));
  CombineHash(result, std::hash<FontWeight>{}(font.Weight()));
  CombineHash(result, std::hash<FontSlant>{}(font.Slant()));
  return result;
}

std::size_t HashShaping(const TextShapingOptions& shaping) noexcept {
  std::size_t result = std::hash<TextDirection>{}(shaping.direction);
  CombineHash(result, std::hash<std::string>{}(shaping.locale));
  return result;
}

TextDirection ResolveTextDirection(std::wstring_view text, TextDirection requested) {
  if (requested != TextDirection::Auto) {
    return requested;
  }
  if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return TextDirection::LeftToRight;
  }

  std::vector<WORD> character_types(text.size());
  if (GetStringTypeW(CT_CTYPE2, text.data(), static_cast<int>(text.size()), character_types.data()) == 0) {
    return TextDirection::LeftToRight;
  }
  for (WORD type : character_types) {
    if (type == C2_RIGHTTOLEFT) {
      return TextDirection::RightToLeft;
    }
    if (type == C2_LEFTTORIGHT) {
      return TextDirection::LeftToRight;
    }
  }
  return TextDirection::LeftToRight;
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

  struct CachedPathGeometry {
    Path path;
    PathFillRule fill_rule = PathFillRule::NonZero;
    ComPtr<ID2D1PathGeometry> geometry;
  };

  struct CachedImage {
    std::uint64_t identity = 0;
    std::size_t byte_size = 0;
    ComPtr<ID2D1Bitmap1> bitmap;
  };

  struct CachedExternalTexture {
    std::weak_ptr<Win32ExternalTextureState> source;
    // The CPU frame outlives device resources so a reset can rebuild its Direct2D bitmap without another publish.
    std::optional<Win32ExternalTextureFrame> frame;
    ComPtr<ID2D1Bitmap1> bitmap;
    std::uint64_t draw_epoch = std::numeric_limits<std::uint64_t>::max();
  };

  struct FontHash {
    std::size_t operator()(const Font& font) const noexcept {
      return HashFont(font);
    }
  };

  struct TextRunKey {
    std::string text;
    Font font;
    TextDecoration decoration = TextDecoration::None;
    TextShapingOptions shaping;

    bool operator==(const TextRunKey&) const = default;
  };

  struct TextRunKeyView {
    std::string_view text;
    const Font& font;
    TextDecoration decoration;
    const TextShapingOptions& shaping;
  };

  struct TextRunKeyHash {
    using is_transparent = void;

    std::size_t operator()(const TextRunKey& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.shaping);
    }

    std::size_t operator()(const TextRunKeyView& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.shaping);
    }

  private:
    static std::size_t Hash(
        std::string_view text, const Font& font, TextDecoration decoration, const TextShapingOptions& shaping
    ) noexcept {
      std::size_t result = std::hash<std::string_view>{}(text);
      CombineHash(result, HashFont(font));
      CombineHash(result, std::hash<TextDecoration>{}(decoration));
      CombineHash(result, HashShaping(shaping));
      return result;
    }
  };

  struct TextRunKeyEqual {
    using is_transparent = void;

    bool operator()(const TextRunKey& left, const TextRunKey& right) const noexcept {
      return left == right;
    }

    bool operator()(const TextRunKey& left, const TextRunKeyView& right) const noexcept {
      return std::string_view(left.text) == right.text && left.font == right.font &&
             left.decoration == right.decoration && left.shaping == right.shaping;
    }

    bool operator()(const TextRunKeyView& left, const TextRunKey& right) const noexcept {
      return (*this)(right, left);
    }
  };

  struct CachedTextRun {
    ComPtr<IDWriteTextLayout> layout;
    TextRunMetrics metrics;
    float baseline = 0.0F;
  };

  struct ParagraphKey {
    std::string text;
    Font font;
    TextDecoration decoration = TextDecoration::None;
    TextLayoutOptions options;
    Size size;

    bool operator==(const ParagraphKey&) const = default;
  };

  struct ParagraphKeyView {
    std::string_view text;
    const Font& font;
    TextDecoration decoration;
    const TextLayoutOptions& options;
    Size size;
  };

  struct ParagraphKeyHash {
    using is_transparent = void;

    std::size_t operator()(const ParagraphKey& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.options, key.size);
    }

    std::size_t operator()(const ParagraphKeyView& key) const noexcept {
      return Hash(key.text, key.font, key.decoration, key.options, key.size);
    }

  private:
    static std::size_t Hash(
        std::string_view text, const Font& font, TextDecoration decoration, const TextLayoutOptions& options, Size size
    ) noexcept {
      std::size_t result = std::hash<std::string_view>{}(text);
      CombineHash(result, HashFont(font));
      CombineHash(result, std::hash<TextDecoration>{}(decoration));
      CombineHash(result, HashShaping(options.shaping));
      CombineHash(result, std::hash<TextAlign>{}(options.align));
      CombineHash(result, std::hash<TextVerticalAlign>{}(options.vertical_align));
      CombineHash(result, std::hash<TextWrap>{}(options.wrap));
      CombineHash(result, std::hash<float>{}(size.width));
      CombineHash(result, std::hash<float>{}(size.height));
      return result;
    }
  };

  struct ParagraphKeyEqual {
    using is_transparent = void;

    bool operator()(const ParagraphKey& left, const ParagraphKey& right) const noexcept {
      return left == right;
    }

    bool operator()(const ParagraphKey& left, const ParagraphKeyView& right) const noexcept {
      return std::string_view(left.text) == right.text && left.font == right.font &&
             left.decoration == right.decoration && left.options == right.options && left.size == right.size;
    }

    bool operator()(const ParagraphKeyView& left, const ParagraphKey& right) const noexcept {
      return (*this)(right, left);
    }
  };

  struct CachedParagraph {
    ComPtr<IDWriteTextLayout> layout;
  };

  TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options) {
    const std::wstring wide = Utf8ToWide(text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(style.font, options.shaping.locale);
    const bool constrained = std::isfinite(max_width);
    if (constrained && max_width <= 0.0F) {
      return {};
    }
    TextLayoutOptions measurement_options = options;
    // Alignment does not affect intrinsic metrics. Keeping text at the physical left avoids precision loss when an
    // unbounded DirectWrite layout uses its maximum finite width.
    const TextDirection direction = ResolveTextDirection(wide, options.shaping.direction);
    measurement_options.shaping.direction = direction;
    measurement_options.align = direction == TextDirection::RightToLeft ? TextAlign::Trailing : TextAlign::Leading;
    ConfigureTextFormat(*format.Get(), measurement_options, wide);

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
    UINT32 line_count = 0;
    layout->GetLineMetrics(nullptr, 0, &line_count);
    std::vector<DWRITE_LINE_METRICS> lines(line_count);
    if (line_count > 0) {
      ThrowIfFailed(
          layout->GetLineMetrics(lines.data(), line_count, &line_count),
          "HuxerUI could not query DirectWrite line metrics"
      );
    }
    const float first_baseline = lines.empty() ? 0.0F : lines.front().baseline;
    const float last_baseline = lines.empty() ? 0.0F : metrics.height - lines.back().height + lines.back().baseline;
    const Size size{std::ceil(width), std::ceil(metrics.height)};
    return {
        .size = size,
        .first_baseline = first_baseline,
        .last_baseline = last_baseline,
        .line_count = line_count,
    };
  }

  TextRunMetrics MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
    if (text.find_first_of("\r\n") != std::string_view::npos) {
      throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
    }
    if (text.empty()) {
      return {.font_metrics = Metrics(style.font)};
    }
    return TextRunFor(text, style, options).metrics;
  }

  std::unique_ptr<TextLayout>
  CreateTextLayout(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options) {
    std::wstring wide = Utf8ToWide(text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(style.font, options.shaping.locale);
    const bool constrained = std::isfinite(max_width);
    ConfigureTextFormat(*format.Get(), options, wide);

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
    static_cast<void>(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic_factory_.GetAddressOf())
    ));
  }

  [[nodiscard]] float DpiScale() const noexcept {
    return std::max(dpi_, 1.0F) / kDipsPerInch;
  }

  std::wstring FontFamilyName(const Font& font) const {
    if (font.FamilyKind() == FontFamilyKind::Monospace) {
      return L"Consolas";
    }
    if (font.FamilyKind() == FontFamilyKind::Named) {
      return Utf8ToWide(font.FamilyName());
    }
    return L"Segoe UI";
  }

  ComPtr<IDWriteTextFormat> CreateTextFormat(const Font& font, std::string_view locale = {}) const {
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    std::wstring requested_locale;
    if (locale.empty()) {
      if (GetUserDefaultLocaleName(locale_name, static_cast<int>(std::size(locale_name))) == 0) {
        wcscpy_s(locale_name, L"en-us");
      }
    } else {
      requested_locale = Utf8ToWide(locale);
    }

    ComPtr<IDWriteTextFormat> format;
    const std::wstring family = FontFamilyName(font);
    ThrowIfFailed(
        write_factory_->CreateTextFormat(
            family.c_str(),
            nullptr,
            static_cast<DWRITE_FONT_WEIGHT>(font.Weight()),
            font.Slant() == FontSlant::Italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            font.Size(),
            requested_locale.empty() ? locale_name : requested_locale.c_str(),
            format.GetAddressOf()
        ),
        "HuxerUI could not create a DirectWrite text format"
    );
    return format;
  }

  static void ConfigureTextFormat(IDWriteTextFormat& format, const TextLayoutOptions& options, std::wstring_view text) {
    format.SetWordWrapping(options.wrap == TextWrap::Word ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    switch (options.align) {
    case TextAlign::Center:
      format.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      break;
    case TextAlign::Trailing:
      format.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
      break;
    case TextAlign::Leading:
      format.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      break;
    }
    if (ResolveTextDirection(text, options.shaping.direction) == TextDirection::RightToLeft) {
      format.SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    } else {
      format.SetReadingDirection(DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    }
  }

  CachedTextRun& TextRunFor(std::string_view text, const TextStyle& style, const TextShapingOptions& shaping) {
    const auto cached = text_runs_.find(TextRunKeyView{text, style.font, style.decoration, shaping});
    if (cached != text_runs_.end()) {
      return cached->second;
    }

    const std::wstring wide = Utf8ToWide(text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(style.font, shaping.locale);
    ConfigureTextFormat(*format.Get(), TextLayoutOptions{.shaping = shaping, .wrap = TextWrap::NoWrap}, wide);
    ComPtr<IDWriteTextLayout> layout;
    ThrowIfFailed(
        write_factory_->CreateTextLayout(
            wide.data(),
            static_cast<UINT32>(wide.size()),
            format.Get(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            layout.GetAddressOf()
        ),
        "HuxerUI could not create a DirectWrite text run layout"
    );
    if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
      ThrowIfFailed(
          layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(wide.size())}),
          "HuxerUI could not configure DirectWrite text run underline"
      );
    }
    if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
      ThrowIfFailed(
          layout->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(wide.size())}),
          "HuxerUI could not configure DirectWrite text run strikethrough"
      );
    }

    DWRITE_TEXT_METRICS text_metrics{};
    ThrowIfFailed(layout->GetMetrics(&text_metrics), "HuxerUI could not measure a DirectWrite text run");
    UINT32 line_count = 0;
    layout->GetLineMetrics(nullptr, 0, &line_count);
    DWRITE_LINE_METRICS line{};
    if (line_count == 0 || FAILED(layout->GetLineMetrics(&line, 1, &line_count))) {
      throw std::runtime_error("HuxerUI could not query DirectWrite text run baseline");
    }
    const float advance = text_metrics.widthIncludingTrailingWhitespace;
    const float layout_width = std::max(1.0F, advance);
    const float layout_height = std::max(1.0F, line.height);
    ThrowIfFailed(layout->SetMaxWidth(layout_width), "HuxerUI could not constrain DirectWrite text run width");
    ThrowIfFailed(layout->SetMaxHeight(layout_height), "HuxerUI could not constrain DirectWrite text run height");

    DWRITE_OVERHANG_METRICS overhang{};
    ThrowIfFailed(layout->GetOverhangMetrics(&overhang), "HuxerUI could not query DirectWrite text run bounds");
    const float left = std::max(0.0F, overhang.left);
    const float top = std::max(0.0F, overhang.top);
    const float right = std::max(0.0F, overhang.right);
    const float bottom = std::max(0.0F, overhang.bottom);
    Rect visual_bounds{
        -left,
        -line.baseline - top,
        layout_width + left + right,
        layout_height + top + bottom,
    };
    const FontMetrics font_metrics = Metrics(style.font);
    const auto include_decoration = [&](float center, float thickness) {
      if (advance <= 0.0F || thickness <= 0.0F) {
        return;
      }
      const Rect decoration{0.0F, center - thickness * 0.5F, advance, thickness};
      const float left_edge = std::min(visual_bounds.x, decoration.x);
      const float top_edge = std::min(visual_bounds.y, decoration.y);
      const float right_edge = std::max(visual_bounds.x + visual_bounds.width, decoration.x + decoration.width);
      const float bottom_edge = std::max(visual_bounds.y + visual_bounds.height, decoration.y + decoration.height);
      visual_bounds = {left_edge, top_edge, right_edge - left_edge, bottom_edge - top_edge};
    };
    if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
      include_decoration(font_metrics.underline_position, font_metrics.underline_thickness);
    }
    if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
      include_decoration(-font_metrics.strike_through_position, font_metrics.strike_through_thickness);
    }

    constexpr std::size_t maximum_text_runs = 1024;
    if (text_runs_.size() >= maximum_text_runs) {
      text_runs_.erase(text_runs_.begin());
    }
    auto [inserted, was_inserted] = text_runs_.emplace(
        TextRunKey{std::string(text), style.font, style.decoration, shaping},
        CachedTextRun{
            std::move(layout),
            {advance, visual_bounds, font_metrics},
            line.baseline,
        }
    );
    static_cast<void>(was_inserted);
    return inserted->second;
  }

  ComPtr<IDWriteTextLayout> ParagraphFor(const DrawTextCommand& command) {
    const Size size{command.rect.width, command.rect.height};
    const auto cached = paragraphs_.find(
        ParagraphKeyView{command.text, command.style.font, command.style.decoration, command.options, size}
    );
    if (cached != paragraphs_.end()) {
      return cached->second.layout;
    }

    const std::wstring text = Utf8ToWide(command.text);
    ComPtr<IDWriteTextFormat> format = CreateTextFormat(command.style.font, command.options.shaping.locale);
    ConfigureTextFormat(*format.Get(), command.options, text);
    ComPtr<IDWriteTextLayout> layout;
    ThrowIfFailed(
        write_factory_->CreateTextLayout(
            text.data(),
            static_cast<UINT32>(text.size()),
            format.Get(),
            command.rect.width,
            command.rect.height,
            layout.GetAddressOf()
        ),
        "HuxerUI could not create a DirectWrite text paragraph"
    );
    if (HasTextDecoration(command.style.decoration, TextDecoration::Underline)) {
      ThrowIfFailed(
          layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())}),
          "HuxerUI could not configure DirectWrite paragraph underline"
      );
    }
    if (HasTextDecoration(command.style.decoration, TextDecoration::StrikeThrough)) {
      ThrowIfFailed(
          layout->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())}),
          "HuxerUI could not configure DirectWrite paragraph strikethrough"
      );
    }
    constexpr std::size_t maximum_paragraphs = 256;
    if (paragraphs_.size() >= maximum_paragraphs) {
      paragraphs_.erase(paragraphs_.begin());
    }
    paragraphs_.emplace(
        ParagraphKey{command.text, command.style.font, command.style.decoration, command.options, size},
        CachedParagraph{layout}
    );
    return layout;
  }

  FontMetrics Metrics(const Font& font) {
    const auto cached = font_metrics_.find(font);
    if (cached != font_metrics_.end()) {
      return cached->second;
    }
    ComPtr<IDWriteFontCollection> collection;
    ThrowIfFailed(
        write_factory_->GetSystemFontCollection(collection.GetAddressOf()),
        "HuxerUI could not access the DirectWrite font collection"
    );
    const std::wstring family_name = FontFamilyName(font);
    UINT32 family_index = 0;
    BOOL exists = FALSE;
    ThrowIfFailed(
        collection->FindFamilyName(family_name.c_str(), &family_index, &exists),
        "HuxerUI could not resolve a DirectWrite font family"
    );
    if (exists == FALSE) {
      collection->FindFamilyName(L"Segoe UI", &family_index, &exists);
    }
    ComPtr<IDWriteFontFamily> family;
    ThrowIfFailed(
        collection->GetFontFamily(family_index, family.GetAddressOf()),
        "HuxerUI could not access a font family"
    );
    ComPtr<IDWriteFont> resolved;
    ThrowIfFailed(
        family->GetFirstMatchingFont(
            static_cast<DWRITE_FONT_WEIGHT>(font.Weight()),
            DWRITE_FONT_STRETCH_NORMAL,
            font.Slant() == FontSlant::Italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            resolved.GetAddressOf()
        ),
        "HuxerUI could not resolve a DirectWrite font"
    );
    ComPtr<IDWriteFontFace> face;
    ThrowIfFailed(resolved->CreateFontFace(face.GetAddressOf()), "HuxerUI could not create a DirectWrite font face");
    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);
    const float scale = font.Size() / static_cast<float>(metrics.designUnitsPerEm);
    const FontMetrics result{
        .ascent = metrics.ascent * scale,
        .descent = metrics.descent * scale,
        .leading = metrics.lineGap * scale,
        .underline_position = -metrics.underlinePosition * scale,
        .underline_thickness = metrics.underlineThickness * scale,
        .strike_through_position = metrics.strikethroughPosition * scale,
        .strike_through_thickness = metrics.strikethroughThickness * scale,
    };
    constexpr std::size_t maximum_font_metrics = 64;
    if (font_metrics_.size() >= maximum_font_metrics) {
      font_metrics_.erase(font_metrics_.begin());
    }
    font_metrics_.emplace(font, result);
    return result;
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

  bool EnsureDirectComposition() {
    if (!platform_composition_) {
      return true;
    }
    if (composition_device_ && composition_target_ && composition_visual_) {
      return true;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(d3d_device_.As(&dxgi_device))) {
      return false;
    }

    HRESULT result = E_NOTIMPL;
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    if (dcomp_library_ == nullptr) {
      dcomp_library_ = LoadLibraryW(L"dcomp.dll");
    }
    using CreateDeviceFunction = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);
    const auto create_device =
        dcomp_library_ == nullptr
            ? nullptr
            : reinterpret_cast<CreateDeviceFunction>(GetProcAddress(dcomp_library_, "DCompositionCreateDevice"));
    if (create_device != nullptr) {
      result = create_device(
          dxgi_device.Get(),
          __uuidof(IDCompositionDevice),
          reinterpret_cast<void**>(composition_device_.GetAddressOf())
      );
    }
#else
    result = DCompositionCreateDevice(
        dxgi_device.Get(),
        __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(composition_device_.GetAddressOf())
    );
#endif
    if (FAILED(result) ||
        FAILED(composition_device_->CreateTargetForHwnd(window_, TRUE, composition_target_.GetAddressOf())) ||
        FAILED(composition_device_->CreateVisual(composition_visual_.GetAddressOf())) ||
        FAILED(composition_target_->SetRoot(composition_visual_.Get()))) {
      composition_visual_.Reset();
      composition_target_.Reset();
      composition_device_.Reset();
      return false;
    }
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

  HRESULT CreateSwapChain(IDXGIFactory2& factory, DXGI_SWAP_EFFECT effect, UINT buffer_count, UINT width, UINT height) {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = platform_composition_ ? width : 0;
    description.Height = platform_composition_ ? height : 0;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = buffer_count;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = effect;
    description.AlphaMode = platform_composition_ ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    if (platform_composition_) {
      return factory
          .CreateSwapChainForComposition(d3d_device_.Get(), &description, nullptr, swap_chain_.GetAddressOf());
    }
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
    if (!EnsureDirectComposition()) {
      throw std::logic_error("HuxerUI Windows PlatformView requires unavailable DirectComposition");
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
      HRESULT swap_chain_result = CreateSwapChain(*factory.Get(), DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, width, height);
      supports_dirty_present_ = SUCCEEDED(swap_chain_result);
      if (FAILED(swap_chain_result) && !platform_composition_) {
        swap_chain_.Reset();
        swap_chain_result = CreateSwapChain(*factory.Get(), DXGI_SWAP_EFFECT_SEQUENTIAL, 1, width, height);
        supports_dirty_present_ = false;
      }
#else
      const HRESULT swap_chain_result =
          CreateSwapChain(*factory.Get(), DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, width, height);
      supports_dirty_present_ = SUCCEEDED(swap_chain_result);
#endif
      if (FAILED(swap_chain_result)) {
        DiscardDeviceResources();
        if (platform_composition_) {
          ThrowIfFailed(swap_chain_result, "HuxerUI could not create the Windows PlatformView composition surface");
        }
        return false;
      }
      if (platform_composition_) {
        HRESULT composition_result = composition_visual_->SetContent(swap_chain_.Get());
        if (SUCCEEDED(composition_result)) {
          composition_result = composition_device_->Commit();
        }
        if (FAILED(composition_result)) {
          DiscardDeviceResources();
          ThrowIfFailed(composition_result, "HuxerUI could not attach the Windows PlatformView composition surface");
        }
      } else {
        static_cast<void>(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER));
      }
    }

    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf())))) {
      DiscardDeviceResources();
      return false;
    }

    const D2D1_BITMAP_PROPERTIES1 swap_chain_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            platform_composition_ ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE
        ),
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

  void ResetExternalTextureBitmaps() noexcept {
    for (CachedExternalTexture& entry : external_textures_) {
      entry.bitmap.Reset();
    }
  }

  void BeginExternalTextureFrame() {
    if (external_texture_draw_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
      external_texture_draw_epoch_ = 0;
      for (CachedExternalTexture& entry : external_textures_) {
        entry.draw_epoch = std::numeric_limits<std::uint64_t>::max();
      }
    } else {
      ++external_texture_draw_epoch_;
    }

    std::erase_if(external_textures_, [](const CachedExternalTexture& entry) {
      const std::shared_ptr<Win32ExternalTextureState> source = entry.source.lock();
      return !source || !source->IsActive();
    });
  }

  void DiscardDeviceResources() noexcept {
    clip_stack_.clear();
    transform_stack_.clear();
    DiscardSizeDependentResources();
    DiscardShadowResources();
    ResetExternalTextureBitmaps();
    path_geometries_.clear();
    images_.clear();
    image_cache_bytes_ = 0;
    brush_.Reset();
    swap_chain_.Reset();
    if (composition_target_ && composition_device_) {
      static_cast<void>(composition_target_->SetRoot(nullptr));
      static_cast<void>(composition_device_->Commit());
    }
    composition_visual_.Reset();
    composition_target_.Reset();
    composition_device_.Reset();
    device_context_.Reset();
    d2d_device_.Reset();
    d3d_device_.Reset();
    supports_dirty_present_ = false;
    force_full_repaint_ = true;
  }

  bool EnablePlatformComposition(HWND window) {
    window_ = window;
    if (platform_composition_) {
      return false;
    }
    // Switching back to an HWND swap chain would tear down the window surface during every PlatformView unmount.
    platform_composition_ = true;
    DiscardDeviceResources();
    return true;
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
    BeginExternalTextureFrame();
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
    if (platform_composition_) {
      device_context_->Clear(D2D1::ColorF(0.0F, 0.0F));
    }
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
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this](const auto& command) { RenderCommand(command); }, clip);
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
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
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
    if (command.rect.width <= 0.0F || command.rect.height <= 0.0F || command.style.foreground.alpha <= 0.0F) {
      return;
    }
    ComPtr<IDWriteTextLayout> layout = ParagraphFor(command);
    DWRITE_TEXT_METRICS metrics{};
    ThrowIfFailed(layout->GetMetrics(&metrics), "HuxerUI could not measure a DirectWrite text paragraph");
    const float remaining_height = std::max(0.0F, command.rect.height - metrics.height);
    float vertical_offset = 0.0F;
    if (command.options.vertical_align == TextVerticalAlign::Center) {
      vertical_offset = remaining_height * 0.5F;
    } else if (command.options.vertical_align == TextVerticalAlign::Bottom) {
      vertical_offset = remaining_height;
    }
    SetBrushColor(command.style.foreground);
    device_context_->PushAxisAlignedClip(ToD2DRect(command.rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    device_context_->DrawTextLayout(
        D2D1::Point2F(
            command.rect.x + command.paragraph_offset.x,
            command.rect.y + vertical_offset + command.paragraph_offset.y
        ),
        layout.Get(),
        brush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE
    );
    device_context_->PopAxisAlignedClip();
  }

  void RenderCommand(const DrawTextRunsCommand& command) {
    for (const TextRun& run : command.runs) {
      if (run.text.empty() || run.bounds.IsEmpty() || run.style.foreground.alpha <= 0.0F) {
        continue;
      }
      CachedTextRun& cached = TextRunFor(run.text, run.style, run.shaping);
      SetBrushColor(run.style.foreground);
      device_context_->DrawTextLayout(
          D2D1::Point2F(run.baseline_origin.x, run.baseline_origin.y - cached.baseline),
          cached.layout.Get(),
          brush_.Get(),
          D2D1_DRAW_TEXT_OPTIONS_NONE
      );
    }
  }

  ComPtr<ID2D1Bitmap1> ImageBitmapFor(const ImageAsset& image) {
    const std::uint64_t identity = ResourceAccess::ImageIdentity(image);
    const auto cached =
        std::ranges::find_if(images_, [identity](const CachedImage& entry) { return entry.identity == identity; });
    if (cached != images_.end()) {
      std::rotate(cached, cached + 1, images_.end());
      return images_.back().bitmap;
    }
    if (!wic_factory_ && FAILED(CoCreateInstance(
                             CLSID_WICImagingFactory,
                             nullptr,
                             CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(wic_factory_.GetAddressOf())
                         ))) {
      return {};
    }
    const std::span<const std::byte> bytes = image.EncodedBytes();
    if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max()) {
      return {};
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(wic_factory_->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
            static_cast<DWORD>(bytes.size())
        )) ||
        FAILED(
            wic_factory_
                ->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())
        ) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(wic_factory_->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut
        )) ||
        FAILED(device_context_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf()))) {
      return {};
    }
    const D2D1_SIZE_U pixel_size = bitmap->GetPixelSize();
    const std::size_t byte_size =
        pixel_size.width > std::numeric_limits<std::size_t>::max() / 4U / std::max(1U, pixel_size.height)
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(pixel_size.width) * pixel_size.height * 4U;
    constexpr std::size_t image_cache_budget = 64U * 1024U * 1024U;
    if (byte_size > image_cache_budget) {
      // Retain one oversized image so repeated frames do not decode it again; the next insertion evicts it.
      images_.clear();
      images_.push_back({identity, byte_size, bitmap});
      image_cache_bytes_ = byte_size;
      return images_.back().bitmap;
    }
    while (!images_.empty() && image_cache_bytes_ > image_cache_budget - byte_size) {
      image_cache_bytes_ -= images_.front().byte_size;
      images_.erase(images_.begin());
    }
    images_.push_back({identity, byte_size, bitmap});
    image_cache_bytes_ += byte_size;
    return bitmap;
  }

  void RenderCommand(const DrawImageCommand& command) {
    if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
      return;
    }
    ComPtr<ID2D1Bitmap1> bitmap = ImageBitmapFor(command.image);
    if (!bitmap) {
      return;
    }
    const float scale = command.image.Scale();
    const D2D1_RECT_F source = D2D1::RectF(
        command.source.x * scale,
        command.source.y * scale,
        (command.source.x + command.source.width) * scale,
        (command.source.y + command.source.height) * scale
    );
    device_context_->DrawBitmap(
        bitmap.Get(),
        ToD2DRect(command.destination),
        command.opacity,
        command.sampling == ImageSampling::Nearest ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                   : D2D1_INTERPOLATION_MODE_LINEAR,
        source
    );
  }

  ComPtr<ID2D1GradientStopCollection> CreateGradientStops(const std::vector<GradientStop>& stops) {
    std::vector<D2D1_GRADIENT_STOP> native;
    native.reserve(stops.size());
    for (const GradientStop& stop : stops) {
      native.push_back({stop.offset, ToD2DColor(stop.color)});
    }
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(device_context_->CreateGradientStopCollection(
            native.data(),
            static_cast<UINT32>(native.size()),
            collection.GetAddressOf()
        ))) {
      return {};
    }
    return collection;
  }

  ComPtr<ID2D1LinearGradientBrush> CreateLinearGradientBrush(Rect rect, const LinearGradient& value) {
    const ComPtr<ID2D1GradientStopCollection> stops = CreateGradientStops(value.stops);
    if (!stops) {
      return {};
    }
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(device_context_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(value.start.x, value.start.y), D2D1::Point2F(value.end.x, value.end.y)
            ),
            stops.Get(),
            gradient.GetAddressOf()))) {
      return {};
    }
    gradient->SetTransform(ToD2DTransform(ResolveGradientTransform(rect, value)));
    return gradient;
  }

  ComPtr<ID2D1RadialGradientBrush> CreateRadialGradientBrush(Rect rect, const RadialGradient& value) {
    const ComPtr<ID2D1GradientStopCollection> stops = CreateGradientStops(value.stops);
    if (!stops) {
      return {};
    }
    ComPtr<ID2D1RadialGradientBrush> gradient;
    if (FAILED(device_context_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(value.center.x, value.center.y), D2D1::Point2F(), value.radius.width, value.radius.width
            ),
            stops.Get(), gradient.GetAddressOf()))) {
      return {};
    }
    gradient->SetTransform(ToD2DTransform(ResolveGradientTransform(rect, value)));
    return gradient;
  }

  template <class Brush>
  void FillGradientRect(Rect rect, float corner_radius, Brush* brush) {
    if (corner_radius > 0.0F) {
      device_context_->FillRoundedRectangle(
          D2D1::RoundedRect(ToD2DRect(rect), corner_radius, corner_radius),
          brush
      );
    } else {
      device_context_->FillRectangle(ToD2DRect(rect), brush);
    }
  }

  void RenderCommand(const DrawLinearGradientCommand& command) {
    if (command.rect.IsEmpty()) {
      return;
    }
    const ComPtr<ID2D1LinearGradientBrush> gradient = CreateLinearGradientBrush(command.rect, command.gradient);
    if (gradient) {
      FillGradientRect(command.rect, command.corner_radius, gradient.Get());
    }
  }

  void RenderCommand(const DrawRadialGradientCommand& command) {
    if (command.rect.IsEmpty()) {
      return;
    }
    const ComPtr<ID2D1RadialGradientBrush> gradient = CreateRadialGradientBrush(command.rect, command.gradient);
    if (gradient) {
      FillGradientRect(command.rect, command.corner_radius, gradient.Get());
    }
  }

  void UploadExternalTextureFrame(CachedExternalTexture& cached, const Win32ExternalTextureFrame& frame) {
    const std::span<const std::byte> pixels = frame.Pixels();
    const auto pitch = static_cast<UINT32>(frame.BytesPerRow());
    if (cached.bitmap) {
      const D2D1_SIZE_U size = cached.bitmap->GetPixelSize();
      if (size.width == static_cast<UINT32>(frame.PixelWidth()) &&
          size.height == static_cast<UINT32>(frame.PixelHeight())) {
        ThrowIfFailed(
            cached.bitmap->CopyFromMemory(nullptr, pixels.data(), pitch),
            "HuxerUI could not update the Windows external texture bitmap"
        );
        return;
      }
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        kDipsPerInch,
        kDipsPerInch
    );
    ComPtr<ID2D1Bitmap1> bitmap;
    ThrowIfFailed(
        device_context_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(frame.PixelWidth()), static_cast<UINT32>(frame.PixelHeight())),
            pixels.data(),
            pitch,
            &properties,
            bitmap.GetAddressOf()
        ),
        "HuxerUI could not create the Windows external texture bitmap"
    );
    cached.bitmap = std::move(bitmap);
  }

  ID2D1Bitmap1* ExternalTextureBitmapFor(const ExternalTexture& texture) {
    const std::shared_ptr<Win32ExternalTextureState> source =
        std::dynamic_pointer_cast<Win32ExternalTextureState>(ExternalTextureState::From(texture));
    if (!source) {
      throw std::logic_error("HuxerUI external texture does not contain a Windows pixel frame source");
    }
    auto cached = std::ranges::find_if(external_textures_, [&source](const CachedExternalTexture& entry) {
      return entry.source.lock() == source;
    });
    if (cached == external_textures_.end()) {
      external_textures_.push_back({.source = source});
      cached = external_textures_.end() - 1;
    }
    if (cached->draw_epoch == external_texture_draw_epoch_) {
      return cached->bitmap.Get();
    }
    cached->draw_epoch = external_texture_draw_epoch_;

    std::optional<Win32ExternalTextureFrame> pending = source->AcquireLatestFrame();
    if (pending.has_value()) {
      UploadExternalTextureFrame(*cached, *pending);
      cached->frame = std::move(*pending);
    } else if (!cached->bitmap && cached->frame.has_value()) {
      UploadExternalTextureFrame(*cached, *cached->frame);
    }
    return cached->bitmap.Get();
  }

  void RenderCommand(const DrawExternalTextureCommand& command) {
    if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
      return;
    }
    ID2D1Bitmap1* bitmap = ExternalTextureBitmapFor(command.texture);
    if (bitmap == nullptr) {
      return;
    }
    const Size intrinsic_size = command.texture.IntrinsicSize();
    const D2D1_SIZE_U pixel_size = bitmap->GetPixelSize();
    const float scale_x = static_cast<float>(pixel_size.width) / intrinsic_size.width;
    const float scale_y = static_cast<float>(pixel_size.height) / intrinsic_size.height;
    const D2D1_RECT_F source = D2D1::RectF(
        command.source.x * scale_x,
        command.source.y * scale_y,
        (command.source.x + command.source.width) * scale_x,
        (command.source.y + command.source.height) * scale_y
    );
    device_context_->DrawBitmap(
        bitmap,
        ToD2DRect(command.destination),
        command.opacity,
        command.sampling == ImageSampling::Nearest ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                   : D2D1_INTERPOLATION_MODE_LINEAR,
        source
    );
  }

  ComPtr<ID2D1StrokeStyle> CreateStrokeStyle(const StrokeStyle& stroke) const {
    ComPtr<ID2D1StrokeStyle> result;
    const D2D1_CAP_STYLE d2d_cap = ToD2DCap(stroke.cap);
    std::vector<float> dashes;
    if (!stroke.dash_pattern.empty()) {
      dashes.reserve(stroke.dash_pattern.size());
      for (const float length : stroke.dash_pattern) {
        dashes.push_back(length / stroke.width);
      }
    }
    const D2D1_STROKE_STYLE_PROPERTIES properties{
        d2d_cap,
        d2d_cap,
        d2d_cap,
        ToD2DJoin(stroke.join),
        stroke.miter_limit,
        dashes.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM,
        dashes.empty() ? 0.0F : stroke.dash_offset / stroke.width,
    };
    if (FAILED(d2d_factory_->CreateStrokeStyle(
            properties,
            dashes.empty() ? nullptr : dashes.data(),
            static_cast<UINT32>(dashes.size()),
            result.GetAddressOf()
        ))) {
      return {};
    }
    return result;
  }

  ComPtr<ID2D1PathGeometry> CreatePathGeometry(const Path& path, PathFillRule fill_rule) const {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(d2d_factory_->CreatePathGeometry(geometry.GetAddressOf()))) {
      return {};
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf()))) {
      return {};
    }
    sink->SetFillMode(fill_rule == PathFillRule::EvenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    bool figure_open = false;
    for (const PathElement& element : PathAccess::Elements(path)) {
      switch (element.verb) {
      case PathVerb::MoveTo:
        if (figure_open) {
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        sink->BeginFigure(D2D1::Point2F(element.points[0].x, element.points[0].y), D2D1_FIGURE_BEGIN_FILLED);
        figure_open = true;
        break;
      case PathVerb::LineTo:
        sink->AddLine(D2D1::Point2F(element.points[0].x, element.points[0].y));
        break;
      case PathVerb::QuadraticTo:
        sink->AddQuadraticBezier(
            D2D1::QuadraticBezierSegment(
                D2D1::Point2F(element.points[0].x, element.points[0].y),
                D2D1::Point2F(element.points[1].x, element.points[1].y)
            )
        );
        break;
      case PathVerb::CubicTo:
        sink->AddBezier(
            D2D1::BezierSegment(
                D2D1::Point2F(element.points[0].x, element.points[0].y),
                D2D1::Point2F(element.points[1].x, element.points[1].y),
                D2D1::Point2F(element.points[2].x, element.points[2].y)
            )
        );
        break;
      case PathVerb::Close:
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        figure_open = false;
        break;
      }
    }
    if (figure_open) {
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
    }
    if (FAILED(sink->Close())) {
      return {};
    }
    return geometry;
  }

  ComPtr<ID2D1PathGeometry> PathGeometryFor(const Path& path, PathFillRule fill_rule) {
    const auto existing = std::find_if(path_geometries_.begin(), path_geometries_.end(), [&](const auto& entry) {
      return entry.fill_rule == fill_rule && entry.path == path;
    });
    if (existing != path_geometries_.end()) {
      return existing->geometry;
    }
    ComPtr<ID2D1PathGeometry> geometry = CreatePathGeometry(path, fill_rule);
    if (!geometry) {
      return {};
    }
    constexpr std::size_t maximum_path_geometries = 128;
    if (path_geometries_.size() >= maximum_path_geometries) {
      path_geometries_.erase(path_geometries_.begin());
    }
    path_geometries_.push_back({path, fill_rule, geometry});
    return geometry;
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

  void RenderCommand(const DrawLineCommand& command) {
    if (command.start == command.end || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    ComPtr<ID2D1StrokeStyle> stroke_style = CreateStrokeStyle(command.style);
    if (!stroke_style) {
      return;
    }
    SetBrushColor(command.color);
    device_context_->DrawLine(
        D2D1::Point2F(command.start.x, command.start.y),
        D2D1::Point2F(command.end.x, command.end.y),
        brush_.Get(),
        command.style.width,
        stroke_style.Get()
    );
  }

  void RenderCommand(const DrawArcCommand& command) {
    if (command.radius <= 0.0F || command.style.width <= 0.0F || command.color.alpha <= 0.0F ||
        !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
      return;
    }

    SetBrushColor(command.color);
    ComPtr<ID2D1StrokeStyle> stroke_style = CreateStrokeStyle(command.style);
    const bool full_circle = std::abs(command.sweep_angle) >= kFullCircle - 0.0001F;
    if (full_circle && command.style.dash_pattern.empty()) {
      device_context_->DrawEllipse(
          D2D1::Ellipse(D2D1::Point2F(command.center.x, command.center.y), command.radius, command.radius),
          brush_.Get(),
          command.style.width,
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
    const D2D1_SWEEP_DIRECTION direction =
        command.sweep_angle > 0.0F ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    if (full_circle) {
      const float middle_angle = command.start_angle + std::copysign(kFullCircle * 0.5F, command.sweep_angle);
      sink->AddArc(
          D2D1::ArcSegment(
              D2D1::Point2F(
                  command.center.x + std::cos(middle_angle) * command.radius,
                  command.center.y + std::sin(middle_angle) * command.radius
              ),
              D2D1::SizeF(command.radius, command.radius),
              0.0F,
              direction,
              D2D1_ARC_SIZE_SMALL
          )
      );
      sink->AddArc(
          D2D1::ArcSegment(start, D2D1::SizeF(command.radius, command.radius), 0.0F, direction, D2D1_ARC_SIZE_SMALL)
      );
    } else {
      sink->AddArc(
          D2D1::ArcSegment(
              end,
              D2D1::SizeF(command.radius, command.radius),
              0.0F,
              direction,
              std::abs(command.sweep_angle) > 3.14159265358979323846F ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL
          )
      );
    }
    sink->EndFigure(full_circle ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
      return;
    }

    device_context_->DrawGeometry(geometry.Get(), brush_.Get(), command.style.width, stroke_style.Get());
  }

  void RenderCommand(const DrawBorderCommand& command) {
    if (command.style.width <= 0.0F || command.color.alpha <= 0.0F || command.rect.width <= 0.0F ||
        command.rect.height <= 0.0F) {
      return;
    }
    if (!command.style.dash_pattern.empty() || command.style.join != StrokeJoin::Miter ||
        command.style.miter_limit != 4.0F) {
      RenderCommand(StrokePathCommand{
          CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width),
          command.color,
          command.style,
      });
      return;
    }
    const float inset = command.style.width * 0.5F;
    const Rect rect{
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.style.width),
        std::max(0.0F, command.rect.height - command.style.width),
    };
    const float radius = std::max(0.0F, command.corner_radius - inset);
    SetBrushColor(command.color);
    device_context_
        ->DrawRoundedRectangle(D2D1::RoundedRect(ToD2DRect(rect), radius, radius), brush_.Get(), command.style.width);
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
        FAILED(shadow_effect_->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, kShadowOptimizationBalanced))) {
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

  void RenderCommand(const FillPathCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, command.fill_rule);
    if (!geometry) {
      return;
    }
    SetBrushColor(command.color);
    device_context_->FillGeometry(geometry.Get(), brush_.Get());
  }

  void RenderCommand(const FillLinearGradientPathCommand& command) {
    if (command.path.IsEmpty() || command.gradient_rect.IsEmpty()) {
      return;
    }
    const ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, command.fill_rule);
    const ComPtr<ID2D1LinearGradientBrush> gradient =
        CreateLinearGradientBrush(command.gradient_rect, command.gradient);
    if (geometry && gradient) {
      device_context_->FillGeometry(geometry.Get(), gradient.Get());
    }
  }

  void RenderCommand(const FillRadialGradientPathCommand& command) {
    if (command.path.IsEmpty() || command.gradient_rect.IsEmpty()) {
      return;
    }
    const ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, command.fill_rule);
    const ComPtr<ID2D1RadialGradientBrush> gradient =
        CreateRadialGradientBrush(command.gradient_rect, command.gradient);
    if (geometry && gradient) {
      device_context_->FillGeometry(geometry.Get(), gradient.Get());
    }
  }

  void RenderCommand(const StrokePathCommand& command) {
    if (command.path.IsEmpty() || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, PathFillRule::NonZero);
    ComPtr<ID2D1StrokeStyle> stroke_style = CreateStrokeStyle(command.style);
    if (!geometry || !stroke_style) {
      return;
    }
    SetBrushColor(command.color);
    device_context_->DrawGeometry(geometry.Get(), brush_.Get(), command.style.width, stroke_style.Get());
  }

  void RenderCommand(const StrokeLinearGradientPathCommand& command) {
    if (command.path.IsEmpty() || command.gradient_rect.IsEmpty() || command.style.width <= 0.0F) {
      return;
    }
    const ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, PathFillRule::NonZero);
    const ComPtr<ID2D1StrokeStyle> stroke_style = CreateStrokeStyle(command.style);
    const ComPtr<ID2D1LinearGradientBrush> gradient =
        CreateLinearGradientBrush(command.gradient_rect, command.gradient);
    if (geometry && stroke_style && gradient) {
      device_context_->DrawGeometry(geometry.Get(), gradient.Get(), command.style.width, stroke_style.Get());
    }
  }

  void RenderCommand(const StrokeRadialGradientPathCommand& command) {
    if (command.path.IsEmpty() || command.gradient_rect.IsEmpty() || command.style.width <= 0.0F) {
      return;
    }
    const ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, PathFillRule::NonZero);
    const ComPtr<ID2D1StrokeStyle> stroke_style = CreateStrokeStyle(command.style);
    const ComPtr<ID2D1RadialGradientBrush> gradient =
        CreateRadialGradientBrush(command.gradient_rect, command.gradient);
    if (geometry && stroke_style && gradient) {
      device_context_->DrawGeometry(geometry.Get(), gradient.Get(), command.style.width, stroke_style.Get());
    }
  }

  void RenderCommand(const DrawPathShadowCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, command.fill_rule);
    if (!geometry) {
      return;
    }

    D2D1_MATRIX_3X2_F previous;
    device_context_->GetTransform(&previous);
    const D2D1_MATRIX_3X2_F translation = D2D1::Matrix3x2F::Translation(command.offset.x, command.offset.y);
    if (command.blur_radius <= 0.0F) {
      device_context_->SetTransform(translation * previous);
      SetBrushColor(command.color);
      device_context_->FillGeometry(geometry.Get(), brush_.Get());
      device_context_->SetTransform(previous);
      return;
    }
    if (!EnsureShadowResources()) {
      return;
    }

    ComPtr<ID2D1CommandList> commands;
    if (FAILED(shadow_context_->CreateCommandList(commands.GetAddressOf()))) {
      return;
    }
    shadow_context_->SetTarget(commands.Get());
    shadow_context_->BeginDraw();
    shadow_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    shadow_context_->FillGeometry(geometry.Get(), shadow_brush_.Get());
    const HRESULT draw_result = shadow_context_->EndDraw();
    shadow_context_->SetTarget(nullptr);
    if (FAILED(draw_result) || FAILED(commands->Close())) {
      return;
    }

    const Rect bounds = command.path.Bounds();
    ComPtr<ID2D1RectangleGeometry> outer;
    ComPtr<ID2D1PathGeometry> exterior;
    ComPtr<ID2D1GeometrySink> exterior_sink;
    if (FAILED(d2d_factory_->CreateRectangleGeometry(
            D2D1::RectF(
                bounds.x - command.blur_radius,
                bounds.y - command.blur_radius,
                bounds.x + bounds.width + command.blur_radius,
                bounds.y + bounds.height + command.blur_radius
            ),
            outer.GetAddressOf()
        )) ||
        FAILED(d2d_factory_->CreatePathGeometry(exterior.GetAddressOf())) ||
        FAILED(exterior->Open(exterior_sink.GetAddressOf())) ||
        FAILED(outer->CombineWithGeometry(
            geometry.Get(),
            D2D1_COMBINE_MODE_EXCLUDE,
            nullptr,
            D2D1_DEFAULT_FLATTENING_TOLERANCE,
            exterior_sink.Get()
        )) ||
        FAILED(exterior_sink->Close())) {
      return;
    }

    shadow_effect_->SetInput(0, commands.Get());
    if (FAILED(shadow_effect_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, command.blur_radius / 3.0F)) ||
        FAILED(shadow_effect_->SetValue(
            D2D1_SHADOW_PROP_COLOR,
            D2D1_VECTOR_4F{command.color.red, command.color.green, command.color.blue, command.color.alpha}
        )) ||
        FAILED(shadow_effect_->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, kShadowOptimizationBalanced))) {
      shadow_effect_->SetInput(0, nullptr);
      return;
    }

    device_context_->SetTransform(translation * previous);
    device_context_->PushLayer(
        D2D1::LayerParameters1(
            D2D1::InfiniteRect(),
            exterior.Get(),
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

  void RenderCommand(const PushPathClipCommand& command) {
    ComPtr<ID2D1PathGeometry> geometry = PathGeometryFor(command.path, command.fill_rule);
    ComPtr<ID2D1Layer> layer;
    if (!geometry || FAILED(device_context_->CreateLayer(nullptr, layer.GetAddressOf()))) {
      device_context_->PushAxisAlignedClip(D2D1::RectF(0.0F, 0.0F, 0.0F, 0.0F), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
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

  void RenderCommand(const PlacePlatformViewCommand& command) {
    if (!platform_composition_) {
      throw std::logic_error("HuxerUI Windows PlatformView requires DirectComposition");
    }
    const D2D1_PRIMITIVE_BLEND previous = device_context_->GetPrimitiveBlend();
    const D2D1_ANTIALIAS_MODE previous_antialias = device_context_->GetAntialiasMode();
    device_context_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    device_context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    brush_->SetColor(D2D1::ColorF(0.0F, 0.0F));
    device_context_->FillRectangle(ToD2DRect(command.Bounds()), brush_.Get());
    device_context_->SetAntialiasMode(previous_antialias);
    device_context_->SetPrimitiveBlend(previous);
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
  bool platform_composition_ = false;
  ComPtr<ID2D1Factory1> d2d_factory_;
  ComPtr<IDWriteFactory> write_factory_;
  ComPtr<IWICImagingFactory> wic_factory_;
  ComPtr<ID3D11Device> d3d_device_;
  ComPtr<ID2D1Device> d2d_device_;
  ComPtr<ID2D1DeviceContext> device_context_;
  ComPtr<ID2D1DeviceContext> shadow_context_;
  ComPtr<IDXGISwapChain1> swap_chain_;
  ComPtr<IDCompositionDevice> composition_device_;
  ComPtr<IDCompositionTarget> composition_target_;
  ComPtr<IDCompositionVisual> composition_visual_;
  ComPtr<ID2D1Bitmap1> swap_chain_target_;
  ComPtr<ID2D1Bitmap1> scene_target_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<ID2D1SolidColorBrush> shadow_brush_;
  ComPtr<ID2D1Effect> shadow_effect_;
  ComPtr<ID2D1Layer> shadow_clip_layer_;
  std::vector<ShadowMask> shadow_masks_;
  std::vector<CachedPathGeometry> path_geometries_;
  std::vector<CachedImage> images_;
  std::size_t image_cache_bytes_ = 0;
  std::vector<CachedExternalTexture> external_textures_;
  std::uint64_t external_texture_draw_epoch_ = 0;
  std::unordered_map<Font, FontMetrics, FontHash> font_metrics_;
  std::unordered_map<TextRunKey, CachedTextRun, TextRunKeyHash, TextRunKeyEqual> text_runs_;
  std::unordered_map<ParagraphKey, CachedParagraph, ParagraphKeyHash, ParagraphKeyEqual> paragraphs_;
  std::vector<ClipState> clip_stack_;
  std::vector<D2D1_MATRIX_3X2_F> transform_stack_;
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  HMODULE dcomp_library_ = nullptr;
#endif
};

Win32Renderer::Win32Renderer() : state_(std::make_unique<State>()) {}

Win32Renderer::~Win32Renderer() = default;

void Win32Renderer::Initialize() {
  state_->InitializeFactories();
}

void Win32Renderer::Discard() noexcept {
  state_->DiscardDeviceResources();
  state_->external_textures_.clear();
  state_->platform_composition_ = false;
  state_->paragraphs_.clear();
  state_->text_runs_.clear();
  state_->font_metrics_.clear();
  state_->wic_factory_.Reset();
  state_->write_factory_.Reset();
  state_->d2d_factory_.Reset();
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  if (state_->dcomp_library_ != nullptr) {
    FreeLibrary(state_->dcomp_library_);
    state_->dcomp_library_ = nullptr;
  }
#endif
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

bool Win32Renderer::EnablePlatformComposition(HWND window) {
  return state_->EnablePlatformComposition(window);
}

FontMetrics Win32Renderer::Metrics(const Font& font) {
  return state_->Metrics(font);
}

TextRunMetrics
Win32Renderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  return state_->MeasureRun(text, style, options);
}

TextLayoutMetrics Win32Renderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return state_->MeasureText(text, style, max_width, options);
}

std::unique_ptr<TextLayout> Win32Renderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return state_->CreateTextLayout(text, style, max_width, options);
}

Win32RenderResult Win32Renderer::Render(HWND window, float dpi, const RenderFrame& frame, const RECT& paint_rect) {
  state_->window_ = window;
  state_->dpi_ = dpi;
  return state_->Render(frame, paint_rect);
}

} // namespace huxerui::detail
