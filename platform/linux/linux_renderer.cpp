#include <algorithm>
#include <cctype>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#include <jpeglib.h>
#include <png.h>
#define EGL_PLATFORM_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "linux_internal.h"

#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/paint.h>
#include <huxerui/render_scene.h>
#include <huxerui/resource.h>
#include <huxerui/text.h>

#include "linux_renderer.h"
#include "path_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {

namespace {

constexpr float kDipsPerInch = 96.0F;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kTau = 6.28318530717958647692F;

void CombineHash(std::size_t& seed, std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::size_t HashFont(const Font& font) noexcept {
  std::size_t result = std::hash<std::string_view>{}(font.FamilyName());
  CombineHash(result, std::hash<float>{}(font.Size()));
  CombineHash(result, std::hash<FontWeight>{}(font.Weight()));
  CombineHash(result, std::hash<FontSlant>{}(font.Slant()));
  return result;
}

struct FontHash {
  std::size_t operator()(const Font& font) const noexcept {
    return HashFont(font);
  }
};

// Shaping output depends only on the run text plus the font; decoration, shaping
// locale, and direction are applied after shaping, so the cache key omits them.
// This diverges from win32, where DirectWrite embeds those values in the layout.
struct TextRunKey {
  std::string text;
  Font font;

  bool operator==(const TextRunKey&) const = default;
};

struct TextRunKeyView {
  std::string_view text;
  const Font& font;
};

struct TextRunKeyHash {
  using is_transparent = void;

  std::size_t operator()(const TextRunKey& key) const noexcept {
    return Hash(key.text, key.font);
  }

  std::size_t operator()(const TextRunKeyView& key) const noexcept {
    return Hash(key.text, key.font);
  }

private:
  static std::size_t Hash(std::string_view text, const Font& font) noexcept {
    std::size_t result = std::hash<std::string_view>{}(text);
    CombineHash(result, HashFont(font));
    return result;
  }
};

struct TextRunKeyEqual {
  using is_transparent = void;

  bool operator()(const TextRunKey& left, const TextRunKey& right) const noexcept {
    return left == right;
  }

  bool operator()(const TextRunKey& left, const TextRunKeyView& right) const noexcept {
    return std::string_view(left.text) == right.text && left.font == right.font;
  }

  bool operator()(const TextRunKeyView& left, const TextRunKey& right) const noexcept {
    return (*this)(right, left);
  }
};

// Line breaking depends only on the paragraph text, font, wrap width, and wrap
// mode; alignment is applied at draw time and shaping locale/direction never
// influence wrapping, so the cache key omits them. This diverges from win32,
// where DirectWrite embeds those values in the layout.
struct ParagraphKey {
  std::string text;
  Font font;
  float wrap_width = 0.0F;
  TextWrap wrap = TextWrap::NoWrap;

  bool operator==(const ParagraphKey&) const = default;
};

struct ParagraphKeyView {
  std::string_view text;
  const Font& font;
  float wrap_width;
  TextWrap wrap;
};

struct ParagraphKeyHash {
  using is_transparent = void;

  std::size_t operator()(const ParagraphKey& key) const noexcept {
    return Hash(key.text, key.font, key.wrap_width, key.wrap);
  }

  std::size_t operator()(const ParagraphKeyView& key) const noexcept {
    return Hash(key.text, key.font, key.wrap_width, key.wrap);
  }

private:
  static std::size_t Hash(std::string_view text, const Font& font, float wrap_width, TextWrap wrap) noexcept {
    std::size_t result = std::hash<std::string_view>{}(text);
    CombineHash(result, HashFont(font));
    CombineHash(result, std::hash<float>{}(wrap_width));
    CombineHash(result, std::hash<TextWrap>{}(wrap));
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
           left.wrap_width == right.wrap_width && left.wrap == right.wrap;
  }

  bool operator()(const ParagraphKeyView& left, const ParagraphKey& right) const noexcept {
    return (*this)(right, left);
  }
};

struct ShapedGlyph {
  std::uint32_t index = 0;
  float x_advance = 0.0F;
  float x_offset = 0.0F;
  float y_offset = 0.0F;
  std::uint32_t cluster = 0;
  bool fallback = false;
};

struct ShapedRun {
  std::vector<ShapedGlyph> glyphs;
  float advance = 0.0F;
};

struct LayoutLine {
  std::string text;
  std::vector<ShapedGlyph> glyphs;
  float advance = 0.0F;
  float ascent = 0.0F;
  float descent = 0.0F;
  float leading = 0.0F;
  float baseline = 0.0F;
  std::size_t byte_start = 0;
};

[[nodiscard]] std::uint32_t DecodeCodePoint(std::string_view text, std::size_t offset, std::size_t width) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (width == 1) {
    return first;
  }
  if (width == 2) {
    return ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[offset + 1]) & 0x3F);
  }
  if (width == 3) {
    return ((first & 0x0F) << 12) | ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[offset + 2]) & 0x3F);
  }
  return ((first & 0x07) << 18) | ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(text[offset + 2]) & 0x3F) << 6) |
         (static_cast<unsigned char>(text[offset + 3]) & 0x3F);
}

[[nodiscard]] std::size_t Utf8Width(unsigned char first) noexcept {
  if (first < 0x80) {
    return 1;
  }
  if ((first & 0xE0) == 0xC0) {
    return 2;
  }
  if ((first & 0xF0) == 0xE0) {
    return 3;
  }
  return 4;
}

[[nodiscard]] std::uint32_t Utf8CodePointAt(std::string_view text, std::size_t offset) noexcept {
  if (offset >= text.size()) {
    return 0;
  }
  const std::size_t width = Utf8Width(static_cast<unsigned char>(text[offset]));
  if (offset + width > text.size()) {
    return 0;
  }
  return DecodeCodePoint(text, offset, width);
}

[[nodiscard]] bool IsRtlCodePoint(std::uint32_t code_point) noexcept {
  return (code_point >= 0x0590 && code_point <= 0x08FF) || (code_point >= 0xFB1D && code_point <= 0xFDFF) ||
         (code_point >= 0xFE70 && code_point <= 0xFEFC) || (code_point >= 0x10800 && code_point <= 0x10FFF);
}

[[nodiscard]] TextDirection ResolveDirection(std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t width = Utf8Width(static_cast<unsigned char>(text[offset]));
    if (offset + width > text.size()) {
      break;
    }
    const std::uint32_t code_point = DecodeCodePoint(text, offset, width);
    if (IsRtlCodePoint(code_point)) {
      return TextDirection::RightToLeft;
    }
    if (code_point > 0x20 && code_point != 0x7F && !std::isspace(code_point)) {
      return TextDirection::LeftToRight;
    }
    offset += width;
  }
  return TextDirection::LeftToRight;
}

[[nodiscard]] int FcWeightFor(FontWeight weight) noexcept {
  return FcWeightFromOpenType(static_cast<int>(weight));
}

[[nodiscard]] std::string FcFamilyFor(const Font& font) {
  switch (font.FamilyKind()) {
  case FontFamilyKind::Monospace:
    return "monospace";
  case FontFamilyKind::Named:
    return std::string(font.FamilyName());
  case FontFamilyKind::System:
  default:
    return "sans-serif";
  }
}

class LinuxTextLayout final : public TextLayout {
public:
  LinuxTextLayout(std::string text, std::vector<LayoutLine> lines) : text_(std::move(text)), lines_(std::move(lines)) {
    for (const LayoutLine& line : lines_) {
      width_ = std::max(width_, line.advance);
      total_height_ += line.ascent + line.descent + line.leading;
    }
    BuildUtf16Offsets();
  }

  Size Measure() const override {
    return {std::ceil(width_), std::ceil(total_height_)};
  }

  TextPosition HitTest(Point point) const override {
    TextOffset best_offset = 0;
    TextAffinity best_affinity = TextAffinity::Downstream;
    float best_distance = std::numeric_limits<float>::max();
    for (const LayoutLine& line : lines_) {
      const float top = line.baseline - line.ascent;
      const float bottom = line.baseline + line.descent;
      const float vertical_distance = point.y < top ? top - point.y : (point.y > bottom ? point.y - bottom : 0.0F);
      if (vertical_distance > best_distance) {
        break;
      }
      float x = 0.0F;
      for (const ShapedGlyph& glyph : line.glyphs) {
        const float glyph_end = x + glyph.x_advance;
        const float distance = ClampTo(x, glyph_end, point.x);
        const std::size_t glyph_utf16 = Utf16At(line.byte_start + glyph.cluster);
        if (distance < best_distance || (distance == best_distance && glyph_utf16 > 0)) {
          const bool trailing = point.x > (x + glyph_end) * 0.5F;
          const std::size_t resolved_utf16 =
              trailing ? Utf16At(
                             line.byte_start + glyph.cluster +
                                 Utf8Width(static_cast<unsigned char>(text_[line.byte_start + glyph.cluster]))
                         )
                       : glyph_utf16;
          best_offset = static_cast<TextOffset>(resolved_utf16);
          best_affinity = trailing ? TextAffinity::Upstream : TextAffinity::Downstream;
          best_distance = distance;
        }
        x = glyph_end;
      }
      if (point.x >= x && x - point.x <= best_distance) {
        best_offset = static_cast<TextOffset>(Utf16At(line.byte_start + line.text.size()));
        best_affinity = TextAffinity::Downstream;
        best_distance = x - point.x;
      }
    }
    return {best_offset, best_affinity};
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const TextOffset clamped = std::clamp<TextOffset>(offset, 0, static_cast<TextOffset>(utf16_total_));
    for (const LayoutLine& line : lines_) {
      const std::size_t line_start_utf16 = Utf16At(line.byte_start);
      const std::size_t line_end_utf16 = Utf16At(line.byte_start + line.text.size());
      if (clamped < static_cast<TextOffset>(line_start_utf16) || clamped > static_cast<TextOffset>(line_end_utf16)) {
        continue;
      }
      float x = 0.0F;
      for (const ShapedGlyph& glyph : line.glyphs) {
        const std::size_t glyph_utf16 = Utf16At(line.byte_start + glyph.cluster);
        if (glyph_utf16 >= static_cast<std::size_t>(clamped)) {
          break;
        }
        x += glyph.x_advance;
      }
      static_cast<void>(affinity);
      const float height = line.ascent + line.descent + line.leading;
      return {
          x,
          line.baseline - line.ascent,
          1.0F,
          height,
      };
    }
    return {0.0F, 0.0F, 1.0F, 0.0F};
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    std::vector<Rect> rects;
    const TextOffset start = std::clamp<TextOffset>(range.start, 0, static_cast<TextOffset>(utf16_total_));
    const TextOffset end = std::clamp<TextOffset>(range.end, start, static_cast<TextOffset>(utf16_total_));
    if (start == end) {
      return rects;
    }
    for (const LayoutLine& line : lines_) {
      const std::size_t line_start_utf16 = Utf16At(line.byte_start);
      const std::size_t line_end_utf16 = Utf16At(line.byte_start + line.text.size());
      const TextOffset visible_start = std::max(start, static_cast<TextOffset>(line_start_utf16));
      const TextOffset visible_end = std::min(end, static_cast<TextOffset>(line_end_utf16));
      if (visible_start >= visible_end) {
        continue;
      }
      const float left = XForUtf16(line, visible_start - static_cast<TextOffset>(line_start_utf16));
      const float right = XForUtf16(line, visible_end - static_cast<TextOffset>(line_start_utf16));
      const float height = line.ascent + line.descent + line.leading;
      rects.push_back({
          left,
          line.baseline - line.ascent,
          right - left,
          height,
      });
    }
    return rects;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const TextOffset clamped = std::clamp<TextOffset>(offset, 0, static_cast<TextOffset>(utf16_total_));
    if (clamped <= 0) {
      return 0;
    }
    const std::size_t utf8 = ByteAt(static_cast<std::size_t>(clamped));
    if (utf8 == 0) {
      return 0;
    }
    std::size_t previous = utf8 - 1;
    while (previous > 0 && (static_cast<unsigned char>(text_[previous]) & 0xC0U) == 0x80U) {
      --previous;
    }
    return static_cast<TextOffset>(Utf16At(previous));
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const TextOffset clamped = std::clamp<TextOffset>(offset, 0, static_cast<TextOffset>(utf16_total_));
    const std::size_t utf8 = ByteAt(static_cast<std::size_t>(clamped));
    if (utf8 >= text_.size()) {
      return static_cast<TextOffset>(utf16_total_);
    }
    const std::size_t width = Utf8Width(static_cast<unsigned char>(text_[utf8]));
    return static_cast<TextOffset>(Utf16At(utf8 + width));
  }

private:
  void BuildUtf16Offsets() {
    utf16_offsets_.assign(text_.size() + 1, 0);
    std::size_t utf16 = 0;
    std::size_t byte = 0;
    while (byte < text_.size()) {
      const std::size_t width = Utf8Width(static_cast<unsigned char>(text_[byte]));
      if (byte + width > text_.size()) {
        break;
      }
      const std::uint32_t code_point = DecodeCodePoint(text_, byte, width);
      const std::size_t units = code_point > 0xFFFFU ? 2 : 1;
      for (std::size_t index = 0; index < width; ++index) {
        utf16_offsets_[byte + index] = utf16;
      }
      utf16 += units;
      byte += width;
    }
    utf16_offsets_[text_.size()] = utf16;
    utf16_total_ = utf16;
  }

  [[nodiscard]] std::size_t Utf16At(std::size_t byte) const {
    return byte < utf16_offsets_.size() ? utf16_offsets_[byte] : utf16_total_;
  }

  [[nodiscard]] std::size_t ByteAt(std::size_t utf16) const {
    if (utf16 >= utf16_total_) {
      return text_.size();
    }
    const auto it = std::upper_bound(utf16_offsets_.begin(), utf16_offsets_.end(), utf16);
    std::size_t byte = static_cast<std::size_t>(it - utf16_offsets_.begin());
    if (byte > 0) {
      --byte;
    }
    while (byte > 0 && (static_cast<unsigned char>(text_[byte]) & 0xC0U) == 0x80U) {
      --byte;
    }
    return byte;
  }

  [[nodiscard]] static float ClampTo(float start, float end, float value) noexcept {
    if (value < start) {
      return start - value;
    }
    if (value > end) {
      return value - end;
    }
    return 0.0F;
  }

  [[nodiscard]] float XForUtf16(const LayoutLine& line, TextOffset utf16_within_line) const {
    const std::size_t line_start_utf16 = Utf16At(line.byte_start);
    float x = 0.0F;
    for (const ShapedGlyph& glyph : line.glyphs) {
      const std::size_t glyph_utf16 = Utf16At(line.byte_start + glyph.cluster);
      if (static_cast<TextOffset>(glyph_utf16 - line_start_utf16) >= utf16_within_line) {
        break;
      }
      x += glyph.x_advance;
    }
    return x;
  }

  std::string text_;
  std::vector<LayoutLine> lines_;
  std::vector<std::size_t> utf16_offsets_;
  std::size_t utf16_total_ = 0;
  float width_ = 0.0F;
  float total_height_ = 0.0F;
};

struct DecodedImage {
  std::vector<std::byte> pixels;
  int width = 0;
  int height = 0;
};

[[nodiscard]] DecodedImage DecodePng(std::span<const std::byte> bytes) {
  DecodedImage result;
  png_image image{};
  std::memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()) == 0) {
    return result;
  }
  image.format = PNG_FORMAT_RGBA;
  result.width = static_cast<int>(image.width);
  result.height = static_cast<int>(image.height);
  std::vector<std::byte> raw(PNG_IMAGE_SIZE(image));
  if (png_image_finish_read(&image, nullptr, raw.data(), 0, nullptr) == 0) {
    png_image_free(&image);
    return {};
  }
  png_image_free(&image);
  result.pixels = std::move(raw);
  return result;
}

struct JpegErrorState {
  jpeg_error_mgr manager;
  jmp_buf jump;
};

void JpegErrorExit(j_common_ptr cinfo) {
  auto* state = reinterpret_cast<JpegErrorState*>(cinfo->err);
  longjmp(state->jump, 1);
}

[[nodiscard]] DecodedImage DecodeJpeg(std::span<const std::byte> bytes) {
  DecodedImage result;
  jpeg_decompress_struct cinfo{};
  JpegErrorState error_state{};
  cinfo.err = jpeg_std_error(&error_state.manager);
  error_state.manager.error_exit = JpegErrorExit;
  if (setjmp(error_state.jump) != 0) {
    jpeg_destroy_decompress(&cinfo);
    return result;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(
      &cinfo,
      reinterpret_cast<unsigned char*>(const_cast<std::byte*>(bytes.data())),
      static_cast<unsigned long>(bytes.size())
  );
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return result;
  }
  jpeg_start_decompress(&cinfo);
  result.width = static_cast<int>(cinfo.output_width);
  result.height = static_cast<int>(cinfo.output_height);
  const int row_stride = result.width * cinfo.output_components;
  result.pixels.resize(static_cast<std::size_t>(row_stride) * static_cast<std::size_t>(result.height));
  while (cinfo.output_scanline < cinfo.output_height) {
    auto* row = reinterpret_cast<unsigned char*>(
        result.pixels.data() + static_cast<std::size_t>(cinfo.output_scanline) * static_cast<std::size_t>(row_stride)
    );
    static_cast<void>(jpeg_read_scanlines(&cinfo, &row, 1));
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return result;
}

struct HarfBuzzShapeState {
  explicit HarfBuzzShapeState(
      hb_face_t* face_value, hb_font_t* font_value, hb_buffer_t* buffer_value, bool owns_buffer_value = true
  )
      : face(face_value), font(font_value), buffer(buffer_value), owns_buffer(owns_buffer_value) {}

  ~HarfBuzzShapeState() {
    if (owns_buffer) {
      hb_buffer_destroy(buffer);
    }
    hb_font_destroy(font);
    hb_face_destroy(face);
  }

  HarfBuzzShapeState(const HarfBuzzShapeState&) = delete;
  HarfBuzzShapeState& operator=(const HarfBuzzShapeState&) = delete;

  hb_face_t* face;
  hb_font_t* font;
  hb_buffer_t* buffer;
  bool owns_buffer = true;
};

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

[[nodiscard]] GLuint CompileProgram() {
  const char* vertex_source =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "  v_uv = a_uv;\n"
      "}\n";
  const char* fragment_source =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "uniform bool u_bgra;\n"
      "void main() {\n"
      "  vec4 c = texture2D(u_tex, v_uv);\n"
      "  gl_FragColor = u_bgra ? c.bgra : c;\n"
      "}\n";

  const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex, 1, &vertex_source, nullptr);
  glCompileShader(vertex);
  GLint vertex_ok = GL_FALSE;
  glGetShaderiv(vertex, GL_COMPILE_STATUS, &vertex_ok);
  if (vertex_ok != GL_TRUE) {
    char log[256] = {};
    glGetShaderInfoLog(vertex, sizeof(log), nullptr, log);
    std::fprintf(stderr, "HuxerUI: EGL vertex shader failed to compile: %s\n", log);
    glDeleteShader(vertex);
    return 0;
  }
  const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment, 1, &fragment_source, nullptr);
  glCompileShader(fragment);
  GLint fragment_ok = GL_FALSE;
  glGetShaderiv(fragment, GL_COMPILE_STATUS, &fragment_ok);
  if (fragment_ok != GL_TRUE) {
    char log[256] = {};
    glGetShaderInfoLog(fragment, sizeof(log), nullptr, log);
    std::fprintf(stderr, "HuxerUI: EGL fragment shader failed to compile: %s\n", log);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }

  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    char log[256] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::fprintf(stderr, "HuxerUI: EGL shader program failed to link: %s\n", log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

[[nodiscard]] bool ExtensionSupported(const char* extensions, const char* name) noexcept {
  if (extensions == nullptr || name == nullptr) {
    return false;
  }
  const std::string_view haystack(extensions);
  const std::string_view needle(name);
  std::size_t offset = 0;
  while (offset < haystack.size()) {
    const std::size_t end = haystack.find(' ', offset);
    const std::size_t token_end = end == std::string_view::npos ? haystack.size() : end;
    if (haystack.substr(offset, token_end - offset) == needle) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  return false;
}

} // namespace

struct LinuxRenderer::State {
  FT_Library ft_library = nullptr;
  bool ft_ready = false;
  bool fc_ready = false;
  std::unordered_map<Font, FT_Face, FontHash> font_cache;
  std::unordered_map<Font, cairo_font_face_t*, FontHash> cairo_font_cache;
  std::map<std::pair<std::uint32_t, float>, FT_Face> fallback_font_cache;
  std::map<std::pair<std::uint32_t, float>, cairo_font_face_t*> cairo_fallback_font_cache;
  std::unordered_map<TextRunKey, ShapedRun, TextRunKeyHash, TextRunKeyEqual> shaped_run_cache;
  std::unordered_map<ParagraphKey, std::vector<LayoutLine>, ParagraphKeyHash, ParagraphKeyEqual> paragraph_cache;
  static constexpr std::size_t kMaxShapedRuns = 1024;
  static constexpr std::size_t kMaxParagraphs = 256;
  static constexpr std::size_t kMaxFallbackFonts = 256;
  hb_buffer_t* hb_scratch_buffer = nullptr;
  // Scratch buffer for the BoxBlurMask horizontal pass, reused across
  // sequential per-frame shadow draws. ScenePainter runs once per frame on a
  // single thread, so the buffer is never reentrant.
  std::vector<unsigned char> blur_scratch;

  cairo_surface_t* retained_surface = nullptr;
  cairo_t* retained_context = nullptr;
  int surface_width = 0;
  int surface_height = 0;
  bool force_full_repaint = true;
  // Damage rects are stashed by Render in retained-surface pixel coordinates
  // and consumed by PresentGl for partial texture uploads; reset after each
  // present so a standalone PresentGl uploads the full surface.
  std::vector<XRectangle> pending_damage_rects;
  bool pending_damage_full = true;

  Display* display = nullptr;
  Window window = 0;
  float dpi = kDipsPerInch;

  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLConfig egl_config = nullptr;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  EGLContext egl_context = EGL_NO_CONTEXT;
  bool egl_ready = false;
  bool gl_ready = false;
  bool diag_printed_ = false;
  unsigned long native_visual_id = 0;
  bool use_bgra_upload = false;
  GLuint texture = 0;
  int texture_width = 0;
  int texture_height = 0;
  GLuint quad_vbo = 0;
  GLuint program = 0;
  GLint uniform_sampler = -1;
  GLint uniform_bgra = -1;
  GLint attrib_position = -1;
  GLint attrib_uv = -1;

  struct ImageCacheEntry {
    cairo_surface_t* surface = nullptr;
    std::uint64_t bytes = 0;
  };
  std::unordered_map<std::uint64_t, ImageCacheEntry> image_cache;
  std::uint64_t image_cache_bytes = 0;
  static constexpr std::uint64_t kImageCacheBudget = 32 * 1024 * 1024;

  ~State() {
    for (auto& [key, entry] : image_cache) {
      if (entry.surface != nullptr) {
        cairo_surface_destroy(entry.surface);
      }
    }
    image_cache.clear();
    for (auto& [key, face] : cairo_font_cache) {
      cairo_font_face_destroy(face);
    }
    cairo_font_cache.clear();
    for (auto& [key, face] : fallback_font_cache) {
      FT_Done_Face(face);
    }
    fallback_font_cache.clear();
    for (auto& [key, face] : cairo_fallback_font_cache) {
      cairo_font_face_destroy(face);
    }
    cairo_fallback_font_cache.clear();
    if (retained_context != nullptr) {
      cairo_destroy(retained_context);
    }
    if (retained_surface != nullptr) {
      cairo_surface_destroy(retained_surface);
    }
    for (auto& [key, face] : font_cache) {
      FT_Done_Face(face);
    }
    font_cache.clear();
    if (hb_scratch_buffer != nullptr) {
      hb_buffer_destroy(hb_scratch_buffer);
      hb_scratch_buffer = nullptr;
    }
    // FcInit is refcounted; pair the single successful call, and after the
    // FreeType faces resolved through fontconfig are gone.
    if (fc_ready) {
      FcFini();
      fc_ready = false;
    }
    if (ft_library != nullptr) {
      FT_Done_FreeType(ft_library);
    }
  }

  [[nodiscard]] float Scale() const noexcept {
    return std::max(dpi, 1.0F) / kDipsPerInch;
  }

  void EnsureFontconfig() {
    if (fc_ready) {
      return;
    }
    if (FcInit() == FcFalse) {
      throw std::runtime_error("HuxerUI could not initialize fontconfig");
    }
    fc_ready = true;
  }

  void EnsureFreeType() {
    if (ft_ready) {
      return;
    }
    if (FT_Init_FreeType(&ft_library) != 0) {
      throw std::runtime_error("HuxerUI could not initialize FreeType");
    }
    ft_ready = true;
  }

  FT_Face FaceFor(const Font& font) {
    EnsureFreeType();
    EnsureFontconfig();
    const auto existing = font_cache.find(font);
    if (existing != font_cache.end()) {
      return existing->second;
    }

    const std::string family = FcFamilyFor(font);
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    FcPatternAddInteger(pattern, FC_WEIGHT, FcWeightFor(font.Weight()));
    FcPatternAddInteger(pattern, FC_SLANT, font.Slant() == FontSlant::Italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultMatch;
    FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (matched == nullptr) {
      throw std::runtime_error("HuxerUI could not match a font for the requested family");
    }

    FcChar8* file = nullptr;
    int face_index = 0;
    if (FcPatternGetString(matched, FC_FILE, 0, &file) != FcResultMatch ||
        FcPatternGetInteger(matched, FC_INDEX, 0, &face_index) != FcResultMatch) {
      FcPatternDestroy(matched);
      throw std::runtime_error("HuxerUI could not resolve a font file");
    }
    const std::string path = reinterpret_cast<const char*>(file);
    FcPatternDestroy(matched);

    FT_Face face = nullptr;
    if (FT_New_Face(ft_library, path.c_str(), face_index, &face) != 0) {
      throw std::runtime_error("HuxerUI could not open the resolved font file");
    }
    if (FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(font.Size() * 64.0F), 96, 96) != 0) {
      FT_Done_Face(face);
      throw std::runtime_error("HuxerUI could not set the font size");
    }
    font_cache.emplace(font, face);
    return face;
  }

  [[nodiscard]] cairo_font_face_t* CairoFontFor(const Font& font) {
    const auto existing = cairo_font_cache.find(font);
    if (existing != cairo_font_cache.end()) {
      return existing->second;
    }
    FT_Face face = FaceFor(font);
    static_cast<void>(FT_Select_Charmap(face, FT_ENCODING_UNICODE));
    cairo_font_face_t* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
    cairo_font_cache.emplace(font, cairo_face);
    return cairo_face;
  }

  [[nodiscard]] cairo_font_face_t* CairoFallbackFontFor(std::uint32_t code_point, float size) {
    const std::pair<std::uint32_t, float> key{code_point, size};
    const auto existing = cairo_fallback_font_cache.find(key);
    if (existing != cairo_fallback_font_cache.end()) {
      return existing->second;
    }
    FT_Face face = FallbackFaceFor(code_point, size);
    if (face == nullptr) {
      return nullptr;
    }
    static_cast<void>(FT_Select_Charmap(face, FT_ENCODING_UNICODE));
    cairo_font_face_t* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
    if (cairo_fallback_font_cache.size() >= kMaxFallbackFonts) {
      cairo_fallback_font_cache.erase(cairo_fallback_font_cache.begin());
    }
    cairo_fallback_font_cache.emplace(key, cairo_face);
    return cairo_face;
  }

  [[nodiscard]] FontMetrics MetricsFor(const Font& font) {
    FT_Face face = FaceFor(font);
    const double scale = font.Size() / static_cast<double>(face->units_per_EM > 0 ? face->units_per_EM : 1000);
    FontMetrics metrics{};
    metrics.ascent = static_cast<float>(face->ascender * scale);
    metrics.descent = static_cast<float>(-face->descender * scale);
    metrics.leading = static_cast<float>(std::max(0.0, (face->height - face->ascender + face->descender) * scale));
    metrics.underline_position = static_cast<float>(-face->underline_position * scale);
    metrics.underline_thickness = static_cast<float>(face->underline_thickness * scale);
    metrics.strike_through_position = static_cast<float>(face->ascender * 0.5 * scale);
    metrics.strike_through_thickness = metrics.underline_thickness;
    return metrics;
  }

  [[nodiscard]] FT_Face FallbackFaceFor(std::uint32_t code_point, float size) {
    EnsureFreeType();
    EnsureFontconfig();
    const std::pair<std::uint32_t, float> key{code_point, size};
    const auto existing = fallback_font_cache.find(key);
    if (existing != fallback_font_cache.end()) {
      return existing->second;
    }
    FcPattern* pattern = FcPatternCreate();
    FcCharSet* charset = FcCharSetCreate();
    FcCharSetAddChar(charset, code_point);
    FcPatternAddCharSet(pattern, FC_CHARSET, charset);
    FcCharSetDestroy(charset);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultMatch;
    FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (matched == nullptr) {
      return nullptr;
    }
    FcChar8* file = nullptr;
    int face_index = 0;
    if (FcPatternGetString(matched, FC_FILE, 0, &file) != FcResultMatch ||
        FcPatternGetInteger(matched, FC_INDEX, 0, &face_index) != FcResultMatch) {
      FcPatternDestroy(matched);
      return nullptr;
    }
    const std::string path = reinterpret_cast<const char*>(file);
    FcPatternDestroy(matched);

    FT_Face face = nullptr;
    if (FT_New_Face(ft_library, path.c_str(), face_index, &face) != 0) {
      return nullptr;
    }
    if (FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(size * 64.0F), 96, 96) != 0) {
      FT_Done_Face(face);
      return nullptr;
    }
    if (fallback_font_cache.size() >= kMaxFallbackFonts) {
      fallback_font_cache.erase(fallback_font_cache.begin());
    }
    fallback_font_cache.emplace(key, face);
    return face;
  }

  [[nodiscard]] ShapedRun ShapeRun(std::string_view text, const TextStyle& style) {
    // A cache hit skips FaceFor and the char-size reset below. That is safe
    // because the next cache miss re-establishes the shared face's 96-dpi char
    // size, MetricsFor uses font-wide metrics independent of char size, and
    // Cairo re-scales faces during rendering.
    const auto cached = shaped_run_cache.find(TextRunKeyView{text, style.font});
    if (cached != shaped_run_cache.end()) {
      return cached->second;
    }
    FT_Face face = FaceFor(style.font);
    // Cairo's FT load face rewrites the shared face's character size while
    // rendering; reset it to the 96-dpi reference so shaping stays in DIPs.
    FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(style.font.Size() * 64.0F), 96, 96);
    if (hb_scratch_buffer == nullptr) {
      hb_scratch_buffer = hb_buffer_create();
    } else {
      hb_buffer_clear_contents(hb_scratch_buffer);
    }
    HarfBuzzShapeState shaping{
        hb_ft_face_create(face, nullptr),
        hb_ft_font_create_referenced(face),
        hb_scratch_buffer,
        false,
    };
    hb_buffer_add_utf8(shaping.buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_set_direction(
        shaping.buffer,
        ResolveDirection(text) == TextDirection::RightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR
    );
    hb_buffer_guess_segment_properties(shaping.buffer);
    hb_shape(shaping.font, shaping.buffer, nullptr, 0);

    const unsigned int glyph_count = hb_buffer_get_length(shaping.buffer);
    const hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(shaping.buffer, nullptr);
    const hb_glyph_position_t* glyph_position = hb_buffer_get_glyph_positions(shaping.buffer, nullptr);

    ShapedRun run;
    run.glyphs.reserve(glyph_count);
    for (unsigned int index = 0; index < glyph_count; ++index) {
      run.glyphs.push_back({
          glyph_info[index].codepoint,
          static_cast<float>(glyph_position[index].x_advance) / 64.0F,
          static_cast<float>(glyph_position[index].x_offset) / 64.0F,
          static_cast<float>(glyph_position[index].y_offset) / 64.0F,
          glyph_info[index].cluster,
      });
      run.advance += static_cast<float>(glyph_position[index].x_advance) / 64.0F;
    }

    if (std::any_of(run.glyphs.begin(), run.glyphs.end(), [](const ShapedGlyph& glyph) { return glyph.index == 0; })) {
      run = ApplyFallbackShaping(text, style, std::move(run));
    }
    if (shaped_run_cache.size() >= kMaxShapedRuns) {
      shaped_run_cache.erase(shaped_run_cache.begin());
    }
    shaped_run_cache.emplace(TextRunKey{std::string(text), style.font}, run);
    return run;
  }

  [[nodiscard]] float ProbeAdvance(std::string_view text, const TextStyle& style) {
    const auto cached = shaped_run_cache.find(TextRunKeyView{text, style.font});
    if (cached != shaped_run_cache.end()) {
      return cached->second.advance;
    }
    return ShapeRun(text, style).advance;
  }

  [[nodiscard]] ShapedRun ApplyFallbackShaping(std::string_view text, const TextStyle& style, ShapedRun run) {
    std::size_t offset = 0;
    while (offset < run.glyphs.size()) {
      ShapedGlyph& glyph = run.glyphs[offset];
      if (glyph.index != 0) {
        ++offset;
        continue;
      }
      const std::size_t byte_offset = glyph.cluster;
      const std::size_t width = Utf8Width(static_cast<unsigned char>(text[byte_offset]));
      if (byte_offset + width > text.size()) {
        ++offset;
        continue;
      }
      const std::uint32_t code_point = DecodeCodePoint(text, byte_offset, width);
      FT_Face fallback = FallbackFaceFor(code_point, style.font.Size());
      if (fallback == nullptr) {
        ++offset;
        continue;
      }
      FT_Set_Char_Size(fallback, 0, static_cast<FT_F26Dot6>(style.font.Size() * 64.0F), 96, 96);
      HarfBuzzShapeState fallback_shaping{
          hb_ft_face_create(fallback, nullptr),
          hb_ft_font_create_referenced(fallback),
          hb_buffer_create(),
      };
      hb_buffer_add_utf8(
          fallback_shaping.buffer,
          text.data() + byte_offset,
          static_cast<int>(width),
          0,
          static_cast<int>(width)
      );
      hb_buffer_guess_segment_properties(fallback_shaping.buffer);
      hb_shape(fallback_shaping.font, fallback_shaping.buffer, nullptr, 0);
      const unsigned int count = hb_buffer_get_length(fallback_shaping.buffer);
      const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(fallback_shaping.buffer, nullptr);
      const hb_glyph_position_t* position = hb_buffer_get_glyph_positions(fallback_shaping.buffer, nullptr);
      if (count > 0) {
        glyph.index = info[0].codepoint;
        glyph.x_advance = static_cast<float>(position[0].x_advance) / 64.0F;
        glyph.x_offset = static_cast<float>(position[0].x_offset) / 64.0F;
        glyph.y_offset = static_cast<float>(position[0].y_offset) / 64.0F;
        glyph.fallback = true;
        run.advance = 0.0F;
        for (const ShapedGlyph& updated : run.glyphs) {
          run.advance += updated.x_advance;
        }
      }
      ++offset;
    }
    return run;
  }

  void
  ShapeLine(std::string_view text, const TextStyle& style, std::size_t byte_start, std::vector<LayoutLine>& lines) {
    ShapedRun run = ShapeRun(text, style);
    const FontMetrics metrics = MetricsFor(style.font);
    lines.push_back({
        std::string(text),
        std::move(run.glyphs),
        run.advance,
        metrics.ascent,
        metrics.descent,
        metrics.leading,
        0.0F,
        byte_start,
    });
  }

  void DestroyGlResources() {
    // EGL teardown on some drivers raises X protocol errors against a window
    // that is already gone; swallow them with a temporary error handler.
    const auto previous_error_handler = XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
    if (egl_display != EGL_NO_DISPLAY) {
      if (egl_context != EGL_NO_CONTEXT && eglGetCurrentContext() == egl_context) {
        if (texture != 0) {
          glDeleteTextures(1, &texture);
        }
        if (quad_vbo != 0) {
          glDeleteBuffers(1, &quad_vbo);
        }
        if (program != 0) {
          glDeleteProgram(program);
        }
      }
      if (egl_context != EGL_NO_CONTEXT) {
        static_cast<void>(eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
      }
      if (egl_surface != EGL_NO_SURFACE) {
        static_cast<void>(eglDestroySurface(egl_display, egl_surface));
      }
      if (egl_context != EGL_NO_CONTEXT) {
        static_cast<void>(eglDestroyContext(egl_display, egl_context));
      }
      static_cast<void>(eglTerminate(egl_display));
    }
    XSetErrorHandler(previous_error_handler);

    egl_display = EGL_NO_DISPLAY;
    egl_config = nullptr;
    egl_surface = EGL_NO_SURFACE;
    egl_context = EGL_NO_CONTEXT;
    egl_ready = false;
    gl_ready = false;
    native_visual_id = 0;
    use_bgra_upload = false;
    texture = 0;
    texture_width = 0;
    texture_height = 0;
    quad_vbo = 0;
    program = 0;
    uniform_sampler = -1;
    uniform_bgra = -1;
    attrib_position = -1;
    attrib_uv = -1;
  }

  [[nodiscard]] cairo_surface_t* ImageSurfaceFor(const ImageAsset& image) {
    if (!image.HasValue()) {
      return nullptr;
    }
    const std::uint64_t identity = ResourceAccess::ImageIdentity(image);
    const auto existing = image_cache.find(identity);
    if (existing != image_cache.end()) {
      return existing->second.surface;
    }
    DecodedImage decoded;
    if (image.Format() == ImageFormat::Png) {
      decoded = DecodePng(image.EncodedBytes());
    } else if (image.Format() == ImageFormat::Jpeg) {
      decoded = DecodeJpeg(image.EncodedBytes());
    }
    if (decoded.pixels.empty() || decoded.width <= 0 || decoded.height <= 0) {
      return nullptr;
    }
    CairoSurfaceHandle surface_handle{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, decoded.width, decoded.height)};
    cairo_surface_t* surface = surface_handle.surface;
    unsigned char* data = cairo_image_surface_get_data(surface);
    const std::size_t stride = cairo_image_surface_get_stride(surface);
    const std::size_t pixel_bytes =
        decoded.pixels.size() / (static_cast<std::size_t>(decoded.width) * static_cast<std::size_t>(decoded.height));
    const bool has_alpha = pixel_bytes >= 4;
    for (int y = 0; y < decoded.height; ++y) {
      const auto* src = reinterpret_cast<const unsigned char*>(
          decoded.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(decoded.width) * pixel_bytes
      );
      auto* dst = data + static_cast<std::size_t>(y) * stride;
      for (int x = 0; x < decoded.width; ++x) {
        const auto* pixel = src + static_cast<std::size_t>(x) * pixel_bytes;
        const unsigned char red = pixel[0];
        const unsigned char green = pixel_bytes >= 3 ? pixel[1] : pixel[0];
        const unsigned char blue = pixel_bytes >= 3 ? pixel[2] : pixel[0];
        const unsigned char alpha = has_alpha ? pixel[3] : 255;
        const float a = alpha / 255.0F;
        dst[x * 4] = static_cast<unsigned char>(blue * a);
        dst[x * 4 + 1] = static_cast<unsigned char>(green * a);
        dst[x * 4 + 2] = static_cast<unsigned char>(red * a);
        dst[x * 4 + 3] = alpha;
      }
    }
    cairo_surface_mark_dirty(surface);
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(decoded.width) * static_cast<std::uint64_t>(decoded.height) * 4;
    while (!image_cache.empty() && image_cache_bytes + bytes > kImageCacheBudget) {
      auto oldest = image_cache.begin();
      image_cache_bytes -= oldest->second.bytes;
      cairo_surface_destroy(oldest->second.surface);
      image_cache.erase(oldest);
    }
    image_cache[identity] = {surface, bytes};
    surface_handle.surface = nullptr;
    image_cache_bytes += bytes;
    return surface;
  }

  // Creates only the EGL display and picks a config; the native window does not
  // exist yet, so the surface and context are deferred to EnsureGl. The default
  // display is never used because it opens a second X connection on this thread.
  [[nodiscard]] bool InitializePresentation(Display* display) {
    if (egl_ready) {
      return true;
    }
    if (egl_display == EGL_NO_DISPLAY) {
      egl_display = GetPlatformDisplay(display);
      if (egl_display == EGL_NO_DISPLAY) {
        return false;
      }
      if (eglInitialize(egl_display, nullptr, nullptr) != EGL_TRUE) {
        egl_display = EGL_NO_DISPLAY;
        return false;
      }
      if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        eglTerminate(egl_display);
        egl_display = EGL_NO_DISPLAY;
        return false;
      }
    }
    if (egl_config == nullptr && !ChooseConfig(0)) {
      eglTerminate(egl_display);
      egl_display = EGL_NO_DISPLAY;
      return false;
    }
    egl_ready = true;
    return true;
  }

  [[nodiscard]] EGLDisplay GetPlatformDisplay(Display* display) const {
    const auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplay")
    );
    if (get_platform_display != nullptr) {
      return get_platform_display(EGL_PLATFORM_X11_KHR, display, nullptr);
    }
    return eglGetDisplay(display);
  }

  // eglCreateWindowSurface matches the window's visual against the config's
  // EGL_NATIVE_VISUAL_ID, so a config without one fails with EGL_BAD_MATCH.
  // preferred_visual_id of zero asks for any config with a non-zero id.
  [[nodiscard]] bool ChooseConfig(unsigned long preferred_visual_id) {
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        0,
        EGL_SAMPLE_BUFFERS,
        0,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (eglChooseConfig(egl_display, config_attribs, nullptr, 0, &config_count) != EGL_TRUE || config_count <= 0) {
      return false;
    }
    std::vector<EGLConfig> configs(static_cast<std::size_t>(config_count));
    if (eglChooseConfig(egl_display, config_attribs, configs.data(), config_count, &config_count) != EGL_TRUE) {
      return false;
    }
    EGLConfig fallback = nullptr;
    unsigned long fallback_visual_id = 0;
    for (const EGLConfig candidate : configs) {
      EGLint visual_id = 0;
      if (eglGetConfigAttrib(egl_display, candidate, EGL_NATIVE_VISUAL_ID, &visual_id) != EGL_TRUE) {
        continue;
      }
      const unsigned long candidate_visual_id = static_cast<unsigned long>(visual_id);
      if (candidate_visual_id == 0) {
        continue;
      }
      if (preferred_visual_id != 0 && candidate_visual_id == preferred_visual_id) {
        egl_config = candidate;
        native_visual_id = candidate_visual_id;
        return true;
      }
      if (fallback == nullptr) {
        fallback = candidate;
        fallback_visual_id = candidate_visual_id;
      }
    }
    if (fallback != nullptr) {
      egl_config = fallback;
      native_visual_id = fallback_visual_id;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool EnsureGl(Display* display, Window window) {
    if (gl_ready) {
      return true;
    }
    if (!InitializePresentation(display)) {
      return false;
    }
    if (egl_context == EGL_NO_CONTEXT) {
      const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
      egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
      if (egl_context == EGL_NO_CONTEXT) {
        DestroyGlResources();
        return false;
      }
    }
    if (egl_surface == EGL_NO_SURFACE) {
      const EGLint surface_attribs[] = {EGL_NONE};
      egl_surface = eglCreateWindowSurface(egl_display, egl_config, window, surface_attribs);
      if (egl_surface == EGL_NO_SURFACE) {
        // The initial config was chosen before the window existed; if its
        // visual does not match the window (zero id or an exotic X stack) the
        // driver rejects the surface with EGL_BAD_MATCH. Retry once against the
        // window's actual visual.
        if (eglGetError() == EGL_BAD_MATCH) {
          XWindowAttributes attributes{};
          if (XGetWindowAttributes(display, window, &attributes) != 0 &&
              ChooseConfig(static_cast<unsigned long>(XVisualIDFromVisual(attributes.visual)))) {
            egl_surface = eglCreateWindowSurface(egl_display, egl_config, window, surface_attribs);
          }
        }
      }
      if (egl_surface == EGL_NO_SURFACE) {
        DestroyGlResources();
        return false;
      }
    }
    if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
      DestroyGlResources();
      return false;
    }
    static_cast<void>(eglSwapInterval(egl_display, 1));

    if (program == 0) {
      program = CompileProgram();
      if (program == 0) {
        DestroyGlResources();
        return false;
      }
      uniform_sampler = glGetUniformLocation(program, "u_tex");
      uniform_bgra = glGetUniformLocation(program, "u_bgra");
      attrib_position = glGetAttribLocation(program, "a_pos");
      attrib_uv = glGetAttribLocation(program, "a_uv");
      static constexpr float kQuadVertices[] = {
          -1.0F, -1.0F, 0.0F, 1.0F,
           1.0F, -1.0F, 1.0F, 1.0F,
          -1.0F,  1.0F, 0.0F, 0.0F,
           1.0F,  1.0F, 1.0F, 0.0F,
      };
      glGenBuffers(1, &quad_vbo);
      glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kQuadVertices)), kQuadVertices, GL_STATIC_DRAW);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glUseProgram(program);
      glUniform1i(uniform_bgra, 0);
      glUseProgram(0);
    }

    // Cairo paints premultiplied ARGB32; the BGRA extension uploads it
    // byte-for-byte and the shader swizzles to RGBA when it is missing.
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    use_bgra_upload = ExtensionSupported(extensions, "GL_EXT_texture_format_BGRA8888");

    gl_ready = true;
    return true;
  }

  [[nodiscard]] bool PresentGl() {
    if (!gl_ready || retained_surface == nullptr || surface_width <= 0 || surface_height <= 0) {
      return false;
    }
    // The EGL window surface tracks the X window's real client size, which may
    // briefly differ from the recorded surface size (e.g. right after a window
    // manager resize). View to the actual surface and fall back to linear
    // filtering whenever a non-1:1 blit would otherwise drop thin glyph
    // strokes under nearest sampling.
    EGLint egl_width = 0;
    EGLint egl_height = 0;
    const bool egl_size_valid =
        eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &egl_width) == EGL_TRUE &&
        eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &egl_height) == EGL_TRUE;
    glViewport(
        0,
        0,
        egl_size_valid ? static_cast<GLsizei>(egl_width) : surface_width,
        egl_size_valid ? static_cast<GLsizei>(egl_height) : surface_height
    );
    if (!diag_printed_) {
      std::fprintf(
          stderr,
          "[HuxerUI GL] surface=%dx%d egl=%dx%d scale=%.2f bgra=%d renderer=%s\n",
          surface_width,
          surface_height,
          static_cast<int>(egl_width),
          static_cast<int>(egl_height),
          Scale(),
          use_bgra_upload ? 1 : 0,
          reinterpret_cast<const char*>(glGetString(GL_RENDERER))
      );
      diag_printed_ = true;
    }

    const GLenum format = use_bgra_upload ? GL_BGRA_EXT : GL_RGBA;
    if (texture_width != surface_width || texture_height != surface_height) {
      if (texture == 0) {
        glGenTextures(1, &texture);
      }
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, format, surface_width, surface_height, 0, format, GL_UNSIGNED_BYTE, nullptr);
      texture_width = surface_width;
      texture_height = surface_height;
    } else {
      glBindTexture(GL_TEXTURE_2D, texture);
    }
    const GLenum filter =
        egl_size_valid && static_cast<int>(egl_width) == surface_width &&
                static_cast<int>(egl_height) == surface_height
            ? GL_NEAREST
            : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const unsigned char* data = cairo_image_surface_get_data(retained_surface);
    const int stride = cairo_image_surface_get_stride(retained_surface);
    cairo_surface_flush(retained_surface);
    // Cairo drawing is clipped to the damage rects, so only the damaged rows of
    // the retained surface change between presents. Uploading just those rows
    // (GLES2 has no GL_UNPACK_ROW_LENGTH, hence per-row) reproduces the texture
    // exactly; multi-rect or large-rect damage falls back to the full upload.
    // A freshly reallocated texture has undefined storage outside the upload, so
    // partial uploads require the texture to already match the surface size.
    const bool partial =
        !pending_damage_full && pending_damage_rects.size() == 1 &&
        pending_damage_rects[0].height > 0 &&
        pending_damage_rects[0].height <= static_cast<unsigned short>(surface_height / 2) && surface_height > 0 &&
        texture_width == surface_width && texture_height == surface_height;
    if (partial) {
      const XRectangle& rect = pending_damage_rects[0];
      const int rect_left = std::clamp(static_cast<int>(rect.x), 0, surface_width);
      const int rect_top = std::clamp(static_cast<int>(rect.y), 0, surface_height);
      const int rect_width = std::clamp(static_cast<int>(rect.width), 0, surface_width - rect_left);
      const int rect_height = std::clamp(static_cast<int>(rect.height), 0, surface_height - rect_top);
      if (rect_width > 0 && rect_height > 0) {
        for (int row = 0; row < rect_height; ++row) {
          const int y = rect_top + row;
          glTexSubImage2D(
              GL_TEXTURE_2D,
              0,
              rect_left,
              y,
              rect_width,
              1,
              format,
              GL_UNSIGNED_BYTE,
              data + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
                  static_cast<std::size_t>(rect_left) * 4U
          );
        }
      }
    } else {
      if (stride == surface_width * 4) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface_width, surface_height, format, GL_UNSIGNED_BYTE, data);
      } else {
        // GLES2 has no GL_UNPACK_ROW_LENGTH, so a padded Cairo stride is copied
        // one row at a time.
        for (int row = 0; row < surface_height; ++row) {
          glTexSubImage2D(
              GL_TEXTURE_2D,
              0,
              0,
              row,
              surface_width,
              1,
              format,
              GL_UNSIGNED_BYTE,
              data + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride)
          );
        }
      }
    }
    pending_damage_full = true;
    pending_damage_rects.clear();

    if (!use_bgra_upload) {
      glUniform1i(uniform_bgra, 1);
    }

    glUseProgram(program);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
    glEnableVertexAttribArray(attrib_position);
    glVertexAttribPointer(attrib_uv, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));
    glEnableVertexAttribArray(attrib_uv);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(uniform_sampler, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (eglSwapBuffers(egl_display, egl_surface) != EGL_TRUE) {
      const EGLint error = eglGetError();
      if (error == EGL_BAD_SURFACE || error == EGL_BAD_CONTEXT || error == EGL_BAD_NATIVE_WINDOW) {
        // A lost or resized native window invalidates the surface; drop it so
        // the next Render recreates it, mirroring an out-of-date swap chain.
        static_cast<void>(eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        if (egl_surface != EGL_NO_SURFACE) {
          static_cast<void>(eglDestroySurface(egl_display, egl_surface));
          egl_surface = EGL_NO_SURFACE;
        }
        gl_ready = false;
      }
      return false;
    }
    return true;
  }
};

[[nodiscard]] std::vector<LayoutLine>
WrapLines(std::string_view text, const TextStyle& style, float wrap_width, TextWrap wrap, LinuxRenderer::State& state) {
  std::vector<LayoutLine> lines;
  if (text.empty()) {
    return lines;
  }
  const auto cached = state.paragraph_cache.find(ParagraphKeyView{text, style.font, wrap_width, wrap});
  if (cached != state.paragraph_cache.end()) {
    return cached->second;
  }
  const std::size_t length = text.size();
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  std::size_t last_space = std::string_view::npos;

  const auto flush_line = [&]() {
    if (line_end <= line_start) {
      return;
    }
    const std::string_view span = text.substr(line_start, line_end - line_start);
    state.ShapeLine(span, style, line_start, lines);
  };

  if (wrap == TextWrap::NoWrap) {
    std::size_t offset = 0;
    while (offset < length) {
      if (text[offset] == '\n') {
        line_end = offset;
        flush_line();
        line_start = offset + 1;
      }
      ++offset;
    }
    line_end = length;
    flush_line();
    return lines;
  }

  std::size_t offset = 0;
  while (offset < length) {
    const char ch = text[offset];
    if (ch == '\n') {
      line_end = offset;
      flush_line();
      line_start = offset + 1;
      last_space = std::string_view::npos;
      ++offset;
      continue;
    }
    if (ch == ' ' || ch == '\t') {
      last_space = offset;
    }
    const std::string_view candidate = text.substr(line_start, offset + 1 - line_start);
    const float probe_advance = state.ProbeAdvance(candidate, style);
    if (probe_advance <= wrap_width) {
      ++offset;
      continue;
    }
    if (last_space != std::string_view::npos && last_space > line_start) {
      line_end = last_space;
      flush_line();
      line_start = last_space + 1;
      last_space = std::string_view::npos;
      continue;
    }
    line_end = offset > line_start ? offset : line_start + 1;
    flush_line();
    line_start = line_end;
    last_space = std::string_view::npos;
  }
  line_end = length;
  flush_line();
  if (state.paragraph_cache.size() >= state.kMaxParagraphs) {
    state.paragraph_cache.erase(state.paragraph_cache.begin());
  }
  state.paragraph_cache.emplace(ParagraphKey{std::string(text), style.font, wrap_width, wrap}, lines);
  return lines;
}

LinuxRenderer::LinuxRenderer() : state_(std::make_unique<State>()) {}
LinuxRenderer::~LinuxRenderer() {
  Discard();
}

void LinuxRenderer::Initialize() {
  state_->EnsureFreeType();
  state_->EnsureFontconfig();
}

void LinuxRenderer::Discard() noexcept {
  if (state_ == nullptr) {
    return;
  }
  state_->DestroyGlResources();
  for (auto& [key, entry] : state_->image_cache) {
    if (entry.surface != nullptr) {
      cairo_surface_destroy(entry.surface);
    }
  }
  state_->image_cache.clear();
  state_->image_cache_bytes = 0;
  for (auto& [key, face] : state_->cairo_font_cache) {
    cairo_font_face_destroy(face);
  }
  state_->cairo_font_cache.clear();
  for (auto& [key, face] : state_->cairo_fallback_font_cache) {
    cairo_font_face_destroy(face);
  }
  state_->cairo_fallback_font_cache.clear();
  for (auto& [key, face] : state_->fallback_font_cache) {
    FT_Done_Face(face);
  }
  state_->fallback_font_cache.clear();
  if (state_->retained_context != nullptr) {
    cairo_destroy(state_->retained_context);
    state_->retained_context = nullptr;
  }
  if (state_->retained_surface != nullptr) {
    cairo_surface_destroy(state_->retained_surface);
    state_->retained_surface = nullptr;
  }
  state_->surface_width = 0;
  state_->surface_height = 0;
}

void LinuxRenderer::ResetDeviceResources() noexcept {
  if (state_ == nullptr) {
    return;
  }
  state_->DestroyGlResources();
  state_->force_full_repaint = true;
}

void LinuxRenderer::Resize(Display* display, Window window, int width, int height, float dpi) {
  state_->display = display;
  state_->window = window;
  state_->dpi = dpi;
  state_->surface_width = std::max(1, width);
  state_->surface_height = std::max(1, height);
  state_->force_full_repaint = true;
}

void LinuxRenderer::DpiChanged(Display* display, Window window, float dpi) {
  state_->display = display;
  state_->window = window;
  state_->dpi = dpi;
  state_->force_full_repaint = true;
}

FontMetrics LinuxRenderer::Metrics(const Font& font) {
  return state_->MetricsFor(font);
}

TextRunMetrics
LinuxRenderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
  static_cast<void>(options);
  if (text.empty()) {
    const FontMetrics empty_metrics = Metrics(style.font);
    return {0.0F, {0.0F, 0.0F, 0.0F, 0.0F}, empty_metrics};
  }
  ShapedRun run = state_->ShapeRun(text, style);
  const FontMetrics metrics = state_->MetricsFor(style.font);
  const Rect visual_bounds{0.0F, -metrics.ascent, run.advance, metrics.ascent + metrics.descent};
  return {run.advance, visual_bounds, metrics};
}

TextLayoutMetrics LinuxRenderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  if (text.empty()) {
    return {};
  }
  const bool constrained = std::isfinite(max_width) && max_width > 0.0F;
  const float wrap_width = constrained ? max_width : std::numeric_limits<float>::max();
  std::vector<LayoutLine> lines = WrapLines(text, style, wrap_width, options.wrap, *state_);

  if (lines.empty()) {
    return {};
  }

  float width = 0.0F;
  float total_height = 0.0F;
  for (LayoutLine& line : lines) {
    line.baseline = total_height + line.ascent;
    total_height += line.ascent + line.descent + line.leading;
    width = std::max(width, line.advance);
  }
  if (constrained) {
    width = std::min(width, max_width);
  }
  const float first_baseline = lines.front().baseline;
  const float last_baseline = lines.back().baseline;
  return {
      {std::ceil(width), std::ceil(total_height)},
      first_baseline,
      last_baseline,
      lines.size(),
  };
}

std::unique_ptr<TextLayout> LinuxRenderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  const bool constrained = std::isfinite(max_width) && max_width > 0.0F;
  const float wrap_width = constrained ? max_width : std::numeric_limits<float>::max();
  std::vector<LayoutLine> lines = WrapLines(text, style, wrap_width, options.wrap, *state_);

  if (lines.empty()) {
    const FontMetrics metrics = state_->MetricsFor(style.font);
    LayoutLine empty_line;
    empty_line.ascent = metrics.ascent;
    empty_line.descent = metrics.descent;
    empty_line.leading = metrics.leading;
    empty_line.baseline = metrics.ascent;
    lines.push_back(std::move(empty_line));
  }

  float width = 0.0F;
  float total_height = 0.0F;
  for (LayoutLine& line : lines) {
    line.baseline = total_height + line.ascent;
    total_height += line.ascent + line.descent + line.leading;
    width = std::max(width, line.advance);
  }
  if (constrained) {
    width = std::min(width, max_width);
  }
  return std::make_unique<LinuxTextLayout>(std::string(text), std::move(lines));
}

LinuxRenderResult LinuxRenderer::Render(
    Display* display,
    Window window,
    float dpi,
    const RenderFrame& frame,
    const XRectangle* damage_rects,
    unsigned long damage_count
) {
  state_->display = display;
  state_->window = window;
  state_->dpi = dpi;
  if (state_->surface_width <= 0 || state_->surface_height <= 0) {
    return LinuxRenderResult::Skipped;
  }

  if (state_->retained_surface == nullptr ||
      cairo_image_surface_get_width(state_->retained_surface) != state_->surface_width ||
      cairo_image_surface_get_height(state_->retained_surface) != state_->surface_height) {
    if (state_->retained_context != nullptr) {
      cairo_destroy(state_->retained_context);
      state_->retained_context = nullptr;
    }
    if (state_->retained_surface != nullptr) {
      cairo_surface_destroy(state_->retained_surface);
      state_->retained_surface = nullptr;
    }
    state_->retained_surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, state_->surface_width, state_->surface_height);
    state_->retained_context = cairo_create(state_->retained_surface);
    state_->force_full_repaint = true;
  }
  if (state_->retained_surface == nullptr || state_->retained_context == nullptr) {
    return LinuxRenderResult::Skipped;
  }

  const float scale = state_->Scale();
  cairo_t* cr = state_->retained_context;
  cairo_save(cr);
  cairo_scale(cr, scale, scale);

  const bool full = state_->force_full_repaint || damage_count == 0;
  cairo_reset_clip(cr);
  if (!full) {
    cairo_new_path(cr);
    for (unsigned long index = 0; index < damage_count; ++index) {
      const XRectangle& rect = damage_rects[index];
      cairo_rectangle(
          cr,
          static_cast<double>(rect.x) / scale,
          static_cast<double>(rect.y) / scale,
          static_cast<double>(rect.width) / scale,
          static_cast<double>(rect.height) / scale
      );
    }
    cairo_clip(cr);
  }

  const Color background = Color::Rgb(247, 248, 250);
  cairo_set_source_rgba(cr, background.red, background.green, background.blue, background.alpha);
  cairo_paint(cr);

  if (frame.scene.root != nullptr) {
    RenderSceneNode(*frame.scene.root);
  }

  cairo_restore(cr);

  if (!state_->gl_ready && !EnsureGl(display, window)) {
    state_->force_full_repaint = true;
    return LinuxRenderResult::Skipped;
  }

  state_->pending_damage_full = full;
  if (!full) {
    state_->pending_damage_rects.assign(damage_rects, damage_rects + damage_count);
  }

  const bool presented = PresentGl();
  state_->force_full_repaint = false;
  return presented ? LinuxRenderResult::Presented : LinuxRenderResult::Recreate;
}

namespace {

void SetSourceColor(cairo_t* cr, const Color& color) {
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
}

[[nodiscard]] cairo_line_cap_t CairoLineCap(StrokeCap cap) noexcept {
  switch (cap) {
  case StrokeCap::Butt:
    return CAIRO_LINE_CAP_BUTT;
  case StrokeCap::Round:
    return CAIRO_LINE_CAP_ROUND;
  case StrokeCap::Square:
    return CAIRO_LINE_CAP_SQUARE;
  default:
    return CAIRO_LINE_CAP_BUTT;
  }
}

[[nodiscard]] cairo_line_join_t CairoLineJoin(StrokeJoin join) noexcept {
  switch (join) {
  case StrokeJoin::Miter:
    return CAIRO_LINE_JOIN_MITER;
  case StrokeJoin::Round:
    return CAIRO_LINE_JOIN_ROUND;
  case StrokeJoin::Bevel:
    return CAIRO_LINE_JOIN_BEVEL;
  default:
    return CAIRO_LINE_JOIN_MITER;
  }
}

void AddRoundedRect(cairo_t* cr, const Rect& rect, float corner_radius) {
  const float radius = std::clamp(corner_radius, 0.0F, std::min(rect.width, rect.height) * 0.5F);
  if (radius <= 0.0F) {
    cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
    return;
  }
  const double right = rect.x + rect.width;
  const double bottom = rect.y + rect.height;
  cairo_new_sub_path(cr);
  cairo_arc(cr, right - radius, rect.y + radius, radius, -kPi / 2.0, 0.0);
  cairo_arc(cr, right - radius, bottom - radius, radius, 0.0, kPi / 2.0);
  cairo_arc(cr, rect.x + radius, bottom - radius, radius, kPi / 2.0, kPi);
  cairo_arc(cr, rect.x + radius, rect.y + radius, radius, kPi, 3.0 * kPi / 2.0);
  cairo_close_path(cr);
}

void AppendPathContour(cairo_t* cr, const Path& path) {
  double previous_x = 0.0;
  double previous_y = 0.0;
  for (const PathElement& element : PathAccess::Elements(path)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      cairo_move_to(cr, element.points[0].x, element.points[0].y);
      previous_x = element.points[0].x;
      previous_y = element.points[0].y;
      break;
    case PathVerb::LineTo:
      cairo_line_to(cr, element.points[0].x, element.points[0].y);
      previous_x = element.points[0].x;
      previous_y = element.points[0].y;
      break;
    case PathVerb::QuadraticTo: {
      const Point control = element.points[0];
      const Point end = element.points[1];
      const double first_control_x = previous_x + 2.0 / 3.0 * (control.x - previous_x);
      const double first_control_y = previous_y + 2.0 / 3.0 * (control.y - previous_y);
      const double second_control_x = end.x + 2.0 / 3.0 * (control.x - end.x);
      const double second_control_y = end.y + 2.0 / 3.0 * (control.y - end.y);
      cairo_curve_to(cr, first_control_x, first_control_y, second_control_x, second_control_y, end.x, end.y);
      previous_x = end.x;
      previous_y = end.y;
      break;
    }
    case PathVerb::CubicTo:
      cairo_curve_to(
          cr,
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y,
          element.points[2].x,
          element.points[2].y
      );
      previous_x = element.points[2].x;
      previous_y = element.points[2].y;
      break;
    case PathVerb::Close:
      cairo_close_path(cr);
      break;
    }
  }
}

void AppendPath(cairo_t* cr, const Path& path) {
  cairo_new_path(cr);
  AppendPathContour(cr, path);
}
class ScenePainter {
public:
  ScenePainter(LinuxRenderer::State& state, cairo_t* context) : state_(state), cr_(context) {}

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
      cairo_save(cr_);
      ApplyTransform(transform);
    }

    const bool group_opacity = opacity < 1.0F;
    if (group_opacity) {
      cairo_push_group(cr_);
    }

    RenderSequence(node.content);
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this](const auto& command) { RenderCommand(command); }, clip);
    }
    const bool children_transformed = !node.children_transform.IsIdentity();
    if (children_transformed) {
      cairo_save(cr_);
      ApplyTransform(node.children_transform);
    }
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        RenderSceneNode(*child);
      }
    }
    if (children_transformed) {
      cairo_restore(cr_);
    }
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
      cairo_restore(cr_);
    }
    RenderSequence(node.foreground);

    if (group_opacity) {
      cairo_pop_group_to_source(cr_);
      cairo_paint_with_alpha(cr_, opacity);
    }
    if (transformed) {
      cairo_restore(cr_);
    }
  }

private:
  void RenderSequence(const PaintSequence& sequence) {
    for (const PaintCommand& command : sequence.Commands()) {
      std::visit([this](const auto& value) { RenderCommand(value); }, command);
    }
  }

  void ApplyTransform(const Transform2D& transform) {
    cairo_matrix_t previous{};
    cairo_get_matrix(cr_, &previous);
    cairo_matrix_t current{};
    cairo_matrix_init(
        &current,
        transform.m11,
        transform.m12,
        transform.m21,
        transform.m22,
        transform.translate_x,
        transform.translate_y
    );
    cairo_matrix_t combined{};
    cairo_matrix_multiply(&combined, &current, &previous);
    cairo_set_matrix(cr_, &combined);
  }

  void RenderCommand(const DrawRectCommand& command) {
    SetSourceColor(cr_, command.color);
    AddRoundedRect(cr_, command.rect, command.corner_radius);
    cairo_fill(cr_);
  }

  void RenderCommand(const DrawTextCommand& command) {
    DrawParagraph(command.rect, command.text, command.style, command.options);
  }

  void RenderCommand(const DrawTextRunsCommand& command) {
    for (const TextRun& run : command.runs) {
      ShapedRun shaped = state_.ShapeRun(run.text, run.style);
      cairo_font_face_t* current_face = state_.CairoFontFor(run.style.font);
      cairo_set_font_face(cr_, current_face);
      cairo_set_font_size(cr_, run.style.font.Size());
      SetSourceColor(cr_, run.style.foreground);
      std::vector<cairo_glyph_t> batch;
      batch.reserve(shaped.glyphs.size());
      const auto flush = [&]() {
        if (!batch.empty()) {
          cairo_show_glyphs(cr_, batch.data(), static_cast<int>(batch.size()));
          batch.clear();
        }
      };
      double x = run.baseline_origin.x;
      for (const ShapedGlyph& glyph : shaped.glyphs) {
        if (glyph.fallback) {
          cairo_font_face_t* fallback =
              state_.CairoFallbackFontFor(Utf8CodePointAt(run.text, glyph.cluster), run.style.font.Size());
          if (fallback != nullptr && fallback != current_face) {
            flush();
            cairo_set_font_face(cr_, fallback);
            current_face = fallback;
          }
        }
        batch.push_back({glyph.index, x + glyph.x_offset, run.baseline_origin.y + glyph.y_offset});
        x += glyph.x_advance;
      }
      flush();
    }
  }

  void RenderCommand(const DrawImageCommand& command) {
    cairo_surface_t* surface = state_.ImageSurfaceFor(command.image);
    if (surface == nullptr) {
      return;
    }
    const float image_scale = command.image.Scale();
    const double source_x = command.source.x * image_scale;
    const double source_y = command.source.y * image_scale;
    const double source_width = command.source.width * image_scale;
    const double source_height = command.source.height * image_scale;
    cairo_save(cr_);
    cairo_rectangle(
        cr_,
        command.destination.x,
        command.destination.y,
        command.destination.width,
        command.destination.height
    );
    cairo_clip(cr_);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(surface);
    cairo_matrix_t matrix{};
    cairo_matrix_init(
        &matrix,
        source_width / command.destination.width,
        0.0,
        0.0,
        source_height / command.destination.height,
        source_x,
        source_y
    );
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_filter(
        pattern,
        command.sampling == ImageSampling::Nearest ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR
    );
    cairo_set_source(cr_, pattern);
    if (command.opacity >= 1.0F) {
      cairo_paint(cr_);
    } else {
      cairo_paint_with_alpha(cr_, command.opacity);
    }
    cairo_pattern_destroy(pattern);
    cairo_restore(cr_);
  }

  void RenderCommand(const DrawCircleCommand& command) {
    SetSourceColor(cr_, command.color);
    cairo_new_path(cr_);
    cairo_arc(cr_, command.center.x, command.center.y, command.radius, 0.0, kTau);
    cairo_fill(cr_);
  }

  void RenderCommand(const DrawArcCommand& command) {
    SetSourceColor(cr_, command.color);
    cairo_set_line_width(cr_, command.width);
    cairo_set_line_cap(cr_, CairoLineCap(command.cap));
    cairo_new_path(cr_);
    cairo_arc(
        cr_,
        command.center.x,
        command.center.y,
        command.radius,
        command.start_angle,
        command.start_angle + command.sweep_angle
    );
    cairo_stroke(cr_);
  }

  void RenderCommand(const DrawBorderCommand& command) {
    SetSourceColor(cr_, command.color);
    cairo_set_line_width(cr_, command.width);
    const float inset = command.width * 0.5F;
    const Rect inner{
        command.rect.x + inset,
        command.rect.y + inset,
        std::max(0.0F, command.rect.width - command.width),
        std::max(0.0F, command.rect.height - command.width),
    };
    AddRoundedRect(cr_, inner, std::max(0.0F, command.corner_radius - inset));
    cairo_stroke(cr_);
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
    const float scale = state_.Scale();
    const float blur_pixels = command.blur_radius * scale;
    const int mask_width = std::max(1, static_cast<int>(std::ceil(resolved.bounds.width * scale)));
    const int mask_height = std::max(1, static_cast<int>(std::ceil(resolved.bounds.height * scale)));
    CairoSurfaceHandle mask_handle{cairo_image_surface_create(CAIRO_FORMAT_A8, mask_width, mask_height)};
    cairo_surface_t* mask = mask_handle.surface;
    cairo_t* mask_cr = cairo_create(mask);
    cairo_scale(mask_cr, scale, scale);
    cairo_translate(mask_cr, -resolved.bounds.x, -resolved.bounds.y);
    cairo_set_source_rgb(mask_cr, 1.0, 1.0, 1.0);
    AddRoundedRect(mask_cr, resolved.caster, resolved.corner_radius);
    cairo_fill(mask_cr);
    cairo_destroy(mask_cr);

    const int radius = std::max(1, static_cast<int>(std::ceil(blur_pixels * 0.57735)));
    cairo_surface_t* blurred = BoxBlurMask(mask, radius);

    SetSourceColor(cr_, command.color);
    cairo_save(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, resolved.bounds.x, resolved.bounds.y, mask_width / scale, mask_height / scale);
    AddRoundedRect(cr_, resolved.caster, resolved.corner_radius);
    cairo_set_fill_rule(cr_, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr_);
    cairo_translate(cr_, resolved.bounds.x, resolved.bounds.y);
    cairo_scale(cr_, 1.0 / scale, 1.0 / scale);
    cairo_pattern_t* blur_pattern = cairo_pattern_create_for_surface(blurred);
    cairo_pattern_set_extend(blur_pattern, CAIRO_EXTEND_NONE);
    cairo_mask(cr_, blur_pattern);
    cairo_pattern_destroy(blur_pattern);
    cairo_restore(cr_);
    cairo_surface_destroy(blurred);
  }

  [[nodiscard]] cairo_surface_t* BoxBlurMask(cairo_surface_t* mask, int radius) {
    const int width = cairo_image_surface_get_width(mask);
    const int height = cairo_image_surface_get_height(mask);
    const unsigned char* source = cairo_image_surface_get_data(mask);
    state_.blur_scratch.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
      int sum = 0;
      for (int x = -radius; x <= radius; ++x) {
        const int clamped_x = std::clamp(x, 0, width - 1);
        sum += source[y * width + clamped_x];
      }
      for (int x = 0; x < width; ++x) {
        state_.blur_scratch[y * width + x] = static_cast<unsigned char>(sum / window);
        const int remove_x = std::clamp(x - radius, 0, width - 1);
        const int add_x = std::clamp(x + radius + 1, 0, width - 1);
        sum += source[y * width + add_x] - source[y * width + remove_x];
      }
    }
    cairo_surface_t* result = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    unsigned char* result_data = cairo_image_surface_get_data(result);
    for (int x = 0; x < width; ++x) {
      int sum = 0;
      for (int y = -radius; y <= radius; ++y) {
        const int clamped_y = std::clamp(y, 0, height - 1);
        sum += state_.blur_scratch[clamped_y * width + x];
      }
      for (int y = 0; y < height; ++y) {
        result_data[y * width + x] = static_cast<unsigned char>(sum / window);
        const int remove_y = std::clamp(y - radius, 0, height - 1);
        const int add_y = std::clamp(y + radius + 1, 0, height - 1);
        sum += state_.blur_scratch[add_y * width + x] - state_.blur_scratch[remove_y * width + x];
      }
    }
    cairo_surface_mark_dirty(result);
    return result;
  }

  void RenderCommand(const FillPathCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(cr_, command.color);
    AppendPath(cr_, command.path);
    cairo_set_fill_rule(
        cr_,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(cr_);
  }

  void RenderCommand(const StrokePathCommand& command) {
    if (command.path.IsEmpty() || command.width <= 0.0F || command.color.alpha <= 0.0F) {
      return;
    }
    SetSourceColor(cr_, command.color);
    AppendPath(cr_, command.path);
    cairo_set_line_width(cr_, command.width);
    cairo_set_line_cap(cr_, CairoLineCap(command.cap));
    cairo_set_line_join(cr_, CairoLineJoin(command.join));
    cairo_set_miter_limit(cr_, command.miter_limit);
    cairo_stroke(cr_);
  }

  void RenderCommand(const DrawPathShadowCommand& command) {
    if (command.path.IsEmpty() || command.color.alpha <= 0.0F) {
      return;
    }
    const float scale = state_.Scale();
    const Rect bounds = command.path.Bounds();
    const Rect shadow_bounds{
        bounds.x + command.offset.x - command.blur_radius,
        bounds.y + command.offset.y - command.blur_radius,
        bounds.width + command.blur_radius * 2.0F,
        bounds.height + command.blur_radius * 2.0F,
    };
    if (command.blur_radius <= 0.0F) {
      SetSourceColor(cr_, command.color);
      cairo_save(cr_);
      cairo_translate(cr_, command.offset.x, command.offset.y);
      AppendPath(cr_, command.path);
      cairo_set_fill_rule(
          cr_,
          command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
      );
      cairo_fill(cr_);
      cairo_restore(cr_);
      return;
    }
    const int mask_width = std::max(1, static_cast<int>(std::ceil(shadow_bounds.width * scale)));
    const int mask_height = std::max(1, static_cast<int>(std::ceil(shadow_bounds.height * scale)));
    CairoSurfaceHandle mask_handle{cairo_image_surface_create(CAIRO_FORMAT_A8, mask_width, mask_height)};
    cairo_surface_t* mask = mask_handle.surface;
    cairo_t* mask_cr = cairo_create(mask);
    cairo_scale(mask_cr, scale, scale);
    cairo_translate(mask_cr, -shadow_bounds.x, -shadow_bounds.y);
    cairo_translate(mask_cr, command.offset.x, command.offset.y);
    cairo_set_source_rgb(mask_cr, 1.0, 1.0, 1.0);
    AppendPath(mask_cr, command.path);
    cairo_set_fill_rule(
        mask_cr,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(mask_cr);
    cairo_destroy(mask_cr);
    cairo_surface_t* blurred =
        BoxBlurMask(mask, std::max(1, static_cast<int>(std::ceil(command.blur_radius * scale * 0.57735))));

    SetSourceColor(cr_, command.color);
    cairo_save(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, shadow_bounds.x, shadow_bounds.y, mask_width / scale, mask_height / scale);
    cairo_matrix_t previous{};
    cairo_get_matrix(cr_, &previous);
    cairo_translate(cr_, command.offset.x, command.offset.y);
    AppendPathContour(cr_, command.path);
    cairo_set_matrix(cr_, &previous);
    cairo_set_fill_rule(cr_, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr_);
    cairo_translate(cr_, shadow_bounds.x, shadow_bounds.y);
    cairo_scale(cr_, 1.0 / scale, 1.0 / scale);
    cairo_pattern_t* blur_pattern = cairo_pattern_create_for_surface(blurred);
    cairo_pattern_set_extend(blur_pattern, CAIRO_EXTEND_NONE);
    cairo_mask(cr_, blur_pattern);
    cairo_pattern_destroy(blur_pattern);
    cairo_restore(cr_);
    cairo_surface_destroy(blurred);
  }

  void RenderCommand(const PushClipCommand& command) {
    cairo_save(cr_);
    AddRoundedRect(cr_, command.rect, command.corner_radius);
    cairo_clip(cr_);
  }

  void RenderCommand(const PushPathClipCommand& command) {
    cairo_save(cr_);
    AppendPath(cr_, command.path);
    cairo_set_fill_rule(
        cr_,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_clip(cr_);
  }

  void RenderCommand(const PopClipCommand&) {
    cairo_restore(cr_);
  }

  void RenderCommand(const PushTransformCommand& command) {
    cairo_save(cr_);
    ApplyTransform(command.transform);
  }

  void RenderCommand(const PopTransformCommand&) {
    cairo_restore(cr_);
  }

  void RenderCommand(const PlacePlatformViewCommand&) {
    throw std::logic_error("HuxerUI Linux adapter does not support PlatformView composition yet");
  }

  void
  DrawParagraph(const Rect& rect, std::string_view text, const TextStyle& style, const TextLayoutOptions& options) {
    if (text.empty()) {
      return;
    }
    const float wrap_width =
        std::isfinite(rect.width) && rect.width > 0.0F ? rect.width : std::numeric_limits<float>::max();
    std::vector<LayoutLine> lines = WrapLines(text, style, wrap_width, options.wrap, state_);
    if (lines.empty()) {
      return;
    }
    float total_height = 0.0F;
    for (LayoutLine& line : lines) {
      line.baseline = total_height + line.ascent;
      total_height += line.ascent + line.descent + line.leading;
    }
    float y = rect.y;
    if (options.wrap == TextWrap::NoWrap && total_height < rect.height) {
      y += (rect.height - total_height) * 0.5F;
    }
    cairo_font_face_t* current_face = state_.CairoFontFor(style.font);
    cairo_set_font_face(cr_, current_face);
    cairo_set_font_size(cr_, style.font.Size());
    SetSourceColor(cr_, style.foreground);
    std::vector<cairo_glyph_t> batch;
    const auto flush = [&]() {
      if (!batch.empty()) {
        cairo_show_glyphs(cr_, batch.data(), static_cast<int>(batch.size()));
        batch.clear();
      }
    };
    for (const LayoutLine& line : lines) {
      float x = rect.x;
      if (options.align == TextAlign::Center) {
        x += std::max(0.0F, (rect.width - line.advance) * 0.5F);
      } else if (options.align == TextAlign::Trailing) {
        x += std::max(0.0F, rect.width - line.advance);
      }
      batch.reserve(line.glyphs.size());
      const double baseline_y = y + line.baseline;
      double glyph_x = x;
      for (const ShapedGlyph& glyph : line.glyphs) {
        if (glyph.fallback) {
          cairo_font_face_t* fallback =
              state_.CairoFallbackFontFor(Utf8CodePointAt(line.text, glyph.cluster), style.font.Size());
          if (fallback != nullptr && fallback != current_face) {
            flush();
            cairo_set_font_face(cr_, fallback);
            current_face = fallback;
          }
        }
        batch.push_back({glyph.index, glyph_x + glyph.x_offset, baseline_y + glyph.y_offset});
        glyph_x += glyph.x_advance;
      }
      flush();
    }
  }

  LinuxRenderer::State& state_;
  cairo_t* cr_;
};

} // namespace

void LinuxRenderer::RenderSceneNode(const RenderNode& node) {
  if (state_->retained_context == nullptr) {
    // A null cairo_t would crash on the first cairo call in ScenePainter.
    return;
  }
  ScenePainter painter(*state_, state_->retained_context);
  painter.RenderSceneNode(node);
}

bool LinuxRenderer::InitializePresentation(Display* display) {
  return state_->InitializePresentation(display);
}

bool LinuxRenderer::EnsureGl(Display* display, Window window) {
  return state_->EnsureGl(display, window);
}

bool LinuxRenderer::PresentGl() {
  return state_->PresentGl();
}

bool LinuxRenderer::HasPresentation() const noexcept {
  // egl_ready means a display and config were chosen, so the adapter can adopt
  // the native visual id before the window is created.
  return state_->egl_ready;
}

unsigned long LinuxRenderer::NativeVisualId() const noexcept {
  return state_->native_visual_id;
}

} // namespace huxerui::detail
