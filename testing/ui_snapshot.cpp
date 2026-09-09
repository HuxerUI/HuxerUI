#include "testing_internal.h"

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

class SnapshotWriter {
public:
  SnapshotWriter() {
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6);
  }

  void Write(std::string_view value) {
    out << '"';
    for (unsigned char c : value) {
      if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
      else if (c < 32 || c == 127) {
        const char* digits = "0123456789abcdef";
        out << "\\u00" << digits[c >> 4] << digits[c & 15];
      } else out << static_cast<char>(c);
    }
    out << '"';
  }
  void Write(const std::string& value) { Write(std::string_view(value)); }
  void Write(const char* value) { Write(std::string_view(value)); }
  void Write(bool value) { out << (value ? "true" : "false"); }
  template <class T> requires std::is_arithmetic_v<T>
  void Write(T value) {
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(value)) throw testing::UiTestFailure("HuxerUI snapshot contains non-finite geometry");
      if (value == 0) value = 0;
    }
    out << value;
  }
  template <class T> requires std::is_enum_v<T>
  void Write(T value) { out << static_cast<long long>(value); }
  template <class T> void Write(const std::optional<T>& value) {
    if (value) Write(*value); else out << "null";
  }
  template <class T> void Write(const std::vector<T>& values) {
    out << '[';
    for (const auto& value : values) {
      Write(value);
      out << ',';
    }
    out << ']';
  }
  template <class... T> void Fields(const T&... values) {
    out << '[';
    ((Write(values), out << ','), ...);
    out << ']';
  }
  void Write(Point value) { Fields(value.x, value.y); }
  void Write(Size value) { Fields(value.width, value.height); }
  void Write(Rect value) { Fields(value.x, value.y, value.width, value.height); }
  void Write(Color value) { Fields(value.red, value.green, value.blue, value.alpha); }
  void Write(Transform2D value) {
    Fields(value.m11, value.m12, value.m21, value.m22, value.translate_x, value.translate_y);
  }
  void Write(TextRange value) { Fields(value.start, value.end); }
  void Write(const SemanticRange& value) { Fields(value.minimum, value.maximum, value.current, value.step); }
  void Write(const SemanticCollection& value) { Fields(value.item_count, value.row_count, value.column_count); }
  void Write(const SemanticCollectionItem& value) {
    Fields(value.index, value.row_index, value.column_index, value.row_span, value.column_span);
  }
  void Write(const ScrollMetrics& value) {
    Fields(value.axis, value.offset, value.maximum_offset, value.viewport_extent, value.content_extent);
  }
  void Write(const Font& value) {
    Fields(value.FamilyKind(), value.FamilyName(), value.Size(), value.Weight(), value.Slant());
  }
  void Write(const TextStyle& value) { Fields(value.font, value.foreground, value.decoration); }
  void Write(const TextShapingOptions& value) { Fields(value.direction, value.locale); }
  void Write(const TextLayoutOptions& value) { Fields(value.shaping, value.align, value.vertical_align, value.wrap); }
  void Write(const AttributedText& value) {
    Write(value.PlainText());
    out << " styles[";
    for (const auto& range : value.StyleRanges()) {
      const auto& s = range.style;
      Fields(range.range, s.font, s.font_size, s.font_weight, s.font_slant, s.foreground, s.background, s.decoration);
    }
    out << "] links[";
    for (const auto& link : value.LinkRanges()) Fields(link.range, link.target.ToString());
    out << ']';
  }
  void Write(const StrokeStyle& value) {
    Fields(value.width, value.cap, value.join, value.miter_limit, value.dash_pattern, value.dash_offset);
  }
  void Write(const Path& value) {
    out << "path[";
    for (const auto& element : InternalAccess::Elements(value)) {
      Write(element.verb);
      const int count = element.verb == PathVerb::CubicTo ? 3 : element.verb == PathVerb::QuadraticTo ? 2
          : element.verb == PathVerb::Close ? 0 : 1;
      for (int i = 0; i < count; ++i) Write(element.points[static_cast<std::size_t>(i)]);
      out << ',';
    }
    out << ']';
  }
  void Write(const Brush& value) {
    std::visit([&](const auto& brush) {
      using T = std::decay_t<decltype(brush)>;
      if constexpr (std::same_as<T, Color>) {
        out << "solid";
        Write(brush);
      } else {
        if constexpr (std::same_as<T, LinearGradient>) {
          out << "linear";
          Fields(brush.start, brush.end);
        } else {
          out << "radial";
          Fields(brush.center, brush.radius);
        }
        Write(brush.transform);
        out << '[';
        for (const auto& stop : brush.stops) Fields(stop.offset, stop.color);
        out << ']';
      }
    }, value.Get());
  }
  void Write(const DrawRectCommand& c) { out << "rect"; Fields(c.rect, c.brush, c.corner_radius); }
  void Write(const DrawTextCommand& c) {
    out << "text"; Fields(c.rect, c.text, c.style, c.options, c.paragraph_offset);
  }
  void Write(const DrawTextRunsCommand& c) {
    out << "text-runs[";
    for (const auto& run : c.runs) Fields(run.bounds, run.baseline_origin, run.text, run.style, run.shaping);
    out << ']';
  }
  void Write(const DrawImageCommand& c) {
    out << "image"; Fields(c.source, c.destination, c.sampling, c.opacity, c.image.Scale());
    // Encoded content is stable and owning at capture time; never serialize an allocation identity.
    out << " bytes[";
    const char* digits = "0123456789abcdef";
    for (auto byte : c.image.EncodedBytes()) {
      const auto value = std::to_integer<unsigned char>(byte);
      out << digits[value >> 4] << digits[value & 15];
    }
    out << ']';
  }
  void Write(const DrawExternalTextureCommand& c) {
    out << "external-texture(content=uncaptured)"; Fields(c.source, c.destination, c.sampling, c.opacity);
  }
  void Write(const DrawCircleCommand& c) { out << "circle"; Fields(c.center, c.radius, c.color); }
  void Write(const DrawLineCommand& c) { out << "line"; Fields(c.start, c.end, c.color, c.style); }
  void Write(const DrawArcCommand& c) {
    out << "arc"; Fields(c.center, c.radius, c.start_angle, c.sweep_angle, c.color, c.style);
  }
  void Write(const DrawBorderCommand& c) { out << "border"; Fields(c.rect, c.color, c.style, c.corner_radius); }
  void Write(const DrawShadowCommand& c) {
    out << "shadow"; Fields(c.rect, c.color, c.offset, c.blur_radius, c.spread, c.corner_radius);
  }
  void Write(const FillPathCommand& c) { out << "fill-path"; Fields(c.path, c.brush, c.brush_bounds, c.fill_rule); }
  void Write(const StrokePathCommand& c) { out << "stroke-path"; Fields(c.path, c.brush, c.brush_bounds, c.style); }
  void Write(const DrawPathShadowCommand& c) {
    out << "path-shadow"; Fields(c.path, c.color, c.offset, c.blur_radius, c.fill_rule);
  }
  void Write(const PushClipCommand& c) { out << "push-clip"; Fields(c.rect, c.corner_radius); }
  void Write(const PushPathClipCommand& c) { out << "push-path-clip"; Fields(c.path, c.fill_rule); }
  void Write(const PopClipCommand&) { out << "pop-clip"; }
  void Write(const PushTransformCommand& c) { out << "push-transform"; Write(c.transform); }
  void Write(const PopTransformCommand&) { out << "pop-transform"; }
  void Write(const PlacePlatformViewCommand& c) {
    out << "platform-view(content=uncaptured)"; Fields(c.Type(), c.Bounds());
  }

  void Sequence(const PaintSequence& sequence) {
    for (const auto& command : sequence.Commands()) {
      std::visit([&](const auto& value) { Write(value); }, command);
      out << '\n';
    }
  }
  void Render(const RenderNode* node) {
    if (!node) {
      out << "null\n";
      return;
    }
    out << "node ";
    Fields(node->offset, node->transform, node->opacity, node->visible);
    out << " {\ncontent\n";
    Sequence(node->content);
    out << "children-clips[";
    for (const auto& clip : node->child_clips) std::visit([&](const auto& value) { Write(value); }, clip);
    out << "] transform";
    Write(node->children_transform);
    out << "\nchildren\n";
    for (const auto* child : node->children) Render(child);
    out << "foreground\n";
    Sequence(node->foreground);
    out << "}\n";
  }
  void Semantics(const UiSemanticIndex& index, SemanticNodeId id) {
    const auto& n = SemanticNodeAt(index, id);
    out << "semantic ";
    Fields(n.role, n.identifier, n.label, n.secure ? std::string{} : n.value, n.placeholder, n.hint,
           n.state_description, n.error, n.enabled, n.focused, n.checked, n.selected, n.expanded, n.busy,
           n.read_only, n.required, n.invalid, n.heading_level, n.multiline, n.secure, n.offscreen,
           n.secure ? std::optional<TextRange>{} : n.text_selection, n.bounds, n.actions, n.live_region,
           n.range, n.collection, n.collection_item, n.scroll);
    out << " custom[";
    // Custom action IDs are runtime-local; their order and labels remain observable.
    for (const auto& action : n.custom_actions) Write(action.second);
    out << "] {\n";
    for (auto child : n.children) Semantics(index, child);
    out << "}\n";
  }

  std::ostringstream out;
};

} // namespace

std::string CaptureUiStructure(const FrameCommit& commit, const UiSemanticIndex& semantics) {
  SnapshotWriter writer;
  writer.out << "huxerui-ui-snapshot 1\nsemantics\n";
  if (commit.semantic_frame) writer.Semantics(semantics, commit.semantic_frame->root);
  writer.out << "render\n";
  writer.Render(commit.render_frame.scene.root);
  return writer.out.str();
}

} // namespace huxerui::detail
