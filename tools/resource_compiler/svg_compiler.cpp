#include "svg_compiler.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
#include <utility>
#include <vector>

namespace huxerui::resource_compiler {

namespace {

struct Point {
  float x = 0.0F;
  float y = 0.0F;
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
  std::optional<Color> stroke;
  float fill_opacity = 1.0F;
  float stroke_opacity = 1.0F;
  float stroke_width = 1.0F;
  std::uint8_t fill_rule = 0;
  std::uint8_t stroke_cap = 0;
  std::uint8_t stroke_join = 0;
  float miter_limit = 4.0F;
  std::vector<float> dash_pattern;
  float dash_offset = 0.0F;
};

struct ElementFrame {
  std::string name;
  Style style;
  bool pushed_transform = false;
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

  void PathValue(const Path& path) {
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
  if (!suffix.empty() && suffix != "px") {
    throw std::runtime_error("SVG " + std::string(field) + " uses an unsupported unit");
  }
  return result;
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
    char* next = nullptr;
    const float length = std::strtof(cursor, &next);
    if (next == cursor || !std::isfinite(length)) {
      throw std::runtime_error("SVG stroke-dasharray contains an invalid length");
    }
    if (length < 0.0F) {
      throw std::runtime_error("SVG stroke-dasharray lengths must be non-negative");
    }
    cursor = next;
    bool has_explicit_unit = false;
    if (end - cursor >= 2 && cursor[0] == 'p' && cursor[1] == 'x') {
      cursor += 2;
      has_explicit_unit = true;
    }
    if (cursor < end && !std::isspace(static_cast<unsigned char>(*cursor)) && *cursor != ',') {
      throw std::runtime_error(
          has_explicit_unit ? "SVG stroke-dasharray lengths must be separated"
                            : "SVG stroke-dasharray uses an unsupported unit"
      );
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
  const float opacity = ParseNumber(value, field);
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
  if (text == "currentColor" || text == "black") {
    return Color{};
  }
  if (text == "white") {
    return Color{1.0F, 1.0F, 1.0F, 1.0F};
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
    } else {
      throw std::runtime_error("SVG contains an unsupported transform: " + name);
    }
    result = Compose(result, current);
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
    if (!attributes.emplace(name, std::string(value.substr(offset, end - offset))).second) {
      throw std::runtime_error("SVG element contains a duplicate attribute: " + name);
    }
    offset = end + 1;
  }
  return attributes;
}

void ApplyStyleProperty(Style& style, std::string_view name, std::string_view value) {
  if (name == "fill") {
    style.fill = ParseColor(value);
  } else if (name == "stroke") {
    style.stroke = ParseColor(value);
  } else if (name == "fill-opacity") {
    style.fill_opacity = ParseOpacity(value, name);
  } else if (name == "stroke-opacity") {
    style.stroke_opacity = ParseOpacity(value, name);
  } else if (name == "stroke-width") {
    style.stroke_width = ParseNumber(value, name);
    if (style.stroke_width < 0.0F) {
      throw std::runtime_error("SVG stroke-width must be non-negative");
    }
  } else if (name == "fill-rule" || name == "clip-rule") {
    if (value == "nonzero") {
      style.fill_rule = 0;
    } else if (value == "evenodd") {
      style.fill_rule = 1;
    } else {
      throw std::runtime_error("SVG contains an unsupported fill rule");
    }
  } else if (name == "stroke-linecap") {
    if (value == "butt") {
      style.stroke_cap = 0;
    } else if (value == "round") {
      style.stroke_cap = 1;
    } else if (value == "square") {
      style.stroke_cap = 2;
    } else {
      throw std::runtime_error("SVG contains an unsupported stroke line cap");
    }
  } else if (name == "stroke-linejoin") {
    if (value == "miter") {
      style.stroke_join = 0;
    } else if (value == "round") {
      style.stroke_join = 1;
    } else if (value == "bevel") {
      style.stroke_join = 2;
    } else {
      throw std::runtime_error("SVG contains an unsupported stroke line join");
    }
  } else if (name == "stroke-miterlimit") {
    style.miter_limit = ParseNumber(value, name);
    if (style.miter_limit < 1.0F) {
      throw std::runtime_error("SVG stroke-miterlimit must be at least one");
    }
  } else if (name == "stroke-dasharray") {
    style.dash_pattern = ParseDashPattern(value);
  } else if (name == "stroke-dashoffset") {
    style.dash_offset = ParseNumber(value, name);
  } else if (name == "opacity") {
    throw std::runtime_error("SVG group opacity is not supported; use fill-opacity and stroke-opacity");
  } else {
    throw std::runtime_error("SVG contains an unsupported style property: " + std::string(name));
  }
}

Style ResolveStyle(Style inherited, const std::map<std::string, std::string>& attributes) {
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
  for (const auto& [name, value] : attributes) {
    if (name == "fill" || name == "stroke" || name == "fill-opacity" || name == "stroke-opacity" ||
        name == "stroke-width" || name == "fill-rule" || name == "clip-rule" || name == "stroke-linecap" ||
        name == "stroke-linejoin" || name == "stroke-miterlimit" || name == "stroke-dasharray" ||
        name == "stroke-dashoffset" || name == "opacity") {
      ApplyStyleProperty(inherited, name, value);
    }
  }
  return inherited;
}

void ValidateAttributes(std::string_view element, const std::map<std::string, std::string>& attributes) {
  static const std::vector<std::string_view> common{
      "id",
      "xmlns",
      "xmlns:xlink",
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
      "opacity",
      "style",
      "transform",
  };
  const auto allowed_for_element = [element](std::string_view name) {
    if (element == "svg") {
      return name == "viewBox" || name == "width" || name == "height" || name == "version";
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
        name != "role") {
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
  writer.U8(5);
  writer.F32(transform.m11);
  writer.F32(transform.m12);
  writer.F32(transform.m21);
  writer.F32(transform.m22);
  writer.F32(transform.tx);
  writer.F32(transform.ty);
}

void WriteShape(Writer& writer, const Path& path, Style style, std::uint32_t& operation_count) {
  if (path.empty()) {
    return;
  }
  if (style.fill.has_value()) {
    Color color = *style.fill;
    color.alpha *= style.fill_opacity;
    writer.U8(1);
    writer.ColorValue(color);
    writer.U8(style.fill_rule);
    writer.PathValue(path);
    ++operation_count;
  }
  if (style.stroke.has_value() && style.stroke_width > 0.0F) {
    ValidateDashPattern(style.dash_pattern);
    if (style.dash_pattern.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("SVG stroke-dasharray contains too many lengths");
    }
    Color color = *style.stroke;
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

struct ParsedDocument {
  Writer operations;
  std::uint32_t operation_count = 0;
  float intrinsic_width = 0.0F;
  float intrinsic_height = 0.0F;
  float view_x = 0.0F;
  float view_y = 0.0F;
  float view_width = 0.0F;
  float view_height = 0.0F;
};

ParsedDocument ParseDocument(std::string_view xml) {
  if (xml.find("<!DOCTYPE") != std::string_view::npos || xml.find("<!ENTITY") != std::string_view::npos) {
    throw std::runtime_error("SVG external entities and document types are not supported");
  }
  ParsedDocument result;
  std::vector<ElementFrame> stack;
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
    if (!Trim(xml.substr(offset, open - offset)).empty() &&
        (stack.empty() ||
         (stack.back().name != "title" && stack.back().name != "desc" && stack.back().name != "metadata"))) {
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
    const std::size_t close = xml.find('>', open + 1);
    if (close == std::string_view::npos) {
      throw std::runtime_error("SVG element is unterminated");
    }
    std::string tag = Trim(xml.substr(open + 1, close - open - 1));
    const bool closing = !tag.empty() && tag.front() == '/';
    const bool self_closing = !closing && !tag.empty() && tag.back() == '/';
    if (closing) {
      tag = Trim(std::string_view(tag).substr(1));
      if (stack.empty() || stack.back().name != tag) {
        throw std::runtime_error("SVG closing element does not match its opening element: " + tag);
      }
      if (stack.back().pushed_transform) {
        result.operations.U8(6);
        ++result.operation_count;
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
    ValidateAttributes(name, attributes);
    const Style inherited = stack.empty() ? Style{} : stack.back().style;
    const Style style = ResolveStyle(inherited, attributes);
    bool pushed_transform = false;
    if (const auto transform = attributes.find("transform"); transform != attributes.end()) {
      WriteTransform(result.operations, ParseTransform(transform->second));
      ++result.operation_count;
      pushed_transform = true;
    }

    if (name == "svg") {
      if (root_seen || !stack.empty()) {
        throw std::runtime_error("SVG must contain exactly one root svg element");
      }
      root_seen = true;
      const auto view_box = attributes.find("viewBox");
      if (view_box != attributes.end()) {
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
          attributes.contains("width") ? ParseNumber(attributes.at("width"), "width") : result.view_width;
      result.intrinsic_height =
          attributes.contains("height") ? ParseNumber(attributes.at("height"), "height") : result.view_height;
      if (result.view_width == 0.0F || result.view_height == 0.0F) {
        result.view_width = result.intrinsic_width;
        result.view_height = result.intrinsic_height;
      }
      if (result.intrinsic_width <= 0.0F || result.intrinsic_height <= 0.0F) {
        throw std::runtime_error("SVG requires positive intrinsic width and height or a viewBox");
      }
    } else if (name == "g") {
    } else if (name == "path" || name == "rect" || name == "circle" || name == "ellipse" || name == "line" || name == "polyline" || name == "polygon") {
      WriteShape(result.operations, ShapePath(name, attributes), style, result.operation_count);
    } else if (name == "title" || name == "desc" || name == "metadata") {
    } else {
      throw std::runtime_error("SVG contains an unsupported element: " + name);
    }

    if (!self_closing) {
      stack.push_back({name, style, pushed_transform});
    } else if (pushed_transform) {
      result.operations.U8(6);
      ++result.operation_count;
    }
    offset = close + 1;
  }
  if (!root_seen || !stack.empty()) {
    throw std::runtime_error("SVG document is incomplete");
  }
  return result;
}

} // namespace

CompiledSvg CompileSvg(const std::filesystem::path& path) {
  try {
    ParsedDocument document = ParseDocument(ReadText(path));
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
