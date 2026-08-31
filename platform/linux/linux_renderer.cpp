#include "linux_renderer.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "external_texture_internal.h"
#include "linux_external_texture_internal.h"
#include "path_internal.h"
#include "paint_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

float PangoUnits(int value) noexcept {
  return static_cast<float>(value) / static_cast<float>(PANGO_SCALE);
}

PangoDirection PangoTextDirection(TextDirection direction) noexcept {
  switch (direction) {
  case TextDirection::LeftToRight:
    return PANGO_DIRECTION_LTR;
  case TextDirection::RightToLeft:
    return PANGO_DIRECTION_RTL;
  case TextDirection::Auto:
    return PANGO_DIRECTION_NEUTRAL;
  }
  return PANGO_DIRECTION_NEUTRAL;
}

PangoFontDescription* CreateFontDescription(const Font& font) {
  PangoFontDescription* description = pango_font_description_new();
  switch (font.FamilyKind()) {
  case FontFamilyKind::System:
    pango_font_description_set_family(description, "sans-serif");
    break;
  case FontFamilyKind::Monospace:
    pango_font_description_set_family(description, "monospace");
    break;
  case FontFamilyKind::Named:
    pango_font_description_set_family(description, std::string(font.FamilyName()).c_str());
    break;
  }
  pango_font_description_set_absolute_size(
      description,
      std::max(0.0F, font.Size()) * static_cast<float>(PANGO_SCALE)
  );
  pango_font_description_set_weight(description, static_cast<PangoWeight>(font.Weight()));
  pango_font_description_set_style(
      description,
      font.Slant() == FontSlant::Italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL
  );
  return description;
}

PangoAttrList* CreateTextAttributes(const TextStyle& style, const TextShapingOptions& shaping) {
  PangoAttrList* attributes = pango_attr_list_new();
  if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
    pango_attr_list_insert(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
  }
  if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
    pango_attr_list_insert(attributes, pango_attr_strikethrough_new(TRUE));
  }
  if (!shaping.locale.empty()) {
    pango_attr_list_insert(attributes, pango_attr_language_new(pango_language_from_string(shaping.locale.c_str())));
  }
  return attributes;
}

void ConfigureLayout(
    PangoLayout* layout,
    std::string_view text,
    const TextStyle& style,
    float max_width,
    const TextLayoutOptions& options
) {
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("HuxerUI Linux text is too large");
  }
  pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
  PangoFontDescription* description = CreateFontDescription(style.font);
  pango_layout_set_font_description(layout, description);
  pango_font_description_free(description);
  PangoAttrList* attributes = CreateTextAttributes(style, options.shaping);
  pango_layout_set_attributes(layout, attributes);
  pango_attr_list_unref(attributes);

  const PangoDirection direction = PangoTextDirection(options.shaping.direction);
  pango_layout_set_auto_dir(layout, direction == PANGO_DIRECTION_NEUTRAL);
  PangoContext* context = pango_layout_get_context(layout);
  pango_context_set_base_dir(context, direction == PANGO_DIRECTION_NEUTRAL ? PANGO_DIRECTION_LTR : direction);

  const bool constrained = std::isfinite(max_width) && max_width > 0.0F;
  if (constrained && options.wrap == TextWrap::Word) {
    pango_layout_set_width(layout, static_cast<int>(std::ceil(max_width * PANGO_SCALE)));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  } else {
    pango_layout_set_width(layout, -1);
    pango_layout_set_single_paragraph_mode(layout, FALSE);
  }

  const bool rtl = direction == PANGO_DIRECTION_RTL;
  switch (options.align) {
  case TextAlign::Leading:
    pango_layout_set_alignment(layout, rtl ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    break;
  case TextAlign::Center:
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    break;
  case TextAlign::Trailing:
    pango_layout_set_alignment(layout, rtl ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);
    break;
  }
}

std::optional<TextOffset> Utf8ByteToUtf16(std::string_view text, int byte_offset) noexcept {
  if (byte_offset < 0 || static_cast<std::size_t>(byte_offset) > text.size() ||
      !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  TextOffset result = 0;
  const char* current = text.data();
  const char* end = text.data() + byte_offset;
  while (current < end) {
    const gunichar code_point = g_utf8_get_char(current);
    current = g_utf8_next_char(current);
    if (current > end) {
      return std::nullopt;
    }
    result += code_point > 0xFFFFU ? 2 : 1;
  }
  return result;
}

std::optional<int> Utf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept {
  if (offset < 0 || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  TextOffset utf16 = 0;
  const char* current = text.data();
  const char* end = text.data() + text.size();
  while (current < end && utf16 < offset) {
    const gunichar code_point = g_utf8_get_char(current);
    const TextOffset units = code_point > 0xFFFFU ? 2 : 1;
    if (utf16 + units > offset) {
      return std::nullopt;
    }
    utf16 += units;
    current = g_utf8_next_char(current);
  }
  if (utf16 != offset) {
    return std::nullopt;
  }
  return static_cast<int>(current - text.data());
}

TextLayoutMetrics LayoutMetrics(PangoLayout* layout) {
  PangoRectangle logical{};
  pango_layout_get_extents(layout, nullptr, &logical);
  TextLayoutMetrics result{
      .size = {std::ceil(PangoUnits(logical.width)), std::ceil(PangoUnits(logical.height))},
      .line_count = static_cast<std::size_t>(std::max(1, pango_layout_get_line_count(layout))),
  };
  PangoLayoutIter* iterator = pango_layout_get_iter(layout);
  if (iterator != nullptr) {
    result.first_baseline = PangoUnits(pango_layout_iter_get_baseline(iterator));
    result.last_baseline = result.first_baseline;
    while (pango_layout_iter_next_line(iterator)) {
      result.last_baseline = PangoUnits(pango_layout_iter_get_baseline(iterator));
    }
    pango_layout_iter_free(iterator);
  }
  return result;
}

class PangoTextLayout final : public TextLayout {
public:
  PangoTextLayout(
      PangoContext* context,
      std::string_view text,
      const TextStyle& style,
      float max_width,
      const TextLayoutOptions& options
  ) : text_(text) {
    PangoFontMap* font_map = pango_context_get_font_map(context);
    PangoContext* layout_context = pango_font_map_create_context(font_map);
    pango_context_set_language(layout_context, pango_context_get_language(context));
    pango_cairo_context_set_resolution(layout_context, pango_cairo_context_get_resolution(context));
    layout_ = pango_layout_new(layout_context);
    g_object_unref(layout_context);
    ConfigureLayout(layout_, text_, style, max_width, options);
    metrics_ = LayoutMetrics(layout_);
    if (options.wrap == TextWrap::NoWrap && std::isfinite(max_width) && metrics_.size.width < max_width) {
      pango_layout_set_width(layout_, static_cast<int>(std::ceil(max_width * PANGO_SCALE)));
    }
    BuildCaretOffsets();
  }

  ~PangoTextLayout() override {
    g_object_unref(layout_);
  }

  Size Measure() const override {
    return metrics_.size;
  }

  const TextLayoutMetrics& MetricsValue() const noexcept {
    return metrics_;
  }

  TextPosition HitTest(Point point) const override {
    int byte_index = 0;
    int trailing = 0;
    const gboolean inside = pango_layout_xy_to_index(
        layout_,
        static_cast<int>(std::lround(point.x * PANGO_SCALE)),
        static_cast<int>(std::lround(point.y * PANGO_SCALE)),
        &byte_index,
        &trailing
    );
    const char* current = text_.data() + std::clamp(byte_index, 0, static_cast<int>(text_.size()));
    const char* end = text_.data() + text_.size();
    while (trailing-- > 0 && current < end) {
      current = g_utf8_next_char(current);
    }
    const int resolved_byte = static_cast<int>(current - text_.data());
    const TextOffset offset = Utf8ByteToUtf16(text_, resolved_byte).value_or(0);
    return {offset, inside ? TextAffinity::Downstream : TextAffinity::Upstream};
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const int byte_index = Utf16ToUtf8Byte(text_, offset).value_or(static_cast<int>(text_.size()));
    PangoRectangle strong{};
    PangoRectangle weak{};
    pango_layout_get_cursor_pos(layout_, byte_index, &strong, &weak);
    const PangoRectangle& selected = affinity == TextAffinity::Upstream ? weak : strong;
    return {
        PangoUnits(selected.x),
        PangoUnits(selected.y),
        1.0F,
        std::ceil(PangoUnits(selected.height)),
    };
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const int start = Utf16ToUtf8Byte(text_, std::max<TextOffset>(0, range.start)).value_or(0);
    const int end = Utf16ToUtf8Byte(text_, std::max(range.start, range.end)).value_or(static_cast<int>(text_.size()));
    if (start >= end) {
      return {};
    }
    std::vector<Rect> result;
    PangoLayoutIter* iterator = pango_layout_get_iter(layout_);
    if (iterator == nullptr) {
      return result;
    }
    do {
      PangoLayoutLine* line = pango_layout_iter_get_line_readonly(iterator);
      if (line == nullptr) {
        continue;
      }
      const int line_start = line->start_index;
      const int line_end = line_start + line->length;
      const int selected_start = std::max(start, line_start);
      const int selected_end = std::min(end, line_end);
      if (selected_start >= selected_end) {
        continue;
      }
      int* ranges = nullptr;
      int range_count = 0;
      pango_layout_line_get_x_ranges(line, selected_start, selected_end, &ranges, &range_count);
      PangoRectangle logical{};
      pango_layout_iter_get_line_extents(iterator, nullptr, &logical);
      std::vector<Rect> line_rects;
      line_rects.reserve(static_cast<std::size_t>(range_count));
      for (int range_index = 0; range_index < range_count; ++range_index) {
        const int first_x = ranges[range_index * 2];
        const int last_x = ranges[range_index * 2 + 1];
        line_rects.push_back({
            PangoUnits(logical.x + std::min(first_x, last_x)),
            PangoUnits(logical.y),
            PangoUnits(std::abs(last_x - first_x)),
            std::ceil(PangoUnits(logical.height)),
        });
      }
      g_free(ranges);
      std::ranges::sort(line_rects, {}, &Rect::x);
      for (const Rect& rect : line_rects) {
        if (!result.empty() && result.back().y == rect.y &&
            rect.x <= result.back().x + result.back().width + 1.0F / PANGO_SCALE) {
          result.back().width = std::max(result.back().x + result.back().width, rect.x + rect.width) - result.back().x;
        } else {
          result.push_back(rect);
        }
      }
    } while (pango_layout_iter_next_line(iterator));
    pango_layout_iter_free(iterator);
    return result;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto iterator = std::lower_bound(caret_offsets_.begin(), caret_offsets_.end(), offset);
    return iterator == caret_offsets_.begin() ? 0 : *std::prev(iterator);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto iterator = std::upper_bound(caret_offsets_.begin(), caret_offsets_.end(), offset);
    return iterator == caret_offsets_.end() ? caret_offsets_.back() : *iterator;
  }

private:
  void BuildCaretOffsets() {
    caret_offsets_.push_back(0);
    int attribute_count = 0;
    const PangoLogAttr* attributes = pango_layout_get_log_attrs_readonly(layout_, &attribute_count);
    const char* current = text_.data();
    const char* end = text_.data() + text_.size();
    for (int index = 1; index < attribute_count && current < end; ++index) {
      current = g_utf8_next_char(current);
      if (attributes[index].is_cursor_position != 0) {
        caret_offsets_.push_back(
            Utf8ByteToUtf16(text_, static_cast<int>(current - text_.data())).value_or(caret_offsets_.back())
        );
      }
    }
    const TextOffset length = Utf8ByteToUtf16(text_, static_cast<int>(text_.size())).value_or(0);
    if (caret_offsets_.back() != length) {
      caret_offsets_.push_back(length);
    }
  }

  std::string text_;
  PangoLayout* layout_ = nullptr;
  TextLayoutMetrics metrics_;
  std::vector<TextOffset> caret_offsets_;
};

void SetSourceColor(cairo_t* context, const Color& color) {
  cairo_set_source_rgba(context, color.red, color.green, color.blue, color.alpha);
}

cairo_line_cap_t CairoLineCap(StrokeCap cap) noexcept {
  switch (cap) {
  case StrokeCap::Butt:
    return CAIRO_LINE_CAP_BUTT;
  case StrokeCap::Round:
    return CAIRO_LINE_CAP_ROUND;
  case StrokeCap::Square:
    return CAIRO_LINE_CAP_SQUARE;
  }
  return CAIRO_LINE_CAP_BUTT;
}

cairo_line_join_t CairoLineJoin(StrokeJoin join) noexcept {
  switch (join) {
  case StrokeJoin::Miter:
    return CAIRO_LINE_JOIN_MITER;
  case StrokeJoin::Round:
    return CAIRO_LINE_JOIN_ROUND;
  case StrokeJoin::Bevel:
    return CAIRO_LINE_JOIN_BEVEL;
  }
  return CAIRO_LINE_JOIN_MITER;
}

void ApplyStrokeStyle(cairo_t* context, const StrokeStyle& style) {
  std::vector<double> dashes;
  dashes.reserve(style.dash_pattern.size());
  for (const float length : style.dash_pattern) {
    dashes.push_back(length);
  }
  cairo_set_line_width(context, style.width);
  cairo_set_line_cap(context, CairoLineCap(style.cap));
  cairo_set_line_join(context, CairoLineJoin(style.join));
  cairo_set_miter_limit(context, style.miter_limit);
  cairo_set_dash(context, dashes.empty() ? nullptr : dashes.data(), static_cast<int>(dashes.size()), style.dash_offset);
}

void AddRoundedRect(cairo_t* context, Rect rect, float corner_radius) {
  const float radius = std::clamp(corner_radius, 0.0F, std::min(rect.width, rect.height) * 0.5F);
  if (radius <= 0.0F) {
    cairo_rectangle(context, rect.x, rect.y, rect.width, rect.height);
    return;
  }
  const double right = rect.x + rect.width;
  const double bottom = rect.y + rect.height;
  cairo_new_sub_path(context);
  cairo_arc(context, right - radius, rect.y + radius, radius, -kPi / 2.0, 0.0);
  cairo_arc(context, right - radius, bottom - radius, radius, 0.0, kPi / 2.0);
  cairo_arc(context, rect.x + radius, bottom - radius, radius, kPi / 2.0, kPi);
  cairo_arc(context, rect.x + radius, rect.y + radius, radius, kPi, 3.0 * kPi / 2.0);
  cairo_close_path(context);
}

void AppendPathContour(cairo_t* context, const Path& path) {
  Point previous;
  for (const PathElement& element : PathAccess::Elements(path)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      cairo_move_to(context, element.points[0].x, element.points[0].y);
      previous = element.points[0];
      break;
    case PathVerb::LineTo:
      cairo_line_to(context, element.points[0].x, element.points[0].y);
      previous = element.points[0];
      break;
    case PathVerb::QuadraticTo: {
      const Point control = element.points[0];
      const Point end = element.points[1];
      cairo_curve_to(
          context,
          previous.x + 2.0 / 3.0 * (control.x - previous.x),
          previous.y + 2.0 / 3.0 * (control.y - previous.y),
          end.x + 2.0 / 3.0 * (control.x - end.x),
          end.y + 2.0 / 3.0 * (control.y - end.y),
          end.x,
          end.y
      );
      previous = end;
      break;
    }
    case PathVerb::CubicTo:
      cairo_curve_to(
          context,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y,
          element.points[2].x,
          element.points[2].y
      );
      previous = element.points[2];
      break;
    case PathVerb::Close:
      cairo_close_path(context);
      break;
    }
  }
}

void AppendPath(cairo_t* context, const Path& path) {
  cairo_new_path(context);
  AppendPathContour(context, path);
}

struct CairoSurfaceHandle {
  explicit CairoSurfaceHandle(cairo_surface_t* value) : surface(value) {}

  ~CairoSurfaceHandle() {
    if (surface != nullptr) {
      cairo_surface_destroy(surface);
    }
  }

  CairoSurfaceHandle(const CairoSurfaceHandle&) = delete;
  CairoSurfaceHandle& operator=(const CairoSurfaceHandle&) = delete;

  cairo_surface_t* surface = nullptr;
};

void ApplyTransform(cairo_t* context, const Transform2D& transform) {
  cairo_matrix_t matrix{};
  cairo_matrix_init(
      &matrix,
      transform.m11,
      transform.m12,
      transform.m21,
      transform.m22,
      transform.translate_x,
      transform.translate_y
  );
  cairo_transform(context, &matrix);
}

} // namespace

struct LinuxRenderer::State {
  struct CachedImage {
    std::uint64_t identity = 0;
    GdkPixbuf* pixbuf = nullptr;
    std::uint64_t bytes = 0;
  };

  struct CachedExternalTexture {
    std::weak_ptr<LinuxExternalTextureState> source;
    std::vector<std::byte> pixels;
    cairo_surface_t* surface = nullptr;
  };

  struct ShadowMaskKey {
    float width = 0.0F;
    float height = 0.0F;
    float corner_radius = 0.0F;
    float blur_radius = 0.0F;
    float scale = 1.0F;

    bool operator==(const ShadowMaskKey&) const = default;
  };

  struct ShadowMaskEntry {
    ShadowMaskKey key;
    cairo_surface_t* surface = nullptr;
    std::uint64_t bytes = 0;
  };

  struct PathShadowMaskKey {
    Path path;
    PathFillRule fill_rule = PathFillRule::NonZero;
    float blur_radius = 0.0F;
    float scale = 1.0F;

    bool operator==(const PathShadowMaskKey&) const = default;
  };

  struct PathShadowMaskEntry {
    PathShadowMaskKey key;
    cairo_surface_t* surface = nullptr;
    std::uint64_t bytes = 0;
  };

  State() {
    PangoFontMap* font_map = pango_cairo_font_map_get_default();
    context = pango_font_map_create_context(font_map);
    pango_cairo_context_set_resolution(context, 96.0);
  }

  ~State() {
    for (CachedImage& image : images) {
      g_object_unref(image.pixbuf);
    }
    for (CachedExternalTexture& texture : external_textures) {
      if (texture.surface != nullptr) {
        cairo_surface_destroy(texture.surface);
      }
    }
    for (ShadowMaskEntry& shadow : shadow_masks) {
      cairo_surface_destroy(shadow.surface);
    }
    for (PathShadowMaskEntry& shadow : path_shadow_masks) {
      cairo_surface_destroy(shadow.surface);
    }
    g_object_unref(context);
  }

  GdkPixbuf* ImageFor(const ImageAsset& image) {
    if (!image.HasValue()) {
      return nullptr;
    }
    const std::uint64_t identity = ResourceAccess::ImageIdentity(image);
    const auto existing = std::find_if(images.begin(), images.end(), [identity](const CachedImage& candidate) {
      return candidate.identity == identity;
    });
    if (existing != images.end()) {
      std::rotate(existing, existing + 1, images.end());
      return images.back().pixbuf;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* error = nullptr;
    const std::span<const std::byte> bytes = image.EncodedBytes();
    const gboolean wrote = gdk_pixbuf_loader_write(
        loader,
        reinterpret_cast<const guchar*>(bytes.data()),
        bytes.size(),
        &error
    );
    const gboolean closed = wrote != FALSE && gdk_pixbuf_loader_close(loader, &error);
    GdkPixbuf* pixbuf = closed != FALSE ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (pixbuf != nullptr) {
      g_object_ref(pixbuf);
      const std::uint64_t decoded_bytes = gdk_pixbuf_get_byte_length(pixbuf);
      while (!images.empty() &&
             (images.size() >= kMaxImages || image_cache_bytes + decoded_bytes > kImageCacheBudget)) {
        image_cache_bytes -= images.front().bytes;
        g_object_unref(images.front().pixbuf);
        images.erase(images.begin());
      }
      images.push_back({identity, pixbuf, decoded_bytes});
      image_cache_bytes += decoded_bytes;
    }
    if (error != nullptr) {
      g_error_free(error);
    }
    g_object_unref(loader);
    return pixbuf;
  }

  cairo_surface_t* ExternalTextureFor(const ExternalTexture& texture) {
    const std::shared_ptr<LinuxExternalTextureState> source =
        std::dynamic_pointer_cast<LinuxExternalTextureState>(ExternalTextureState::From(texture));
    if (!source) {
      throw std::logic_error("HuxerUI external texture does not contain a Linux frame source");
    }
    for (auto iterator = external_textures.begin(); iterator != external_textures.end();) {
      const std::shared_ptr<LinuxExternalTextureState> retained = iterator->source.lock();
      if (retained && retained->IsActive()) {
        ++iterator;
        continue;
      }
      if (iterator->surface != nullptr) {
        cairo_surface_destroy(iterator->surface);
      }
      iterator = external_textures.erase(iterator);
    }
    auto entry = std::find_if(external_textures.begin(), external_textures.end(), [&source](const auto& candidate) {
      return candidate.source.lock() == source;
    });
    if (entry == external_textures.end()) {
      entry = external_textures.insert(
          external_textures.end(),
          CachedExternalTexture{.source = source, .pixels = {}, .surface = nullptr}
      );
    }
    std::optional<LinuxExternalTextureFrame> frame = source->AcquireLatestFrame();
    if (frame.has_value()) {
      if (entry->surface != nullptr) {
        cairo_surface_destroy(entry->surface);
      }
      entry->pixels.assign(frame->Pixels().begin(), frame->Pixels().end());
      entry->surface = cairo_image_surface_create_for_data(
          reinterpret_cast<unsigned char*>(entry->pixels.data()),
          CAIRO_FORMAT_ARGB32,
          frame->PixelWidth(),
          frame->PixelHeight(),
          static_cast<int>(frame->BytesPerRow())
      );
    }
    return entry->surface;
  }

  PangoContext* context = nullptr;
  std::vector<CachedImage> images;
  std::vector<CachedExternalTexture> external_textures;
  std::vector<ShadowMaskEntry> shadow_masks;
  std::vector<PathShadowMaskEntry> path_shadow_masks;
  std::vector<unsigned char> blur_scratch;
  std::uint64_t shadow_mask_bytes = 0;
  std::uint64_t path_shadow_mask_bytes = 0;
  std::uint64_t image_cache_bytes = 0;
  static constexpr std::uint64_t kImageCacheBudget = 32 * 1024 * 1024;
  static constexpr std::size_t kMaxImages = 64;
  static constexpr std::uint64_t kShadowMaskBudget = 32 * 1024 * 1024;
  static constexpr std::uint64_t kPathShadowMaskBudget = 32 * 1024 * 1024;
  static constexpr std::size_t kMaxShadowMasks = 64;
  static constexpr std::size_t kMaxPathShadowMasks = 32;
};

namespace {

class ScenePainter final {
public:
  ScenePainter(LinuxRenderer::State& state, cairo_t* context) : state_(state), context_(context) {
    double horizontal_x = 1.0;
    double horizontal_y = 0.0;
    double vertical_x = 0.0;
    double vertical_y = 1.0;
    cairo_user_to_device_distance(context_, &horizontal_x, &horizontal_y);
    cairo_user_to_device_distance(context_, &vertical_x, &vertical_y);
    device_scale_ = static_cast<float>(std::max(
        std::hypot(horizontal_x, horizontal_y),
        std::hypot(vertical_x, vertical_y)
    ));
    device_scale_ = std::max(device_scale_, 0.001F);
  }

  void Draw(const RenderScene& scene) {
    if (scene.root != nullptr) {
      DrawNode(*scene.root);
    }
  }

private:
  int MaskDimension(float length) const noexcept {
    const double pixels = std::ceil(static_cast<double>(length) * device_scale_);
    if (!std::isfinite(pixels) || pixels <= 0.0 || pixels > std::numeric_limits<int>::max()) {
      return 0;
    }
    return std::max(1, static_cast<int>(pixels));
  }

  void DrawNode(const RenderNode& node) {
    const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
    if (!node.visible || opacity <= 0.0F) {
      return;
    }
    cairo_save(context_);
    cairo_translate(context_, node.offset.x, node.offset.y);
    ApplyTransform(context_, node.transform);
    if (opacity < 1.0F) {
      cairo_push_group(context_);
    }
    DrawSequence(node.content);
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this](const auto& value) { DrawCommand(value); }, clip);
    }
    cairo_save(context_);
    ApplyTransform(context_, node.children_transform);
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        DrawNode(*child);
      }
    }
    cairo_restore(context_);
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
      cairo_restore(context_);
    }
    DrawSequence(node.foreground);
    if (opacity < 1.0F) {
      cairo_pop_group_to_source(context_);
      cairo_paint_with_alpha(context_, opacity);
    }
    cairo_restore(context_);
  }

  void DrawSequence(const PaintSequence& sequence) {
    for (const PaintCommand& command : sequence.Commands()) {
      std::visit([this](const auto& value) { DrawCommand(value); }, command);
    }
  }

  static void AddStops(cairo_pattern_t* pattern, const std::vector<GradientStop>& stops) {
    for (const GradientStop& stop : stops) {
      cairo_pattern_add_color_stop_rgba(
          pattern, stop.offset, stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha
      );
    }
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
  }

  template <class Gradient>
  static void ApplyGradientTransform(cairo_pattern_t* pattern, Rect rect, const Gradient& gradient) {
    const Transform2D transform = ResolveGradientTransform(rect, gradient);
    cairo_matrix_t matrix{};
    cairo_matrix_init(&matrix, transform.m11, transform.m12, transform.m21, transform.m22, transform.translate_x,
                      transform.translate_y);
    static_cast<void>(cairo_matrix_invert(&matrix));
    cairo_pattern_set_matrix(pattern, &matrix);
  }

  void DrawCommand(const DrawRectCommand& command) {
    if (command.rect.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    cairo_new_path(context_);
    AddRoundedRect(context_, command.rect, command.corner_radius);
    cairo_fill(context_);
  }

  void DrawCommand(const DrawLinearGradientCommand& command) {
    if (command.rect.IsEmpty()) {
      return;
    }
    cairo_pattern_t* pattern = cairo_pattern_create_linear(
        command.gradient.start.x, command.gradient.start.y, command.gradient.end.x, command.gradient.end.y
    );
    AddStops(pattern, command.gradient.stops);
    ApplyGradientTransform(pattern, command.rect, command.gradient);
    cairo_set_source(context_, pattern);
    cairo_new_path(context_);
    AddRoundedRect(context_, command.rect, command.corner_radius);
    cairo_fill(context_);
    cairo_pattern_destroy(pattern);
  }

  void DrawCommand(const DrawRadialGradientCommand& command) {
    if (command.rect.IsEmpty()) {
      return;
    }
    cairo_pattern_t* pattern = cairo_pattern_create_radial(command.gradient.center.x, command.gradient.center.y, 0.0,
                                                           command.gradient.center.x, command.gradient.center.y,
                                                           command.gradient.radius.width);
    AddStops(pattern, command.gradient.stops);
    ApplyGradientTransform(pattern, command.rect, command.gradient);
    cairo_set_source(context_, pattern);
    cairo_new_path(context_);
    AddRoundedRect(context_, command.rect, command.corner_radius);
    cairo_fill(context_);
    cairo_pattern_destroy(pattern);
  }

  void DrawCommand(const DrawTextCommand& command) {
    if (command.text.empty() || command.rect.IsEmpty() || command.style.foreground.alpha <= 0.0F) {
      return;
    }
    PangoLayout* layout = pango_layout_new(state_.context);
    ConfigureLayout(layout, command.text, command.style, command.rect.width, command.options);
    PangoRectangle logical{};
    pango_layout_get_extents(layout, nullptr, &logical);
    if (command.options.wrap == TextWrap::NoWrap && PangoUnits(logical.width) < command.rect.width) {
      pango_layout_set_width(layout, static_cast<int>(std::ceil(command.rect.width * PANGO_SCALE)));
    }
    const float remaining_height = std::max(0.0F, command.rect.height - PangoUnits(logical.height));
    float vertical_offset = 0.0F;
    if (command.options.vertical_align == TextVerticalAlign::Center) {
      vertical_offset = remaining_height * 0.5F;
    } else if (command.options.vertical_align == TextVerticalAlign::Bottom) {
      vertical_offset = remaining_height;
    }
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(context_, command.rect.x, command.rect.y, command.rect.width, command.rect.height);
    cairo_clip(context_);
    SetSourceColor(context_, command.style.foreground);
    cairo_move_to(
        context_,
        command.rect.x + command.paragraph_offset.x,
        command.rect.y + vertical_offset + command.paragraph_offset.y
    );
    pango_cairo_show_layout(context_, layout);
    cairo_restore(context_);
    g_object_unref(layout);
  }

  void DrawCommand(const DrawTextRunsCommand& command) {
    for (const TextRun& run : command.runs) {
      if (run.text.empty() || run.bounds.IsEmpty() || run.style.foreground.alpha <= 0.0F) {
        continue;
      }
      PangoLayout* layout = pango_layout_new(state_.context);
      ConfigureLayout(
          layout,
          run.text,
          run.style,
          std::numeric_limits<float>::infinity(),
          {.shaping = run.shaping, .wrap = TextWrap::NoWrap}
      );
      SetSourceColor(context_, run.style.foreground);
      const float baseline = PangoUnits(pango_layout_get_baseline(layout));
      cairo_move_to(context_, run.baseline_origin.x, run.baseline_origin.y - baseline);
      pango_cairo_show_layout(context_, layout);
      g_object_unref(layout);
    }
  }

  void DrawCommand(const DrawImageCommand& command) {
    if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
      return;
    }
    GdkPixbuf* pixbuf = state_.ImageFor(command.image);
    if (pixbuf == nullptr) {
      return;
    }
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, command.destination.x, command.destination.y, command.destination.width, command.destination.height
    );
    cairo_clip(context_);
    const float image_scale = command.image.Scale();
    const double scale_x = command.destination.width / (command.source.width * image_scale);
    const double scale_y = command.destination.height / (command.source.height * image_scale);
    cairo_translate(context_, command.destination.x, command.destination.y);
    cairo_scale(context_, scale_x, scale_y);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_cairo_set_source_pixbuf(
        context_, pixbuf, -command.source.x * image_scale, -command.source.y * image_scale
    );
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_pattern_set_filter(
        cairo_get_source(context_),
        command.sampling == ImageSampling::Nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR
    );
    cairo_paint_with_alpha(context_, std::clamp(command.opacity, 0.0F, 1.0F));
    cairo_restore(context_);
  }

  void DrawCommand(const DrawExternalTextureCommand& command) {
    if (command.destination.IsEmpty() || command.source.IsEmpty() || command.opacity <= 0.0F) {
      return;
    }
    cairo_surface_t* surface = state_.ExternalTextureFor(command.texture);
    if (surface == nullptr) {
      return;
    }
    const Size intrinsic = command.texture.IntrinsicSize();
    const double source_scale_x = cairo_image_surface_get_width(surface) / std::max(0.001F, intrinsic.width);
    const double source_scale_y = cairo_image_surface_get_height(surface) / std::max(0.001F, intrinsic.height);
    const double source_x = command.source.x * source_scale_x;
    const double source_y = command.source.y * source_scale_y;
    const double source_width = command.source.width * source_scale_x;
    const double source_height = command.source.height * source_scale_y;
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, command.destination.x, command.destination.y, command.destination.width, command.destination.height
    );
    cairo_clip(context_);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(surface);
    cairo_matrix_t matrix{};
    cairo_matrix_init(
        &matrix,
        source_width / command.destination.width,
        0.0,
        0.0,
        source_height / command.destination.height,
        source_x - command.destination.x * source_width / command.destination.width,
        source_y - command.destination.y * source_height / command.destination.height
    );
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_filter(
        pattern, command.sampling == ImageSampling::Nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR
    );
    cairo_set_source(context_, pattern);
    cairo_paint_with_alpha(context_, std::clamp(command.opacity, 0.0F, 1.0F));
    cairo_pattern_destroy(pattern);
    cairo_restore(context_);
  }

  void DrawCommand(const DrawCircleCommand& command) {
    if (command.radius <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    cairo_new_path(context_);
    cairo_arc(context_, command.center.x, command.center.y, command.radius, 0.0, kTau);
    cairo_fill(context_);
  }

  void DrawCommand(const DrawLineCommand& command) {
    if (command.start == command.end || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    ApplyStrokeStyle(context_, command.style);
    cairo_new_path(context_);
    cairo_move_to(context_, command.start.x, command.start.y);
    cairo_line_to(context_, command.end.x, command.end.y);
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawArcCommand& command) {
    if (command.radius <= 0.0F || command.style.width <= 0.0F || command.color.alpha <= 0.0F ||
        !std::isfinite(command.start_angle) || !std::isfinite(command.sweep_angle) || command.sweep_angle == 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    ApplyStrokeStyle(context_, command.style);
    cairo_new_path(context_);
    if (std::abs(command.sweep_angle) >= kTau - 0.0001) {
      const double end_angle = command.start_angle + std::copysign(kTau, command.sweep_angle);
      if (command.sweep_angle > 0.0F) {
        cairo_arc(context_, command.center.x, command.center.y, command.radius, command.start_angle, end_angle);
      } else {
        cairo_arc_negative(
            context_, command.center.x, command.center.y, command.radius, command.start_angle, end_angle
        );
      }
      cairo_close_path(context_);
    } else if (command.sweep_angle > 0.0F) {
      cairo_arc(
          context_, command.center.x, command.center.y, command.radius, command.start_angle,
          command.start_angle + command.sweep_angle
      );
    } else {
      cairo_arc_negative(
          context_, command.center.x, command.center.y, command.radius, command.start_angle,
          command.start_angle + command.sweep_angle
      );
    }
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawBorderCommand& command) {
    if (command.rect.IsEmpty() || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    if (!command.style.dash_pattern.empty() || command.style.join != StrokeJoin::Miter ||
        command.style.miter_limit != 4.0F) {
      DrawCommand(StrokePathCommand{
          CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width),
          command.color,
          command.style,
      });
      return;
    }
    const float inset = command.style.width * 0.5F;
    const Rect inner{
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.style.width),
        std::max(0.0F, command.rect.height - command.style.width),
    };
    SetSourceColor(context_, command.color);
    ApplyStrokeStyle(context_, command.style);
    cairo_new_path(context_);
    AddRoundedRect(context_, inner, std::max(0.0F, command.corner_radius - inset));
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawShadowCommand& command) {
    const ResolvedShadow resolved = ResolveShadow(command);
    if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    if (command.blur_radius <= 0.0F) {
      DrawCommand(DrawRectCommand{resolved.caster, command.color, resolved.corner_radius});
      return;
    }
    const int mask_width = MaskDimension(resolved.bounds.width);
    const int mask_height = MaskDimension(resolved.bounds.height);
    if (mask_width == 0 || mask_height == 0) {
      return;
    }
    cairo_surface_t* blurred = BlurredRectMaskFor(resolved, command.blur_radius, mask_width, mask_height);
    if (blurred == nullptr) {
      return;
    }

    SetSourceColor(context_, command.color);
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, resolved.bounds.x, resolved.bounds.y, mask_width / device_scale_, mask_height / device_scale_
    );
    AddRoundedRect(context_, resolved.caster, resolved.corner_radius);
    cairo_set_fill_rule(context_, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(context_);
    cairo_translate(context_, resolved.bounds.x, resolved.bounds.y);
    cairo_scale(context_, 1.0 / device_scale_, 1.0 / device_scale_);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(blurred);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
    cairo_mask(context_, pattern);
    cairo_pattern_destroy(pattern);
    cairo_restore(context_);
  }

  cairo_surface_t*
  BlurredRectMaskFor(const ResolvedShadow& shadow, float blur_radius, int width, int height) {
    const LinuxRenderer::State::ShadowMaskKey key{
        shadow.caster.width,
        shadow.caster.height,
        shadow.corner_radius,
        blur_radius,
        device_scale_,
    };
    const auto cached = std::find_if(
        state_.shadow_masks.begin(),
        state_.shadow_masks.end(),
        [&key](const LinuxRenderer::State::ShadowMaskEntry& entry) { return entry.key == key; }
    );
    if (cached != state_.shadow_masks.end()) {
      std::rotate(cached, cached + 1, state_.shadow_masks.end());
      return state_.shadow_masks.back().surface;
    }

    CairoSurfaceHandle mask{cairo_image_surface_create(CAIRO_FORMAT_A8, width, height)};
    if (cairo_surface_status(mask.surface) != CAIRO_STATUS_SUCCESS) {
      return nullptr;
    }
    cairo_t* mask_context = cairo_create(mask.surface);
    cairo_scale(mask_context, device_scale_, device_scale_);
    cairo_translate(mask_context, blur_radius - shadow.caster.x, blur_radius - shadow.caster.y);
    cairo_set_source_rgb(mask_context, 1.0, 1.0, 1.0);
    AddRoundedRect(mask_context, shadow.caster, shadow.corner_radius);
    cairo_fill(mask_context);
    cairo_destroy(mask_context);

    const int radius = std::max(1, static_cast<int>(std::ceil(blur_radius * device_scale_ * 0.57735F)));
    cairo_surface_t* blurred = BoxBlurMask(mask.surface, radius);
    if (blurred == nullptr) {
      return nullptr;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(cairo_image_surface_get_stride(blurred)) * static_cast<std::uint64_t>(height);
    while (!state_.shadow_masks.empty() &&
           (state_.shadow_masks.size() >= LinuxRenderer::State::kMaxShadowMasks ||
            state_.shadow_mask_bytes + bytes > LinuxRenderer::State::kShadowMaskBudget)) {
      state_.shadow_mask_bytes -= state_.shadow_masks.front().bytes;
      cairo_surface_destroy(state_.shadow_masks.front().surface);
      state_.shadow_masks.erase(state_.shadow_masks.begin());
    }
    state_.shadow_masks.push_back({key, blurred, bytes});
    state_.shadow_mask_bytes += bytes;
    return blurred;
  }

  cairo_surface_t* BoxBlurMask(cairo_surface_t* mask, int radius) {
    cairo_surface_flush(mask);
    const int width = cairo_image_surface_get_width(mask);
    const int height = cairo_image_surface_get_height(mask);
    const unsigned char* source = cairo_image_surface_get_data(mask);
    const int source_stride = cairo_image_surface_get_stride(mask);
    state_.blur_scratch.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
      int sum = 0;
      for (int x = -radius; x <= radius; ++x) {
        sum += source[y * source_stride + std::clamp(x, 0, width - 1)];
      }
      for (int x = 0; x < width; ++x) {
        state_.blur_scratch[static_cast<std::size_t>(y) * width + x] = static_cast<unsigned char>(sum / window);
        sum += source[y * source_stride + std::clamp(x + radius + 1, 0, width - 1)] -
               source[y * source_stride + std::clamp(x - radius, 0, width - 1)];
      }
    }
    cairo_surface_t* result = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    if (cairo_surface_status(result) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(result);
      return nullptr;
    }
    unsigned char* destination = cairo_image_surface_get_data(result);
    const int destination_stride = cairo_image_surface_get_stride(result);
    for (int x = 0; x < width; ++x) {
      int sum = 0;
      for (int y = -radius; y <= radius; ++y) {
        sum += state_.blur_scratch[static_cast<std::size_t>(std::clamp(y, 0, height - 1)) * width + x];
      }
      for (int y = 0; y < height; ++y) {
        destination[y * destination_stride + x] = static_cast<unsigned char>(sum / window);
        sum += state_.blur_scratch[static_cast<std::size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width + x] -
               state_.blur_scratch[static_cast<std::size_t>(std::clamp(y - radius, 0, height - 1)) * width + x];
      }
    }
    cairo_surface_mark_dirty(result);
    return result;
  }

  void DrawCommand(const FillPathCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    AppendPath(context_, command.path);
    cairo_set_fill_rule(
        context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(context_);
  }

  void DrawCommand(const FillLinearGradientPathCommand& command) {
    if (command.path.IsEmpty() || command.gradient_rect.IsEmpty()) {
      return;
    }
    cairo_pattern_t* pattern = cairo_pattern_create_linear(
        command.gradient.start.x, command.gradient.start.y, command.gradient.end.x, command.gradient.end.y
    );
    AddStops(pattern, command.gradient.stops);
    ApplyGradientTransform(pattern, command.gradient_rect, command.gradient);
    cairo_set_source(context_, pattern);
    AppendPath(context_, command.path);
    cairo_set_fill_rule(
        context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(context_);
    cairo_pattern_destroy(pattern);
  }

  void DrawCommand(const FillRadialGradientPathCommand& command) {
    if (command.path.IsEmpty() || command.gradient_rect.IsEmpty()) {
      return;
    }
    cairo_pattern_t* pattern = cairo_pattern_create_radial(command.gradient.center.x, command.gradient.center.y, 0.0,
                                                           command.gradient.center.x, command.gradient.center.y,
                                                           command.gradient.radius.width);
    AddStops(pattern, command.gradient.stops);
    ApplyGradientTransform(pattern, command.gradient_rect, command.gradient);
    cairo_set_source(context_, pattern);
    AppendPath(context_, command.path);
    cairo_set_fill_rule(
        context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(context_);
    cairo_pattern_destroy(pattern);
  }

  void DrawCommand(const StrokePathCommand& command) {
    if (command.path.IsEmpty() || command.style.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(context_, command.color);
    AppendPath(context_, command.path);
    ApplyStrokeStyle(context_, command.style);
    cairo_stroke(context_);
  }

  void DrawCommand(const DrawPathShadowCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    if (command.blur_radius <= 0.0F) {
      SetSourceColor(context_, command.color);
      cairo_save(context_);
      cairo_translate(context_, command.offset.x, command.offset.y);
      AppendPath(context_, command.path);
      cairo_set_fill_rule(
          context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
      );
      cairo_fill(context_);
      cairo_restore(context_);
      return;
    }

    const Rect bounds = command.path.Bounds();
    const Rect shadow_bounds{
        bounds.x + command.offset.x - command.blur_radius,
        bounds.y + command.offset.y - command.blur_radius,
        bounds.width + command.blur_radius * 2.0F,
        bounds.height + command.blur_radius * 2.0F,
    };
    const int mask_width = MaskDimension(shadow_bounds.width);
    const int mask_height = MaskDimension(shadow_bounds.height);
    if (mask_width == 0 || mask_height == 0) {
      return;
    }
    cairo_surface_t* blurred = BlurredPathMaskFor(command, bounds, mask_width, mask_height);
    if (blurred == nullptr) {
      return;
    }

    SetSourceColor(context_, command.color);
    cairo_save(context_);
    cairo_new_path(context_);
    cairo_rectangle(
        context_, shadow_bounds.x, shadow_bounds.y, mask_width / device_scale_, mask_height / device_scale_
    );
    cairo_matrix_t previous{};
    cairo_get_matrix(context_, &previous);
    cairo_translate(context_, command.offset.x, command.offset.y);
    AppendPathContour(context_, command.path);
    cairo_set_matrix(context_, &previous);
    cairo_set_fill_rule(context_, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(context_);
    cairo_translate(context_, shadow_bounds.x, shadow_bounds.y);
    cairo_scale(context_, 1.0 / device_scale_, 1.0 / device_scale_);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(blurred);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
    cairo_mask(context_, pattern);
    cairo_pattern_destroy(pattern);
    cairo_restore(context_);
  }

  cairo_surface_t*
  BlurredPathMaskFor(const DrawPathShadowCommand& command, const Rect& bounds, int width, int height) {
    const LinuxRenderer::State::PathShadowMaskKey key{
        command.path,
        command.fill_rule,
        command.blur_radius,
        device_scale_,
    };
    const auto cached = std::find_if(
        state_.path_shadow_masks.begin(),
        state_.path_shadow_masks.end(),
        [&key](const LinuxRenderer::State::PathShadowMaskEntry& entry) { return entry.key == key; }
    );
    if (cached != state_.path_shadow_masks.end()) {
      std::rotate(cached, cached + 1, state_.path_shadow_masks.end());
      return state_.path_shadow_masks.back().surface;
    }

    CairoSurfaceHandle mask{cairo_image_surface_create(CAIRO_FORMAT_A8, width, height)};
    if (cairo_surface_status(mask.surface) != CAIRO_STATUS_SUCCESS) {
      return nullptr;
    }
    cairo_t* mask_context = cairo_create(mask.surface);
    cairo_scale(mask_context, device_scale_, device_scale_);
    cairo_translate(mask_context, command.blur_radius - bounds.x, command.blur_radius - bounds.y);
    cairo_set_source_rgb(mask_context, 1.0, 1.0, 1.0);
    AppendPath(mask_context, command.path);
    cairo_set_fill_rule(
        mask_context,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(mask_context);
    cairo_destroy(mask_context);

    const int radius =
        std::max(1, static_cast<int>(std::ceil(command.blur_radius * device_scale_ * 0.57735F)));
    cairo_surface_t* blurred = BoxBlurMask(mask.surface, radius);
    if (blurred == nullptr) {
      return nullptr;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(cairo_image_surface_get_stride(blurred)) * static_cast<std::uint64_t>(height);
    while (!state_.path_shadow_masks.empty() &&
           (state_.path_shadow_masks.size() >= LinuxRenderer::State::kMaxPathShadowMasks ||
            state_.path_shadow_mask_bytes + bytes > LinuxRenderer::State::kPathShadowMaskBudget)) {
      state_.path_shadow_mask_bytes -= state_.path_shadow_masks.front().bytes;
      cairo_surface_destroy(state_.path_shadow_masks.front().surface);
      state_.path_shadow_masks.erase(state_.path_shadow_masks.begin());
    }
    state_.path_shadow_masks.push_back({key, blurred, bytes});
    state_.path_shadow_mask_bytes += bytes;
    return blurred;
  }

  void DrawCommand(const PushClipCommand& command) {
    cairo_save(context_);
    cairo_new_path(context_);
    AddRoundedRect(context_, command.rect, command.corner_radius);
    cairo_clip(context_);
  }

  void DrawCommand(const PushPathClipCommand& command) {
    cairo_save(context_);
    AppendPath(context_, command.path);
    cairo_set_fill_rule(
        context_, command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_clip(context_);
  }

  void DrawCommand(const PopClipCommand&) {
    cairo_restore(context_);
  }

  void DrawCommand(const PushTransformCommand& command) {
    cairo_save(context_);
    ApplyTransform(context_, command.transform);
  }

  void DrawCommand(const PopTransformCommand&) {
    cairo_restore(context_);
  }

  void DrawCommand(const PlacePlatformViewCommand&) {
    throw std::logic_error("HuxerUI Linux adapter does not support PlatformView composition yet");
  }

  LinuxRenderer::State& state_;
  cairo_t* context_;
  float device_scale_ = 1.0F;
};

} // namespace

LinuxRenderer::LinuxRenderer() : state_(std::make_unique<State>()) {}

LinuxRenderer::~LinuxRenderer() = default;

void LinuxRenderer::Initialize() {
  if (!state_) {
    state_ = std::make_unique<State>();
  }
}

void LinuxRenderer::Discard() noexcept {
  state_.reset();
}

FontMetrics LinuxRenderer::Metrics(const Font& font) {
  PangoFontDescription* description = CreateFontDescription(font);
  PangoFontMetrics* metrics = pango_context_get_metrics(state_->context, description, nullptr);
  pango_font_description_free(description);
  const float ascent = PangoUnits(pango_font_metrics_get_ascent(metrics));
  const float descent = PangoUnits(pango_font_metrics_get_descent(metrics));
  const float height = PangoUnits(pango_font_metrics_get_height(metrics));
  FontMetrics result{
      .ascent = ascent,
      .descent = descent,
      .leading = std::max(0.0F, height - ascent - descent),
      .underline_position = std::abs(PangoUnits(pango_font_metrics_get_underline_position(metrics))),
      .underline_thickness = std::max(1.0F, PangoUnits(pango_font_metrics_get_underline_thickness(metrics))),
      .strike_through_position = PangoUnits(pango_font_metrics_get_strikethrough_position(metrics)),
      .strike_through_thickness =
          std::max(1.0F, PangoUnits(pango_font_metrics_get_strikethrough_thickness(metrics))),
  };
  pango_font_metrics_unref(metrics);
  return result;
}

TextRunMetrics LinuxRenderer::MeasureRun(
    std::string_view text, const TextStyle& style, const TextShapingOptions& options
) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
  PangoLayout* layout = pango_layout_new(state_->context);
  ConfigureLayout(
      layout,
      text,
      style,
      std::numeric_limits<float>::infinity(),
      {.shaping = options, .wrap = TextWrap::NoWrap}
  );
  PangoRectangle ink{};
  PangoRectangle logical{};
  pango_layout_get_extents(layout, &ink, &logical);
  const float baseline = PangoUnits(pango_layout_get_baseline(layout));
  const FontMetrics font_metrics = Metrics(style.font);
  const TextRunMetrics result{
      .advance = PangoUnits(logical.width),
      .visual_bounds = {PangoUnits(ink.x), PangoUnits(ink.y) - baseline, PangoUnits(ink.width), PangoUnits(ink.height)},
      .font_metrics = font_metrics,
  };
  g_object_unref(layout);
  return result;
}

TextLayoutMetrics LinuxRenderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  if (std::isfinite(max_width) && max_width <= 0.0F) {
    return {};
  }
  PangoTextLayout layout(state_->context, text, style, max_width, options);
  TextLayoutMetrics metrics = layout.MetricsValue();
  if (std::isfinite(max_width)) {
    metrics.size.width = std::min(metrics.size.width, max_width);
  }
  return metrics;
}

std::unique_ptr<TextLayout> LinuxRenderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return std::make_unique<PangoTextLayout>(state_->context, text, style, max_width, options);
}

void LinuxRenderer::Draw(cairo_t* context, const RenderFrame& frame) {
  if (context == nullptr) {
    throw std::invalid_argument("HuxerUI Linux renderer requires a Cairo context");
  }
  ScenePainter(*state_, context).Draw(frame.scene);
}

} // namespace huxerui::detail
