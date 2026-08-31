#include "svg_compiler.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace huxerui::resource_compiler {

namespace {

struct Point {
  float x = 0.0F;
  float y = 0.0F;
};

struct Rect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

struct Transform {
  float m11 = 1.0F;
  float m12 = 0.0F;
  float m21 = 0.0F;
  float m22 = 1.0F;
  float tx = 0.0F;
  float ty = 0.0F;
};

struct Color {
  float red = 0.0F;
  float green = 0.0F;
  float blue = 0.0F;
  float alpha = 1.0F;
};

struct PathOperation {
  std::uint8_t verb = 0;
  std::vector<float> values;
};

using Path = std::vector<PathOperation>;

struct Style {
  std::optional<Color> fill = Color{};
  std::optional<std::string> fill_reference;
  std::optional<Color> stroke;
  Color current_color{};
  bool fill_uses_current_color = false;
  bool stroke_uses_current_color = false;
  Color stop_color{};
  bool stop_uses_current_color = false;
  float stop_opacity = 1.0F;
  float fill_opacity = 1.0F;
  float stroke_opacity = 1.0F;
  float stroke_width = 1.0F;
  std::uint8_t fill_rule = 0;
  std::uint8_t clip_rule = 0;
  std::uint8_t stroke_cap = 0;
  std::uint8_t stroke_join = 0;
  float miter_limit = 4.0F;
  std::vector<float> dash_pattern;
  float dash_offset = 0.0F;
  bool displayed = true;
  bool visible = true;
  float opacity = 1.0F;
  std::optional<std::string> clip_reference;
};

struct SvgNode {
  std::string name;
  std::map<std::string, std::string> attributes;
  std::vector<std::size_t> children;
  std::optional<Path> path;
  std::optional<std::size_t> parent;
};

struct SvgDocument {
  std::vector<SvgNode> nodes;
  std::size_t root = 0;
  std::unordered_map<std::string, std::size_t> ids;
};

class Writer {
public:
  void U8(std::uint8_t value) {
    bytes_.push_back(static_cast<std::byte>(value));
  }

  void U32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      U8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void F32(float value) {
    U32(std::bit_cast<std::uint32_t>(value));
  }

  void ColorValue(Color color) {
    F32(color.red);
    F32(color.green);
    F32(color.blue);
    F32(color.alpha);
  }

  void RectValue(Rect rect) {
    F32(rect.x);
    F32(rect.y);
    F32(rect.width);
    F32(rect.height);
  }

  void PathValue(const Path& path) {
    if (path.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("SVG path contains too many operations");
    }
    U32(static_cast<std::uint32_t>(path.size()));
    for (const PathOperation& operation : path) {
      U8(operation.verb);
      for (float value : operation.values) {
        F32(value);
      }
    }
  }

  void Append(std::span<const std::byte> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  [[nodiscard]] const std::vector<std::byte>& Bytes() const noexcept {
    return bytes_;
  }

  [[nodiscard]] std::vector<std::byte> Take() && {
    return std::move(bytes_);
  }

private:
  std::vector<std::byte> bytes_;
};

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to read SVG resource: " + path.string());
  }
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

float ParseNumber(std::string_view value, std::string_view field) {
  const std::string text = Trim(value);
  if (text.empty()) {
    throw std::runtime_error("SVG " + std::string(field) + " must be a number");
  }
  char* end = nullptr;
  const float result = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(result)) {
    throw std::runtime_error("SVG " + std::string(field) + " must be a finite number");
  }
  const std::string_view suffix(end, text.c_str() + text.size() - end);
  float scale = 1.0F;
  if (suffix.empty() || suffix == "px") {
    scale = 1.0F;
  } else if (suffix == "in") {
    scale = 96.0F;
  } else if (suffix == "cm") {
    scale = 96.0F / 2.54F;
  } else if (suffix == "mm") {
    scale = 96.0F / 25.4F;
  } else if (suffix == "q") {
    scale = 96.0F / 101.6F;
  } else if (suffix == "pt") {
    scale = 96.0F / 72.0F;
  } else if (suffix == "pc") {
    scale = 16.0F;
  } else {
    throw std::runtime_error("SVG " + std::string(field) + " uses an unsupported unit");
  }
  const float scaled = result * scale;
  if (!std::isfinite(scaled)) {
    throw std::runtime_error("SVG " + std::string(field) + " must be a finite number");
  }
  return scaled;
}

std::vector<float> ParseNumberList(std::string_view value, std::string_view field) {
  std::vector<float> result;
  std::string text(value);
  const char* cursor = text.c_str();
  const char* end = cursor + text.size();
  while (cursor < end) {
    while (cursor < end && (std::isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',')) {
      ++cursor;
    }
    if (cursor == end) {
      break;
    }
    char* next = nullptr;
    const float number = std::strtof(cursor, &next);
    if (next == cursor || !std::isfinite(number)) {
      throw std::runtime_error("SVG " + std::string(field) + " contains an invalid number");
    }
    result.push_back(number);
    cursor = next;
  }
  return result;
}

std::vector<float> ParseDashPattern(std::string_view value) {
  const std::string trimmed = Trim(value);
  if (trimmed == "none") {
    return {};
  }
  if (trimmed.empty()) {
    throw std::runtime_error("SVG stroke-dasharray must contain lengths or none");
  }

  std::vector<float> result;
  const char* cursor = trimmed.c_str();
  const char* end = cursor + trimmed.size();
  while (cursor < end) {
    if (*cursor == ',') {
      throw std::runtime_error("SVG stroke-dasharray must not contain empty lengths");
    }
    const char* token_start = cursor;
    char* number_end = nullptr;
    const float unscaled_length = std::strtof(cursor, &number_end);
    if (number_end == cursor || !std::isfinite(unscaled_length)) {
      throw std::runtime_error("SVG stroke-dasharray contains an invalid length");
    }
    cursor = number_end;
    while (cursor < end && std::isalpha(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    const float length = ParseNumber(std::string_view(token_start, static_cast<std::size_t>(cursor - token_start)),
                                     "stroke-dasharray length");
    if (length < 0.0F) {
      throw std::runtime_error("SVG stroke-dasharray lengths must be non-negative");
    }
    if (cursor < end && !std::isspace(static_cast<unsigned char>(*cursor)) && *cursor != ',') {
      throw std::runtime_error("SVG stroke-dasharray lengths must be separated");
    }
    result.push_back(length);

    bool separated_by_space = false;
    while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor))) {
      separated_by_space = true;
      ++cursor;
    }
    if (cursor == end) {
      break;
    }
    if (*cursor == ',') {
      ++cursor;
      while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
      }
      if (cursor == end || *cursor == ',') {
        throw std::runtime_error("SVG stroke-dasharray must not contain empty lengths");
      }
    } else if (!separated_by_space) {
      throw std::runtime_error("SVG stroke-dasharray lengths must be separated");
    }
  }
  if (result.empty()) {
    throw std::runtime_error("SVG stroke-dasharray must contain lengths or none");
  }
  return result;
}

void ValidateDashPattern(const std::vector<float>& pattern) {
  double cycle = 0.0;
  for (const float length : pattern) {
    cycle += length;
  }
  if (pattern.size() % 2 != 0) {
    cycle *= 2.0;
  }
  if (!std::isfinite(cycle) || cycle > std::numeric_limits<float>::max()) {
    throw std::runtime_error("SVG stroke-dasharray cycle must be finite");
  }
}

float ParseOpacity(std::string_view value, std::string_view field) {
  const std::string text = Trim(value);
  float opacity = 0.0F;
  if (text.ends_with('%')) {
    opacity = ParseNumber(std::string_view(text).substr(0, text.size() - 1), field) / 100.0F;
  } else {
    opacity = ParseNumber(text, field);
  }
  if (opacity < 0.0F || opacity > 1.0F) {
    throw std::runtime_error("SVG " + std::string(field) + " must be between zero and one");
  }
  return opacity;
}

int HexDigit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

std::optional<Color> ParseColor(std::string_view value) {
  const std::string text = Trim(value);
  if (text == "none") {
    return std::nullopt;
  }
  if (text == "black") {
    return Color{};
  }
  if (text == "transparent") {
    return Color{0.0F, 0.0F, 0.0F, 0.0F};
  }
  if (text == "white") {
    return Color{1.0F, 1.0F, 1.0F, 1.0F};
  }
  if (text == "red") {
    return Color{1.0F, 0.0F, 0.0F, 1.0F};
  }
  if (text == "green") {
    return Color{0.0F, 0.5019608F, 0.0F, 1.0F};
  }
  if (text == "lime") {
    return Color{0.0F, 1.0F, 0.0F, 1.0F};
  }
  if (text == "blue") {
    return Color{0.0F, 0.0F, 1.0F, 1.0F};
  }
  if (text == "yellow") {
    return Color{1.0F, 1.0F, 0.0F, 1.0F};
  }
  if (text == "orange") {
    return Color{1.0F, 0.6470588F, 0.0F, 1.0F};
  }
  if (text == "gray" || text == "grey") {
    return Color{0.5019608F, 0.5019608F, 0.5019608F, 1.0F};
  }
  if (text == "cyan" || text == "aqua") {
    return Color{0.0F, 1.0F, 1.0F, 1.0F};
  }
  if (text == "magenta" || text == "fuchsia") {
    return Color{1.0F, 0.0F, 1.0F, 1.0F};
  }
  if (text == "maroon") {
    return Color{0.5019608F, 0.0F, 0.0F, 1.0F};
  }
  if (text == "navy") {
    return Color{0.0F, 0.0F, 0.5019608F, 1.0F};
  }
  if (text == "olive") {
    return Color{0.5019608F, 0.5019608F, 0.0F, 1.0F};
  }
  if (text == "purple") {
    return Color{0.5019608F, 0.0F, 0.5019608F, 1.0F};
  }
  if (text == "silver") {
    return Color{0.7529412F, 0.7529412F, 0.7529412F, 1.0F};
  }
  if (text == "teal") {
    return Color{0.0F, 0.5019608F, 0.5019608F, 1.0F};
  }
  if (text.size() == 4 && text.front() == '#') {
    const int red = HexDigit(text[1]);
    const int green = HexDigit(text[2]);
    const int blue = HexDigit(text[3]);
    if (red >= 0 && green >= 0 && blue >= 0) {
      return Color{
          static_cast<float>(red * 17) / 255.0F,
          static_cast<float>(green * 17) / 255.0F,
          static_cast<float>(blue * 17) / 255.0F,
          1.0F,
      };
    }
  }
  if (text.size() == 5 && text.front() == '#') {
    const int red = HexDigit(text[1]);
    const int green = HexDigit(text[2]);
    const int blue = HexDigit(text[3]);
    const int alpha = HexDigit(text[4]);
    if (red >= 0 && green >= 0 && blue >= 0 && alpha >= 0) {
      return Color{
          static_cast<float>(red * 17) / 255.0F,
          static_cast<float>(green * 17) / 255.0F,
          static_cast<float>(blue * 17) / 255.0F,
          static_cast<float>(alpha * 17) / 255.0F,
      };
    }
  }
  if ((text.size() == 7 || text.size() == 9) && text.front() == '#') {
    const auto byte = [&text](std::size_t index) {
      const int high = HexDigit(text[index]);
      const int low = HexDigit(text[index + 1]);
      return high < 0 || low < 0 ? -1 : high * 16 + low;
    };
    const int red = byte(1);
    const int green = byte(3);
    const int blue = byte(5);
    const int alpha = text.size() == 9 ? byte(7) : 255;
    if (red >= 0 && green >= 0 && blue >= 0 && alpha >= 0) {
      return Color{
          static_cast<float>(red) / 255.0F,
          static_cast<float>(green) / 255.0F,
          static_cast<float>(blue) / 255.0F,
          static_cast<float>(alpha) / 255.0F,
      };
    }
  }
  const bool rgb = text.starts_with("rgb(") && text.ends_with(')');
  const bool rgba = text.starts_with("rgba(") && text.ends_with(')');
  if (rgb || rgba) {
    std::vector<std::string> components;
    std::string_view remaining(text);
    remaining.remove_prefix(rgba ? 5 : 4);
    remaining.remove_suffix(1);
    while (true) {
      const std::size_t comma = remaining.find(',');
      components.push_back(Trim(remaining.substr(0, comma)));
      if (comma == std::string_view::npos) {
        break;
      }
      remaining.remove_prefix(comma + 1);
    }
    if (components.size() == (rgba ? 4U : 3U)) {
      const auto channel = [](std::string_view component) {
        const std::string value = Trim(component);
        const bool percentage = value.ends_with('%');
        const float parsed = ParseNumber(
            percentage ? std::string_view(value).substr(0, value.size() - 1) : std::string_view(value),
            "color channel"
        );
        return std::clamp(parsed / (percentage ? 100.0F : 255.0F), 0.0F, 1.0F);
      };
      return Color{
          channel(components[0]),
          channel(components[1]),
          channel(components[2]),
          rgba ? ParseOpacity(components[3], "color alpha") : 1.0F,
      };
    }
  }
  throw std::runtime_error("SVG contains an unsupported color: " + text);
}

Transform Compose(Transform outer, Transform inner) {
  return {
      outer.m11 * inner.m11 + outer.m21 * inner.m12,
      outer.m12 * inner.m11 + outer.m22 * inner.m12,
      outer.m11 * inner.m21 + outer.m21 * inner.m22,
      outer.m12 * inner.m21 + outer.m22 * inner.m22,
      outer.m11 * inner.tx + outer.m21 * inner.ty + outer.tx,
      outer.m12 * inner.tx + outer.m22 * inner.ty + outer.ty,
  };
}

bool IsFinite(Transform transform) {
  return std::isfinite(transform.m11) && std::isfinite(transform.m12) && std::isfinite(transform.m21) &&
         std::isfinite(transform.m22) && std::isfinite(transform.tx) && std::isfinite(transform.ty);
}

Transform ParseTransform(std::string_view value) {
  Transform result;
  std::size_t offset = 0;
  while (offset < value.size()) {
    while (offset < value.size() && (std::isspace(static_cast<unsigned char>(value[offset])) || value[offset] == ',')) {
      ++offset;
    }
    if (offset == value.size()) {
      break;
    }
    const std::size_t open = value.find('(', offset);
    const std::size_t close = open == std::string_view::npos ? std::string_view::npos : value.find(')', open + 1);
    if (open == std::string_view::npos || close == std::string_view::npos) {
      throw std::runtime_error("SVG transform is malformed");
    }
    const std::string name = Trim(value.substr(offset, open - offset));
    const std::vector<float> arguments = ParseNumberList(value.substr(open + 1, close - open - 1), "transform");
    Transform current;
    if (name == "matrix" && arguments.size() == 6) {
      current = {arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]};
    } else if (name == "translate" && (arguments.size() == 1 || arguments.size() == 2)) {
      current.tx = arguments[0];
      current.ty = arguments.size() == 2 ? arguments[1] : 0.0F;
    } else if (name == "scale" && (arguments.size() == 1 || arguments.size() == 2)) {
      current.m11 = arguments[0];
      current.m22 = arguments.size() == 2 ? arguments[1] : arguments[0];
    } else if (name == "rotate" && (arguments.size() == 1 || arguments.size() == 3)) {
      const float radians = arguments[0] * std::numbers::pi_v<float> / 180.0F;
      const float cosine = std::cos(radians);
      const float sine = std::sin(radians);
      current = {cosine, sine, -sine, cosine, 0.0F, 0.0F};
      if (arguments.size() == 3) {
        const Transform to_origin{1.0F, 0.0F, 0.0F, 1.0F, -arguments[1], -arguments[2]};
        const Transform from_origin{1.0F, 0.0F, 0.0F, 1.0F, arguments[1], arguments[2]};
        current = Compose(from_origin, Compose(current, to_origin));
      }
    } else if (name == "skewX" && arguments.size() == 1) {
      current.m21 = std::tan(arguments[0] * std::numbers::pi_v<float> / 180.0F);
    } else if (name == "skewY" && arguments.size() == 1) {
      current.m12 = std::tan(arguments[0] * std::numbers::pi_v<float> / 180.0F);
    } else {
      throw std::runtime_error("SVG contains an unsupported transform: " + name);
    }
    result = Compose(result, current);
    if (!IsFinite(result)) {
      throw std::runtime_error("SVG transform must remain finite after composition");
    }
    offset = close + 1;
  }
  return result;
}

class PathParser {
public:
  explicit PathParser(std::string value) : value_(std::move(value)) {}

  Path Parse() {
    char command = 0;
    while (SkipSeparators()) {
      if (std::isalpha(static_cast<unsigned char>(value_[offset_]))) {
        command = value_[offset_++];
      } else if (command == 0) {
        Fail();
      }
      ParseCommand(command);
      if (command == 'M') {
        command = 'L';
      } else if (command == 'm') {
        command = 'l';
      } else if (command == 'Z' || command == 'z') {
        command = 0;
      }
    }
    return std::move(path_);
  }

private:
  bool SkipSeparators() {
    while (offset_ < value_.size() &&
           (std::isspace(static_cast<unsigned char>(value_[offset_])) || value_[offset_] == ',')) {
      ++offset_;
    }
    return offset_ < value_.size();
  }

  bool HasNumber() {
    if (!SkipSeparators()) {
      return false;
    }
    const char value = value_[offset_];
    return value == '+' || value == '-' || value == '.' || std::isdigit(static_cast<unsigned char>(value));
  }

  float Number() {
    if (!HasNumber()) {
      Fail();
    }
    char* end = nullptr;
    const float result = std::strtof(value_.c_str() + offset_, &end);
    if (end == value_.c_str() + offset_ || !std::isfinite(result)) {
      Fail();
    }
    offset_ = static_cast<std::size_t>(end - value_.c_str());
    return result;
  }

  Point PointValue(bool relative) {
    Point point{Number(), Number()};
    if (relative) {
      point.x += current_.x;
      point.y += current_.y;
    }
    return point;
  }

  void Add(std::uint8_t verb, std::initializer_list<float> values = {}) {
    path_.push_back({verb, values});
  }

  void Move(Point point) {
    Add(1, {point.x, point.y});
    current_ = point;
    contour_start_ = point;
    has_current_ = true;
    last_cubic_control_.reset();
    last_quadratic_control_.reset();
  }

  void Line(Point point) {
    RequireCurrent();
    Add(2, {point.x, point.y});
    current_ = point;
    last_cubic_control_.reset();
    last_quadratic_control_.reset();
  }

  void Cubic(Point first, Point second, Point end) {
    RequireCurrent();
    Add(4, {first.x, first.y, second.x, second.y, end.x, end.y});
    current_ = end;
    last_cubic_control_ = second;
    last_quadratic_control_.reset();
  }

  void Quadratic(Point control, Point end) {
    RequireCurrent();
    Add(3, {control.x, control.y, end.x, end.y});
    current_ = end;
    last_quadratic_control_ = control;
    last_cubic_control_.reset();
  }

  static Point Reflect(Point control, Point around) {
    return {around.x * 2.0F - control.x, around.y * 2.0F - control.y};
  }

  void Arc(float rx, float ry, float rotation, bool large_arc, bool sweep, Point end) {
    RequireCurrent();
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0.0F || ry == 0.0F || (current_.x == end.x && current_.y == end.y)) {
      if (current_.x != end.x || current_.y != end.y) {
        Line(end);
      } else {
        last_cubic_control_.reset();
        last_quadratic_control_.reset();
      }
      return;
    }
    const float phi = rotation * std::numbers::pi_v<float> / 180.0F;
    const float cosine = std::cos(phi);
    const float sine = std::sin(phi);
    const float dx = (current_.x - end.x) * 0.5F;
    const float dy = (current_.y - end.y) * 0.5F;
    const float x1 = cosine * dx + sine * dy;
    const float y1 = -sine * dx + cosine * dy;
    const float radii_scale = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (radii_scale > 1.0F) {
      const float factor = std::sqrt(radii_scale);
      rx *= factor;
      ry *= factor;
    }
    const float numerator = std::max(0.0F, rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1);
    const float denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    const float factor =
        denominator == 0.0F ? 0.0F : (large_arc == sweep ? -1.0F : 1.0F) * std::sqrt(numerator / denominator);
    const float cx1 = factor * rx * y1 / ry;
    const float cy1 = factor * -ry * x1 / rx;
    const float center_x = cosine * cx1 - sine * cy1 + (current_.x + end.x) * 0.5F;
    const float center_y = sine * cx1 + cosine * cy1 + (current_.y + end.y) * 0.5F;
    const auto angle = [](float ux, float uy, float vx, float vy) {
      return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    float start = angle(1.0F, 0.0F, (x1 - cx1) / rx, (y1 - cy1) / ry);
    float delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && delta > 0.0F) {
      delta -= std::numbers::pi_v<float> * 2.0F;
    } else if (sweep && delta < 0.0F) {
      delta += std::numbers::pi_v<float> * 2.0F;
    }
    const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi_v<float> * 0.5F))));
    const float step = delta / static_cast<float>(segments);
    const auto map = [&](float x, float y) {
      return Point{
          center_x + cosine * rx * x - sine * ry * y,
          center_y + sine * rx * x + cosine * ry * y,
      };
    };
    for (int segment = 0; segment < segments; ++segment) {
      const float next = start + step;
      const float alpha = 4.0F / 3.0F * std::tan(step * 0.25F);
      const Point first = map(std::cos(start) - alpha * std::sin(start), std::sin(start) + alpha * std::cos(start));
      const Point second = map(std::cos(next) + alpha * std::sin(next), std::sin(next) - alpha * std::cos(next));
      const Point endpoint = segment + 1 == segments ? end : map(std::cos(next), std::sin(next));
      Cubic(first, second, endpoint);
      start = next;
    }
    last_cubic_control_.reset();
    last_quadratic_control_.reset();
  }

  void ParseCommand(char command) {
    const bool relative = std::islower(static_cast<unsigned char>(command));
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(command)))) {
    case 'M':
      Move(PointValue(relative));
      break;
    case 'L':
      Line(PointValue(relative));
      break;
    case 'H': {
      float x = Number();
      if (relative) {
        x += current_.x;
      }
      Line({x, current_.y});
      break;
    }
    case 'V': {
      float y = Number();
      if (relative) {
        y += current_.y;
      }
      Line({current_.x, y});
      break;
    }
    case 'C': {
      const Point first = PointValue(relative);
      const Point second = PointValue(relative);
      Cubic(first, second, PointValue(relative));
      break;
    }
    case 'S': {
      const Point first = last_cubic_control_.has_value() ? Reflect(*last_cubic_control_, current_) : current_;
      const Point second = PointValue(relative);
      Cubic(first, second, PointValue(relative));
      break;
    }
    case 'Q': {
      const Point control = PointValue(relative);
      Quadratic(control, PointValue(relative));
      break;
    }
    case 'T': {
      const Point control =
          last_quadratic_control_.has_value() ? Reflect(*last_quadratic_control_, current_) : current_;
      Quadratic(control, PointValue(relative));
      break;
    }
    case 'A': {
      const float rx = Number();
      const float ry = Number();
      const float rotation = Number();
      const float large_arc = Number();
      const float sweep = Number();
      if ((large_arc != 0.0F && large_arc != 1.0F) || (sweep != 0.0F && sweep != 1.0F)) {
        Fail();
      }
      Arc(rx, ry, rotation, large_arc != 0.0F, sweep != 0.0F, PointValue(relative));
      break;
    }
    case 'Z':
      RequireCurrent();
      Add(5);
      current_ = contour_start_;
      has_current_ = true;
      last_cubic_control_.reset();
      last_quadratic_control_.reset();
      break;
    default:
      throw std::runtime_error(std::string("SVG path contains an unsupported command: ") + command);
    }
  }

  void RequireCurrent() {
    if (!has_current_) {
      Fail();
    }
  }

  [[noreturn]] void Fail() const {
    throw std::runtime_error("SVG path data is malformed near byte " + std::to_string(offset_));
  }

  std::string value_;
  std::size_t offset_ = 0;
  Path path_;
  Point current_;
  Point contour_start_;
  std::optional<Point> last_cubic_control_;
  std::optional<Point> last_quadratic_control_;
  bool has_current_ = false;
};

void AppendUtf8(std::string& output, std::uint32_t value) {
  if (value <= 0x7FU) {
    output.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0x10FFFFU) {
    output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    throw std::runtime_error("SVG attribute contains an invalid character reference");
  }
}

std::string DecodeXml(std::string_view value) {
  std::string result;
  while (!value.empty()) {
    const std::size_t ampersand = value.find('&');
    result.append(value.substr(0, ampersand));
    if (ampersand == std::string_view::npos) {
      break;
    }
    value.remove_prefix(ampersand + 1);
    const std::size_t semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
      throw std::runtime_error("SVG attribute contains an unterminated entity reference");
    }
    const std::string entity(value.substr(0, semicolon));
    value.remove_prefix(semicolon + 1);
    if (entity == "amp") {
      result.push_back('&');
    } else if (entity == "lt") {
      result.push_back('<');
    } else if (entity == "gt") {
      result.push_back('>');
    } else if (entity == "quot") {
      result.push_back('"');
    } else if (entity == "apos") {
      result.push_back('\'');
    } else if (entity.starts_with("#")) {
      const bool hexadecimal = entity.starts_with("#x") || entity.starts_with("#X");
      const std::string digits = entity.substr(hexadecimal ? 2 : 1);
      if (digits.empty()) {
        throw std::runtime_error("SVG attribute contains an invalid character reference");
      }
      std::size_t consumed = 0;
      unsigned long code_point = 0;
      try {
        code_point = std::stoul(digits, &consumed, hexadecimal ? 16 : 10);
      } catch (const std::exception&) {
        throw std::runtime_error("SVG attribute contains an invalid character reference");
      }
      if (consumed != digits.size() || code_point == 0 || code_point > 0x10FFFFUL ||
          (code_point >= 0xD800UL && code_point <= 0xDFFFUL)) {
        throw std::runtime_error("SVG attribute contains an invalid character reference");
      }
      AppendUtf8(result, static_cast<std::uint32_t>(code_point));
    } else {
      throw std::runtime_error("SVG attribute contains an unsupported entity reference: &" + entity + ";");
    }
  }
  return result;
}

std::map<std::string, std::string> ParseAttributes(std::string_view value) {
  std::map<std::string, std::string> attributes;
  std::size_t offset = 0;
  while (offset < value.size()) {
    while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset]))) {
      ++offset;
    }
    if (offset == value.size()) {
      break;
    }
    const std::size_t name_start = offset;
    while (offset < value.size() && !std::isspace(static_cast<unsigned char>(value[offset])) && value[offset] != '=') {
      ++offset;
    }
    const std::string name(value.substr(name_start, offset - name_start));
    while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset]))) {
      ++offset;
    }
    if (name.empty() || offset == value.size() || value[offset++] != '=') {
      throw std::runtime_error("SVG element contains a malformed attribute");
    }
    while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset]))) {
      ++offset;
    }
    if (offset == value.size() || (value[offset] != '\'' && value[offset] != '"')) {
      throw std::runtime_error("SVG attribute values must be quoted");
    }
    const char quote = value[offset++];
    const std::size_t end = value.find(quote, offset);
    if (end == std::string_view::npos) {
      throw std::runtime_error("SVG attribute value is unterminated");
    }
    if (!attributes.emplace(name, DecodeXml(value.substr(offset, end - offset))).second) {
      throw std::runtime_error("SVG element contains a duplicate attribute: " + name);
    }
    offset = end + 1;
  }
  return attributes;
}

bool IsStyleProperty(std::string_view name) {
  return name == "color" || name == "fill" || name == "stroke" || name == "fill-opacity" ||
         name == "stroke-opacity" || name == "stroke-width" || name == "fill-rule" || name == "clip-rule" ||
         name == "stroke-linecap" || name == "stroke-linejoin" || name == "stroke-miterlimit" ||
         name == "stroke-dasharray" || name == "stroke-dashoffset" || name == "display" || name == "visibility" ||
         name == "opacity" || name == "clip-path" || name == "stop-color" || name == "stop-opacity";
}

std::optional<std::string> ParseLocalReference(std::string_view value, std::string_view field) {
  const std::string text = Trim(value);
  if (text == "none") {
    return std::nullopt;
  }
  if (text.starts_with("url(#") && text.ends_with(')') && text.size() > 6) {
    return text.substr(5, text.size() - 6);
  }
  throw std::runtime_error("SVG " + std::string(field) + " must be a file-local reference");
}

void ApplyStyleProperty(Style& style, std::string_view name, std::string_view value) {
  const std::string text = Trim(value);
  if (name == "color") {
    if (text != "currentColor") {
      const std::optional<Color> color = ParseColor(text);
      if (!color.has_value()) {
        throw std::runtime_error("SVG color does not accept none");
      }
      style.current_color = *color;
    }
  } else if (name == "fill" || name == "stroke") {
    if (text.starts_with("url(")) {
      if (name == "stroke") {
        throw std::runtime_error("SVG gradient strokes are not supported");
      }
      style.fill_reference = ParseLocalReference(text, name);
      style.fill.reset();
      style.fill_uses_current_color = false;
      return;
    }
    const bool current = text == "currentColor";
    std::optional<Color>& paint = name == "fill" ? style.fill : style.stroke;
    bool& uses_current = name == "fill" ? style.fill_uses_current_color : style.stroke_uses_current_color;
    paint = current ? std::optional<Color>(style.current_color) : ParseColor(text);
    uses_current = current;
    if (name == "fill") {
      style.fill_reference.reset();
    }
  } else if (name == "fill-opacity") {
    style.fill_opacity = ParseOpacity(text, name);
  } else if (name == "stop-color") {
    style.stop_uses_current_color = text == "currentColor";
    if (!style.stop_uses_current_color) {
      const std::optional<Color> color = ParseColor(text);
      if (!color.has_value()) {
        throw std::runtime_error("SVG stop-color does not accept none");
      }
      style.stop_color = *color;
    }
  } else if (name == "stop-opacity") {
    style.stop_opacity = ParseOpacity(text, name);
  } else if (name == "stroke-opacity") {
    style.stroke_opacity = ParseOpacity(text, name);
  } else if (name == "stroke-width") {
    style.stroke_width = ParseNumber(text, name);
    if (style.stroke_width < 0.0F) {
      throw std::runtime_error("SVG stroke-width must be non-negative");
    }
  } else if (name == "fill-rule" || name == "clip-rule") {
    std::uint8_t& rule = name == "fill-rule" ? style.fill_rule : style.clip_rule;
    if (text == "nonzero") {
      rule = 0;
    } else if (text == "evenodd") {
      rule = 1;
    } else {
      throw std::runtime_error("SVG contains an unsupported " + std::string(name));
    }
  } else if (name == "stroke-linecap") {
    if (text == "butt") {
      style.stroke_cap = 0;
    } else if (text == "round") {
      style.stroke_cap = 1;
    } else if (text == "square") {
      style.stroke_cap = 2;
    } else {
      throw std::runtime_error("SVG contains an unsupported stroke line cap");
    }
  } else if (name == "stroke-linejoin") {
    if (text == "miter") {
      style.stroke_join = 0;
    } else if (text == "round") {
      style.stroke_join = 1;
    } else if (text == "bevel") {
      style.stroke_join = 2;
    } else {
      throw std::runtime_error("SVG contains an unsupported stroke line join");
    }
  } else if (name == "stroke-miterlimit") {
    style.miter_limit = ParseNumber(text, name);
    if (style.miter_limit < 1.0F) {
      throw std::runtime_error("SVG stroke-miterlimit must be at least one");
    }
  } else if (name == "stroke-dasharray") {
    style.dash_pattern = ParseDashPattern(text);
  } else if (name == "stroke-dashoffset") {
    style.dash_offset = ParseNumber(text, name);
  } else if (name == "display") {
    if (text == "none") {
      style.displayed = false;
    } else if (text != "inline") {
      throw std::runtime_error("SVG contains an unsupported display value");
    }
  } else if (name == "visibility") {
    if (text == "visible") {
      style.visible = true;
    } else if (text == "hidden" || text == "collapse") {
      style.visible = false;
    } else {
      throw std::runtime_error("SVG contains an unsupported visibility value");
    }
  } else if (name == "opacity") {
    const float opacity = ParseOpacity(text, name);
    if (opacity != 0.0F && opacity != 1.0F) {
      throw std::runtime_error("SVG opacity must be exactly zero or one");
    }
    style.opacity *= opacity;
  } else if (name == "clip-path") {
    style.clip_reference = ParseLocalReference(text, name);
  } else {
    throw std::runtime_error("SVG contains an unsupported style property: " + std::string(name));
  }
}

Style ResolveStyle(Style inherited, const std::map<std::string, std::string>& attributes) {
  inherited.clip_reference.reset();
  for (const auto& [name, value] : attributes) {
    if (IsStyleProperty(name)) {
      ApplyStyleProperty(inherited, name, value);
    }
  }
  if (const auto style_attribute = attributes.find("style"); style_attribute != attributes.end()) {
    std::string_view value = style_attribute->second;
    while (!value.empty()) {
      const std::size_t separator = value.find(';');
      const std::string_view item = value.substr(0, separator);
      const std::string trimmed_item = Trim(item);
      if (trimmed_item.empty()) {
        if (separator == std::string_view::npos) {
          break;
        }
        value.remove_prefix(separator + 1);
        continue;
      }
      const std::size_t colon = trimmed_item.find(':');
      if (colon == std::string_view::npos) {
        throw std::runtime_error("SVG style attribute is malformed");
      }
      ApplyStyleProperty(
          inherited,
          Trim(std::string_view(trimmed_item).substr(0, colon)),
          Trim(std::string_view(trimmed_item).substr(colon + 1))
      );
      if (separator == std::string_view::npos) {
        break;
      }
      value.remove_prefix(separator + 1);
    }
  }
  return inherited;
}

bool IsShape(std::string_view name) {
  return name == "path" || name == "rect" || name == "circle" || name == "ellipse" || name == "line" ||
         name == "polyline" || name == "polygon";
}

void ValidateAttributes(std::string_view element, const std::map<std::string, std::string>& attributes) {
  static const std::vector<std::string_view> common{
      "id",
      "xmlns",
      "xmlns:xlink",
      "color",
      "fill",
      "stroke",
      "fill-opacity",
      "stroke-opacity",
      "stroke-width",
      "fill-rule",
      "clip-rule",
      "stroke-linecap",
      "stroke-linejoin",
      "stroke-miterlimit",
      "stroke-dasharray",
      "stroke-dashoffset",
      "display",
      "visibility",
      "opacity",
      "clip-path",
      "style",
      "transform",
  };
  const auto allowed_for_element = [element](std::string_view name) {
    if (element == "svg") {
      return name == "viewBox" || name == "width" || name == "height" || name == "version" ||
             name == "preserveAspectRatio";
    }
    if (element == "use") {
      return name == "href" || name == "xlink:href" || name == "x" || name == "y";
    }
    if (element == "clipPath") {
      return name == "clipPathUnits";
    }
    if (element == "linearGradient") {
      return name == "x1" || name == "y1" || name == "x2" || name == "y2" || name == "gradientUnits" ||
             name == "gradientTransform" || name == "spreadMethod" || name == "href" || name == "xlink:href";
    }
    if (element == "radialGradient") {
      return name == "cx" || name == "cy" || name == "r" || name == "fx" || name == "fy" || name == "fr" ||
             name == "gradientUnits" || name == "gradientTransform" || name == "spreadMethod" || name == "href" ||
             name == "xlink:href";
    }
    if (element == "stop") {
      return name == "offset" || name == "stop-color" || name == "stop-opacity";
    }
    if (element == "path") {
      return name == "d";
    }
    if (element == "rect") {
      return name == "x" || name == "y" || name == "width" || name == "height" || name == "rx" || name == "ry";
    }
    if (element == "circle") {
      return name == "cx" || name == "cy" || name == "r";
    }
    if (element == "ellipse") {
      return name == "cx" || name == "cy" || name == "rx" || name == "ry";
    }
    if (element == "line") {
      return name == "x1" || name == "y1" || name == "x2" || name == "y2";
    }
    if (element == "polyline" || element == "polygon") {
      return name == "points";
    }
    return false;
  };
  for (const auto& [name, unused] : attributes) {
    static_cast<void>(unused);
    if (std::ranges::find(common, name) == common.end() && !allowed_for_element(name) && !name.starts_with("aria-") &&
        !name.starts_with("data-") && name != "role") {
      throw std::runtime_error("SVG " + std::string(element) + " contains an unsupported attribute: " + name);
    }
  }
}

Path RectPath(float x, float y, float width, float height, float rx, float ry) {
  if (width < 0.0F || height < 0.0F || rx < 0.0F || ry < 0.0F) {
    throw std::runtime_error("SVG rect dimensions and radii must be non-negative");
  }
  rx = std::min(rx, width * 0.5F);
  ry = std::min(ry, height * 0.5F);
  if (rx == 0.0F || ry == 0.0F) {
    return {
        {1, {x, y}},
        {2, {x + width, y}},
        {2, {x + width, y + height}},
        {2, {x, y + height}},
        {5, {}},
    };
  }
  constexpr float kappa = 0.552284749831F;
  return {
      {1, {x + rx, y}},
      {2, {x + width - rx, y}},
      {4, {x + width - rx + rx * kappa, y, x + width, y + ry - ry * kappa, x + width, y + ry}},
      {2, {x + width, y + height - ry}},
      {4,
       {x + width, y + height - ry + ry * kappa, x + width - rx + rx * kappa, y + height, x + width - rx, y + height}},
      {2, {x + rx, y + height}},
      {4, {x + rx - rx * kappa, y + height, x, y + height - ry + ry * kappa, x, y + height - ry}},
      {2, {x, y + ry}},
      {4, {x, y + ry - ry * kappa, x + rx - rx * kappa, y, x + rx, y}},
      {5, {}},
  };
}

Path EllipsePath(float cx, float cy, float rx, float ry) {
  if (rx < 0.0F || ry < 0.0F) {
    throw std::runtime_error("SVG ellipse radii must be non-negative");
  }
  constexpr float kappa = 0.552284749831F;
  return {
      {1, {cx + rx, cy}},
      {4, {cx + rx, cy + ry * kappa, cx + rx * kappa, cy + ry, cx, cy + ry}},
      {4, {cx - rx * kappa, cy + ry, cx - rx, cy + ry * kappa, cx - rx, cy}},
      {4, {cx - rx, cy - ry * kappa, cx - rx * kappa, cy - ry, cx, cy - ry}},
      {4, {cx + rx * kappa, cy - ry, cx + rx, cy - ry * kappa, cx + rx, cy}},
      {5, {}},
  };
}

float AttributeNumber(
    const std::map<std::string, std::string>& attributes, std::string_view name, float default_value = 0.0F
) {
  const auto found = attributes.find(std::string(name));
  return found == attributes.end() ? default_value : ParseNumber(found->second, name);
}

Path ShapePath(std::string_view name, const std::map<std::string, std::string>& attributes) {
  if (name == "path") {
    const auto data = attributes.find("d");
    if (data == attributes.end()) {
      throw std::runtime_error("SVG path requires a d attribute");
    }
    return PathParser(data->second).Parse();
  }
  if (name == "rect") {
    const float width = AttributeNumber(attributes, "width");
    const float height = AttributeNumber(attributes, "height");
    const bool has_rx = attributes.contains("rx");
    const bool has_ry = attributes.contains("ry");
    const float rx = has_rx ? AttributeNumber(attributes, "rx") : has_ry ? AttributeNumber(attributes, "ry") : 0.0F;
    const float ry = has_ry ? AttributeNumber(attributes, "ry") : rx;
    return RectPath(AttributeNumber(attributes, "x"), AttributeNumber(attributes, "y"), width, height, rx, ry);
  }
  if (name == "circle") {
    const float radius = AttributeNumber(attributes, "r");
    return EllipsePath(AttributeNumber(attributes, "cx"), AttributeNumber(attributes, "cy"), radius, radius);
  }
  if (name == "ellipse") {
    return EllipsePath(
        AttributeNumber(attributes, "cx"),
        AttributeNumber(attributes, "cy"),
        AttributeNumber(attributes, "rx"),
        AttributeNumber(attributes, "ry")
    );
  }
  if (name == "line") {
    return {
        {1, {AttributeNumber(attributes, "x1"), AttributeNumber(attributes, "y1")}},
        {2, {AttributeNumber(attributes, "x2"), AttributeNumber(attributes, "y2")}},
    };
  }
  if (name == "polyline" || name == "polygon") {
    const auto points = attributes.find("points");
    if (points == attributes.end()) {
      throw std::runtime_error("SVG " + std::string(name) + " requires a points attribute");
    }
    const std::vector<float> values = ParseNumberList(points->second, "points");
    if (values.size() < 4 || values.size() % 2 != 0) {
      throw std::runtime_error("SVG points must contain coordinate pairs");
    }
    Path path{{1, {values[0], values[1]}}};
    for (std::size_t index = 2; index < values.size(); index += 2) {
      path.push_back({2, {values[index], values[index + 1]}});
    }
    if (name == "polygon") {
      path.push_back({5, {}});
    }
    return path;
  }
  return {};
}

void WriteTransform(Writer& writer, Transform transform) {
  if (!IsFinite(transform)) {
    throw std::runtime_error("SVG transform must be finite");
  }
  writer.U8(5);
  writer.F32(transform.m11);
  writer.F32(transform.m12);
  writer.F32(transform.m21);
  writer.F32(transform.m22);
  writer.F32(transform.tx);
  writer.F32(transform.ty);
}

Point TransformPoint(Transform transform, Point point) {
  return {
      transform.m11 * point.x + transform.m21 * point.y + transform.tx,
      transform.m12 * point.x + transform.m22 * point.y + transform.ty,
  };
}

Path TransformPath(Path path, Transform transform) {
  for (PathOperation& operation : path) {
    for (std::size_t index = 0; index + 1 < operation.values.size(); index += 2) {
      const Point point = TransformPoint(transform, {operation.values[index], operation.values[index + 1]});
      if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        throw std::runtime_error("SVG transformed path coordinates must remain finite");
      }
      operation.values[index] = point.x;
      operation.values[index + 1] = point.y;
    }
  }
  return path;
}

float QuadraticValue(float start, float control, float end, float time) {
  const float inverse = 1.0F - time;
  return inverse * inverse * start + 2.0F * inverse * time * control + time * time * end;
}

float CubicValue(float start, float first_control, float second_control, float end, float time) {
  const float inverse = 1.0F - time;
  return inverse * inverse * inverse * start + 3.0F * inverse * inverse * time * first_control +
         3.0F * inverse * time * time * second_control + time * time * time * end;
}

template <class Include> void IncludeQuadraticExtrema(float start, float control, float end, Include&& include) {
  const float denominator = start - 2.0F * control + end;
  if (std::abs(denominator) <= 0.000001F) {
    return;
  }
  const float time = (start - control) / denominator;
  if (time > 0.0F && time < 1.0F) {
    include(time);
  }
}

template <class Include>
void IncludeCubicExtrema(float start, float first_control, float second_control, float end, Include&& include) {
  const float a = -start + 3.0F * first_control - 3.0F * second_control + end;
  const float b = 2.0F * (start - 2.0F * first_control + second_control);
  const float c = first_control - start;
  if (std::abs(a) <= 0.000001F) {
    if (std::abs(b) > 0.000001F) {
      const float time = -c / b;
      if (time > 0.0F && time < 1.0F) {
        include(time);
      }
    }
    return;
  }
  const float discriminant = b * b - 4.0F * a * c;
  if (discriminant < 0.0F) {
    return;
  }
  const float root = std::sqrt(discriminant);
  const float first_time = (-b + root) / (2.0F * a);
  const float second_time = (-b - root) / (2.0F * a);
  if (first_time > 0.0F && first_time < 1.0F) {
    include(first_time);
  }
  if (second_time > 0.0F && second_time < 1.0F && second_time != first_time) {
    include(second_time);
  }
}

Rect PathBounds(const Path& path) {
  bool has_bounds = false;
  float minimum_x = 0.0F;
  float minimum_y = 0.0F;
  float maximum_x = 0.0F;
  float maximum_y = 0.0F;
  Point current;
  Point contour_start;
  bool has_current = false;
  auto include = [&](Point point) {
    if (!has_bounds) {
      minimum_x = maximum_x = point.x;
      minimum_y = maximum_y = point.y;
      has_bounds = true;
      return;
    }
    minimum_x = std::min(minimum_x, point.x);
    minimum_y = std::min(minimum_y, point.y);
    maximum_x = std::max(maximum_x, point.x);
    maximum_y = std::max(maximum_y, point.y);
  };
  for (const PathOperation& operation : path) {
    switch (operation.verb) {
    case 1:
      current = {operation.values[0], operation.values[1]};
      contour_start = current;
      has_current = true;
      break;
    case 2: {
      const Point end{operation.values[0], operation.values[1]};
      include(current);
      include(end);
      current = end;
      break;
    }
    case 3: {
      const Point control{operation.values[0], operation.values[1]};
      const Point end{operation.values[2], operation.values[3]};
      include(current);
      include(end);
      IncludeQuadraticExtrema(current.x, control.x, end.x, [&](float time) {
        include({QuadraticValue(current.x, control.x, end.x, time),
                 QuadraticValue(current.y, control.y, end.y, time)});
      });
      IncludeQuadraticExtrema(current.y, control.y, end.y, [&](float time) {
        include({QuadraticValue(current.x, control.x, end.x, time),
                 QuadraticValue(current.y, control.y, end.y, time)});
      });
      current = end;
      break;
    }
    case 4: {
      const Point first{operation.values[0], operation.values[1]};
      const Point second{operation.values[2], operation.values[3]};
      const Point end{operation.values[4], operation.values[5]};
      include(current);
      include(end);
      IncludeCubicExtrema(current.x, first.x, second.x, end.x, [&](float time) {
        include({CubicValue(current.x, first.x, second.x, end.x, time),
                 CubicValue(current.y, first.y, second.y, end.y, time)});
      });
      IncludeCubicExtrema(current.y, first.y, second.y, end.y, [&](float time) {
        include({CubicValue(current.x, first.x, second.x, end.x, time),
                 CubicValue(current.y, first.y, second.y, end.y, time)});
      });
      current = end;
      break;
    }
    case 5:
      if (has_current && (current.x != contour_start.x || current.y != contour_start.y)) {
        include(current);
        include(contour_start);
      }
      current = contour_start;
      break;
    default:
      break;
    }
  }
  return has_bounds ? Rect{minimum_x, minimum_y, maximum_x - minimum_x, maximum_y - minimum_y} : Rect{};
}

struct GradientLength {
  float value = 0.0F;
  bool percentage = false;
};

GradientLength ParseGradientLength(std::string_view value, std::string_view field) {
  const std::string text = Trim(value);
  if (text.ends_with('%')) {
    return {ParseNumber(std::string_view(text).substr(0, text.size() - 1), field) / 100.0F, true};
  }
  return {ParseNumber(text, field), false};
}

float ResolveGradientCoordinate(GradientLength value, float origin, float extent, bool object_bounding_box) {
  if (object_bounding_box) {
    return value.value;
  }
  const float absolute = value.percentage ? origin + value.value * extent : value.value;
  return (absolute - origin) / extent;
}

float ResolveGradientRadius(GradientLength value, Rect coordinate_rect, bool horizontal, bool object_bounding_box) {
  if (object_bounding_box) {
    return value.value;
  }
  const float absolute = value.percentage
                             ? value.value * std::hypot(coordinate_rect.width, coordinate_rect.height) /
                                   std::numbers::sqrt2_v<float>
                             : value.value;
  return absolute / (horizontal ? coordinate_rect.width : coordinate_rect.height);
}

struct ResolvedGradient {
  bool radial = false;
  Rect coordinate_rect;
  Point first;
  Point second;
  std::vector<std::pair<float, Color>> stops;
};

struct GradientDefinition {
  bool radial = false;
  bool object_bounding_box = true;
  GradientLength first_x;
  GradientLength first_y;
  GradientLength second_x;
  GradientLength second_y;
  std::optional<GradientLength> focal_x;
  std::optional<GradientLength> focal_y;
  std::optional<GradientLength> focal_radius;
  std::vector<std::pair<float, Color>> stops;
};

void WriteGradientStops(Writer& writer, const std::vector<std::pair<float, Color>>& stops) {
  if (stops.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("SVG gradient contains too many stops");
  }
  writer.U32(static_cast<std::uint32_t>(stops.size()));
  for (const auto& [offset, color] : stops) {
    writer.F32(offset);
    writer.ColorValue(color);
  }
}

template <class ResolveGradient>
void WriteShape(Writer& writer, const Path& path, Style style, std::uint32_t& operation_count,
                const ResolveGradient& resolve_gradient) {
  if (path.empty() || !style.visible) {
    return;
  }
  if (style.fill.has_value()) {
    Color color = style.fill_uses_current_color ? style.current_color : *style.fill;
    color.alpha *= style.fill_opacity;
    writer.U8(1);
    writer.ColorValue(color);
    writer.U8(style.fill_rule);
    writer.PathValue(path);
    ++operation_count;
  } else if (style.fill_reference.has_value()) {
    ResolvedGradient gradient = resolve_gradient(*style.fill_reference, PathBounds(path));
    if (gradient.coordinate_rect.width > 0.0F && gradient.coordinate_rect.height > 0.0F) {
      for (auto& stop : gradient.stops) {
        stop.second.alpha *= style.fill_opacity;
      }
      writer.U8(gradient.radial ? 8 : 7);
      writer.U8(style.fill_rule);
      writer.RectValue(gradient.coordinate_rect);
      writer.F32(gradient.first.x);
      writer.F32(gradient.first.y);
      writer.F32(gradient.second.x);
      writer.F32(gradient.second.y);
      WriteGradientStops(writer, gradient.stops);
      writer.PathValue(path);
      ++operation_count;
    }
  }
  if (style.stroke.has_value() && style.stroke_width > 0.0F) {
    ValidateDashPattern(style.dash_pattern);
    if (style.dash_pattern.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("SVG stroke-dasharray contains too many lengths");
    }
    Color color = style.stroke_uses_current_color ? style.current_color : *style.stroke;
    color.alpha *= style.stroke_opacity;
    writer.U8(2);
    writer.ColorValue(color);
    writer.F32(style.stroke_width);
    writer.U8(style.stroke_cap);
    writer.U8(style.stroke_join);
    writer.F32(style.miter_limit);
    writer.U32(static_cast<std::uint32_t>(style.dash_pattern.size()));
    for (const float length : style.dash_pattern) {
      writer.F32(length);
    }
    writer.F32(style.dash_offset);
    writer.PathValue(path);
    ++operation_count;
  }
}

struct CompiledDocument {
  Writer operations;
  std::uint32_t operation_count = 0;
  float intrinsic_width = 0.0F;
  float intrinsic_height = 0.0F;
  float view_x = 0.0F;
  float view_y = 0.0F;
  float view_width = 0.0F;
  float view_height = 0.0F;
};

SvgDocument ParseSvgDocument(std::string_view xml) {
  if (xml.find("<!DOCTYPE") != std::string_view::npos || xml.find("<!ENTITY") != std::string_view::npos) {
    throw std::runtime_error("SVG external entities and document types are not supported");
  }
  constexpr std::size_t max_nesting = 256;
  constexpr std::size_t max_nodes = 1'000'000;
  SvgDocument document;
  std::vector<std::size_t> stack;
  std::size_t offset = 0;
  bool root_seen = false;
  while (offset < xml.size()) {
    const std::size_t open = xml.find('<', offset);
    if (open == std::string_view::npos) {
      if (!Trim(xml.substr(offset)).empty()) {
        throw std::runtime_error("SVG text nodes are not supported");
      }
      break;
    }
    const bool metadata_text = !stack.empty() &&
                               (document.nodes[stack.back()].name == "title" ||
                                document.nodes[stack.back()].name == "desc" ||
                                document.nodes[stack.back()].name == "metadata");
    if (!Trim(xml.substr(offset, open - offset)).empty() && !metadata_text) {
      throw std::runtime_error("SVG text nodes are not supported");
    }
    if (xml.substr(open).starts_with("<!--")) {
      const std::size_t end = xml.find("-->", open + 4);
      if (end == std::string_view::npos) {
        throw std::runtime_error("SVG comment is unterminated");
      }
      offset = end + 3;
      continue;
    }
    if (xml.substr(open).starts_with("<?")) {
      const std::size_t end = xml.find("?>", open + 2);
      if (end == std::string_view::npos) {
        throw std::runtime_error("SVG processing instruction is unterminated");
      }
      offset = end + 2;
      continue;
    }
    std::size_t close = open + 1;
    char quote = 0;
    for (; close < xml.size(); ++close) {
      const char character = xml[close];
      if (quote != 0) {
        if (character == quote) {
          quote = 0;
        }
      } else if (character == '\'' || character == '"') {
        quote = character;
      } else if (character == '>') {
        break;
      }
    }
    if (close == xml.size()) {
      throw std::runtime_error("SVG element is unterminated");
    }
    std::string tag = Trim(xml.substr(open + 1, close - open - 1));
    const bool closing = !tag.empty() && tag.front() == '/';
    const bool self_closing = !closing && !tag.empty() && tag.back() == '/';
    if (closing) {
      tag = Trim(std::string_view(tag).substr(1));
      if (stack.empty() || document.nodes[stack.back()].name != tag) {
        throw std::runtime_error("SVG closing element does not match its opening element: " + tag);
      }
      stack.pop_back();
      offset = close + 1;
      continue;
    }
    if (self_closing) {
      tag = Trim(std::string_view(tag).substr(0, tag.size() - 1));
    }
    const std::size_t separator = tag.find_first_of(" \t\r\n");
    const std::string name = tag.substr(0, separator);
    const auto attributes =
        ParseAttributes(separator == std::string::npos ? std::string_view{} : std::string_view(tag).substr(separator));
    if (name != "svg" && stack.empty()) {
      throw std::runtime_error("SVG elements must be inside the root svg element");
    }
    if (name != "svg" && name != "g" && name != "defs" && name != "use" && name != "clipPath" &&
        name != "linearGradient" && name != "radialGradient" && name != "stop" && name != "title" &&
        name != "desc" && name != "metadata" && !IsShape(name)) {
      throw std::runtime_error("SVG contains an unsupported element: " + name);
    }
    ValidateAttributes(name, attributes);
    const std::size_t node_index = document.nodes.size();
    if (node_index >= max_nodes) {
      throw std::runtime_error("SVG contains too many elements");
    }
    document.nodes.push_back({name, attributes, {}, std::nullopt, stack.empty() ? std::nullopt
                                                                               : std::optional(stack.back())});
    if (name == "svg") {
      if (root_seen || !stack.empty()) {
        throw std::runtime_error("SVG must contain exactly one root svg element");
      }
      root_seen = true;
      document.root = node_index;
    } else {
      document.nodes[stack.back()].children.push_back(node_index);
    }
    if (const auto id = attributes.find("id"); id != attributes.end()) {
      if (id->second.empty()) {
        throw std::runtime_error("SVG id must not be empty");
      }
      if (!document.ids.emplace(id->second, node_index).second) {
        throw std::runtime_error("SVG contains a duplicate id: " + id->second);
      }
    }
    if (!self_closing) {
      stack.push_back(node_index);
      if (stack.size() > max_nesting) {
        throw std::runtime_error("SVG element nesting exceeds the supported limit");
      }
    }
    offset = close + 1;
  }
  if (!root_seen || !stack.empty()) {
    throw std::runtime_error("SVG document is incomplete");
  }
  return document;
}

CompiledDocument EmitDocument(SvgDocument document) {
  const SvgNode& root = document.nodes[document.root];
  CompiledDocument result;
  if (const auto view_box = root.attributes.find("viewBox"); view_box != root.attributes.end()) {
    const std::vector<float> values = ParseNumberList(view_box->second, "viewBox");
    if (values.size() != 4 || values[2] <= 0.0F || values[3] <= 0.0F) {
      throw std::runtime_error("SVG viewBox must contain four values with positive dimensions");
    }
    result.view_x = values[0];
    result.view_y = values[1];
    result.view_width = values[2];
    result.view_height = values[3];
  }
  result.intrinsic_width =
      root.attributes.contains("width") ? ParseNumber(root.attributes.at("width"), "width") : result.view_width;
  result.intrinsic_height =
      root.attributes.contains("height") ? ParseNumber(root.attributes.at("height"), "height") : result.view_height;
  if (result.view_width == 0.0F || result.view_height == 0.0F) {
    result.view_width = result.intrinsic_width;
    result.view_height = result.intrinsic_height;
  }
  if (result.intrinsic_width <= 0.0F || result.intrinsic_height <= 0.0F) {
    throw std::runtime_error("SVG requires positive intrinsic width and height or a viewBox");
  }

  constexpr std::uint32_t max_operations = 1'000'000;
  constexpr std::size_t max_reference_depth = 64;
  std::vector<std::size_t> references;
  const auto count_operation = [&] {
    if (++result.operation_count > max_operations) {
      throw std::runtime_error("SVG produces too many vector operations");
    }
  };
  const auto push_transform = [&](Transform transform) {
    WriteTransform(result.operations, transform);
    count_operation();
  };
  const auto pop_transform = [&] {
    result.operations.U8(6);
    count_operation();
  };
  const auto find_reference = [&](std::string_view id, std::string_view field) {
    const auto found = document.ids.find(std::string(id));
    if (found == document.ids.end()) {
      throw std::runtime_error("SVG " + std::string(field) + " references a missing id: " + std::string(id));
    }
    return found->second;
  };
  const auto href = [](const SvgNode& node) -> std::string {
    const auto direct = node.attributes.find("href");
    const auto legacy = node.attributes.find("xlink:href");
    if (direct != node.attributes.end() && legacy != node.attributes.end()) {
      throw std::runtime_error("SVG use must not declare both href and xlink:href");
    }
    const auto& found = direct != node.attributes.end() ? direct : legacy;
    if (found == node.attributes.end() || !found->second.starts_with('#') || found->second.size() == 1) {
      throw std::runtime_error("SVG use href must be a file-local reference");
    }
    return found->second.substr(1);
  };
  const auto node_transform = [](const SvgNode& node) {
    const auto found = node.attributes.find("transform");
    return found == node.attributes.end() ? Transform{} : ParseTransform(found->second);
  };

  const auto gradient_href = [](const SvgNode& node) -> std::optional<std::string> {
    const auto direct = node.attributes.find("href");
    const auto legacy = node.attributes.find("xlink:href");
    if (direct != node.attributes.end() && legacy != node.attributes.end()) {
      throw std::runtime_error("SVG gradient must not declare both href and xlink:href");
    }
    const auto found = direct != node.attributes.end() ? direct : legacy;
    if (found == node.attributes.end()) {
      return std::nullopt;
    }
    if (!found->second.starts_with('#') || found->second.size() == 1) {
      throw std::runtime_error("SVG gradient href must be a file-local reference");
    }
    return found->second.substr(1);
  };

  const auto node_style = [&](std::size_t index) {
    std::vector<std::size_t> lineage;
    for (std::optional<std::size_t> current = index; current.has_value(); current = document.nodes[*current].parent) {
      lineage.push_back(*current);
    }
    Style style;
    for (auto iterator = lineage.rbegin(); iterator != lineage.rend(); ++iterator) {
      style = ResolveStyle(std::move(style), document.nodes[*iterator].attributes);
    }
    return style;
  };

  std::vector<std::size_t> gradient_references;
  std::function<GradientDefinition(std::size_t)> resolve_gradient_definition;
  resolve_gradient_definition = [&](std::size_t index) {
    const SvgNode& node = document.nodes[index];
    const bool radial = node.name == "radialGradient";
    if (!radial && node.name != "linearGradient") {
      throw std::runtime_error("SVG fill reference must target a gradient element");
    }
    if (std::ranges::find(gradient_references, index) != gradient_references.end() ||
        gradient_references.size() >= max_reference_depth) {
      throw std::runtime_error("SVG gradient references form a cycle or exceed the supported depth");
    }
    gradient_references.push_back(index);
    GradientDefinition definition;
    if (const std::optional<std::string> reference = gradient_href(node); reference.has_value()) {
      definition = resolve_gradient_definition(find_reference(*reference, "gradient"));
      if (definition.radial != radial) {
        throw std::runtime_error("SVG gradient href must reference the same gradient kind");
      }
    } else {
      definition.radial = radial;
      if (radial) {
        definition.first_x = {0.5F, true};
        definition.first_y = {0.5F, true};
        definition.second_x = {0.5F, true};
        definition.second_y = {0.5F, true};
      } else {
        definition.second_x = {1.0F, true};
      }
    }
    gradient_references.pop_back();

    if (node.attributes.contains("gradientTransform") || node.attributes.contains("transform")) {
      throw std::runtime_error("SVG gradientTransform is not supported");
    }
    if (const auto spread = node.attributes.find("spreadMethod");
        spread != node.attributes.end() && Trim(spread->second) != "pad") {
      throw std::runtime_error("SVG gradient spreadMethod supports only pad");
    }
    if (const auto units = node.attributes.find("gradientUnits"); units != node.attributes.end()) {
      const std::string value = Trim(units->second);
      if (value == "objectBoundingBox") {
        definition.object_bounding_box = true;
      } else if (value == "userSpaceOnUse") {
        definition.object_bounding_box = false;
      } else {
        throw std::runtime_error("SVG gradientUnits is unsupported");
      }
    }
    const auto override_length = [&](std::string_view name, GradientLength& destination) {
      if (const auto value = node.attributes.find(std::string(name)); value != node.attributes.end()) {
        destination = ParseGradientLength(value->second, name);
      }
    };
    if (radial) {
      override_length("cx", definition.first_x);
      override_length("cy", definition.first_y);
      override_length("r", definition.second_x);
      definition.second_y = definition.second_x;
      if (const auto value = node.attributes.find("fx"); value != node.attributes.end()) {
        definition.focal_x = ParseGradientLength(value->second, "fx");
      }
      if (const auto value = node.attributes.find("fy"); value != node.attributes.end()) {
        definition.focal_y = ParseGradientLength(value->second, "fy");
      }
      if (const auto value = node.attributes.find("fr"); value != node.attributes.end()) {
        definition.focal_radius = ParseGradientLength(value->second, "fr");
      }
    } else {
      override_length("x1", definition.first_x);
      override_length("y1", definition.first_y);
      override_length("x2", definition.second_x);
      override_length("y2", definition.second_y);
    }

    bool declares_stops = false;
    std::vector<std::pair<float, Color>> stops;
    const Style gradient_style = node_style(index);
    float previous_offset = 0.0F;
    for (const std::size_t child : node.children) {
      const SvgNode& stop = document.nodes[child];
      if (stop.name != "stop") {
        throw std::runtime_error("SVG gradients may contain only stop elements");
      }
      if (!stop.children.empty()) {
        throw std::runtime_error("SVG stop elements must not contain child elements");
      }
      declares_stops = true;
      const auto offset = stop.attributes.find("offset");
      const GradientLength parsed =
          offset == stop.attributes.end() ? GradientLength{} : ParseGradientLength(offset->second, "stop offset");
      const float resolved_offset = std::max(previous_offset, std::clamp(parsed.value, 0.0F, 1.0F));
      const Style stop_style = ResolveStyle(gradient_style, stop.attributes);
      Color color = stop_style.stop_uses_current_color ? stop_style.current_color : stop_style.stop_color;
      color.alpha *= stop_style.stop_opacity;
      stops.emplace_back(resolved_offset, color);
      previous_offset = resolved_offset;
    }
    if (declares_stops) {
      definition.stops = std::move(stops);
    }
    if (definition.stops.empty()) {
      throw std::runtime_error("SVG gradients require at least one stop");
    }
    if (definition.stops.size() == 1) {
      const Color color = definition.stops.front().second;
      definition.stops = {{0.0F, color}, {1.0F, color}};
    }
    return definition;
  };

  const auto resolve_gradient = [&](std::string_view id, Rect path_bounds) {
    const GradientDefinition definition = resolve_gradient_definition(find_reference(id, "fill"));
    const Rect coordinate_rect = definition.object_bounding_box
                                     ? path_bounds
                                     : Rect{result.view_x, result.view_y, result.view_width, result.view_height};
    if (coordinate_rect.width <= 0.0F || coordinate_rect.height <= 0.0F) {
      return ResolvedGradient{definition.radial, coordinate_rect};
    }
    const Point first{
        ResolveGradientCoordinate(definition.first_x, coordinate_rect.x, coordinate_rect.width,
                                  definition.object_bounding_box),
        ResolveGradientCoordinate(definition.first_y, coordinate_rect.y, coordinate_rect.height,
                                  definition.object_bounding_box),
    };
    Point second;
    if (definition.radial) {
      second = {
          ResolveGradientRadius(definition.second_x, coordinate_rect, true, definition.object_bounding_box),
          ResolveGradientRadius(definition.second_y, coordinate_rect, false, definition.object_bounding_box),
      };
      if (second.x <= 0.0F || second.y <= 0.0F) {
        throw std::runtime_error("SVG radial gradient radius must be positive");
      }
      const float focal_x = definition.focal_x.has_value()
                                ? ResolveGradientCoordinate(*definition.focal_x, coordinate_rect.x,
                                                            coordinate_rect.width, definition.object_bounding_box)
                                : first.x;
      const float focal_y = definition.focal_y.has_value()
                                ? ResolveGradientCoordinate(*definition.focal_y, coordinate_rect.y,
                                                            coordinate_rect.height, definition.object_bounding_box)
                                : first.y;
      const float focal_radius = definition.focal_radius.has_value()
                                     ? ResolveGradientRadius(*definition.focal_radius, coordinate_rect, true,
                                                             definition.object_bounding_box)
                                     : 0.0F;
      if (focal_x != first.x || focal_y != first.y || focal_radius != 0.0F) {
        throw std::runtime_error("SVG non-concentric radial gradients are not supported");
      }
    } else {
      second = {
          ResolveGradientCoordinate(definition.second_x, coordinate_rect.x, coordinate_rect.width,
                                    definition.object_bounding_box),
          ResolveGradientCoordinate(definition.second_y, coordinate_rect.y, coordinate_rect.height,
                                    definition.object_bounding_box),
      };
    }
    return ResolvedGradient{definition.radial, coordinate_rect, first, second, definition.stops};
  };

  for (std::size_t node_index = 0; node_index < document.nodes.size(); ++node_index) {
    SvgNode& node = document.nodes[node_index];
    const Style style = ResolveStyle(Style{}, node.attributes);
    static_cast<void>(node_transform(node));
    if (IsShape(node.name)) {
      node.path = ShapePath(node.name, node.attributes);
    }
    if (node.name == "use") {
      if (!node.children.empty()) {
        throw std::runtime_error("SVG use elements must not contain child elements");
      }
      const std::string reference = href(node);
      const std::string_view target = document.nodes[find_reference(reference, "use")].name;
      if (target != "g" && target != "use" && !IsShape(target)) {
        throw std::runtime_error("SVG use references an unsupported element: " + std::string(target));
      }
    }
    if (style.clip_reference.has_value()) {
      const std::size_t target = find_reference(*style.clip_reference, "clip-path");
      if (document.nodes[target].name != "clipPath") {
        throw std::runtime_error("SVG clip-path must reference a clipPath element");
      }
    }
    if (node.name == "clipPath") {
      if (const auto units = node.attributes.find("clipPathUnits");
          units != node.attributes.end() && units->second != "userSpaceOnUse") {
        throw std::runtime_error("SVG clipPathUnits supports only userSpaceOnUse");
      }
    }
    if (node.name == "linearGradient" || node.name == "radialGradient") {
      static_cast<void>(resolve_gradient_definition(node_index));
    }
    if (node.name == "stop") {
      if (!node.parent.has_value() || (document.nodes[*node.parent].name != "linearGradient" &&
                                       document.nodes[*node.parent].name != "radialGradient")) {
        throw std::runtime_error("SVG stop elements must belong to a gradient");
      }
      if (node.attributes.contains("transform")) {
        throw std::runtime_error("SVG stop transforms are not supported");
      }
    }
  }
  std::vector<std::uint8_t> reference_state(document.nodes.size());
  std::function<void(std::size_t, std::size_t)> validate_references;
  validate_references = [&](std::size_t index, std::size_t reference_depth) {
    if (reference_state[index] == 1) {
      throw std::runtime_error("SVG use references form a cycle");
    }
    if (reference_state[index] == 2) {
      return;
    }
    reference_state[index] = 1;
    const SvgNode& node = document.nodes[index];
    for (const std::size_t child : node.children) {
      validate_references(child, reference_depth);
    }
    if (node.name == "use") {
      if (reference_depth >= max_reference_depth) {
        throw std::runtime_error("SVG use references exceed the supported depth");
      }
      validate_references(find_reference(href(node), "use"), reference_depth + 1);
    }
    reference_state[index] = 2;
  };
  validate_references(document.root, 0);

  std::function<void(std::size_t, Style, Transform, Path&, std::optional<std::uint8_t>&, std::size_t&)>
      collect_clip;
  collect_clip = [&](std::size_t index, Style inherited, Transform transform, Path& path,
                     std::optional<std::uint8_t>& rule, std::size_t& shape_count) {
    const SvgNode& node = document.nodes[index];
    const Style style = ResolveStyle(std::move(inherited), node.attributes);
    if (!style.displayed || style.opacity == 0.0F) {
      return;
    }
    if (style.clip_reference.has_value()) {
      throw std::runtime_error("SVG nested clip-path references are not supported");
    }
    transform = Compose(transform, node_transform(node));
    if (node.name == "clipPath") {
      for (const std::size_t child : node.children) {
        collect_clip(child, style, transform, path, rule, shape_count);
      }
      return;
    }
    if (node.name == "g") {
      for (const std::size_t child : node.children) {
        collect_clip(child, style, transform, path, rule, shape_count);
      }
      return;
    }
    if (node.name == "use") {
      Transform translation;
      translation.tx = AttributeNumber(node.attributes, "x");
      translation.ty = AttributeNumber(node.attributes, "y");
      transform = Compose(transform, translation);
      const std::size_t target = find_reference(href(node), "use");
      if (std::ranges::find(references, target) != references.end() || references.size() >= max_reference_depth) {
        throw std::runtime_error("SVG use references form a cycle or exceed the supported depth");
      }
      references.push_back(target);
      collect_clip(target, style, transform, path, rule, shape_count);
      references.pop_back();
      return;
    }
    if (IsShape(node.name)) {
      if (!style.visible) {
        return;
      }
      if (rule.has_value() && *rule != style.clip_rule) {
        throw std::runtime_error("SVG clipPath children must use one clip-rule");
      }
      if (++shape_count > 1) {
        throw std::runtime_error("SVG clipPath must resolve to one drawable path");
      }
      rule = style.clip_rule;
      Path shape = TransformPath(*node.path, transform);
      path.insert(path.end(), std::make_move_iterator(shape.begin()), std::make_move_iterator(shape.end()));
      return;
    }
    if (node.name != "title" && node.name != "desc" && node.name != "metadata") {
      throw std::runtime_error("SVG clipPath contains an unsupported element: " + node.name);
    }
  };

  const auto push_clip = [&](const Style& style) {
    if (!style.clip_reference.has_value()) {
      return false;
    }
    const std::size_t target = find_reference(*style.clip_reference, "clip-path");
    Path path;
    std::optional<std::uint8_t> rule;
    std::size_t shape_count = 0;
    references.push_back(target);
    collect_clip(target, Style{}, Transform{}, path, rule, shape_count);
    references.pop_back();
    if (path.empty()) {
      throw std::runtime_error("SVG clipPath must contain drawable geometry");
    }
    result.operations.U8(3);
    result.operations.U8(rule.value_or(0));
    result.operations.PathValue(path);
    count_operation();
    return true;
  };

  std::function<void(std::size_t, Style, bool)> emit_node;
  emit_node = [&](std::size_t index, Style inherited, bool referenced) {
    const SvgNode& node = document.nodes[index];
    const Style style = ResolveStyle(std::move(inherited), node.attributes);
    if (!style.displayed || style.opacity == 0.0F) {
      return;
    }
    if (node.name == "defs" || node.name == "clipPath" || node.name == "linearGradient" ||
        node.name == "radialGradient" || node.name == "stop") {
      if (referenced) {
        throw std::runtime_error("SVG use cannot reference a definition-only element");
      }
      return;
    }
    if (node.name == "title" || node.name == "desc" || node.name == "metadata") {
      return;
    }
    const bool transformed = node.attributes.contains("transform");
    if (transformed) {
      push_transform(node_transform(node));
    }
    const bool translated_use = node.name == "use" &&
                                (node.attributes.contains("x") || node.attributes.contains("y"));
    if (translated_use) {
      Transform translation;
      translation.tx = AttributeNumber(node.attributes, "x");
      translation.ty = AttributeNumber(node.attributes, "y");
      push_transform(translation);
    }
    const bool clipped = push_clip(style);
    if (node.name == "g") {
      for (const std::size_t child : node.children) {
        emit_node(child, style, false);
      }
    } else if (node.name == "use") {
      const std::size_t target = find_reference(href(node), "use");
      if (std::ranges::find(references, target) != references.end() || references.size() >= max_reference_depth) {
        throw std::runtime_error("SVG use references form a cycle or exceed the supported depth");
      }
      references.push_back(target);
      emit_node(target, style, true);
      references.pop_back();
    } else if (IsShape(node.name)) {
      WriteShape(result.operations, *node.path, style, result.operation_count, resolve_gradient);
      if (result.operation_count > max_operations) {
        throw std::runtime_error("SVG produces too many vector operations");
      }
    } else {
      throw std::runtime_error("SVG contains an unsupported render element: " + node.name);
    }
    if (clipped) {
      result.operations.U8(4);
      count_operation();
    }
    if (translated_use) {
      pop_transform();
    }
    if (transformed) {
      pop_transform();
    }
  };

  const auto aspect_transform = [&]() {
    const auto preserve = root.attributes.find("preserveAspectRatio");
    const std::string value = preserve == root.attributes.end() ? "xMidYMid meet" : Trim(preserve->second);
    if (value == "none") {
      return Transform{};
    }
    const std::size_t separator = value.find_first_of(" \t\r\n");
    const std::string alignment = value.substr(0, separator);
    const std::string mode = separator == std::string::npos ? "meet" : Trim(std::string_view(value).substr(separator));
    if ((mode != "meet" && mode != "slice") || alignment.size() != 8 || !alignment.starts_with('x') ||
        alignment[4] != 'Y') {
      throw std::runtime_error("SVG preserveAspectRatio is malformed or unsupported");
    }
    const std::string_view horizontal(alignment.data() + 1, 3);
    const std::string_view vertical(alignment.data() + 5, 3);
    if ((horizontal != "Min" && horizontal != "Mid" && horizontal != "Max") ||
        (vertical != "Min" && vertical != "Mid" && vertical != "Max")) {
      throw std::runtime_error("SVG preserveAspectRatio contains an unsupported alignment");
    }
    const float scale_x = result.intrinsic_width / result.view_width;
    const float scale_y = result.intrinsic_height / result.view_height;
    const float scale = mode == "meet" ? std::min(scale_x, scale_y) : std::max(scale_x, scale_y);
    const auto align_offset = [](std::string_view alignment_value, float available) {
      return alignment_value == "Min" ? 0.0F : alignment_value == "Mid" ? available * 0.5F : available;
    };
    const float offset_x = align_offset(horizontal, result.intrinsic_width - result.view_width * scale);
    const float offset_y = align_offset(vertical, result.intrinsic_height - result.view_height * scale);
    const float ratio_x = scale / scale_x;
    const float ratio_y = scale / scale_y;
    return Transform{
        ratio_x,
        0.0F,
        0.0F,
        ratio_y,
        result.view_x * (1.0F - ratio_x) + offset_x / scale_x,
        result.view_y * (1.0F - ratio_y) + offset_y / scale_y,
    };
  };

  const Style root_style = ResolveStyle(Style{}, root.attributes);
  if (root_style.displayed && root_style.opacity != 0.0F) {
    const bool transformed = root.attributes.contains("transform");
    if (transformed) {
      push_transform(node_transform(root));
    }
    const Transform aspect = aspect_transform();
    const bool preserves_aspect = aspect.m11 != 1.0F || aspect.m22 != 1.0F || aspect.tx != 0.0F || aspect.ty != 0.0F;
    if (preserves_aspect) {
      push_transform(aspect);
    }
    const bool clipped = push_clip(root_style);
    for (const std::size_t child : root.children) {
      emit_node(child, root_style, false);
    }
    if (clipped) {
      result.operations.U8(4);
      count_operation();
    }
    if (preserves_aspect) {
      pop_transform();
    }
    if (transformed) {
      pop_transform();
    }
  }
  return result;
}

CompiledDocument CompileDocument(std::string_view xml) {
  return EmitDocument(ParseSvgDocument(xml));
}

} // namespace

CompiledSvg CompileSvg(const std::filesystem::path& path) {
  try {
    CompiledDocument document = CompileDocument(ReadText(path));
    Writer output;
    constexpr std::byte magic[] = {
        std::byte{'H'},
        std::byte{'U'},
        std::byte{'X'},
        std::byte{'V'},
        std::byte{'E'},
        std::byte{'C'},
        std::byte{0},
        std::byte{0},
    };
    output.Append(magic);
    output.U32(1);
    output.F32(document.view_x);
    output.F32(document.view_y);
    output.F32(document.view_width);
    output.F32(document.view_height);
    output.F32(document.intrinsic_width);
    output.F32(document.intrinsic_height);
    output.U32(document.operation_count);
    output.Append(document.operations.Bytes());
    return {std::move(output).Take(), document.intrinsic_width, document.intrinsic_height};
  } catch (const std::exception& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
}

} // namespace huxerui::resource_compiler
