#include "testing_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>

#include <huxerui/external_texture.h>

#include "internal_access.h"
#include "graphics/path_internal.h"

namespace huxerui::detail {
namespace {

// Named fields preserve canonical structure independently of the text export.
struct SnapshotNode {
  SnapshotNode(std::string name = {}, std::string value = {}) : name(std::move(name)), value(std::move(value)) {}
  std::string name;
  std::string value;
  std::string key;
  std::vector<SnapshotNode> children;
  bool sequence = false;
  bool operator==(const SnapshotNode&) const = default;
};

std::string Escaped(std::string_view value) {
  std::string result = "\"";
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 32 || c == 127) {
      result += "\\u00";
      result += digits[c >> 4];
      result += digits[c & 15];
    } else
      result += static_cast<char>(c);
  }
  return result + '"';
}

class SnapshotBuilder {
public:
  SnapshotNode Value(std::string name, std::string_view value) {
    return {std::move(name), Escaped(value)};
  }
  SnapshotNode Value(std::string name, const std::string& value) {
    return Value(std::move(name), std::string_view(value));
  }
  SnapshotNode Value(std::string name, const char* value) {
    return Value(std::move(name), std::string_view(value));
  }
  SnapshotNode Value(std::string name, bool value) {
    return {std::move(name), value ? "true" : "false"};
  }
  template <class T>
    requires std::is_arithmetic_v<T>
  SnapshotNode Value(std::string name, T value) {
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(value))
        throw testing::UiTestFailure("HuxerUI snapshot contains non-finite geometry");
      if (value == 0)
        value = 0;
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6) << value;
    return {std::move(name), out.str()};
  }
  template <class T>
    requires std::is_enum_v<T>
  SnapshotNode Value(std::string name, T value) {
    return Value(std::move(name), static_cast<long long>(value));
  }
  template <class T> SnapshotNode Value(std::string name, const std::optional<T>& value) {
    return value ? Value(std::move(name), *value) : SnapshotNode{std::move(name), "null"};
  }
  template <class T> SnapshotNode Value(std::string name, const std::vector<T>& values) {
    SnapshotNode result{std::move(name)};
    result.sequence = true;
    for (const auto& value : values)
      result.children.push_back(Value("item", value));
    return result;
  }
  template <class... T>
  SnapshotNode Fields(std::string name, const std::array<const char*, sizeof...(T)>& names, const T&... values) {
    SnapshotNode result{std::move(name)};
    auto field = names.begin();
    (result.children.push_back(Value(*field++, values)), ...);
    return result;
  }
  SnapshotNode Value(std::string name, Point v) {
    return Fields(std::move(name), {"x", "y"}, v.x, v.y);
  }
  SnapshotNode Value(std::string name, Size v) {
    return Fields(std::move(name), {"width", "height"}, v.width, v.height);
  }
  SnapshotNode Value(std::string name, Rect v) {
    return Fields(std::move(name), {"x", "y", "width", "height"}, v.x, v.y, v.width, v.height);
  }
  SnapshotNode Value(std::string name, Color v) {
    return Fields(std::move(name), {"red", "green", "blue", "alpha"}, v.red, v.green, v.blue, v.alpha);
  }
  SnapshotNode Value(std::string name, Transform2D v) {
    return Fields(std::move(name), {"m11", "m12", "m21", "m22", "translate_x", "translate_y"}, v.m11, v.m12, v.m21,
                  v.m22, v.translate_x, v.translate_y);
  }
  SnapshotNode Value(std::string name, TextRange v) {
    return Fields(std::move(name), {"start", "end"}, v.start, v.end);
  }
  SnapshotNode Value(std::string name, const SemanticRange& v) {
    return Fields(std::move(name), {"minimum", "maximum", "current", "step"}, v.minimum, v.maximum, v.current, v.step);
  }
  SnapshotNode Value(std::string name, const SemanticCollection& v) {
    return Fields(std::move(name), {"item_count", "row_count", "column_count"}, v.item_count, v.row_count,
                  v.column_count);
  }
  SnapshotNode Value(std::string name, const SemanticCollectionItem& v) {
    return Fields(std::move(name), {"index", "row_index", "column_index", "row_span", "column_span"}, v.index,
                  v.row_index, v.column_index, v.row_span, v.column_span);
  }
  SnapshotNode Value(std::string name, const ScrollMetrics& v) {
    return Fields(std::move(name), {"axis", "offset", "maximum_offset", "viewport_extent", "content_extent"}, v.axis,
                  v.offset, v.maximum_offset, v.viewport_extent, v.content_extent);
  }
  SnapshotNode Value(std::string name, const Font& v) {
    return Fields(std::move(name), {"family_kind", "family", "size", "weight", "slant"}, v.FamilyKind(), v.FamilyName(),
                  v.Size(), v.Weight(), v.Slant());
  }
  SnapshotNode Value(std::string name, const TextStyle& v) {
    return Fields(std::move(name), {"font", "foreground", "decoration"}, v.font, v.foreground, v.decoration);
  }
  SnapshotNode Value(std::string name, const TextShapingOptions& v) {
    return Fields(std::move(name), {"direction", "locale"}, v.direction, v.locale);
  }
  SnapshotNode Value(std::string name, const TextLayoutOptions& v) {
    return Fields(std::move(name), {"shaping", "align", "vertical_align", "wrap"}, v.shaping, v.align, v.vertical_align,
                  v.wrap);
  }
  SnapshotNode Value(std::string name, const AttributedText& v) {
    auto result = Fields(std::move(name), {"text"}, v.PlainText());
    SnapshotNode styles{"styles"}, links{"links"};
    styles.sequence = links.sequence = true;
    for (const auto& range : v.StyleRanges()) {
      const auto& s = range.style;
      styles.children.push_back(Fields(
          "style",
          {"range", "font", "font_size", "font_weight", "font_slant", "foreground", "background", "decoration"},
          range.range, s.font, s.font_size, s.font_weight, s.font_slant, s.foreground, s.background, s.decoration));
    }
    for (const auto& link : v.LinkRanges())
      links.children.push_back(Fields("link", {"range", "target"}, link.range, link.target.ToString()));
    result.children.push_back(std::move(styles));
    result.children.push_back(std::move(links));
    return result;
  }
  SnapshotNode Value(std::string name, const StrokeStyle& v) {
    return Fields(std::move(name), {"width", "cap", "join", "miter_limit", "dash_pattern", "dash_offset"}, v.width,
                  v.cap, v.join, v.miter_limit, v.dash_pattern, v.dash_offset);
  }
  SnapshotNode Value(std::string name, const Path& v) {
    SnapshotNode result{std::move(name)};
    result.sequence = true;
    for (const auto& element : InternalAccess::Elements(v)) {
      auto command = Fields("path-command", {"verb"}, element.verb);
      const int count = element.verb == PathVerb::CubicTo       ? 3
                        : element.verb == PathVerb::QuadraticTo ? 2
                        : element.verb == PathVerb::Close       ? 0
                                                                : 1;
      for (int i = 0; i < count; ++i)
        command.children.push_back(Value("point" + std::to_string(i), element.points[static_cast<std::size_t>(i)]));
      result.children.push_back(std::move(command));
    }
    return result;
  }
  SnapshotNode Value(std::string name, const Brush& v) {
    return std::visit(
        [&](const auto& brush) {
          using T = std::decay_t<decltype(brush)>;
          if constexpr (std::same_as<T, Color>)
            return Fields(std::move(name), {"kind", "color"}, "solid", brush);
          else {
            SnapshotNode result;
            if constexpr (std::same_as<T, LinearGradient>)
              result = Fields(std::move(name), {"kind", "start", "end"}, "linear", brush.start, brush.end);
            else
              result = Fields(std::move(name), {"kind", "center", "radius"}, "radial", brush.center, brush.radius);
            result.children.push_back(Value("transform", brush.transform));
            SnapshotNode stops{"stops"};
            stops.sequence = true;
            for (const auto& stop : brush.stops)
              stops.children.push_back(Fields("stop", {"offset", "color"}, stop.offset, stop.color));
            result.children.push_back(std::move(stops));
            return result;
          }
        },
        v.Get());
  }
  SnapshotNode Command(const DrawRectCommand& c) {
    return Fields("rect", {"bounds", "brush", "corner_radius"}, c.rect, c.brush, c.corner_radius);
  }
  SnapshotNode Command(const DrawTextCommand& c) {
    return Fields("text", {"bounds", "content", "style", "options", "paragraph_offset"}, c.rect, c.text, c.style,
                  c.options, c.paragraph_offset);
  }
  SnapshotNode Command(const DrawTextRunsCommand& c) {
    SnapshotNode result{"text-runs"};
    result.sequence = true;
    for (const auto& run : c.runs)
      result.children.push_back(Fields("run", {"bounds", "baseline_origin", "text", "style", "shaping"}, run.bounds,
                                       run.baseline_origin, run.text, run.style, run.shaping));
    return result;
  }
  SnapshotNode Command(const DrawImageCommand& c) {
    auto result = Fields("image", {"source", "destination", "sampling", "opacity", "scale"}, c.source, c.destination,
                         c.sampling, c.opacity, c.image.Scale());
    std::string bytes;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : c.image.EncodedBytes()) {
      const auto value = std::to_integer<unsigned char>(byte);
      bytes += digits[value >> 4];
      bytes += digits[value & 15];
    }
    result.children.push_back({"encoded_bytes", std::move(bytes)});
    return result;
  }
  SnapshotNode Command(const DrawExternalTextureCommand& c) {
    return Fields("external-texture", {"content", "intrinsic_size", "source", "destination", "sampling", "opacity"},
                  "uncaptured", c.texture->IntrinsicSize(), c.source, c.destination, c.sampling, c.opacity);
  }
  SnapshotNode Command(const DrawCircleCommand& c) {
    return Fields("circle", {"center", "radius", "color"}, c.center, c.radius, c.color);
  }
  SnapshotNode Command(const DrawLineCommand& c) {
    return Fields("line", {"start", "end", "color", "style"}, c.start, c.end, c.color, c.style);
  }
  SnapshotNode Command(const DrawArcCommand& c) {
    return Fields("arc", {"center", "radius", "start_angle", "sweep_angle", "color", "style"}, c.center, c.radius,
                  c.start_angle, c.sweep_angle, c.color, c.style);
  }
  SnapshotNode Command(const DrawBorderCommand& c) {
    return Fields("border", {"bounds", "color", "style", "corner_radius"}, c.rect, c.color, c.style, c.corner_radius);
  }
  SnapshotNode Command(const DrawShadowCommand& c) {
    return Fields("shadow", {"bounds", "color", "offset", "blur_radius", "spread", "corner_radius"}, c.rect, c.color,
                  c.offset, c.blur_radius, c.spread, c.corner_radius);
  }
  SnapshotNode Command(const FillPathCommand& c) {
    return Fields("fill-path", {"path", "brush", "brush_bounds", "fill_rule"}, c.path, c.brush, c.brush_bounds,
                  c.fill_rule);
  }
  SnapshotNode Command(const StrokePathCommand& c) {
    return Fields("stroke-path", {"path", "brush", "brush_bounds", "style"}, c.path, c.brush, c.brush_bounds, c.style);
  }
  SnapshotNode Command(const DrawPathShadowCommand& c) {
    return Fields("path-shadow", {"path", "color", "offset", "blur_radius", "fill_rule"}, c.path, c.color, c.offset,
                  c.blur_radius, c.fill_rule);
  }
  SnapshotNode Command(const PushClipCommand& c) {
    return Fields("push-clip", {"bounds", "corner_radius"}, c.rect, c.corner_radius);
  }
  SnapshotNode Command(const PushPathClipCommand& c) {
    return Fields("push-path-clip", {"path", "fill_rule"}, c.path, c.fill_rule);
  }
  SnapshotNode Command(const PopClipCommand&) {
    return {"pop-clip"};
  }
  SnapshotNode Command(const PushTransformCommand& c) {
    return Fields("push-transform", {"transform"}, c.transform);
  }
  SnapshotNode Command(const PopTransformCommand&) {
    return {"pop-transform"};
  }
  SnapshotNode Command(const PlacePlatformViewCommand& c) {
    return Fields("platform-view", {"content", "type", "bounds"}, "uncaptured", c.Type(), c.Bounds());
  }
  SnapshotNode Sequence(std::string name, const PaintSequence& sequence) {
    SnapshotNode result{std::move(name)};
    result.sequence = true;
    for (const auto& command : sequence.Commands())
      result.children.push_back(std::visit([&](const auto& value) { return Command(value); }, command));
    return result;
  }
  SnapshotNode Render(const RenderNode* node) {
    if (!node)
      return {"node", "null"};
    auto result = Fields("node", {"offset", "transform", "opacity", "visible"}, node->offset, node->transform,
                         node->opacity, node->visible);
    result.children.push_back(Sequence("content", node->content));
    SnapshotNode clips{"children-clips"}, children{"children"};
    clips.sequence = children.sequence = true;
    for (const auto& clip : node->child_clips)
      clips.children.push_back(std::visit([&](const auto& value) { return Command(value); }, clip));
    result.children.push_back(std::move(clips));
    result.children.push_back(Value("children_transform", node->children_transform));
    for (const auto* child : node->children)
      children.children.push_back(Render(child));
    result.children.push_back(std::move(children));
    result.children.push_back(Sequence("foreground", node->foreground));
    return result;
  }
  SnapshotNode Semantics(const UiSemanticIndex& index, SemanticNodeId id) {
    const auto& n = SemanticNodeAt(index, id);
    auto result = Fields("semantic", {"role",      "identifier",        "label",           "value",     "placeholder",
                                      "hint",      "state_description", "error",           "enabled",   "focused",
                                      "checked",   "selected",          "expanded",        "busy",      "read_only",
                                      "required",  "invalid",           "heading_level",   "multiline", "secure",
                                      "offscreen", "text_selection",    "bounds",          "actions",   "live_region",
                                      "range",     "collection",        "collection_item", "scroll"},
                         n.role, n.identifier, n.label, n.secure ? std::string{} : n.value, n.placeholder, n.hint,
                         n.state_description, n.error, n.enabled, n.focused, n.checked, n.selected, n.expanded, n.busy,
                         n.read_only, n.required, n.invalid, n.heading_level, n.multiline, n.secure, n.offscreen,
                         n.secure ? std::optional<TextRange>{} : n.text_selection, n.bounds, n.actions, n.live_region,
                         n.range, n.collection, n.collection_item, n.scroll);
    result.key = n.identifier;
    SnapshotNode actions{"custom-actions"}, children{"children"};
    actions.sequence = children.sequence = true;
    for (const auto& action : n.custom_actions)
      actions.children.push_back(Value("label", action.second));
    for (auto child : n.children)
      children.children.push_back(Semantics(index, child));
    result.children.push_back(std::move(actions));
    result.children.push_back(std::move(children));
    return result;
  }
};

void Serialize(const SnapshotNode& node, std::ostream& out, std::size_t indent = 0) {
  out << std::string(indent, ' ') << node.name;
  if (!node.value.empty())
    out << '=' << node.value;
  if (node.children.empty()) {
    out << '\n';
    return;
  }
  out << " {\n";
  for (const auto& child : node.children)
    Serialize(child, out, indent + 2);
  out << std::string(indent, ' ') << "}\n";
}

constexpr std::size_t diff_change_limit = 64;
constexpr std::size_t diff_value_limit = 160;
constexpr std::size_t diff_alignment_lookahead = 32;

std::string ShortValue(const std::string& value) {
  std::size_t length = std::min(value.size(), diff_value_limit);
  while (length < value.size() && length && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80)
    --length;
  return value.substr(0, length) + (length < value.size() ? "..." : "");
}

std::string Location(const SnapshotNode& node, const std::string& parent, std::optional<std::size_t> index = {}) {
  std::string result = parent + "/" + node.name;
  if (index)
    result += "[" + std::to_string(*index) + "]";
  if (!node.key.empty())
    result += " identifier=" + ShortValue(Escaped(node.key));
  return result;
}

class SnapshotDiff {
public:
  void Change(const std::string& text) {
    if (changes < diff_change_limit)
      out << text << '\n';
    ++changes;
  }
  void Compare(const SnapshotNode& expected, const SnapshotNode& actual, const std::string& path) {
    if (expected == actual)
      return;
    if (changes >= diff_change_limit) {
      ++changes;
      return;
    }
    if (expected.name != actual.name || expected.sequence != actual.sequence) {
      Change(path + ": " + expected.name + " -> " + actual.name + " (subtree replaced)");
      return;
    }
    if (expected.value != actual.value) {
      if (actual.name == "encoded_bytes")
        Change(path + ": image content changed (" + std::to_string(expected.value.size() / 2) + " -> " +
               std::to_string(actual.value.size() / 2) + " encoded bytes)");
      else
        Change(path + ": " + ShortValue(expected.value) + " -> " + ShortValue(actual.value));
    }
    const auto& before = expected.children;
    const auto& after = actual.children;
    std::size_t i = 0, j = 0;
    const auto stable_match = [&](std::size_t a, std::size_t b) {
      if (before[a] == after[b])
        return true;
      const auto& key = before[a].key;
      return !key.empty() && key == after[b].key &&
             std::count_if(before.begin(), before.end(), [&](const auto& n) { return n.key == key; }) == 1 &&
             std::count_if(after.begin(), after.end(), [&](const auto& n) { return n.key == key; }) == 1;
    };
    while (i < before.size() || j < after.size()) {
      if (changes >= diff_change_limit) {
        if (changes == diff_change_limit &&
            !std::equal(before.begin() + static_cast<std::ptrdiff_t>(i), before.end(),
                        after.begin() + static_cast<std::ptrdiff_t>(j), after.end()))
          ++changes;
        return;
      }
      if (i == before.size()) {
        Change("+ " + Location(after[j], path, actual.sequence ? std::optional{j} : std::nullopt) + " (subtree added)");
        ++j;
      } else if (j == after.size()) {
        Change("- " + Location(before[i], path, expected.sequence ? std::optional{i} : std::nullopt) +
               " (subtree removed)");
        ++i;
      } else {
        if (actual.sequence && !stable_match(i, j)) {
          std::size_t forward = j + 1, backward = i + 1;
          while (forward < after.size() && forward - j <= diff_alignment_lookahead && !stable_match(i, forward))
            ++forward;
          while (backward < before.size() && backward - i <= diff_alignment_lookahead && !stable_match(backward, j))
            ++backward;
          const bool added = forward < after.size() && forward - j <= diff_alignment_lookahead;
          const bool removed = backward < before.size() && backward - i <= diff_alignment_lookahead;
          if (added && (!removed || forward - j <= backward - i)) {
            Change("+ " + Location(after[j], path, j) + " (subtree added)");
            ++j;
            continue;
          }
          if (removed) {
            Change("- " + Location(before[i], path, i) + " (subtree removed)");
            ++i;
            continue;
          }
        }
        Compare(before[i], after[j], Location(after[j], path, actual.sequence ? std::optional{j} : std::nullopt));
        ++i;
        ++j;
      }
    }
  }
  std::string Result() {
    if (changes > diff_change_limit)
      out << "... additional structural changes omitted\n";
    return out.str();
  }

private:
  std::ostringstream out;
  std::size_t changes = 0;
};

} // namespace

struct UiSnapshotData {
  SnapshotNode root;
};

std::shared_ptr<const UiSnapshotData> CaptureUiStructure(const FrameCommit& commit, const UiSemanticIndex& semantics) {
  SnapshotBuilder builder;
  auto result = std::make_shared<UiSnapshotData>();
  result->root.name = "ui";
  SnapshotNode semantic_root{"semantics"}, render_root{"render"};
  if (commit.semantic_frame)
    semantic_root.children.push_back(builder.Semantics(semantics, commit.semantic_frame->root));
  render_root.children.push_back(builder.Render(commit.render_frame.scene.root));
  result->root.children.push_back(std::move(semantic_root));
  result->root.children.push_back(std::move(render_root));
  return result;
}

} // namespace huxerui::detail

namespace huxerui::testing {

std::string UiSnapshot::ToString() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "huxerui-ui-snapshot 2\n";
  detail::Serialize(data_->root, out);
  return out.str();
}
bool UiSnapshot::operator==(const UiSnapshot& other) const {
  return data_ == other.data_ || data_->root == other.data_->root;
}
std::string UiSnapshot::Diff(const UiSnapshot& expected) const {
  detail::SnapshotDiff diff;
  diff.Compare(expected.data_->root, data_->root, "ui");
  return diff.Result();
}

} // namespace huxerui::testing
