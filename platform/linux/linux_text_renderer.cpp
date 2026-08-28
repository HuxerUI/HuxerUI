#include "linux_internal.h"
#include "linux_text_renderer_internal.h"

#include <SDL3_ttf/SDL_ttf.h>
#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "text_layout_internal.h"

namespace huxerui::detail {
namespace {

[[noreturn]] void ThrowTtfError(std::string_view operation) {
  throw std::runtime_error("HuxerUI Linux " + std::string(operation) + " failed: " + SDL_GetError());
}

std::string Lower(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

bool Contains(std::string_view value, std::string_view candidate) noexcept {
  return value.find(candidate) != std::string_view::npos;
}

bool IsFontFile(const std::filesystem::path& path) {
  const std::string extension = Lower(path.extension().string());
  return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
}

std::optional<std::pair<std::uint32_t, int>> DecodeUtf8(std::string_view text, std::size_t offset) noexcept {
  if (offset >= text.size()) {
    return std::nullopt;
  }
  const auto first = static_cast<std::uint8_t>(text[offset]);
  if (first < 0x80U) {
    return std::pair{static_cast<std::uint32_t>(first), 1};
  }
  int length = 0;
  std::uint32_t value = 0;
  if ((first & 0xE0U) == 0xC0U) {
    length = 2;
    value = first & 0x1FU;
  } else if ((first & 0xF0U) == 0xE0U) {
    length = 3;
    value = first & 0x0FU;
  } else if ((first & 0xF8U) == 0xF0U) {
    length = 4;
    value = first & 0x07U;
  } else {
    return std::nullopt;
  }
  if (offset + static_cast<std::size_t>(length) > text.size()) {
    return std::nullopt;
  }
  for (int index = 1; index < length; ++index) {
    const auto continuation = static_cast<std::uint8_t>(text[offset + static_cast<std::size_t>(index)]);
    if ((continuation & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  const bool overlong =
      (length == 2 && value < 0x80U) || (length == 3 && value < 0x800U) || (length == 4 && value < 0x10000U);
  if (overlong || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
    return std::nullopt;
  }
  return std::pair{value, length};
}

bool IsValidUtf8(std::string_view text) noexcept {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto decoded = DecodeUtf8(text, offset);
    if (!decoded.has_value()) {
      return false;
    }
    offset += static_cast<std::size_t>(decoded->second);
  }
  return true;
}

TextDirection ResolveDirection(TextDirection requested) noexcept {
  return requested == TextDirection::Auto ? TextDirection::LeftToRight : requested;
}

bool IsFirstStrongCandidate(std::uint32_t value) noexcept {
  if (value == 0x061CU || value == 0x200EU || value == 0x200FU || value == 0x05BEU || value == 0x05C0U ||
      value == 0x05C3U || value == 0x05C6U) {
    return true;
  }
  switch (g_unichar_type(static_cast<gunichar>(value))) {
  case G_UNICODE_LOWERCASE_LETTER:
  case G_UNICODE_MODIFIER_LETTER:
  case G_UNICODE_OTHER_LETTER:
  case G_UNICODE_TITLECASE_LETTER:
  case G_UNICODE_UPPERCASE_LETTER:
    return true;
  default:
    return false;
  }
}

TextDirection StrongDirection(std::uint32_t value) noexcept {
  if (value == 0x200EU) {
    return TextDirection::LeftToRight;
  }
  const bool rtl = value == 0x061CU || value == 0x200FU || (value >= 0x0590U && value <= 0x08FFU) ||
                   (value >= 0xFB1DU && value <= 0xFDFFU) || (value >= 0xFE70U && value <= 0xFEFFU) ||
                   (value >= 0x10800U && value <= 0x10FFFU) || (value >= 0x1E800U && value <= 0x1EEFFU);
  return rtl ? TextDirection::RightToLeft : TextDirection::LeftToRight;
}

std::optional<TextOffset> Utf8ByteToUtf16(std::string_view text, int byte_offset) noexcept {
  if (byte_offset < 0 || static_cast<std::size_t>(byte_offset) > text.size()) {
    return std::nullopt;
  }
  TextOffset result = 0;
  std::size_t current = 0;
  while (current < static_cast<std::size_t>(byte_offset)) {
    const auto decoded = DecodeUtf8(text, current);
    if (!decoded.has_value() ||
        current + static_cast<std::size_t>(decoded->second) > static_cast<std::size_t>(byte_offset)) {
      return std::nullopt;
    }
    result += decoded->first > 0xFFFFU ? 2 : 1;
    current += static_cast<std::size_t>(decoded->second);
  }
  return current == static_cast<std::size_t>(byte_offset) ? std::optional<TextOffset>{result} : std::nullopt;
}

std::optional<int> Utf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept {
  if (offset < 0) {
    return std::nullopt;
  }
  TextOffset current_utf16 = 0;
  std::size_t current_byte = 0;
  while (current_byte < text.size() && current_utf16 < offset) {
    const auto decoded = DecodeUtf8(text, current_byte);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    const TextOffset units = decoded->first > 0xFFFFU ? 2 : 1;
    if (current_utf16 + units > offset) {
      return std::nullopt;
    }
    current_utf16 += units;
    current_byte += static_cast<std::size_t>(decoded->second);
  }
  if (current_utf16 != offset || current_byte > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  return static_cast<int>(current_byte);
}

TTF_Direction TtfDirection(TextDirection direction) noexcept {
  switch (direction) {
  case TextDirection::LeftToRight:
    return TTF_DIRECTION_LTR;
  case TextDirection::RightToLeft:
    return TTF_DIRECTION_RTL;
  case TextDirection::Auto:
    return TTF_DIRECTION_INVALID;
  }
  return TTF_DIRECTION_INVALID;
}

TTF_HorizontalAlignment TtfAlignment(TextAlign alignment, TextDirection direction) noexcept {
  if (alignment == TextAlign::Center) {
    return TTF_HORIZONTAL_ALIGN_CENTER;
  }
  const bool rtl = direction == TextDirection::RightToLeft;
  if (alignment == TextAlign::Leading) {
    return rtl ? TTF_HORIZONTAL_ALIGN_RIGHT : TTF_HORIZONTAL_ALIGN_LEFT;
  }
  return rtl ? TTF_HORIZONTAL_ALIGN_LEFT : TTF_HORIZONTAL_ALIGN_RIGHT;
}

struct FontFaceInfo final {
  std::string family;
  std::string style;
  int weight = 400;
  bool italic = false;
};

std::pair<int, bool> FaceStyle(std::string_view description) noexcept {
  const bool italic = Contains(description, "italic") || Contains(description, "oblique");
  int weight = 400;
  if (Contains(description, "thin")) {
    weight = 100;
  } else if (
      Contains(description, "extralight") || Contains(description, "extra light") || Contains(description, "ultralight")
  ) {
    weight = 200;
  } else if (Contains(description, "light")) {
    weight = 300;
  } else if (Contains(description, "medium")) {
    weight = 500;
  } else if (
      Contains(description, "semibold") || Contains(description, "semi bold") || Contains(description, "demibold")
  ) {
    weight = 600;
  } else if (
      Contains(description, "extrabold") || Contains(description, "extra bold") || Contains(description, "ultrabold")
  ) {
    weight = 800;
  } else if (Contains(description, "black") || Contains(description, "heavy")) {
    weight = 900;
  } else if (Contains(description, "bold")) {
    weight = 700;
  }
  return {weight, italic};
}

int FaceScore(std::string_view description, const Font& requested) noexcept {
  const auto [weight, italic] = FaceStyle(description);
  return std::abs(weight - static_cast<int>(requested.Weight())) +
         (italic == (requested.Slant() == FontSlant::Italic) ? 0 : 1000);
}

int FallbackFamilyScore(
    std::string_view description, const Font& requested, std::string_view locale, std::optional<bool> fixed_width = {}
) noexcept {
  int score = 0;
  const bool monospace = fixed_width.value_or(Contains(description, "mono") || Contains(description, "code"));
  if (requested.FamilyKind() == FontFamilyKind::Monospace) {
    score += monospace ? 0 : 100;
  } else if (monospace) {
    score += 100;
  }
  const std::string requested_family = Lower(requested.FamilyName());
  const bool wants_serif = Contains(requested_family, "serif");
  const bool serif = Contains(description, "serif");
  if (serif != wants_serif && requested.FamilyKind() == FontFamilyKind::Named) {
    score += 50;
  }
  const std::string language = Lower(locale);
  if (language.starts_with("zh-cn") || language.starts_with("zh-sg") || language.starts_with("zh-hans")) {
    score += Contains(description, "cn") || Contains(description, "sc") ? 0 : 25;
  } else if (
      language.starts_with("zh-tw") || language.starts_with("zh-hk") || language.starts_with("zh-mo") ||
      language.starts_with("zh-hant")
  ) {
    score += Contains(description, "tw") || Contains(description, "tc") || Contains(description, "hk") ? 0 : 25;
  } else if (language.starts_with("ja")) {
    score += Contains(description, "jp") || Contains(description, "japan") ? 0 : 25;
  } else if (language.starts_with("ko")) {
    score += Contains(description, "kr") || Contains(description, "korea") ? 0 : 25;
  }
  return score;
}

TTF_FontStyleFlags TtfStyle(const TextStyle& style, const FontFaceInfo* face = nullptr) noexcept {
  TTF_FontStyleFlags result = TTF_STYLE_NORMAL;
  if (style.font.Slant() == FontSlant::Italic && (face == nullptr || !face->italic)) {
    result |= TTF_STYLE_ITALIC;
  }
  if (static_cast<int>(style.font.Weight()) >= static_cast<int>(FontWeight::SemiBold) &&
      (face == nullptr || face->weight < static_cast<int>(FontWeight::SemiBold))) {
    result |= TTF_STYLE_BOLD;
  }
  if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
    result |= TTF_STYLE_UNDERLINE;
  }
  if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
    result |= TTF_STYLE_STRIKETHROUGH;
  }
  return result;
}

struct FontBundle final {
  explicit FontBundle(TTF_Font* value) : font(value) {}

  ~FontBundle() {
    if (font != nullptr) {
      TTF_ClearFallbackFonts(font);
    }
    for (TTF_Font* fallback : fallbacks) {
      TTF_CloseFont(fallback);
    }
    TTF_CloseFont(font);
  }

  TTF_Font* font = nullptr;
  std::vector<TTF_Font*> fallbacks;
  std::vector<std::filesystem::path> fallback_paths;
};

struct TextHandle final {
  TextHandle() = default;
  explicit TextHandle(TTF_Text* value) : text(value) {}
  ~TextHandle() {
    TTF_DestroyText(text);
  }
  TextHandle(const TextHandle&) = delete;
  TextHandle& operator=(const TextHandle&) = delete;
  TextHandle(TextHandle&& other) noexcept : text(std::exchange(other.text, nullptr)) {}
  TextHandle& operator=(TextHandle&& other) noexcept {
    if (this != &other) {
      TTF_DestroyText(text);
      text = std::exchange(other.text, nullptr);
    }
    return *this;
  }
  TTF_Text* text = nullptr;
};

Rect InkBounds(SDL_Surface& surface, float baseline) {
  const bool locked = SDL_MUSTLOCK(&surface);
  if (locked && !SDL_LockSurface(&surface)) {
    return {};
  }
  const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(surface.format);
  int left = surface.w;
  int top = surface.h;
  int right = 0;
  int bottom = 0;
  if (details != nullptr) {
    for (int y = 0; y < surface.h; ++y) {
      const auto* row = static_cast<const std::uint8_t*>(surface.pixels) + y * surface.pitch;
      for (int x = 0; x < surface.w; ++x) {
        std::uint32_t pixel = 0;
        std::memcpy(&pixel, row + x * details->bytes_per_pixel, details->bytes_per_pixel);
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
        std::uint8_t alpha = 0;
        SDL_GetRGBA(pixel, details, SDL_GetSurfacePalette(&surface), &red, &green, &blue, &alpha);
        if (alpha == 0U) {
          continue;
        }
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + 1);
        bottom = std::max(bottom, y + 1);
      }
    }
  }
  if (locked) {
    SDL_UnlockSurface(&surface);
  }
  if (left >= right || top >= bottom) {
    return {};
  }
  return {
      static_cast<float>(left),
      static_cast<float>(top) - baseline,
      static_cast<float>(right - left),
      static_cast<float>(bottom - top),
  };
}

} // namespace

struct LinuxTextRenderer::State final {
  State() {
    if (!TTF_Init()) {
      ThrowTtfError("could not initialize SDL_ttf");
    }
    surface_engine = TTF_CreateSurfaceTextEngine();
    if (surface_engine == nullptr) {
      TTF_Quit();
      ThrowTtfError("could not create the SDL_ttf surface engine");
    }
  }

  ~State() {
    bundles.clear();
    TTF_DestroySurfaceTextEngine(surface_engine);
    TTF_Quit();
  }

  void DiscoverFonts() {
    if (fonts_discovered) {
      return;
    }
    fonts_discovered = true;
    std::vector<std::filesystem::path> roots{
        "/usr/share/fonts",
        "/usr/local/share/fonts",
    };
    if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME); home != nullptr) {
      roots.emplace_back(std::filesystem::path(home) / ".fonts");
      roots.emplace_back(std::filesystem::path(home) / ".local/share/fonts");
    }
    for (const std::filesystem::path& root : roots) {
      std::error_code error;
      if (!std::filesystem::is_directory(root, error)) {
        continue;
      }
      std::filesystem::recursive_directory_iterator iterator(
          root,
          std::filesystem::directory_options::skip_permission_denied,
          error
      );
      const std::filesystem::recursive_directory_iterator end;
      while (iterator != end) {
        if (!error && iterator->is_regular_file(error) && IsFontFile(iterator->path())) {
          font_files.push_back(iterator->path());
        }
        iterator.increment(error);
        if (error) {
          error.clear();
        }
      }
    }
    std::ranges::sort(font_files);
    font_files.erase(std::ranges::unique(font_files).begin(), font_files.end());
  }

  const FontFaceInfo& InfoFor(const std::filesystem::path& path) {
    const std::string key = path.string();
    if (const auto existing = face_info.find(key); existing != face_info.end()) {
      return existing->second;
    }
    FontFaceInfo info;
    if (TTF_Font* font = TTF_OpenFont(key.c_str(), 12.0F); font != nullptr) {
      if (const char* name = TTF_GetFontFamilyName(font); name != nullptr) {
        info.family = name;
      }
      if (const char* name = TTF_GetFontStyleName(font); name != nullptr) {
        info.style = Lower(name);
      }
      TTF_CloseFont(font);
    }
    const std::string description = Lower(path.filename().string()) + " " + info.style;
    const auto [weight, italic] = FaceStyle(description);
    info.weight = weight;
    info.italic = italic;
    return face_info.emplace(key, std::move(info)).first->second;
  }

  std::filesystem::path ResolveFont(const Font& font) {
    DiscoverFonts();
    const std::string requested = font.FamilyKind() == FontFamilyKind::Named ? Lower(font.FamilyName()) : std::string{};
    if (!requested.empty()) {
      const int requested_weight = static_cast<int>(font.Weight());
      const bool requested_italic = font.Slant() == FontSlant::Italic;
      const std::filesystem::path* best = nullptr;
      int best_score = std::numeric_limits<int>::max();
      for (const std::filesystem::path& path : font_files) {
        const FontFaceInfo& info = InfoFor(path);
        if (Lower(info.family) != requested) {
          continue;
        }
        const int score = std::abs(info.weight - requested_weight) + (info.italic == requested_italic ? 0 : 1000);
        if (score < best_score) {
          best = &path;
          best_score = score;
        }
      }
      if (best != nullptr) {
        return *best;
      }
    }

    const std::vector<std::string_view> preferred = font.FamilyKind() == FontFamilyKind::Monospace
                                                        ? std::vector<std::string_view>{
                                                              "dejavusansmono",
                                                              "notosansmono",
                                                              "liberationmono",
                                                          }
                                                        : std::vector<std::string_view>{
                                                              "dejavusans",
                                                              "notosans-regular",
                                                              "notosans",
                                                          "liberationsans-regular",
                                                      };
    for (std::string_view candidate : preferred) {
      const std::filesystem::path* best = nullptr;
      int best_score = std::numeric_limits<int>::max();
      for (const std::filesystem::path& path : font_files) {
        const std::string name = Lower(path.filename().string());
        if (!Contains(name, candidate) || (font.FamilyKind() == FontFamilyKind::System && Contains(name, "mono"))) {
          continue;
        }
        const FontFaceInfo& info = InfoFor(path);
        const int score = std::abs(info.weight - static_cast<int>(font.Weight())) +
                          (info.italic == (font.Slant() == FontSlant::Italic) ? 0 : 1000);
        if (score < best_score) {
          best = &path;
          best_score = score;
        }
      }
      if (best != nullptr) {
        return *best;
      }
    }
    if (font_files.empty()) {
      throw std::runtime_error("HuxerUI Linux could not find a system TrueType or OpenType font");
    }
    return font_files.front();
  }

  std::shared_ptr<FontBundle> BundleFor(
      const TextStyle& style, const TextShapingOptions& shaping, TextAlign alignment, std::string_view text = {}
  ) {
    const std::filesystem::path primary_path = ResolveFont(style.font);
    const TextDirection direction = ResolveDirection(shaping.direction);
    const std::string key = primary_path.string() + "|" + std::to_string(style.font.Size()) + "|" +
                            std::to_string(static_cast<int>(style.font.Weight())) + "|" +
                            std::to_string(static_cast<int>(style.font.Slant())) + "|" + shaping.locale + "|" +
                            std::to_string(static_cast<int>(direction)) + "|" +
                            std::to_string(static_cast<int>(alignment)) + "|" +
                            std::to_string(static_cast<int>(style.decoration));
    if (const auto existing = bundles.find(key); existing != bundles.end()) {
      EnsureFallbacks(*existing->second, primary_path, style, shaping.locale, text);
      return existing->second;
    }
    TTF_Font* primary = TTF_OpenFont(primary_path.c_str(), std::max(1.0F, style.font.Size()));
    if (primary == nullptr) {
      ThrowTtfError("could not open the selected font");
    }
    const auto bundle = std::make_shared<FontBundle>(primary);
    TTF_SetFontStyle(primary, TtfStyle(style, &InfoFor(primary_path)));
    TTF_SetFontWrapAlignment(primary, TtfAlignment(alignment, direction));
    if (!shaping.locale.empty()) {
      if (!TTF_SetFontLanguage(primary, shaping.locale.c_str())) {
        ThrowTtfError("could not set the SDL_ttf font language");
      }
    }

    EnsureFallbacks(*bundle, primary_path, style, shaping.locale, text);
    if (bundle_order.size() >= kMaxBundles) {
      bundles.erase(bundle_order.front());
      bundle_order.erase(bundle_order.begin());
    }
    bundles.emplace(key, bundle);
    bundle_order.push_back(key);
    return bundle;
  }

  void EnsureFallbacks(
      FontBundle& bundle,
      const std::filesystem::path& primary_path,
      const TextStyle& style,
      std::string_view locale,
      std::string_view text
  ) {
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto decoded = DecodeUtf8(text, offset);
      if (!decoded.has_value()) {
        return;
      }
      const std::uint32_t codepoint = decoded->first;
      offset += static_cast<std::size_t>(decoded->second);
      if (codepoint < 0x20U || TTF_FontHasGlyph(bundle.font, codepoint)) {
        continue;
      }
      const std::string fallback_key =
          primary_path.string() + "|" + std::to_string(static_cast<int>(style.font.FamilyKind())) + "|" +
          std::string(style.font.FamilyName()) + "|" + std::string(locale) + "|" + std::to_string(codepoint) + "|" +
          std::to_string(static_cast<int>(style.font.Weight())) + "|" +
          std::to_string(static_cast<int>(style.font.Slant()));
      std::filesystem::path selected;
      if (const auto cached = fallback_paths.find(fallback_key); cached != fallback_paths.end()) {
        selected = cached->second;
      } else {
        struct Candidate final {
          const std::filesystem::path* path = nullptr;
          int score = 0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(font_files.size());
        for (const std::filesystem::path& path : font_files) {
          if (path == primary_path) {
            continue;
          }
          const std::string description = Lower(path.filename().string());
          candidates.push_back({
              &path,
              FaceScore(description, style.font) * 10 + FallbackFamilyScore(description, style.font, locale),
          });
        }
        std::ranges::stable_sort(candidates, {}, &Candidate::score);
        // SDL_ttf shapes and rasterizes fallback faces but does not discover them. Inspect candidates lazily in
        // style-first order and stop as soon as an exact style, family class, and locale match supports the glyph.
        int selected_score = std::numeric_limits<int>::max();
        for (const Candidate& candidate_path : candidates) {
          TTF_Font* candidate = TTF_OpenFont(candidate_path.path->c_str(), std::max(1.0F, style.font.Size()));
          if (candidate == nullptr) {
            continue;
          }
          const bool supported = TTF_FontHasGlyph(candidate, codepoint);
          if (supported) {
            std::string description = Lower(candidate_path.path->filename().string());
            if (const char* family = TTF_GetFontFamilyName(candidate); family != nullptr) {
              description += " ";
              description += Lower(family);
            }
            if (const char* face = TTF_GetFontStyleName(candidate); face != nullptr) {
              description += " ";
              description += Lower(face);
            }
            const int score = FaceScore(description, style.font) * 10 +
                              FallbackFamilyScore(description, style.font, locale, TTF_FontIsFixedWidth(candidate));
            if (score < selected_score) {
              selected = *candidate_path.path;
              selected_score = score;
            }
          }
          TTF_CloseFont(candidate);
          if (selected_score == 0) {
            break;
          }
        }
        if (fallback_path_order.size() >= kMaxFallbackPaths) {
          fallback_paths.erase(fallback_path_order.front());
          fallback_path_order.erase(fallback_path_order.begin());
        }
        fallback_paths.emplace(fallback_key, selected);
        fallback_path_order.push_back(fallback_key);
      }
      if (selected.empty() || selected == primary_path ||
          std::ranges::find(bundle.fallback_paths, selected) != bundle.fallback_paths.end()) {
        continue;
      }
      TTF_Font* fallback = TTF_OpenFont(selected.c_str(), std::max(1.0F, style.font.Size()));
      if (fallback == nullptr) {
        continue;
      }
      TTF_SetFontStyle(fallback, TtfStyle(style, &InfoFor(selected)));
      if (TTF_AddFallbackFont(bundle.font, fallback)) {
        bundle.fallbacks.push_back(fallback);
        bundle.fallback_paths.push_back(selected);
      } else {
        TTF_CloseFont(fallback);
      }
    }
  }

  TTF_TextEngine* surface_engine = nullptr;
  bool fonts_discovered = false;
  std::vector<std::filesystem::path> font_files;
  std::unordered_map<std::string, FontFaceInfo> face_info;
  std::unordered_map<std::string, std::filesystem::path> fallback_paths;
  std::vector<std::string> fallback_path_order;
  std::unordered_map<std::string, std::shared_ptr<FontBundle>> bundles;
  std::vector<std::string> bundle_order;
  static constexpr std::size_t kMaxBundles = 64;
  static constexpr std::size_t kMaxFallbackPaths = 1024;
};

namespace {

class SdlTextLayout final : public TextLayout {
public:
  SdlTextLayout(
      std::shared_ptr<LinuxTextRenderer::State> state,
      std::string_view text,
      const TextStyle& style,
      float max_width,
      const TextLayoutOptions& options
  )
      : state_(std::move(state)), text_(text), style_(style), options_(options), max_width_(max_width),
        direction_(ResolveDirection(options.shaping.direction)),
        bundle_(state_->BundleFor(style, options.shaping, options.align, text)) {
    if (!IsValidUtf8(text_)) {
      throw std::invalid_argument("HuxerUI Linux text must be valid UTF-8");
    }
    if (text_.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument("HuxerUI Linux text is too large");
    }
    text_handle_ = TextHandle(TTF_CreateText(state_->surface_engine, bundle_->font, text_.data(), text_.size()));
    if (text_handle_.text == nullptr) {
      ThrowTtfError("could not create an SDL_ttf text layout");
    }
    const TTF_Direction direction = TtfDirection(options_.shaping.direction);
    if (direction != TTF_DIRECTION_INVALID && !TTF_SetTextDirection(text_handle_.text, direction)) {
      ThrowTtfError("could not set SDL_ttf text direction");
    }
    if (std::isfinite(max_width_) && max_width_ > 0.0F && options_.wrap == TextWrap::Word) {
      if (!TTF_SetTextWrapWidth(text_handle_.text, std::max(1, static_cast<int>(std::ceil(max_width_))))) {
        ThrowTtfError("could not set SDL_ttf text wrap width");
      }
    }
    if (!TTF_UpdateText(text_handle_.text)) {
      ThrowTtfError("could not update an SDL_ttf text layout");
    }
    if (options_.shaping.direction == TextDirection::Auto && !text_.empty()) {
      std::size_t byte_offset = 0;
      while (byte_offset < text_.size()) {
        const auto decoded = DecodeUtf8(text_, byte_offset);
        if (!decoded.has_value()) {
          break;
        }
        if (IsFirstStrongCandidate(decoded->first)) {
          direction_ = StrongDirection(decoded->first);
          break;
        }
        byte_offset += static_cast<std::size_t>(decoded->second);
      }
      TextShapingOptions resolved_shaping = options_.shaping;
      resolved_shaping.direction = direction_;
      const std::shared_ptr<FontBundle> resolved_bundle =
          state_->BundleFor(style_, resolved_shaping, options_.align, text_);
      if (resolved_bundle != bundle_) {
        bundle_ = resolved_bundle;
        if (!TTF_SetTextFont(text_handle_.text, bundle_->font) || !TTF_UpdateText(text_handle_.text)) {
          ThrowTtfError("could not apply the detected SDL_ttf text direction");
        }
      }
    }
    int width = 0;
    int height = 0;
    if (!TTF_GetTextSize(text_handle_.text, &width, &height)) {
      ThrowTtfError("could not measure an SDL_ttf text layout");
    }
    const FontMetrics font_metrics = MetricsFor(*bundle_->font);
    const std::size_t newline_lines = static_cast<std::size_t>(std::ranges::count(text_, '\n')) + 1U;
    const std::size_t layout_lines = static_cast<std::size_t>(std::max(1, text_handle_.text->num_lines));
    const std::size_t line_count = std::max(newline_lines, layout_lines);
    metrics_ = {
        .size =
            {
                static_cast<float>(width),
                std::max(static_cast<float>(height), static_cast<float>(line_count) * font_metrics.LineHeight()),
            },
        .first_baseline = font_metrics.ascent,
        .last_baseline = font_metrics.ascent + static_cast<float>(line_count - 1U) * font_metrics.LineHeight(),
        .line_count = line_count,
    };
    if (std::isfinite(max_width_) && options_.wrap == TextWrap::Word) {
      metrics_.size.width = std::min(metrics_.size.width, max_width_);
    }
    if (std::isfinite(max_width_) && options_.wrap == TextWrap::NoWrap && metrics_.size.width < max_width_) {
      const bool rtl = direction_ == TextDirection::RightToLeft;
      if (options_.align == TextAlign::Center) {
        alignment_offset_ = (max_width_ - metrics_.size.width) * 0.5F;
      } else if ((options_.align == TextAlign::Trailing && !rtl) || (options_.align == TextAlign::Leading && rtl)) {
        alignment_offset_ = max_width_ - metrics_.size.width;
      }
    }
    BuildCaretOffsets();
  }

  Size Measure() const override {
    return metrics_.size;
  }

  const TextLayoutMetrics& MetricsValue() const noexcept {
    return metrics_;
  }

  LinuxRenderedText Render() {
    const int width = std::max(1, static_cast<int>(std::ceil(metrics_.size.width + alignment_offset_)));
    const int height = std::max(1, static_cast<int>(std::ceil(metrics_.size.height)));
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
      ThrowTtfError("could not create an SDL_ttf text surface");
    }
    static_cast<void>(SDL_FillSurfaceRect(surface, nullptr, 0U));
    if (!TTF_SetTextColorFloat(
            text_handle_.text,
            style_.foreground.red,
            style_.foreground.green,
            style_.foreground.blue,
            style_.foreground.alpha
        ) ||
        !TTF_DrawSurfaceText(text_handle_.text, static_cast<int>(std::lround(alignment_offset_)), 0, surface)) {
      SDL_DestroySurface(surface);
      ThrowTtfError("could not draw SDL_ttf text");
    }
    return {
        .surface = {surface, SDL_DestroySurface},
        .metrics = metrics_,
        .raster_scale = 1.0F,
    };
  }

  TextPosition HitTest(Point point) const override {
    TTF_SubString substring{};
    const int x = static_cast<int>(std::lround(point.x - alignment_offset_));
    const int y = static_cast<int>(std::lround(point.y));
    if (!TTF_GetTextSubStringForPoint(text_handle_.text, x, y, &substring)) {
      return {};
    }
    const bool rtl = (substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL;
    const float midpoint = static_cast<float>(substring.rect.x) + static_cast<float>(substring.rect.w) * 0.5F;
    const bool after = rtl ? static_cast<float>(x) < midpoint : static_cast<float>(x) >= midpoint;
    const int byte_offset = substring.offset + (after ? substring.length : 0);
    return {
        Utf8ByteToUtf16(text_, byte_offset).value_or(0),
        after ? TextAffinity::Upstream : TextAffinity::Downstream,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const int byte_offset = Utf16ToUtf8Byte(text_, offset).value_or(static_cast<int>(text_.size()));
    if (text_.empty()) {
      return {alignment_offset_, 0.0F, 1.0F, std::max(1.0F, metrics_.size.height)};
    }
    int query_offset = byte_offset;
    bool trailing = affinity == TextAffinity::Upstream && byte_offset > 0;
    if (trailing) {
      --query_offset;
      while (query_offset > 0 &&
             (static_cast<std::uint8_t>(text_[static_cast<std::size_t>(query_offset)]) & 0xC0U) == 0x80U) {
        --query_offset;
      }
    }
    TTF_SubString substring{};
    if (!TTF_GetTextSubString(text_handle_.text, query_offset, &substring)) {
      return {alignment_offset_, 0.0F, 1.0F, std::max(1.0F, metrics_.size.height)};
    }
    const bool rtl = (substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL;
    const float x = static_cast<float>(substring.rect.x + ((rtl != trailing) ? substring.rect.w : 0));
    return {
        x + alignment_offset_,
        static_cast<float>(substring.rect.y),
        1.0F,
        static_cast<float>(std::max(substring.rect.h, TTF_GetFontHeight(bundle_->font))),
    };
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const int start = Utf16ToUtf8Byte(text_, std::max<TextOffset>(0, range.start)).value_or(0);
    const int end = Utf16ToUtf8Byte(text_, std::max(range.start, range.end)).value_or(static_cast<int>(text_.size()));
    if (start >= end) {
      return {};
    }
    struct DirectedRect final {
      Rect rect;
      TTF_SubStringFlags direction = TTF_SUBSTRING_DIRECTION_MASK;
    };
    std::vector<DirectedRect> result;
    int byte_offset = start;
    int previous_cluster = -1;
    while (byte_offset < end) {
      TTF_SubString substring{};
      if (!TTF_GetTextSubString(text_handle_.text, byte_offset, &substring)) {
        break;
      }
      const SDL_Rect& rect = substring.rect;
      if (rect.w <= 0 || rect.h <= 0) {
      } else if (substring.cluster_index != previous_cluster) {
        result.push_back({
            {
                static_cast<float>(rect.x) + alignment_offset_,
                static_cast<float>(rect.y),
                static_cast<float>(rect.w),
                static_cast<float>(rect.h),
            },
            substring.flags & TTF_SUBSTRING_DIRECTION_MASK,
        });
        previous_cluster = substring.cluster_index;
      }
      const int next = substring.offset + substring.length;
      if (next <= byte_offset) {
        const auto decoded = DecodeUtf8(text_, static_cast<std::size_t>(byte_offset));
        if (!decoded.has_value()) {
          break;
        }
        byte_offset += decoded->second;
      } else {
        byte_offset = next;
      }
    }
    std::ranges::sort(result, [](const DirectedRect& left, const DirectedRect& right) {
      return left.rect.y == right.rect.y ? left.rect.x < right.rect.x : left.rect.y < right.rect.y;
    });
    std::vector<Rect> merged;
    std::optional<TTF_SubStringFlags> previous_direction;
    for (const DirectedRect& directed : result) {
      const Rect& rect = directed.rect;
      if (!merged.empty() && previous_direction == directed.direction && merged.back().y == rect.y &&
          merged.back().height == rect.height && rect.x <= merged.back().x + merged.back().width + 1.0F) {
        const float right = std::max(merged.back().x + merged.back().width, rect.x + rect.width);
        merged.back().width = right - merged.back().x;
      } else {
        merged.push_back(rect);
      }
      previous_direction = directed.direction;
    }
    return merged;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto iterator = std::ranges::lower_bound(caret_offsets_, offset);
    return iterator == caret_offsets_.begin() ? caret_offsets_.front() : *std::prev(iterator);
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto iterator = std::ranges::upper_bound(caret_offsets_, offset);
    return iterator == caret_offsets_.end() ? caret_offsets_.back() : *iterator;
  }

private:
  static FontMetrics MetricsFor(const TTF_Font& font) {
    const float ascent = static_cast<float>(TTF_GetFontAscent(&font));
    const float descent = static_cast<float>(std::abs(TTF_GetFontDescent(&font)));
    const float height = static_cast<float>(TTF_GetFontHeight(&font));
    return {
        .ascent = ascent,
        .descent = descent,
        .leading = std::max(0.0F, height - ascent - descent),
        .underline_position = std::max(1.0F, descent * 0.5F),
        .underline_thickness = std::max(1.0F, std::round(height / 14.0F)),
        .strike_through_position = ascent * 0.4F,
        .strike_through_thickness = std::max(1.0F, std::round(height / 14.0F)),
    };
  }

  void BuildCaretOffsets() {
    caret_offsets_.push_back(0);
    std::size_t byte_offset = 0;
    while (byte_offset < text_.size()) {
      TTF_SubString cluster{};
      if (TTF_GetTextSubString(text_handle_.text, static_cast<int>(byte_offset), &cluster)) {
        const int cluster_start = std::clamp(cluster.offset, 0, static_cast<int>(text_.size()));
        const int cluster_end = std::clamp(cluster.offset + cluster.length, 0, static_cast<int>(text_.size()));
        if (const auto utf16 = Utf8ByteToUtf16(text_, cluster_start)) {
          caret_offsets_.push_back(*utf16);
        }
        if (const auto utf16 = Utf8ByteToUtf16(text_, cluster_end)) {
          caret_offsets_.push_back(*utf16);
        }
      }
      const auto decoded = DecodeUtf8(text_, byte_offset);
      if (!decoded.has_value()) {
        break;
      }
      byte_offset += static_cast<std::size_t>(decoded->second);
    }
    if (const auto end = Utf8ByteToUtf16(text_, static_cast<int>(text_.size()))) {
      caret_offsets_.push_back(*end);
    }
    std::ranges::sort(caret_offsets_);
    caret_offsets_.erase(std::ranges::unique(caret_offsets_).begin(), caret_offsets_.end());
    if (caret_offsets_.empty()) {
      caret_offsets_.push_back(0);
    }
  }

  std::shared_ptr<LinuxTextRenderer::State> state_;
  std::string text_;
  TextStyle style_;
  TextLayoutOptions options_;
  float max_width_ = std::numeric_limits<float>::infinity();
  float alignment_offset_ = 0.0F;
  TextDirection direction_ = TextDirection::LeftToRight;
  std::shared_ptr<FontBundle> bundle_;
  TextHandle text_handle_;
  TextLayoutMetrics metrics_;
  std::vector<TextOffset> caret_offsets_;

  friend class LinuxTextRenderer;
};

} // namespace

LinuxTextRenderer::LinuxTextRenderer() : state_(std::make_shared<State>()) {}

LinuxTextRenderer::~LinuxTextRenderer() = default;

FontMetrics LinuxTextRenderer::Metrics(const Font& font) {
  const TextStyle style{font, Color::Black()};
  const std::shared_ptr<FontBundle> bundle = state_->BundleFor(style, {}, TextAlign::Leading);
  const float ascent = static_cast<float>(TTF_GetFontAscent(bundle->font));
  const float descent = static_cast<float>(std::abs(TTF_GetFontDescent(bundle->font)));
  const float height = static_cast<float>(TTF_GetFontHeight(bundle->font));
  return {
      .ascent = ascent,
      .descent = descent,
      .leading = std::max(0.0F, height - ascent - descent),
      .underline_position = std::max(1.0F, descent * 0.5F),
      .underline_thickness = std::max(1.0F, std::round(height / 14.0F)),
      .strike_through_position = ascent * 0.4F,
      .strike_through_thickness = std::max(1.0F, std::round(height / 14.0F)),
  };
}

TextRunMetrics
LinuxTextRenderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  if (text.find_first_of("\r\n") != std::string_view::npos) {
    throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
  }
  TextStyle measurement_style = style;
  measurement_style.foreground = Color::Black();
  SdlTextLayout layout(
      state_,
      text,
      measurement_style,
      std::numeric_limits<float>::infinity(),
      {.shaping = options, .wrap = TextWrap::NoWrap}
  );
  const FontMetrics metrics = Metrics(style.font);
  const Size size = layout.Measure();
  LinuxRenderedText rendered = layout.Render();
  return {
      .advance = size.width,
      .visual_bounds = rendered.surface ? InkBounds(*rendered.surface, rendered.metrics.first_baseline) : Rect{},
      .font_metrics = metrics,
  };
}

TextLayoutMetrics LinuxTextRenderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  if (std::isfinite(max_width) && max_width <= 0.0F) {
    return {};
  }
  return SdlTextLayout(state_, text, style, max_width, options).MetricsValue();
}

std::unique_ptr<TextLayout> LinuxTextRenderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return std::make_unique<SdlTextLayout>(state_, text, style, max_width, options);
}

LinuxRenderedText LinuxTextRenderer::Render(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options, float raster_scale
) {
  raster_scale = std::clamp(raster_scale, 1.0F, 8.0F);
  if (raster_scale == 1.0F) {
    LinuxRenderedText rendered = SdlTextLayout(state_, text, style, max_width, options).Render();
    rendered.raster_scale = 1.0F;
    return rendered;
  }
  TextStyle raster_style = style;
  raster_style.font = style.font.WithSize(style.font.Size() * raster_scale);
  const float raster_width = std::isfinite(max_width) ? max_width * raster_scale : max_width;
  LinuxRenderedText rendered = SdlTextLayout(state_, text, raster_style, raster_width, options).Render();
  rendered.metrics.size.width /= raster_scale;
  rendered.metrics.size.height /= raster_scale;
  rendered.metrics.first_baseline /= raster_scale;
  rendered.metrics.last_baseline /= raster_scale;
  rendered.raster_scale = raster_scale;
  return rendered;
}

} // namespace huxerui::detail
