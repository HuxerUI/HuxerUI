#include "web_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <emscripten.h>

#include "path_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"
#include "text_input_internal.h"
#include "text_layout_internal.h"
#include "web_external_texture_internal.h"
#include "web_text_internal.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

// clang-format off
EM_JS(
    void,
    RequestWebImageDecode,
    (std::uintptr_t session_id, std::uint64_t image_id, const void* data, std::size_t size, const char* mime_type),
    {
      const sessions = Module.huxerUIWebSessions;
      const session = sessions && sessions.get(session_id);
      if (!session || session.images.has(image_id)) {
        return;
      }
      if ((session.imageFailures.get(image_id) || 0) >= 3) {
        return;
      }
      session.images.set(image_id, null);
      const bytes = HEAPU8.slice(data, data + size);
      const blob = new Blob([bytes], {
        type:
          UTF8ToString(mime_type)
      });
      createImageBitmap(blob)
          .then(
              (image) =>
                        {
                          const current = sessions.get(session_id);
                          if (!current) {
                            try {
                              image.close();
                            } catch (error) {
                              console.error("HuxerUI Web decoded image cleanup failed", error);
                            }
                            return;
                          }
                          const byteSize = Math.max(0, image.width * image.height * 4);
                          const budget = 64 * 1024 * 1024;
                          const evictOldest = () => {
                            for (const [identity, cached] of current.images) {
                              if (identity === image_id || !cached) {
                                continue;
                              }
                              current.images.delete(identity);
                              current.imageBytes = Math.max(
                                  0,
                                  current.imageBytes - (current.imageSizes.get(identity) || 0)
                              );
                              current.imageSizes.delete(identity);
                              try {
                                if (cached.close) {
                                  cached.close();
                                }
                              } catch (error) {
                                console.error("HuxerUI Web cached image cleanup failed", error);
                              }
                              return true;
                            }
                            return false;
                          };
                          if (byteSize > budget) {
                            while (evictOldest()) {
                            }
                          } else {
                            while (current.imageBytes > budget - byteSize && evictOldest()) {
                            }
                          }
                          current.images.delete(image_id);
                          current.images.set(image_id, image);
                          current.imageSizes.set(image_id, byteSize);
                          current.imageBytes += byteSize;
                          current.imageFailures.delete(image_id);
                          Module._huxerui_web_image_ready(session_id);
                        }
          )
          .catch((error) => {
            const current = sessions.get(session_id);
            if (current) {
              current.images.delete(image_id);
              const failures = (current.imageFailures.get(image_id) || 0) + 1;
              current.imageFailures.delete(image_id);
              current.imageFailures.set(image_id, failures);
              while (current.imageFailures.size > 256) {
                current.imageFailures.delete(current.imageFailures.keys().next().value);
              }
              if (failures < 3) {
                setTimeout(() => {
                  if (sessions.get(session_id) === current && !current.images.has(image_id)) {
                    Module._huxerui_web_image_ready(session_id);
                  }
                }, failures * 500);
              }
            }
            console.error("HuxerUI Web image decode failed", error);
          });
    }
);

EM_JS(emscripten::EM_VAL, GetWebImage, (std::uintptr_t session_id, std::uint64_t image_id), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(session_id);
  const image = session && session.images.get(image_id);
  if (image) {
    session.images.delete(image_id);
    session.images.set(image_id, image);
  }
  return Emval.toHandle(image || null);
});
// clang-format on

float NumberProperty(const val& object, const char* name, float fallback) {
  const val value = object[name];
  if (value.isUndefined() || value.isNull()) {
    return fallback;
  }
  const double number = value.as<double>();
  return std::isfinite(number) ? static_cast<float>(number) : fallback;
}

std::string CssColor(Color color) {
  std::ostringstream result;
  result << "rgba(" << std::clamp(std::lround(color.red * 255.0F), 0L, 255L) << ','
         << std::clamp(std::lround(color.green * 255.0F), 0L, 255L) << ','
         << std::clamp(std::lround(color.blue * 255.0F), 0L, 255L) << ',' << std::clamp(color.alpha, 0.0F, 1.0F) << ')';
  return result.str();
}

std::string CssFont(const Font& font) {
  std::string family;
  switch (font.FamilyKind()) {
  case FontFamilyKind::System:
    family = "system-ui, sans-serif";
    break;
  case FontFamilyKind::Monospace:
    family = "ui-monospace, monospace";
    break;
  case FontFamilyKind::Named:
    family.reserve(font.FamilyName().size() + 2);
    family.push_back('"');
    for (const char value : font.FamilyName()) {
      if (value == '"' || value == '\\') {
        family.push_back('\\');
      }
      family.push_back(value);
    }
    family.push_back('"');
    break;
  }

  std::ostringstream result;
  if (font.Slant() == FontSlant::Italic) {
    result << "italic ";
  }
  result << static_cast<unsigned int>(font.Weight()) << ' ' << font.Size() << "px " << family;
  return result.str();
}

const char* CssDirection(TextDirection direction) noexcept {
  return direction == TextDirection::RightToLeft ? "rtl" : "ltr";
}

const char* CssLineCap(StrokeCap cap) noexcept {
  switch (cap) {
  case StrokeCap::Butt:
    return "butt";
  case StrokeCap::Round:
    return "round";
  case StrokeCap::Square:
    return "square";
  }
  return "butt";
}

const char* CssLineJoin(StrokeJoin join) noexcept {
  switch (join) {
  case StrokeJoin::Miter:
    return "miter";
  case StrokeJoin::Round:
    return "round";
  case StrokeJoin::Bevel:
    return "bevel";
  }
  return "miter";
}

void ApplyTransform(val& context, Transform2D transform) {
  context.call<void>(
      "transform",
      transform.m11,
      transform.m12,
      transform.m21,
      transform.m22,
      transform.translate_x,
      transform.translate_y
  );
}

void AddRoundedRect(val& context, Rect rect, float radius) {
  radius = std::clamp(radius, 0.0F, std::min(rect.width, rect.height) * 0.5F);
  if (radius <= 0.0F) {
    context.call<void>("rect", rect.x, rect.y, rect.width, rect.height);
    return;
  }
  const float right = rect.x + rect.width;
  const float bottom = rect.y + rect.height;
  context.call<void>("moveTo", rect.x + radius, rect.y);
  context.call<void>("lineTo", right - radius, rect.y);
  context.call<void>("arcTo", right, rect.y, right, rect.y + radius, radius);
  context.call<void>("lineTo", right, bottom - radius);
  context.call<void>("arcTo", right, bottom, right - radius, bottom, radius);
  context.call<void>("lineTo", rect.x + radius, bottom);
  context.call<void>("arcTo", rect.x, bottom, rect.x, bottom - radius, radius);
  context.call<void>("lineTo", rect.x, rect.y + radius);
  context.call<void>("arcTo", rect.x, rect.y, rect.x + radius, rect.y, radius);
  context.call<void>("closePath");
}

void AddPath(val& context, const Path& path) {
  for (const PathElement& element : PathAccess::Elements(path)) {
    switch (element.verb) {
    case PathVerb::MoveTo:
      context.call<void>("moveTo", element.points[0].x, element.points[0].y);
      break;
    case PathVerb::LineTo:
      context.call<void>("lineTo", element.points[0].x, element.points[0].y);
      break;
    case PathVerb::QuadraticTo:
      context.call<void>(
          "quadraticCurveTo",
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y
      );
      break;
    case PathVerb::CubicTo:
      context.call<void>(
          "bezierCurveTo",
          element.points[0].x,
          element.points[0].y,
          element.points[1].x,
          element.points[1].y,
          element.points[2].x,
          element.points[2].y
      );
      break;
    case PathVerb::Close:
      context.call<void>("closePath");
      break;
    }
  }
}

template <typename AddCaster>
void DrawExteriorShadow(
    val& context, Rect bounds, Color color, float blur, PathFillRule fill_rule, const AddCaster& add_caster
) {
  if (bounds.IsEmpty()) {
    return;
  }
  context.call<void>("save");
  context.call<void>("beginPath");
  context.call<void>("rect", bounds.x, bounds.y, bounds.width, bounds.height);
  add_caster(context);
  context.call<void>("clip", std::string("evenodd"));
  context.set("shadowColor", CssColor(color));
  context.set("shadowBlur", blur);
  context.set("fillStyle", std::string("rgba(255,255,255,1)"));
  context.call<void>("beginPath");
  add_caster(context);
  context.call<void>("fill", std::string(fill_rule == PathFillRule::EvenOdd ? "evenodd" : "nonzero"));
  context.call<void>("restore");
}

float MeasureWidth(val& context, std::string_view text) {
  return NumberProperty(context.call<val>("measureText", std::string(text)), "width", 0.0F);
}

FontMetrics ResolveWebFontMetrics(val& context, const Font& font) {
  context.set("font", CssFont(font));
  context.set("textBaseline", std::string("alphabetic"));
  const val measured = context.call<val>("measureText", std::string("Mg"));
  const float fallback_ascent = font.Size() * 0.8F;
  const float fallback_descent = font.Size() * 0.2F;
  const float actual_ascent = NumberProperty(measured, "actualBoundingBoxAscent", fallback_ascent);
  const float actual_descent = NumberProperty(measured, "actualBoundingBoxDescent", fallback_descent);
  const float ascent = NumberProperty(measured, "fontBoundingBoxAscent", actual_ascent);
  const float descent = NumberProperty(measured, "fontBoundingBoxDescent", actual_descent);
  const float leading = std::max(0.0F, font.Size() * 1.2F - ascent - descent);
  return {
      ascent,
      descent,
      leading,
      std::max(1.0F, font.Size() * 0.1F),
      std::max(1.0F, font.Size() / 14.0F),
      ascent * 0.4F,
      std::max(1.0F, font.Size() / 14.0F),
  };
}

void DrawTextDecorations(
    val& context, float x, float baseline, float width, const TextStyle& style, const FontMetrics& metrics
) {
  context.set("fillStyle", CssColor(style.foreground));
  if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
    context.call<void>(
        "fillRect",
        x,
        baseline + metrics.underline_position - metrics.underline_thickness * 0.5F,
        width,
        metrics.underline_thickness
    );
  }
  if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
    context.call<void>(
        "fillRect",
        x,
        baseline - metrics.strike_through_position - metrics.strike_through_thickness * 0.5F,
        width,
        metrics.strike_through_thickness
    );
  }
}

std::vector<TextOffset> BrowserGraphemeBoundaries(std::string_view text, std::string_view locale) {
  const TextOffset length = Utf16Length(text).value_or(0);
  std::vector<TextOffset> result{0};
  if (length == 0) {
    return result;
  }
  try {
    const val segmenter_type = val::global("Intl")["Segmenter"];
    if (!segmenter_type.isUndefined()) {
      val options = val::object();
      options.set("granularity", std::string("grapheme"));
      const val locale_value = locale.empty() ? val::undefined() : val(std::string(locale));
      const val segmenter = segmenter_type.new_(locale_value, options);
      const val segments = val::global("Array").call<val>("from", segmenter.call<val>("segment", std::string(text)));
      const auto count = segments["length"].as<unsigned int>();
      for (unsigned int index = 0; index < count; ++index) {
        const TextOffset offset = segments[index]["index"].as<TextOffset>();
        if (offset > result.back() && offset < length) {
          result.push_back(offset);
        }
      }
      result.push_back(length);
      return result;
    }
  } catch (...) {
  }

  for (TextOffset offset = 1; offset <= length; ++offset) {
    if (Utf8TextInRange(text, {0, offset}).has_value()) {
      result.push_back(offset);
    }
  }
  return result;
}

} // namespace

class WebTextLayout final : public TextLayout {
public:
  struct CaretStop {
    TextOffset offset = 0;
    float advance = 0.0F;
  };

  struct Line {
    TextOffset start = 0;
    TextOffset end = 0;
    float width = 0.0F;
    float baseline = 0.0F;
    std::vector<CaretStop> carets;
  };

  WebTextLayout(val context, std::string_view text, Font font, float max_width, TextLayoutOptions options)
      : context_(std::move(context)), text_(text), font_(std::move(font)), options_(std::move(options)),
        max_width_(max_width) {
    direction_ = ResolveWebTextDirection(text_, options_.shaping.direction);
    ConfigureContext();
    font_metrics_ = ResolveWebFontMetrics(context_, font_);
    line_height_ = std::max(1.0F, font_metrics_.LineHeight());
    BuildBoundaries();
    BuildLines(max_width_);
    BuildCaretStops();
    context_ = val::undefined();
  }

  [[nodiscard]] bool
  Matches(std::string_view text, const Font& font, float max_width, const TextLayoutOptions& options) const noexcept {
    return text_ == text && font_ == font && options_ == options && max_width_ == max_width;
  }

  [[nodiscard]] Size Measure() const override {
    return metrics_.size;
  }

  [[nodiscard]] TextPosition HitTest(Point point) const override {
    if (lines_.empty()) {
      return {};
    }
    const std::size_t line_index =
        std::min(static_cast<std::size_t>(std::max(0.0F, std::floor(point.y / line_height_))), lines_.size() - 1);
    const Line& line = lines_[line_index];
    const float local_x = point.x - LineOffset(line);
    if (local_x <= 0.0F) {
      return {direction_ == TextDirection::RightToLeft ? line.end : line.start, TextAffinity::Downstream};
    }
    if (local_x >= line.width) {
      return {direction_ == TextDirection::RightToLeft ? line.start : line.end, TextAffinity::Upstream};
    }
    const float logical_x = direction_ == TextDirection::RightToLeft ? line.width - local_x : local_x;

    for (std::size_t index = 1; index < line.carets.size(); ++index) {
      const CaretStop& previous = line.carets[index - 1];
      const CaretStop& current = line.carets[index];
      if (logical_x < (previous.advance + current.advance) * 0.5F) {
        return {previous.offset, TextAffinity::Downstream};
      }
    }
    return {line.end, TextAffinity::Upstream};
  }

  [[nodiscard]] Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    if (lines_.empty()) {
      return {};
    }
    const Line& line = LineForOffset(offset, affinity);
    const TextOffset bounded = std::clamp(offset, line.start, line.end);
    float x = LineAdvance(line, bounded);
    if (direction_ == TextDirection::RightToLeft) {
      x = line.width - x;
    }
    return {LineOffset(line) + x, LineTop(line), 1.0F, line_height_};
  }

  [[nodiscard]] std::vector<Rect> RangeRects(TextRange range) const override {
    std::vector<Rect> result;
    if (!range.IsValid() || range.IsCollapsed()) {
      return result;
    }
    for (const Line& line : lines_) {
      const TextOffset start = std::max(range.start, line.start);
      const TextOffset end = std::min(range.end, line.end);
      if (start >= end) {
        continue;
      }
      float left = LineAdvance(line, start);
      float right = LineAdvance(line, end);
      if (direction_ == TextDirection::RightToLeft) {
        left = line.width - left;
        right = line.width - right;
      }
      if (left > right) {
        std::swap(left, right);
      }
      result.push_back({LineOffset(line) + left, LineTop(line), right - left, line_height_});
    }
    return result;
  }

  [[nodiscard]] TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto found = std::lower_bound(boundaries_.begin(), boundaries_.end(), offset);
    return found == boundaries_.begin() ? 0 : *std::prev(found);
  }

  [[nodiscard]] TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto found = std::upper_bound(boundaries_.begin(), boundaries_.end(), offset);
    return found == boundaries_.end() ? boundaries_.back() : *found;
  }

  [[nodiscard]] const std::vector<Line>& Lines() const noexcept {
    return lines_;
  }

  [[nodiscard]] const TextLayoutMetrics& Metrics() const noexcept {
    return metrics_;
  }

  [[nodiscard]] const FontMetrics& ResolvedFontMetrics() const noexcept {
    return font_metrics_;
  }

  [[nodiscard]] std::string TextFor(const Line& line) const {
    return Utf8TextInRange(text_, {line.start, line.end}).value_or(std::string{});
  }

  [[nodiscard]] float LineOffset(const Line& line) const noexcept {
    switch (options_.align) {
    case TextAlign::Leading:
      return direction_ == TextDirection::RightToLeft ? alignment_width_ - line.width : 0.0F;
    case TextAlign::Center:
      return (alignment_width_ - line.width) * 0.5F;
    case TextAlign::Trailing:
      return direction_ == TextDirection::RightToLeft ? 0.0F : alignment_width_ - line.width;
    }
    return 0.0F;
  }

  [[nodiscard]] TextDirection Direction() const noexcept {
    return direction_;
  }

  [[nodiscard]] float LineWidth(const Line& line) const noexcept {
    return line.width;
  }

private:
  void ConfigureContext() const {
    context_.set("font", CssFont(font_));
    context_.set("textBaseline", std::string("alphabetic"));
    context_.set("direction", std::string(CssDirection(direction_)));
    context_.set("lang", options_.shaping.locale.empty() ? std::string("inherit") : options_.shaping.locale);
  }

  void BuildBoundaries() {
    boundaries_ = BrowserGraphemeBoundaries(text_, options_.shaping.locale);
  }

  [[nodiscard]] float Width(TextOffset start, TextOffset end) const {
    const std::optional<std::string> value = Utf8TextInRange(text_, {start, end});
    if (!value.has_value()) {
      return 0.0F;
    }
    ConfigureContext();
    return MeasureWidth(context_, *value);
  }

  [[nodiscard]] bool IsBreak(TextOffset start, TextOffset end) const {
    const std::optional<std::string> value = Utf8TextInRange(text_, {start, end});
    return value.has_value() && (*value == " " || *value == "\t" || *value == "-" || *value == "\n");
  }

  void AddLine(TextOffset start, TextOffset end) {
    const float width = Width(start, end);
    const float baseline =
        static_cast<float>(lines_.size()) * line_height_ + font_metrics_.leading * 0.5F + font_metrics_.ascent;
    lines_.push_back({start, end, width, baseline, {}});
    widest_line_ = std::max(widest_line_, width);
  }

  void BuildCaretStops() {
    for (Line& line : lines_) {
      line.carets.push_back({line.start, 0.0F});
      for (const TextOffset boundary : boundaries_) {
        if (boundary > line.start && boundary <= line.end) {
          line.carets.push_back({boundary, Width(line.start, boundary)});
        }
      }
      if (line.carets.back().offset != line.end) {
        line.carets.push_back({line.end, line.width});
      }
    }
  }

  void BuildLines(float max_width) {
    const TextOffset length = boundaries_.back();
    const bool constrained = std::isfinite(max_width);
    if (constrained && max_width <= 0.0F) {
      metrics_ = {};
      return;
    }

    TextOffset line_start = 0;
    TextOffset last_break = -1;
    TextOffset previous = 0;
    for (std::size_t index = 1; index < boundaries_.size(); ++index) {
      const TextOffset boundary = boundaries_[index];
      const std::optional<std::string> cluster = Utf8TextInRange(text_, {previous, boundary});
      if (cluster == std::optional<std::string>{"\n"}) {
        AddLine(line_start, previous);
        line_start = boundary;
        last_break = -1;
        previous = boundary;
        continue;
      }
      if (IsBreak(previous, boundary)) {
        last_break = boundary;
      }
      if (constrained && options_.wrap == TextWrap::Word && line_start < previous &&
          Width(line_start, boundary) > max_width) {
        TextOffset end = last_break > line_start ? last_break : previous;
        if (end <= line_start) {
          end = boundary;
        }
        AddLine(line_start, end);
        line_start = end;
        last_break = -1;
      }
      previous = boundary;
    }
    AddLine(line_start, length);

    const float width =
        constrained && options_.wrap == TextWrap::Word ? std::min(max_width, widest_line_) : widest_line_;
    metrics_ = {
        {std::ceil(width), std::ceil(static_cast<float>(lines_.size()) * line_height_)},
        lines_.empty() ? 0.0F : lines_.front().baseline,
        lines_.empty() ? 0.0F : lines_.back().baseline,
        lines_.size(),
    };
    alignment_width_ = constrained ? max_width : metrics_.size.width;
  }

  [[nodiscard]] float LineTop(const Line& line) const noexcept {
    return line.baseline - font_metrics_.ascent - font_metrics_.leading * 0.5F;
  }

  [[nodiscard]] float LineAdvance(const Line& line, TextOffset offset) const noexcept {
    const auto found =
        std::lower_bound(line.carets.begin(), line.carets.end(), offset, [](const CaretStop& stop, TextOffset target) {
          return stop.offset < target;
        });
    if (found == line.carets.end()) {
      return line.width;
    }
    if (found == line.carets.begin() || found->offset == offset) {
      return found->advance;
    }
    return std::prev(found)->advance;
  }

  [[nodiscard]] const Line& LineForOffset(TextOffset offset, TextAffinity affinity) const {
    for (std::size_t index = 0; index < lines_.size(); ++index) {
      const Line& line = lines_[index];
      if (offset > line.start && offset < line.end) {
        return line;
      }
      if (offset == line.start) {
        if (index == 0 || affinity == TextAffinity::Downstream || lines_[index - 1].end != offset) {
          return line;
        }
        return lines_[index - 1];
      }
      if (offset == line.end) {
        if (affinity == TextAffinity::Downstream && index + 1 < lines_.size() && lines_[index + 1].start == offset) {
          return lines_[index + 1];
        }
        return line;
      }
    }
    return offset < lines_.front().start ? lines_.front() : lines_.back();
  }

  mutable val context_;
  std::string text_;
  Font font_;
  TextLayoutOptions options_;
  TextDirection direction_ = TextDirection::LeftToRight;
  FontMetrics font_metrics_;
  TextLayoutMetrics metrics_;
  float line_height_ = 0.0F;
  float widest_line_ = 0.0F;
  float alignment_width_ = 0.0F;
  float max_width_ = 0.0F;
  std::vector<TextOffset> boundaries_;
  std::vector<Line> lines_;
};

struct WebRenderer::CachedExternalTexture {
  ~CachedExternalTexture() {
    CloseWebVideoFrame(frame);
  }

  std::weak_ptr<WebExternalTextureState> source;
  val frame = val::undefined();
  std::uint64_t draw_epoch = std::numeric_limits<std::uint64_t>::max();
};

WebRenderer::WebRenderer(std::uintptr_t session_id, val canvas)
    : canvas_(std::move(canvas)), context_(canvas_.call<val>("getContext", std::string("2d"))),
      session_id_(session_id) {
  if (context_.isNull() || context_.isUndefined()) {
    throw std::runtime_error("HuxerUI Web Canvas 2D context is unavailable");
  }
}

WebRenderer::~WebRenderer() = default;

void WebRenderer::SetViewport(Size viewport, float display_scale) {
  viewport_ = viewport;
  display_scale_ = std::max(1.0F, display_scale);
  canvas_.set("width", static_cast<unsigned int>(std::ceil(viewport.width * display_scale_)));
  canvas_.set("height", static_cast<unsigned int>(std::ceil(viewport.height * display_scale_)));
  force_redraw_ = true;
}

void WebRenderer::Invalidate() noexcept {
  force_redraw_ = true;
}

bool WebRenderer::TakeInvalidation() noexcept {
  return std::exchange(force_redraw_, false);
}

void WebRenderer::BeginFrame() {
  if (external_texture_draw_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    external_texture_draw_epoch_ = 0;
    for (const std::unique_ptr<CachedExternalTexture>& entry : external_textures_) {
      entry->draw_epoch = std::numeric_limits<std::uint64_t>::max();
    }
  } else {
    ++external_texture_draw_epoch_;
  }

  std::erase_if(external_textures_, [](const std::unique_ptr<CachedExternalTexture>& entry) {
    const std::shared_ptr<WebExternalTextureState> source = entry->source.lock();
    return !source || !source->IsActive();
  });
}

const val* WebRenderer::FrameFor(const ExternalTexture& texture) {
  const std::shared_ptr<WebExternalTextureState> source =
      std::dynamic_pointer_cast<WebExternalTextureState>(ExternalTextureState::From(texture));
  if (!source) {
    throw std::logic_error("HuxerUI external texture does not contain a Web VideoFrame source");
  }
  auto cached = std::find_if(external_textures_.begin(), external_textures_.end(), [&source](const auto& entry) {
    return entry->source.lock() == source;
  });
  if (cached == external_textures_.end()) {
    auto entry = std::make_unique<CachedExternalTexture>();
    entry->source = source;
    external_textures_.push_back(std::move(entry));
    cached = std::prev(external_textures_.end());
  }
  CachedExternalTexture& entry = **cached;
  if (entry.draw_epoch != external_texture_draw_epoch_) {
    entry.draw_epoch = external_texture_draw_epoch_;
    val pending = source->AcquireLatestFrame();
    if (!pending.isNull() && !pending.isUndefined()) {
      CloseWebVideoFrame(entry.frame);
      entry.frame = std::move(pending);
    }
  }
  return entry.frame.isNull() || entry.frame.isUndefined() ? nullptr : &entry.frame;
}

const WebTextLayout& WebRenderer::ParagraphFor(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  const auto cached = std::find_if(paragraph_cache_.begin(), paragraph_cache_.end(), [&](const auto& layout) {
    return layout->Matches(text, style.font, max_width, options);
  });
  if (cached != paragraph_cache_.end()) {
    return **cached;
  }

  constexpr std::size_t maximum_paragraphs = 256;
  if (paragraph_cache_.size() >= maximum_paragraphs) {
    paragraph_cache_.erase(paragraph_cache_.begin());
  }
  paragraph_cache_.push_back(std::make_unique<WebTextLayout>(context_, text, style.font, max_width, options));
  return *paragraph_cache_.back();
}

FontMetrics WebRenderer::Metrics(const Font& font) {
  return ResolveWebFontMetrics(context_, font);
}

TextRunMetrics
WebRenderer::MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) {
  context_.set("font", CssFont(style.font));
  context_.set("direction", std::string(CssDirection(ResolveWebTextDirection(text, options.direction))));
  context_.set("lang", options.locale.empty() ? std::string("inherit") : options.locale);
  const val measured = context_.call<val>("measureText", std::string(text));
  const FontMetrics metrics = Metrics(style.font);
  const float advance = NumberProperty(measured, "width", 0.0F);
  const float left = NumberProperty(measured, "actualBoundingBoxLeft", 0.0F);
  const float right = NumberProperty(measured, "actualBoundingBoxRight", advance);
  const float ascent = NumberProperty(measured, "actualBoundingBoxAscent", metrics.ascent);
  const float descent = NumberProperty(measured, "actualBoundingBoxDescent", metrics.descent);
  Rect visual_bounds{-left, -ascent, left + right, ascent + descent};
  const auto include_decoration = [&](float center, float thickness) {
    const float top = center - thickness * 0.5F;
    const float bottom = center + thickness * 0.5F;
    const float left_edge = std::min(visual_bounds.x, 0.0F);
    const float right_edge = std::max(visual_bounds.x + visual_bounds.width, advance);
    const float top_edge = std::min(visual_bounds.y, top);
    const float bottom_edge = std::max(visual_bounds.y + visual_bounds.height, bottom);
    visual_bounds = {left_edge, top_edge, right_edge - left_edge, bottom_edge - top_edge};
  };
  if (HasTextDecoration(style.decoration, TextDecoration::Underline)) {
    include_decoration(metrics.underline_position, metrics.underline_thickness);
  }
  if (HasTextDecoration(style.decoration, TextDecoration::StrikeThrough)) {
    include_decoration(-metrics.strike_through_position, metrics.strike_through_thickness);
  }
  return {advance, visual_bounds, metrics};
}

TextLayoutMetrics WebRenderer::MeasureText(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return ParagraphFor(text, style, max_width, options).Metrics();
}

std::unique_ptr<TextLayout> WebRenderer::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  return std::make_unique<WebTextLayout>(context_, text, style.font, max_width, options);
}

struct WebRenderer::CommandRange {
  std::size_t first = 0;
  std::size_t end = 0;
  std::size_t cursor = 0;
};

void WebRenderer::RenderSequence(const PaintSequence& sequence, CommandRange* range) {
  for (const PaintCommand& command : sequence.Commands()) {
    if (std::holds_alternative<PlacePlatformViewCommand>(command)) {
      continue;
    }
    const bool selected = range == nullptr || (range->cursor >= range->first && range->cursor < range->end);
    if (range != nullptr) {
      ++range->cursor;
    }
    if (!selected) {
      continue;
    }
    std::visit([this](const auto& value) { RenderCommand(value); }, command);
  }
}

void WebRenderer::RenderSceneNode(const RenderNode& node, CommandRange* range) {
  const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
  if (!node.visible || opacity <= 0.0F) {
    return;
  }

  context_.call<void>("save");
  Transform2D transform = node.transform;
  transform.translate_x += node.offset.x;
  transform.translate_y += node.offset.y;
  ApplyTransform(context_, transform);
  context_.set("globalAlpha", context_["globalAlpha"].as<double>() * opacity);

  RenderSequence(node.content, range);
  for (const RenderClip& clip : node.child_clips) {
    std::visit([this](const auto& command) { RenderCommand(command); }, clip);
  }
  if (!node.children_transform.IsIdentity()) {
    RenderCommand(PushTransformCommand{node.children_transform});
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      RenderSceneNode(*child, range);
    }
  }
  if (!node.children_transform.IsIdentity()) {
    RenderCommand(PopTransformCommand{});
  }
  for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
    RenderCommand(PopClipCommand{});
  }
  RenderSequence(node.foreground, range);
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawRectCommand& command) {
  context_.set("fillStyle", CssColor(command.color));
  if (command.corner_radius <= 0.0F) {
    context_.call<void>("fillRect", command.rect.x, command.rect.y, command.rect.width, command.rect.height);
    return;
  }
  context_.call<void>("beginPath");
  AddRoundedRect(context_, command.rect, command.corner_radius);
  context_.call<void>("fill");
}

void WebRenderer::RenderCommand(const DrawTextCommand& command) {
  if (command.rect.IsEmpty() || command.style.foreground.alpha <= 0.0F) {
    return;
  }
  const WebTextLayout& layout = ParagraphFor(command.text, command.style, command.rect.width, command.options);
  const float vertical_offset =
      command.options.wrap == TextWrap::NoWrap ? (command.rect.height - layout.Metrics().size.height) * 0.5F : 0.0F;
  context_.call<void>("save");
  context_.call<void>("beginPath");
  context_.call<void>("rect", command.rect.x, command.rect.y, command.rect.width, command.rect.height);
  context_.call<void>("clip");
  context_.set("font", CssFont(command.style.font));
  context_.set("textBaseline", std::string("alphabetic"));
  context_.set("textAlign", std::string("left"));
  context_.set("direction", std::string(CssDirection(layout.Direction())));
  context_.set(
      "lang",
      command.options.shaping.locale.empty() ? std::string("inherit") : command.options.shaping.locale
  );
  context_.set("fillStyle", CssColor(command.style.foreground));
  for (const WebTextLayout::Line& line : layout.Lines()) {
    const float x = command.rect.x + layout.LineOffset(line);
    const float baseline = command.rect.y + vertical_offset + line.baseline;
    context_.call<void>("fillText", layout.TextFor(line), x, baseline);
    DrawTextDecorations(context_, x, baseline, layout.LineWidth(line), command.style, layout.ResolvedFontMetrics());
  }
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawTextRunsCommand& command) {
  for (const TextRun& run : command.runs) {
    if (run.text.empty() || run.style.foreground.alpha <= 0.0F) {
      continue;
    }
    context_.set("font", CssFont(run.style.font));
    context_.set("textBaseline", std::string("alphabetic"));
    context_.set("textAlign", std::string("left"));
    context_.set("direction", std::string(CssDirection(ResolveWebTextDirection(run.text, run.shaping.direction))));
    context_.set("lang", run.shaping.locale.empty() ? std::string("inherit") : run.shaping.locale);
    context_.set("fillStyle", CssColor(run.style.foreground));
    const bool decorated = run.style.decoration != TextDecoration::None;
    const float width = decorated ? MeasureWidth(context_, run.text) : 0.0F;
    context_.call<void>("fillText", run.text, run.baseline_origin.x, run.baseline_origin.y);
    if (decorated) {
      DrawTextDecorations(
          context_,
          run.baseline_origin.x,
          run.baseline_origin.y,
          width,
          run.style,
          ResolveWebFontMetrics(context_, run.style.font)
      );
    }
  }
}

void WebRenderer::RenderCommand(const DrawImageCommand& command) {
  const std::uint64_t identity = ResourceAccess::ImageIdentity(command.image);
  val image = val::take_ownership(GetWebImage(session_id_, identity));
  if (image.isNull() || image.isUndefined()) {
    if (session_id_ != 0) {
      const std::span<const std::byte> bytes = command.image.EncodedBytes();
      const std::string mime_type{command.image.MimeType()};
      RequestWebImageDecode(session_id_, identity, bytes.data(), bytes.size(), mime_type.c_str());
    }
    return;
  }
  context_.call<void>("save");
  context_.set("globalAlpha", context_["globalAlpha"].as<double>() * command.opacity);
  context_.set("imageSmoothingEnabled", command.sampling == ImageSampling::Linear);
  const float scale = command.image.Scale();
  context_.call<void>(
      "drawImage",
      image,
      command.source.x * scale,
      command.source.y * scale,
      command.source.width * scale,
      command.source.height * scale,
      command.destination.x,
      command.destination.y,
      command.destination.width,
      command.destination.height
  );
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawExternalTextureCommand& command) {
  const val* frame = FrameFor(command.texture);
  if (frame == nullptr) {
    return;
  }
  const float frame_width = NumberProperty(*frame, "displayWidth", 0.0F);
  const float frame_height = NumberProperty(*frame, "displayHeight", 0.0F);
  const Size intrinsic_size = command.texture.IntrinsicSize();
  if (frame_width <= 0.0F || frame_height <= 0.0F || intrinsic_size.width <= 0.0F || intrinsic_size.height <= 0.0F) {
    return;
  }
  const float scale_x = frame_width / intrinsic_size.width;
  const float scale_y = frame_height / intrinsic_size.height;
  context_.call<void>("save");
  context_.set("globalAlpha", context_["globalAlpha"].as<double>() * command.opacity);
  context_.set("imageSmoothingEnabled", command.sampling == ImageSampling::Linear);
  context_.call<void>(
      "drawImage",
      *frame,
      command.source.x * scale_x,
      command.source.y * scale_y,
      command.source.width * scale_x,
      command.source.height * scale_y,
      command.destination.x,
      command.destination.y,
      command.destination.width,
      command.destination.height
  );
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawCircleCommand& command) {
  context_.call<void>("beginPath");
  context_.call<void>("arc", command.center.x, command.center.y, command.radius, 0.0, std::numbers::pi * 2.0);
  context_.set("fillStyle", CssColor(command.color));
  context_.call<void>("fill");
}

void WebRenderer::RenderCommand(const DrawArcCommand& command) {
  if (command.radius <= 0.0F || command.width <= 0.0F || command.sweep_angle == 0.0F) {
    return;
  }
  context_.call<void>("save");
  context_.call<void>("beginPath");
  context_.call<void>(
      "arc",
      command.center.x,
      command.center.y,
      command.radius,
      command.start_angle,
      command.start_angle + command.sweep_angle,
      command.sweep_angle < 0.0F
  );
  context_.set("strokeStyle", CssColor(command.color));
  context_.set("lineWidth", command.width);
  context_.set("lineCap", std::string(CssLineCap(command.cap)));
  context_.call<void>("stroke");
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawBorderCommand& command) {
  if (command.width <= 0.0F) {
    return;
  }
  const float inset = command.width * 0.5F;
  const Rect rect{
      command.rect.x + inset,
      command.rect.y + inset,
      std::max(0.0F, command.rect.width - command.width),
      std::max(0.0F, command.rect.height - command.width),
  };
  context_.call<void>("save");
  context_.call<void>("beginPath");
  AddRoundedRect(context_, rect, std::max(0.0F, command.corner_radius - inset));
  context_.set("strokeStyle", CssColor(command.color));
  context_.set("lineWidth", command.width);
  context_.call<void>("stroke");
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawShadowCommand& command) {
  const ResolvedShadow resolved = ResolveShadow(command);
  if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
    return;
  }
  if (command.blur_radius <= 0.0F) {
    RenderCommand(DrawRectCommand{resolved.caster, command.color, resolved.corner_radius});
    return;
  }
  DrawExteriorShadow(
      context_,
      resolved.bounds,
      command.color,
      resolved.standard_deviation * 2.0F,
      PathFillRule::NonZero,
      [&resolved](val& context) { AddRoundedRect(context, resolved.caster, resolved.corner_radius); }
  );
}

void WebRenderer::RenderCommand(const FillPathCommand& command) {
  context_.call<void>("beginPath");
  AddPath(context_, command.path);
  context_.set("fillStyle", CssColor(command.color));
  context_.call<void>("fill", std::string(command.fill_rule == PathFillRule::EvenOdd ? "evenodd" : "nonzero"));
}

void WebRenderer::RenderCommand(const StrokePathCommand& command) {
  context_.call<void>("save");
  context_.call<void>("beginPath");
  AddPath(context_, command.path);
  context_.set("strokeStyle", CssColor(command.color));
  context_.set("lineWidth", command.width);
  context_.set("lineCap", std::string(CssLineCap(command.cap)));
  context_.set("lineJoin", std::string(CssLineJoin(command.join)));
  context_.set("miterLimit", command.miter_limit);
  context_.call<void>("stroke");
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const DrawPathShadowCommand& command) {
  if (command.color.alpha <= 0.0F || command.path.IsEmpty()) {
    return;
  }
  context_.call<void>("save");
  context_.call<void>("translate", command.offset.x, command.offset.y);
  if (command.blur_radius <= 0.0F) {
    context_.set("fillStyle", CssColor(command.color));
    context_.call<void>("beginPath");
    AddPath(context_, command.path);
    context_.call<void>("fill", std::string(command.fill_rule == PathFillRule::EvenOdd ? "evenodd" : "nonzero"));
    context_.call<void>("restore");
    return;
  }
  const Rect caster = command.path.Bounds();
  const Rect bounds{
      caster.x - command.blur_radius,
      caster.y - command.blur_radius,
      caster.width + command.blur_radius * 2.0F,
      caster.height + command.blur_radius * 2.0F,
  };
  DrawExteriorShadow(
      context_,
      bounds,
      command.color,
      command.blur_radius * 2.0F / 3.0F,
      command.fill_rule,
      [&command](val& context) { AddPath(context, command.path); }
  );
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const PushClipCommand& command) {
  context_.call<void>("save");
  context_.call<void>("beginPath");
  AddRoundedRect(context_, command.rect, command.corner_radius);
  context_.call<void>("clip");
}

void WebRenderer::RenderCommand(const PushPathClipCommand& command) {
  context_.call<void>("save");
  context_.call<void>("beginPath");
  AddPath(context_, command.path);
  context_.call<void>("clip", std::string(command.fill_rule == PathFillRule::EvenOdd ? "evenodd" : "nonzero"));
}

void WebRenderer::RenderCommand(const PopClipCommand& command) {
  static_cast<void>(command);
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const PushTransformCommand& command) {
  context_.call<void>("save");
  ApplyTransform(context_, command.transform);
}

void WebRenderer::RenderCommand(const PopTransformCommand& command) {
  static_cast<void>(command);
  context_.call<void>("restore");
}

void WebRenderer::RenderCommand(const PlacePlatformViewCommand& command) {
  static_cast<void>(command);
  throw std::logic_error("HuxerUI Web renderer received a PlatformView outside RenderComposition");
}

void WebRenderer::Draw(const RenderFrame& frame) {
  DrawTarget(canvas_, frame, nullptr, true, TakeInvalidation());
}

void WebRenderer::DrawSlice(
    const val& canvas,
    const RenderFrame& frame,
    std::size_t first_command,
    std::size_t command_count,
    bool draw_background,
    bool force_redraw
) {
  const std::size_t end = command_count > std::numeric_limits<std::size_t>::max() - first_command
                              ? std::numeric_limits<std::size_t>::max()
                              : first_command + command_count;
  CommandRange range{first_command, end, 0};
  DrawTarget(canvas, frame, &range, draw_background, force_redraw);
}

void WebRenderer::DrawTarget(
    const val& canvas, const RenderFrame& frame, CommandRange* range, bool draw_background, bool force_redraw
) {
  val previous_context = context_;
  context_ = canvas.call<val>("getContext", std::string("2d"));
  if (context_.isNull() || context_.isUndefined()) {
    context_ = std::move(previous_context);
    throw std::runtime_error("HuxerUI Web Canvas 2D context is unavailable");
  }

  const bool full_redraw = force_redraw || frame.damage.full;
  if (!full_redraw && frame.damage.rects.empty()) {
    context_ = std::move(previous_context);
    return;
  }
  std::vector<Rect> regions = frame.damage.rects;
  if (full_redraw) {
    regions = {{0.0F, 0.0F, viewport_.width, viewport_.height}};
  }
  std::erase_if(regions, [](const Rect& region) { return region.IsEmpty(); });
  if (regions.empty()) {
    context_ = std::move(previous_context);
    return;
  }

  try {
    context_.call<void>("save");
    context_.call<void>("setTransform", 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    context_.call<void>("beginPath");
    const double display_scale = display_scale_;
    for (const Rect& region : regions) {
      const double left = std::floor(static_cast<double>(region.x) * display_scale);
      const double top = std::floor(static_cast<double>(region.y) * display_scale);
      const double right = std::ceil(static_cast<double>(region.x + region.width) * display_scale);
      const double bottom = std::ceil(static_cast<double>(region.y + region.height) * display_scale);
      context_.call<void>("clearRect", left, top, right - left, bottom - top);
      context_.call<void>("rect", left, top, right - left, bottom - top);
    }
    context_.call<void>("clip");
    if (draw_background) {
      context_.set("fillStyle", CssColor(Color::Rgb(247, 248, 250)));
      context_.call<void>(
          "fillRect",
          0.0,
          0.0,
          std::ceil(static_cast<double>(viewport_.width) * display_scale),
          std::ceil(static_cast<double>(viewport_.height) * display_scale)
      );
    }
    context_.call<void>("setTransform", display_scale, 0.0, 0.0, display_scale, 0.0, 0.0);
    if (frame.scene.root != nullptr) {
      RenderSceneNode(*frame.scene.root, range);
    }
    context_.call<void>("restore");
  } catch (...) {
    try {
      context_.call<void>("restore");
    } catch (...) {
    }
    context_ = std::move(previous_context);
    throw;
  }
  context_ = std::move(previous_context);
}

} // namespace huxerui::detail
