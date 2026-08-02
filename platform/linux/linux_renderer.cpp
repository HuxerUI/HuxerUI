#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

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
#include "text_layout_internal.h"

namespace huxerui::detail {

namespace {

constexpr float kDipsPerInch = 96.0F;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kTau = 6.28318530717958647692F;

struct FontKey {
  huxerui::Font font;

  bool operator<(const FontKey& other) const {
    if (font.FamilyKind() != other.font.FamilyKind()) {
      return font.FamilyKind() < other.font.FamilyKind();
    }
    const std::string_view family = font.FamilyName();
    const std::string_view other_family = other.font.FamilyName();
    if (family != other_family) {
      return family < other_family;
    }
    if (font.Size() != other.font.Size()) {
      return font.Size() < other.font.Size();
    }
    if (font.Weight() != other.font.Weight()) {
      return font.Weight() < other.font.Weight();
    }
    return font.Slant() < other.font.Slant();
  }
};

struct ShapedGlyph {
  std::uint32_t index = 0;
  float x_advance = 0.0F;
  float x_offset = 0.0F;
  float y_offset = 0.0F;
  std::uint32_t cluster = 0;
  // Set when the glyph came from a fontconfig fallback face; the index belongs
  // to that face, so drawing must switch Cairo to the matching fallback font.
  bool fallback = false;
};

struct ShapedRun {
  std::vector<ShapedGlyph> glyphs;
  float advance = 0.0F;
};

// One shaped visual line. Offsets into text are UTF-8 byte offsets.
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

// Converts a UTF-16 code unit offset into a UTF-8 byte offset. Surrogate pairs
// count as two units; an offset inside a pair resolves to the pair's start.
[[nodiscard]] std::size_t Utf16ToUtf8(std::string_view text, std::size_t utf16_offset) {
  std::size_t utf16 = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t width = Utf8Width(static_cast<unsigned char>(text[offset]));
    if (offset + width > text.size()) {
      break;
    }
    const std::uint32_t code_point = DecodeCodePoint(text, offset, width);
    const std::size_t units = code_point > 0xFFFF ? 2 : 1;
    if (utf16 + units > utf16_offset) {
      return offset;
    }
    utf16 += units;
    offset += width;
  }
  return text.size();
}

// Converts a UTF-8 byte offset into a UTF-16 code unit offset.
[[nodiscard]] std::size_t Utf8ToUtf16(std::string_view text, std::size_t byte_offset) {
  std::size_t utf16 = 0;
  std::size_t offset = 0;
  while (offset < byte_offset && offset < text.size()) {
    const std::size_t width = Utf8Width(static_cast<unsigned char>(text[offset]));
    if (offset + width > text.size()) {
      break;
    }
    const std::uint32_t code_point = DecodeCodePoint(text, offset, width);
    utf16 += code_point > 0xFFFF ? 2 : 1;
    offset += width;
  }
  return utf16;
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

// FontWeight uses OpenType semantics (100-900) while fontconfig weights run
// from 0 to 210; convert so pattern matching asks for the intended weight.
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

// Shaped paragraph for editing: caret, hit test, and range geometry resolve
// against the shaped lines without re-shaping. Mirrors Win32TextLayout.
class LinuxTextLayout final : public TextLayout {
public:
  LinuxTextLayout(std::string text, std::vector<LayoutLine> lines) : text_(std::move(text)), lines_(std::move(lines)) {
    for (const LayoutLine& line : lines_) {
      width_ = std::max(width_, line.advance);
      total_height_ += line.ascent + line.descent + line.leading;
    }
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
        const std::size_t glyph_utf16 = Utf8ToUtf16(text_, line.byte_start + glyph.cluster);
        if (distance < best_distance || (distance == best_distance && glyph_utf16 > 0)) {
          const bool trailing = point.x > (x + glyph_end) * 0.5F;
          // A trailing hit resolves past the glyph's whole code point so the
          // caret lands on a code point boundary, never inside one.
          const std::size_t resolved_utf16 =
              trailing ? Utf8ToUtf16(
                             text_,
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
        best_offset = static_cast<TextOffset>(Utf8ToUtf16(text_, line.byte_start + line.text.size()));
        best_affinity = TextAffinity::Downstream;
        best_distance = x - point.x;
      }
    }
    return {best_offset, best_affinity};
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const std::size_t utf16_length = Utf8ToUtf16(text_, text_.size());
    const TextOffset clamped = std::clamp<TextOffset>(offset, 0, static_cast<TextOffset>(utf16_length));
    for (const LayoutLine& line : lines_) {
      const std::size_t line_start_utf16 = Utf8ToUtf16(text_, line.byte_start);
      const std::size_t line_end_utf16 = Utf8ToUtf16(text_, line.byte_start + line.text.size());
      if (clamped < static_cast<TextOffset>(line_start_utf16) || clamped > static_cast<TextOffset>(line_end_utf16)) {
        continue;
      }
      float x = 0.0F;
      for (const ShapedGlyph& glyph : line.glyphs) {
        const std::size_t glyph_utf16 = Utf8ToUtf16(text_, line.byte_start + glyph.cluster);
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
    const std::size_t utf16_length = Utf8ToUtf16(text_, text_.size());
    const TextOffset start = std::clamp<TextOffset>(range.start, 0, static_cast<TextOffset>(utf16_length));
    const TextOffset end = std::clamp<TextOffset>(range.end, start, static_cast<TextOffset>(utf16_length));
    if (start == end) {
      return rects;
    }
    for (const LayoutLine& line : lines_) {
      const std::size_t line_start_utf16 = Utf8ToUtf16(text_, line.byte_start);
      const std::size_t line_end_utf16 = Utf8ToUtf16(text_, line.byte_start + line.text.size());
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
    const std::size_t utf16_length = Utf8ToUtf16(text_, text_.size());
    const TextOffset clamped = std::clamp<TextOffset>(offset, 0, static_cast<TextOffset>(utf16_length));
    if (clamped <= 0) {
      return 0;
    }
    const std::size_t utf8 = Utf16ToUtf8(text_, static_cast<std::size_t>(clamped));
    if (utf8 == 0) {
      return 0;
    }
    const std::size_t width = Utf8Width(static_cast<unsigned char>(text_[utf8 - 1]));
    return static_cast<TextOffset>(Utf8ToUtf16(text_, utf8 - width));
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const std::size_t utf16_length = Utf8ToUtf16(text_, text_.size());
    const TextOffset clamped = std::clamp<TextOffset>(offset, 0, static_cast<TextOffset>(utf16_length));
    const std::size_t utf8 = Utf16ToUtf8(text_, static_cast<std::size_t>(clamped));
    if (utf8 >= text_.size()) {
      return static_cast<TextOffset>(utf16_length);
    }
    const std::size_t width = Utf8Width(static_cast<unsigned char>(text_[utf8]));
    return static_cast<TextOffset>(Utf8ToUtf16(text_, utf8 + width));
  }

private:
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
    float x = 0.0F;
    for (const ShapedGlyph& glyph : line.glyphs) {
      const std::size_t glyph_utf16 = Utf8ToUtf16(text_, line.byte_start + glyph.cluster);
      if (static_cast<TextOffset>(glyph_utf16 - Utf8ToUtf16(text_, line.byte_start)) >= utf16_within_line) {
        break;
      }
      x += glyph.x_advance;
    }
    return x;
  }

  std::string text_;
  std::vector<LayoutLine> lines_;
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

// libjpeg's default error handler terminates the process on corrupt input;
// route every decode error through longjmp so a bad file fails the decode
// instead of killing the host application.
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
  // jpeg_mem_src only reads the input bytes; the const_cast drops the const the
  // legacy C API cannot express.
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

} // namespace

struct LinuxRenderer::State {
  FT_Library ft_library = nullptr;
  bool ft_ready = false;
  bool fc_ready = false;
  std::map<FontKey, FT_Face> font_cache;
  std::map<FontKey, cairo_font_face_t*> cairo_font_cache;
  std::map<std::pair<std::uint32_t, float>, FT_Face> fallback_font_cache;
  std::map<std::pair<std::uint32_t, float>, cairo_font_face_t*> cairo_fallback_font_cache;

  cairo_surface_t* retained_surface = nullptr;
  cairo_t* retained_context = nullptr;
  int surface_width = 0;
  int surface_height = 0;
  bool force_full_repaint = true;

  Display* display = nullptr;
  Window window = 0;
  float dpi = kDipsPerInch;

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkQueue present_queue = VK_NULL_HANDLE;
  std::uint32_t graphics_family = 0;
  std::uint32_t present_family = 0;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D swapchain_extent{};
  std::vector<VkImage> swapchain_images;
  VkImage transfer_image = VK_NULL_HANDLE;
  VkDeviceMemory transfer_memory = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkSemaphore image_available = VK_NULL_HANDLE;
  VkSemaphore render_finished = VK_NULL_HANDLE;
  VkFence in_flight = VK_NULL_HANDLE;
  VkBuffer staging_buffer = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  bool vulkan_ready = false;

  PFN_vkCreateXlibSurfaceKHR vk_create_xlib_surface = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR vk_get_physical_device_surface_support = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vk_get_physical_device_surface_capabilities = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vk_get_physical_device_surface_formats = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vk_get_physical_device_surface_present_modes = nullptr;
  PFN_vkCreateSwapchainKHR vk_create_swapchain = nullptr;
  PFN_vkGetSwapchainImagesKHR vk_get_swapchain_images = nullptr;
  PFN_vkAcquireNextImageKHR vk_acquire_next_image = nullptr;
  PFN_vkQueuePresentKHR vk_queue_present = nullptr;
  PFN_vkDestroySwapchainKHR vk_destroy_swapchain = nullptr;
  PFN_vkDestroySurfaceKHR vk_destroy_surface = nullptr;

  struct ImageCacheEntry {
    cairo_surface_t* surface = nullptr;
    std::uint64_t bytes = 0;
  };
  std::map<std::uint64_t, ImageCacheEntry> image_cache;
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
    const FontKey key{font};
    const auto existing = font_cache.find(key);
    if (existing != font_cache.end()) {
      return existing->second;
    }

    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(FcFamilyFor(font).c_str()));
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
    // Measure in a 96-dpi reference space so advances and metrics are DIPs.
    // The Cairo surface applies the display scale separately, so glyph
    // measurement and rendering stay consistent at any Xft.dpi.
    if (FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(font.Size() * 64.0F), 96, 96) != 0) {
      FT_Done_Face(face);
      throw std::runtime_error("HuxerUI could not set the font size");
    }
    font_cache.emplace(key, face);
    return face;
  }

  [[nodiscard]] cairo_font_face_t* CairoFontFor(const Font& font) {
    const FontKey key{font};
    const auto existing = cairo_font_cache.find(key);
    if (existing != cairo_font_cache.end()) {
      return existing->second;
    }
    FT_Face face = FaceFor(font);
    // Cairo renders glyph indices against the face's active charmap; select
    // the Unicode charmap so glyph indices agree with HarfBuzz shaping.
    static_cast<void>(FT_Select_Charmap(face, FT_ENCODING_UNICODE));
    cairo_font_face_t* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
    cairo_font_cache.emplace(key, cairo_face);
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
    // FreeType exposes no x-height metric; approximate with half the ascent.
    metrics.strike_through_position = static_cast<float>(face->ascender * 0.5 * scale);
    metrics.strike_through_thickness = metrics.underline_thickness;
    return metrics;
  }

  // Resolves a fallback FreeType face that contains the given Unicode code
  // point, caching by code point and pixel size. Returns nullptr when
  // fontconfig cannot find one. Used when the primary font lacks a glyph
  // (shaped codepoint == 0).
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
    fallback_font_cache.emplace(key, face);
    return face;
  }

  [[nodiscard]] ShapedRun ShapeRun(std::string_view text, const TextStyle& style) {
    FT_Face face = FaceFor(style.font);
    hb_face_t* hb_face = hb_ft_face_create(face, nullptr);
    hb_font_t* hb_font = hb_ft_font_create_referenced(face);
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_set_direction(
        buffer,
        ResolveDirection(text) == TextDirection::RightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR
    );
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hb_font, buffer, nullptr, 0);

    const unsigned int glyph_count = hb_buffer_get_length(buffer);
    const hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(buffer, nullptr);
    const hb_glyph_position_t* glyph_position = hb_buffer_get_glyph_positions(buffer, nullptr);

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

    hb_buffer_destroy(buffer);
    hb_font_destroy(hb_font);
    hb_face_destroy(hb_face);

    // Reshape missing glyphs (codepoint 0) through fontconfig fallback fonts.
    // Runs through the buffer again so a fallback font contributes its own
    // glyph index, advance, and offsets for the cluster that lacked a glyph.
    if (std::any_of(run.glyphs.begin(), run.glyphs.end(), [](const ShapedGlyph& glyph) { return glyph.index == 0; })) {
      run = ApplyFallbackShaping(text, style, std::move(run));
    }
    return run;
  }

  // Re-shapes clusters whose primary-font glyph index is zero (missing glyph)
  // with a fontconfig fallback face for the cluster's code point.
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
      hb_face_t* fallback_hb_face = hb_ft_face_create(fallback, nullptr);
      hb_font_t* fallback_hb_font = hb_ft_font_create_referenced(fallback);
      hb_buffer_t* fallback_buffer = hb_buffer_create();
      hb_buffer_add_utf8(
          fallback_buffer,
          text.data() + byte_offset,
          static_cast<int>(width),
          0,
          static_cast<int>(width)
      );
      hb_buffer_guess_segment_properties(fallback_buffer);
      hb_shape(fallback_hb_font, fallback_buffer, nullptr, 0);
      const unsigned int count = hb_buffer_get_length(fallback_buffer);
      const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(fallback_buffer, nullptr);
      const hb_glyph_position_t* position = hb_buffer_get_glyph_positions(fallback_buffer, nullptr);
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
      hb_buffer_destroy(fallback_buffer);
      hb_font_destroy(fallback_hb_font);
      hb_face_destroy(fallback_hb_face);
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

  void DestroyVulkanResources() {
    vulkan_ready = false;
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
    }
    if (command_pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, command_pool, nullptr);
      command_pool = VK_NULL_HANDLE;
    }
    if (in_flight != VK_NULL_HANDLE) {
      vkDestroyFence(device, in_flight, nullptr);
      in_flight = VK_NULL_HANDLE;
    }
    if (render_finished != VK_NULL_HANDLE) {
      vkDestroySemaphore(device, render_finished, nullptr);
      render_finished = VK_NULL_HANDLE;
    }
    if (image_available != VK_NULL_HANDLE) {
      vkDestroySemaphore(device, image_available, nullptr);
      image_available = VK_NULL_HANDLE;
    }
    if (staging_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, staging_memory, nullptr);
      staging_memory = VK_NULL_HANDLE;
    }
    if (staging_buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, staging_buffer, nullptr);
      staging_buffer = VK_NULL_HANDLE;
    }
    if (transfer_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, transfer_memory, nullptr);
      transfer_memory = VK_NULL_HANDLE;
    }
    if (transfer_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, transfer_image, nullptr);
      transfer_image = VK_NULL_HANDLE;
    }
    if (swapchain != VK_NULL_HANDLE && vk_destroy_swapchain != nullptr) {
      vk_destroy_swapchain(device, swapchain, nullptr);
      swapchain = VK_NULL_HANDLE;
    }
    swapchain_images.clear();
    if (device != VK_NULL_HANDLE) {
      vkDestroyDevice(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE && vk_destroy_surface != nullptr) {
      vk_destroy_surface(instance, surface, nullptr);
      surface = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
      vkDestroyInstance(instance, nullptr);
      instance = VK_NULL_HANDLE;
    }
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
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, decoded.width, decoded.height);
    unsigned char* data = cairo_image_surface_get_data(surface);
    const std::size_t stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < decoded.height; ++y) {
      const auto* src = reinterpret_cast<const unsigned char*>(
          decoded.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(decoded.width) * 4
      );
      auto* dst = data + static_cast<std::size_t>(y) * stride;
      for (int x = 0; x < decoded.width; ++x) {
        const unsigned char red = src[x * 4];
        const unsigned char green = src[x * 4 + 1];
        const unsigned char blue = src[x * 4 + 2];
        const unsigned char alpha = src[x * 4 + 3];
        // Cairo ARGB32 stores premultiplied BGRA byte order.
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
    image_cache_bytes += bytes;
    return surface;
  }

  [[nodiscard]] bool EnsureVulkan(Display* display, Window window) {
    if (vulkan_ready && device != VK_NULL_HANDLE && swapchain != VK_NULL_HANDLE) {
      return true;
    }
    if (instance == VK_NULL_HANDLE) {
      VkApplicationInfo application_info{};
      application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
      application_info.pApplicationName = "HuxerUI";
      application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
      application_info.apiVersion = VK_API_VERSION_1_0;
      const char* extensions[] = {
          VK_KHR_SURFACE_EXTENSION_NAME,
          VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
      };
      VkInstanceCreateInfo create_info{};
      create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
      create_info.pApplicationInfo = &application_info;
      create_info.enabledExtensionCount = 2;
      create_info.ppEnabledExtensionNames = extensions;
      if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
        instance = VK_NULL_HANDLE;
        return false;
      }
      vk_create_xlib_surface =
          reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
      vk_get_physical_device_surface_support = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
          vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR")
      );
      vk_get_physical_device_surface_capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
          vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")
      );
      vk_get_physical_device_surface_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
          vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR")
      );
      vk_get_physical_device_surface_present_modes = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
          vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR")
      );
      vk_create_swapchain =
          reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetInstanceProcAddr(instance, "vkCreateSwapchainKHR"));
      vk_get_swapchain_images =
          reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(vkGetInstanceProcAddr(instance, "vkGetSwapchainImagesKHR"));
      vk_acquire_next_image =
          reinterpret_cast<PFN_vkAcquireNextImageKHR>(vkGetInstanceProcAddr(instance, "vkAcquireNextImageKHR"));
      vk_queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(vkGetInstanceProcAddr(instance, "vkQueuePresentKHR"));
      vk_destroy_swapchain =
          reinterpret_cast<PFN_vkDestroySwapchainKHR>(vkGetInstanceProcAddr(instance, "vkDestroySwapchainKHR"));
      vk_destroy_surface =
          reinterpret_cast<PFN_vkDestroySurfaceKHR>(vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
    }

    if (surface == VK_NULL_HANDLE) {
      VkXlibSurfaceCreateInfoKHR surface_info{};
      surface_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
      surface_info.dpy = display;
      surface_info.window = window;
      if (vk_create_xlib_surface == nullptr ||
          vk_create_xlib_surface(instance, &surface_info, nullptr, &surface) != VK_SUCCESS) {
        return false;
      }
    }

    if (device == VK_NULL_HANDLE) {
      std::uint32_t device_count = 0;
      if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
        return false;
      }
      std::vector<VkPhysicalDevice> devices(device_count);
      vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
      physical_device = devices[0];
      for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          physical_device = candidate;
          break;
        }
      }

      std::uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
      bool graphics_found = false;
      bool present_found = false;
      for (std::uint32_t index = 0; index < family_count; ++index) {
        const bool supports_graphics = (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        VkBool32 supports_present = VK_FALSE;
        vk_get_physical_device_surface_support(physical_device, index, surface, &supports_present);
        if (supports_graphics && !graphics_found) {
          graphics_family = index;
          graphics_found = true;
        }
        if (supports_present != VK_FALSE && !present_found) {
          present_family = index;
          present_found = true;
        }
      }
      if (!graphics_found || !present_found) {
        return false;
      }

      const float priority = 1.0F;
      std::array<VkDeviceQueueCreateInfo, 2> queue_infos{};
      std::uint32_t queue_count = 0;
      const auto add_queue = [&](std::uint32_t family) {
        queue_infos[queue_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[queue_count].queueFamilyIndex = family;
        queue_infos[queue_count].queueCount = 1;
        queue_infos[queue_count].pQueuePriorities = &priority;
        ++queue_count;
      };
      add_queue(graphics_family);
      if (present_family != graphics_family) {
        add_queue(present_family);
      }
      const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
      VkDeviceCreateInfo device_info{};
      device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
      device_info.queueCreateInfoCount = queue_count;
      device_info.pQueueCreateInfos = queue_infos.data();
      device_info.enabledExtensionCount = 1;
      device_info.ppEnabledExtensionNames = device_extensions;
      if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
        device = VK_NULL_HANDLE;
        return false;
      }
      vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
      vkGetDeviceQueue(device, present_family, 0, &present_queue);
    }

    if (swapchain == VK_NULL_HANDLE) {
      if (!CreateSwapchain()) {
        return false;
      }
    }
    vulkan_ready = true;
    return true;
  }

  [[nodiscard]] bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    if (vk_get_physical_device_surface_capabilities(physical_device, surface, &capabilities) != VK_SUCCESS) {
      return false;
    }
    std::uint32_t format_count = 0;
    vk_get_physical_device_surface_formats(physical_device, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (format_count > 0) {
      vk_get_physical_device_surface_formats(physical_device, surface, &format_count, formats.data());
    }
    swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    for (const VkSurfaceFormatKHR& format : formats) {
      if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        swapchain_format = format.format;
        break;
      }
    }
    std::uint32_t present_mode_count = 0;
    vk_get_physical_device_surface_present_modes(physical_device, surface, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    if (present_mode_count > 0) {
      vk_get_physical_device_surface_present_modes(physical_device, surface, &present_mode_count, present_modes.data());
    }
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (const VkPresentModeKHR mode : present_modes) {
      if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
        present_mode = mode;
        break;
      }
    }

    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
      swapchain_extent = capabilities.currentExtent;
    } else {
      swapchain_extent = {
          std::clamp(
              static_cast<std::uint32_t>(surface_width),
              capabilities.minImageExtent.width,
              capabilities.maxImageExtent.width
          ),
          std::clamp(
              static_cast<std::uint32_t>(surface_height),
              capabilities.minImageExtent.height,
              capabilities.maxImageExtent.height
          ),
      };
    }

    std::uint32_t image_count = std::max(2U, capabilities.minImageCount + 1);
    if (capabilities.maxImageCount > 0) {
      image_count = std::min(image_count, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = swapchain_format;
    swapchain_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchain_info.imageExtent = swapchain_extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (graphics_family != present_family) {
      const std::array<std::uint32_t, 2> families{graphics_family, present_family};
      swapchain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      swapchain_info.queueFamilyIndexCount = 2;
      swapchain_info.pQueueFamilyIndices = families.data();
    }
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = present_mode;
    swapchain_info.clipped = VK_TRUE;
    swapchain_info.oldSwapchain = VK_NULL_HANDLE;
    if (vk_create_swapchain(device, &swapchain_info, nullptr, &swapchain) != VK_SUCCESS) {
      swapchain = VK_NULL_HANDLE;
      return false;
    }
    std::uint32_t actual_image_count = 0;
    vk_get_swapchain_images(device, swapchain, &actual_image_count, nullptr);
    swapchain_images.resize(actual_image_count);
    vk_get_swapchain_images(device, swapchain, &actual_image_count, swapchain_images.data());

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = swapchain_format;
    image_info.extent = {swapchain_extent.width, swapchain_extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &transfer_image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements memory_requirements{};
    vkGetImageMemoryRequirements(device, transfer_image, &memory_requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    std::uint32_t memory_type = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
      if ((memory_requirements.memoryTypeBits & (1U << index)) != 0 &&
          (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        memory_type = index;
        break;
      }
    }
    if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocate_info, nullptr, &transfer_memory) != VK_SUCCESS) {
      return false;
    }
    vkBindImageMemory(device, transfer_image, transfer_memory, 0);

    const VkDeviceSize staging_size = static_cast<VkDeviceSize>(swapchain_extent.width) * swapchain_extent.height * 4;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = staging_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &staging_buffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements buffer_requirements{};
    vkGetBufferMemoryRequirements(device, staging_buffer, &buffer_requirements);
    memory_type = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
      if ((buffer_requirements.memoryTypeBits & (1U << index)) != 0 &&
          (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        memory_type = index;
        break;
      }
    }
    if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    allocate_info.allocationSize = buffer_requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocate_info, nullptr, &staging_memory) != VK_SUCCESS) {
      return false;
    }
    vkBindBufferMemory(device, staging_buffer, staging_memory, 0);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = graphics_family;
    if (vkCreateCommandPool(device, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &command_info, &command_buffer) != VK_SUCCESS) {
      return false;
    }
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device, &semaphore_info, nullptr, &image_available) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished) != VK_SUCCESS) {
      return false;
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fence_info, nullptr, &in_flight) != VK_SUCCESS) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool PresentRetainedBitmap() {
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE || command_buffer == VK_NULL_HANDLE ||
        retained_surface == nullptr || staging_buffer == VK_NULL_HANDLE || surface_width <= 0 || surface_height <= 0) {
      return false;
    }
    if (vkWaitForFences(device, 1, &in_flight, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS) {
      return false;
    }

    std::uint32_t image_index = 0;
    const VkResult acquire_result = vk_acquire_next_image(
        device,
        swapchain,
        std::numeric_limits<std::uint64_t>::max(),
        image_available,
        VK_NULL_HANDLE,
        &image_index
    );
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      DestroyVulkanResources();
      return false;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
      return false;
    }
    vkResetFences(device, 1, &in_flight);

    void* mapped = nullptr;
    if (vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    const std::size_t retained_stride = cairo_image_surface_get_stride(retained_surface);
    const unsigned char* retained_data = cairo_image_surface_get_data(retained_surface);
    const std::uint32_t copy_height = std::min(swapchain_extent.height, static_cast<std::uint32_t>(surface_height));
    const std::uint32_t copy_width = std::min(swapchain_extent.width, static_cast<std::uint32_t>(surface_width));
    auto* destination = static_cast<unsigned char*>(mapped);
    for (std::uint32_t y = 0; y < copy_height; ++y) {
      std::memcpy(
          destination + static_cast<std::size_t>(y) * swapchain_extent.width * 4,
          retained_data + static_cast<std::size_t>(y) * retained_stride,
          static_cast<std::size_t>(copy_width) * 4
      );
    }
    vkUnmapMemory(device, staging_memory);

    vkResetCommandBuffer(command_buffer, 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
      return false;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = transfer_image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy_region.imageExtent = {copy_width, copy_height, 1};
    vkCmdCopyBufferToImage(
        command_buffer,
        staging_buffer,
        transfer_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region
    );

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    barrier.image = swapchain_images[image_index];
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    VkImageBlit blit_region{};
    blit_region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit_region.srcOffsets[1] = {static_cast<std::int32_t>(copy_width), static_cast<std::int32_t>(copy_height), 1};
    blit_region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit_region.dstOffsets[1] = {static_cast<std::int32_t>(copy_width), static_cast<std::int32_t>(copy_height), 1};
    vkCmdBlitImage(
        command_buffer,
        transfer_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchain_images[image_index],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blit_region,
        VK_FILTER_NEAREST
    );

    barrier.image = swapchain_images[image_index];
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
      return false;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished;
    if (vkQueueSubmit(graphics_queue, 1, &submit_info, in_flight) != VK_SUCCESS) {
      return false;
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;
    const VkResult present_result = vk_queue_present(present_queue, &present_info);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
      return false;
    }
    return present_result == VK_SUCCESS;
  }
};

// Breaks `text` into visual lines at whitespace word boundaries, mirroring
// DirectWrite's DWRITE_WRAP_WORD: a line breaks at the last space that fits;
// a single word longer than wrap_width breaks at character boundaries. Explicit
// newlines are preserved. Returns the line text spans with their UTF-8 byte
// starts. wrap_width is the measurement width; the returned lines are used for
// measurement, editing, and drawing so all three agree.
[[nodiscard]] std::vector<LayoutLine>
WrapLines(std::string_view text, const TextStyle& style, float wrap_width, TextWrap wrap, LinuxRenderer::State& state) {
  std::vector<LayoutLine> lines;
  if (text.empty()) {
    return lines;
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
    const ShapedRun probe = state.ShapeRun(candidate, style);
    if (probe.advance <= wrap_width) {
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
  state_->DestroyVulkanResources();
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
  state_->DestroyVulkanResources();
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
    // An empty layout still exposes one line's height so the caret and
    // vertical centering agree with non-empty editors (mirrors CoreText).
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

  if (!state_->vulkan_ready && !EnsureVulkan(display, window)) {
    state_->force_full_repaint = true;
    return LinuxRenderResult::Skipped;
  }

  const bool presented = PresentRetainedBitmap();
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

void AppendPath(cairo_t* cr, const Path& path) {
  cairo_new_path(cr);
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
// Scene traversal owns the clip and transform stacks; RenderSceneNode mirrors
// the retained RenderNode semantics (content, child clip, children transform,
// children, foreground).
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
    const bool clipped = node.child_clip.has_value();
    if (clipped) {
      cairo_save(cr_);
      AddRoundedRect(cr_, node.child_clip->rect, node.child_clip->corner_radius);
      cairo_clip(cr_);
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
    if (clipped) {
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
    // Match Direct2D PushTransform semantics: the new transform is applied
    // first, then the previous one (transform * previous). Cairo's
    // cairo_transform() post-multiplies, so compose explicitly instead.
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
      cairo_set_font_face(cr_, state_.CairoFontFor(run.style.font));
      cairo_set_font_size(cr_, run.style.font.Size() / state_.Scale());
      SetSourceColor(cr_, run.style.foreground);
      double x = run.baseline_origin.x;
      for (const ShapedGlyph& glyph : shaped.glyphs) {
        if (glyph.fallback) {
          cairo_font_face_t* fallback =
              state_.CairoFallbackFontFor(Utf8CodePointAt(run.text, glyph.cluster), run.style.font.Size());
          if (fallback != nullptr) {
            cairo_set_font_face(cr_, fallback);
          }
        }
        cairo_glyph_t cairo_glyph{
            glyph.index,
            x + glyph.x_offset,
            run.baseline_origin.y + glyph.y_offset,
        };
        cairo_show_glyphs(cr_, &cairo_glyph, 1);
        x += glyph.x_advance;
      }
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
    const float scale = state_.Scale();
    const float blur_pixels = command.blur_radius * scale;
    const Rect caster{
        command.rect.x - command.spread,
        command.rect.y - command.spread,
        command.rect.width + command.spread * 2.0F,
        command.rect.height + command.spread * 2.0F,
    };
    const Rect bounds{
        caster.x + command.offset.x - command.blur_radius,
        caster.y + command.offset.y - command.blur_radius,
        caster.width + command.blur_radius * 2.0F,
        caster.height + command.blur_radius * 2.0F,
    };
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
      return;
    }
    const int mask_width = std::max(1, static_cast<int>(std::ceil(bounds.width * scale)));
    const int mask_height = std::max(1, static_cast<int>(std::ceil(bounds.height * scale)));
    cairo_surface_t* mask = cairo_image_surface_create(CAIRO_FORMAT_A8, mask_width, mask_height);
    cairo_t* mask_cr = cairo_create(mask);
    cairo_scale(mask_cr, scale, scale);
    cairo_translate(mask_cr, -bounds.x, -bounds.y);
    cairo_set_source_rgb(mask_cr, 1.0, 1.0, 1.0);
    AddRoundedRect(mask_cr, caster, command.corner_radius);
    cairo_fill(mask_cr);
    cairo_destroy(mask_cr);

    // Separable box blur approximation: two horizontal + two vertical passes
    // on the alpha mask, computed in place into a scratch surface.
    const int radius = std::max(1, static_cast<int>(std::ceil(blur_pixels)));
    cairo_surface_t* blurred = BoxBlurMask(mask, radius);
    cairo_surface_destroy(mask);

    SetSourceColor(cr_, command.color);
    cairo_save(cr_);
    cairo_translate(cr_, bounds.x, bounds.y);
    cairo_scale(cr_, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr_, blurred, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr_), CAIRO_EXTEND_NONE);
    cairo_paint_with_alpha(cr_, command.color.alpha);
    cairo_restore(cr_);
    cairo_surface_destroy(blurred);
  }

  [[nodiscard]] cairo_surface_t* BoxBlurMask(cairo_surface_t* mask, int radius) {
    const int width = cairo_image_surface_get_width(mask);
    const int height = cairo_image_surface_get_height(mask);
    const unsigned char* source = cairo_image_surface_get_data(mask);
    std::vector<unsigned char> horizontal(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<unsigned char> vertical(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
      int sum = 0;
      for (int x = -radius; x <= radius; ++x) {
        const int clamped_x = std::clamp(x, 0, width - 1);
        sum += source[y * width + clamped_x];
      }
      for (int x = 0; x < width; ++x) {
        horizontal[y * width + x] = static_cast<unsigned char>(sum / window);
        const int remove_x = std::clamp(x - radius, 0, width - 1);
        const int add_x = std::clamp(x + radius + 1, 0, width - 1);
        sum += source[y * width + add_x] - source[y * width + remove_x];
      }
    }
    for (int x = 0; x < width; ++x) {
      int sum = 0;
      for (int y = -radius; y <= radius; ++y) {
        const int clamped_y = std::clamp(y, 0, height - 1);
        sum += horizontal[clamped_y * width + x];
      }
      for (int y = 0; y < height; ++y) {
        vertical[y * width + x] = static_cast<unsigned char>(sum / window);
        const int remove_y = std::clamp(y - radius, 0, height - 1);
        const int add_y = std::clamp(y + radius + 1, 0, height - 1);
        sum += horizontal[add_y * width + x] - horizontal[remove_y * width + x];
      }
    }
    cairo_surface_t* result = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    std::memcpy(cairo_image_surface_get_data(result), vertical.data(), vertical.size());
    cairo_surface_mark_dirty(result);
    return result;
  }

  void RenderCommand(const FillPathCommand& command) {
    SetSourceColor(cr_, command.color);
    AppendPath(cr_, command.path);
    cairo_set_fill_rule(
        cr_,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(cr_);
  }

  void RenderCommand(const StrokePathCommand& command) {
    SetSourceColor(cr_, command.color);
    AppendPath(cr_, command.path);
    cairo_set_line_width(cr_, command.width);
    cairo_set_line_cap(cr_, CairoLineCap(command.cap));
    cairo_set_line_join(cr_, CairoLineJoin(command.join));
    cairo_set_miter_limit(cr_, command.miter_limit);
    cairo_stroke(cr_);
  }

  void RenderCommand(const DrawPathShadowCommand& command) {
    const float scale = state_.Scale();
    const float blur_pixels = command.blur_radius * scale;
    const Rect bounds = command.path.Bounds();
    const Rect shadow_bounds{
        bounds.x + command.offset.x - command.blur_radius,
        bounds.y + command.offset.y - command.blur_radius,
        bounds.width + command.blur_radius * 2.0F,
        bounds.height + command.blur_radius * 2.0F,
    };
    if (shadow_bounds.width <= 0.0F || shadow_bounds.height <= 0.0F) {
      return;
    }
    const int mask_width = std::max(1, static_cast<int>(std::ceil(shadow_bounds.width * scale)));
    const int mask_height = std::max(1, static_cast<int>(std::ceil(shadow_bounds.height * scale)));
    cairo_surface_t* mask = cairo_image_surface_create(CAIRO_FORMAT_A8, mask_width, mask_height);
    cairo_t* mask_cr = cairo_create(mask);
    cairo_scale(mask_cr, scale, scale);
    cairo_translate(mask_cr, -shadow_bounds.x, -shadow_bounds.y);
    cairo_set_source_rgb(mask_cr, 1.0, 1.0, 1.0);
    AppendPath(mask_cr, command.path);
    cairo_set_fill_rule(
        mask_cr,
        command.fill_rule == PathFillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING
    );
    cairo_fill(mask_cr);
    cairo_destroy(mask_cr);
    cairo_surface_t* blurred = BoxBlurMask(mask, std::max(1, static_cast<int>(std::ceil(blur_pixels))));
    cairo_surface_destroy(mask);
    SetSourceColor(cr_, command.color);
    cairo_save(cr_);
    cairo_translate(cr_, shadow_bounds.x, shadow_bounds.y);
    cairo_scale(cr_, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr_, blurred, 0, 0);
    cairo_paint_with_alpha(cr_, command.color.alpha);
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
    cairo_set_font_face(cr_, state_.CairoFontFor(style.font));
    cairo_set_font_size(cr_, style.font.Size() / state_.Scale());
    SetSourceColor(cr_, style.foreground);
    for (const LayoutLine& line : lines) {
      float x = rect.x;
      if (options.align == TextAlign::Center) {
        x += std::max(0.0F, (rect.width - line.advance) * 0.5F);
      } else if (options.align == TextAlign::Trailing) {
        x += std::max(0.0F, rect.width - line.advance);
      }
      const double baseline_y = y + line.baseline;
      double glyph_x = x;
      for (const ShapedGlyph& glyph : line.glyphs) {
        if (glyph.fallback) {
          cairo_font_face_t* fallback =
              state_.CairoFallbackFontFor(Utf8CodePointAt(line.text, glyph.cluster), style.font.Size());
          if (fallback != nullptr) {
            cairo_set_font_face(cr_, fallback);
          }
        }
        cairo_glyph_t cairo_glyph{glyph.index, glyph_x + glyph.x_offset, baseline_y + glyph.y_offset};
        cairo_show_glyphs(cr_, &cairo_glyph, 1);
        glyph_x += glyph.x_advance;
      }
    }
  }

  LinuxRenderer::State& state_;
  cairo_t* cr_;
};

} // namespace

void LinuxRenderer::RenderSceneNode(const RenderNode& node) {
  ScenePainter painter(*state_, state_->retained_context);
  painter.RenderSceneNode(node);
}

bool LinuxRenderer::EnsureVulkan(Display* display, Window window) {
  return state_->EnsureVulkan(display, window);
}

bool LinuxRenderer::PresentRetainedBitmap() {
  return state_->PresentRetainedBitmap();
}

} // namespace huxerui::detail
