#include "testing_internal.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

#include "text/text_internal.h"
#include "text/text_input_internal.h"

namespace huxerui::detail {
namespace {

// Scalar cells deliberately do not emulate native shaping, bidi, ligatures, or grapheme navigation.
class ReferenceTextLayout final : public TextLayout {
public:
  ReferenceTextLayout(std::string_view text, float size, float width, const TextLayoutOptions& options)
      : line_height_(size * 1.25F), ascent_(size), maximum_width_(width) {
    if (std::isnan(width) || width < 0) throw std::invalid_argument("HuxerUI testing text width is invalid");
    lines_.push_back({{{0, 0.0F}}, false});
    offsets_.push_back(0);
    TextOffset offset = 0;
    for (std::size_t index = 0; index < text.size();) {
      Utf8CodePoint scalar;
      if (!DecodeCodePoint(text, index, scalar)) throw std::invalid_argument("HuxerUI testing text is invalid UTF-8");
      index += scalar.byte_length;
      if (scalar.value == '\n') {
        lines_.back().hard_break = true;
        ++offset;
        offsets_.push_back(offset);
        lines_.push_back({{{offset, 0.0F}}, false});
        continue;
      }
      const float advance = size * 0.6F;
      if (options.wrap != TextWrap::NoWrap && lines_.back().points.back().x > 0 &&
          lines_.back().points.back().x + advance > width) {
        lines_.push_back({{{offset, 0.0F}}, false});
      }
      offset += scalar.value > 0xFFFF ? 2 : 1;
      offsets_.push_back(offset);
      lines_.back().points.push_back({offset, lines_.back().points.back().x + advance});
    }
  }

  Size Measure() const override {
    float width = 0;
    for (const auto& line : lines_) width = std::max(width, line.points.back().x);
    return {std::min(width, maximum_width_), line_height_ * static_cast<float>(lines_.size())};
  }
  TextLayoutMetrics Metrics() const {
    return {Measure(), ascent_, ascent_ + line_height_ * static_cast<float>(lines_.size() - 1), lines_.size()};
  }
  TextPosition HitTest(Point point) const override {
    const auto index = static_cast<std::size_t>(std::clamp(std::floor(point.y / line_height_),
                                                          0.0F, static_cast<float>(lines_.size() - 1)));
    const auto& line = lines_[index];
    for (std::size_t i = 1; i < line.points.size(); ++i) {
      if (point.x < (line.points[i - 1].x + line.points[i].x) * 0.5F)
        return {line.points[i - 1].offset, TextAffinity::Downstream};
    }
    return {line.points.back().offset, index + 1 < lines_.size() && !line.hard_break
                                          ? TextAffinity::Upstream : TextAffinity::Downstream};
  }
  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    std::size_t line = 0;
    for (; line + 1 < lines_.size(); ++line) {
      const auto end = lines_[line].points.back().offset;
      if (offset < end || (offset == end && (affinity == TextAffinity::Upstream || lines_[line].hard_break))) break;
    }
    return {Position(lines_[line], offset), static_cast<float>(line) * line_height_, 1.0F, line_height_};
  }
  std::vector<Rect> RangeRects(TextRange range) const override {
    std::vector<Rect> result;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
      const auto& line = lines_[i];
      const auto start = std::max(range.start, line.points.front().offset);
      const auto end = std::min(range.end, line.points.back().offset);
      if (start < end) result.push_back({Position(line, start), static_cast<float>(i) * line_height_,
                                        Position(line, end) - Position(line, start), line_height_});
    }
    return result;
  }
  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    const auto found = std::lower_bound(offsets_.begin(), offsets_.end(), offset);
    return found == offsets_.begin() ? 0 : *std::prev(found);
  }
  TextOffset NextCaretOffset(TextOffset offset) const override {
    const auto found = std::upper_bound(offsets_.begin(), offsets_.end(), offset);
    return found == offsets_.end() ? offsets_.back() : *found;
  }

private:
  struct Boundary { TextOffset offset; float x; };
  struct Line { std::vector<Boundary> points; bool hard_break; };
  static float Position(const Line& line, TextOffset offset) {
    const auto found = std::lower_bound(line.points.begin(), line.points.end(), offset,
        [](const Boundary& point, TextOffset value) { return point.offset < value; });
    return found == line.points.end() ? line.points.back().x : found->x;
  }
  float line_height_;
  float ascent_;
  float maximum_width_;
  std::vector<Line> lines_;
  std::vector<TextOffset> offsets_;
};

} // namespace

TestingPlatformAdapter::TestingPlatformAdapter(std::shared_ptr<UiTestQueue> queue,
                                               const testing::UiTestOptions& options)
    : PlatformAdapter([queue = std::move(queue)](std::function<void()> callback) {
        std::scoped_lock lock(queue->mutex);
        if (!queue->closed) queue->callbacks.push_back(std::move(callback));
      }), configuration(options.resources), system_color_scheme(options.system_color_scheme),
      resources_(options.resource_provider) {}

void TestingPlatformAdapter::RequestFrameAt(double deadline) {
  if (!std::isfinite(deadline)) throw std::invalid_argument("HuxerUI testing frame deadline must be finite");
  if (!frame_deadline || deadline < *frame_deadline) frame_deadline = deadline;
}

FontMetrics TestingPlatformAdapter::Metrics(const Font& font) {
  const float size = font.Size();
  return {.ascent = size, .descent = size * 0.25F, .underline_position = size * 0.1F,
          .underline_thickness = 1.0F, .strike_through_position = size * 0.4F, .strike_through_thickness = 1.0F};
}

TextRunMetrics TestingPlatformAdapter::MeasureRun(std::string_view text, const TextStyle& style,
                                                const TextShapingOptions&) {
  ReferenceTextLayout layout(text, style.font.Size(), std::numeric_limits<float>::infinity(), {});
  const auto metrics = Metrics(style.font);
  const float width = layout.Measure().width;
  return {width, {0, -metrics.ascent, width, metrics.LineHeight()}, metrics};
}

TextLayoutMetrics TestingPlatformAdapter::MeasureText(const AttributedText& text, const TextStyle& style,
                                                    float width, const TextLayoutOptions& options) {
  return ReferenceTextLayout(text.PlainText(), style.font.Size(), width, options).Metrics();
}

std::unique_ptr<TextLayout> TestingPlatformAdapter::CreateTextLayout(const AttributedText& text, const TextStyle& style,
                                                                  float width, const TextLayoutOptions& options) {
  return std::make_unique<ReferenceTextLayout>(text.PlainText(), style.font.Size(), width, options);
}

std::optional<InputStream> TestingPlatformAdapter::OpenRead(std::string_view path) {
  return resources_ ? resources_->OpenRead(path) : std::nullopt;
}

bool TestingPlatformAdapter::WriteText(std::string_view text) {
  if (!Utf16Length(text)) return false;
  clipboard_text = text;
  return true;
}

void TestingPlatformAdapter::Start(TextInputSessionId id, const TextInputConfiguration& config,
                                  const TextInputState& state, const TextInputGeometry&) {
  input_state = state;
  input_state->session_id = id;
  input_action = config.action;
}
void TestingPlatformAdapter::Update(TextInputSessionId id, const TextInputState& state, const TextInputGeometry&) {
  if (input_state && input_state->session_id == id) input_state = state;
}
void TestingPlatformAdapter::Restart(TextInputSessionId id, const TextInputConfiguration& config,
                                     const TextInputState& state, const TextInputGeometry& geometry) {
  if (input_state && id == input_state->session_id) Start(id, config, state, geometry);
}
void TestingPlatformAdapter::Stop(TextInputSessionId id) {
  if (input_state && id == input_state->session_id) {
    input_state.reset();
    input_action = TextInputAction::Default;
  }
}

} // namespace huxerui::detail
