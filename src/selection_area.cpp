#include "selection_area_internal.h"

#include "geometry_internal.h"
#include "text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <huxerui/theme.h>

namespace huxerui {
namespace {

struct TextEntryGeometry {
  Rect content;
  Transform2D to_owner;
  float relative_opacity = 1.0F;
  bool valid = false;

  bool operator==(const TextEntryGeometry&) const = default;
};

struct TextEntry {
  std::uint64_t node_identity = 0;
  TextOffset start = 0;
  TextOffset length = 0;
  std::unique_ptr<detail::TextLayout> layout;
  TextEntryGeometry geometry;
};

float ResolveFontSize(const detail::MountedNode& node) {
  return node.style.font_size.value_or(TextStyle::Default().font_size);
}

Rect ContentRect(const detail::MountedNode& node) {
  return {
      node.bounds.x + node.style.padding.left,
      node.bounds.y + node.style.padding.top,
      std::max(0.0F, node.bounds.width - node.style.padding.Horizontal()),
      std::max(0.0F, node.bounds.height - node.style.padding.Vertical()),
  };
}

class SelectionAreaExtension final : public NodeExtension, public TextSelectionClient {
public:
  SelectionAreaExtension(MountedNode&, const detail::SelectionAreaModifier&) {}

  void Update(MountedNode&, const detail::SelectionAreaModifier&) {}

  bool PrepareGeometry(MountedNode& node) override {
    return ResolveGeometry(static_cast<detail::MountedNode&>(node));
  }

  TextSelectionClient* GetTextSelectionClient() noexcept override {
    return this;
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  PointerResult OnPointer(MountedNode&, const PointerEvent& event) override {
    if (event.device_kind == PointerDeviceKind::Touch) {
      return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
    }
    switch (event.type) {
    case PointerEventType::Down:
      pointer_active_ = true;
      selection_ = TextSelection{HitOffset(event.position), HitOffset(event.position)};
      InvalidatePaint();
      return PointerResult::Capture;
    case PointerEventType::Move:
      if (pointer_active_) {
        selection_.active = HitOffset(event.position);
        InvalidatePaint();
        return PointerResult::Handled;
      }
      break;
    case PointerEventType::Up:
      if (pointer_active_) {
        selection_.active = HitOffset(event.position);
        pointer_active_ = false;
        InvalidatePaint();
        return PointerResult::Handled;
      }
      break;
    case PointerEventType::Cancel:
      pointer_active_ = false;
      InvalidatePaint();
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

  void Paint(const MountedNode&, PaintContext& context) const override {
    const TextRange selection = selection_.Range();
    if (selection.IsCollapsed()) {
      return;
    }
    Color color = selection_color_;
    color.alpha = std::min(color.alpha, 0.32F);
    for (const TextEntry& entry : entries_) {
      const TextOffset start = std::max(selection.start, entry.start);
      const TextOffset end = std::min(selection.end, entry.start + entry.length);
      if (start >= end || !entry.layout || !entry.geometry.valid) {
        continue;
      }
      Color entry_color = color;
      entry_color.alpha *= entry.geometry.relative_opacity;
      for (Rect rect : entry.layout->RangeRects({start - entry.start, end - entry.start})) {
        rect.x += entry.geometry.content.x;
        rect.y += entry.geometry.content.y;
        context.DrawRect(detail::TransformBounds(entry.geometry.to_owner, rect), entry_color);
      }
    }
  }

  Size Measure(detail::MountedNode& node, PlatformHost& platform, Runtime& runtime, const Constraints& constraints) {
    Size size;
    if (!node.children.empty()) {
      size = detail::MeasureNode(*node.children.front(), constraints, platform, runtime);
    }
    Rebuild(node, platform);
    return constraints.Constrain(size);
  }

  bool CanPerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) const override {
    const TextRange selection = selection_.Range();
    switch (action) {
    case TextEditingAction::Copy:
      return clipboard != nullptr && !selection.IsCollapsed();
    case TextEditingAction::SelectAll:
      return total_length_ > 0 && selection != TextRange{0, total_length_};
    case TextEditingAction::Cut:
    case TextEditingAction::Paste:
      return false;
    }
    return false;
  }

  bool PerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) override {
    if (!CanPerformTextEditingAction(action, clipboard)) {
      return false;
    }
    if (action == TextEditingAction::SelectAll) {
      selection_ = TextSelection{0, total_length_};
      return true;
    }
    const std::optional<std::string> selected = detail::Utf8TextInRange(document_, selection_.Range());
    return selected.has_value() && clipboard->WriteText(*selected);
  }

  bool SelectWord(Point position) override {
    if (document_.empty()) {
      return false;
    }
    const std::optional<TextRange> range = detail::WordRangeAt(document_, HitOffset(position));
    if (!range.has_value()) {
      return false;
    }
    selection_ = TextSelection{range->start, range->end};
    return true;
  }

  bool ExtendSelection(Point position, bool start_handle) override {
    const TextRange range = selection_.Range();
    if (range.IsCollapsed()) {
      return false;
    }
    const TextOffset hit = HitOffset(position);
    selection_ = start_handle ? TextSelection{range.end, std::min(hit, range.end)}
                              : TextSelection{range.start, std::max(hit, range.start)};
    return true;
  }

  bool QuerySelectionGeometry(Rect& start, Rect& end) const override {
    const TextRange range = selection_.Range();
    if (range.IsCollapsed()) {
      return false;
    }
    const std::optional<Rect> start_caret = CaretRect(range.start, false);
    const std::optional<Rect> end_caret = CaretRect(range.end, true);
    if (!start_caret.has_value() || !end_caret.has_value()) {
      return false;
    }
    start = *start_caret;
    end = *end_caret;
    return true;
  }

  Color SelectionHandleColor() const noexcept override {
    return selection_color_;
  }

private:
  static void
  CollectTextNodes(detail::MountedNode& owner, detail::MountedNode& node, std::vector<detail::MountedNode*>& nodes) {
    if (&node != &owner && node.kind == detail::NodeKind::SelectionArea) {
      return;
    }
    if (node.kind == detail::NodeKind::Text) {
      nodes.push_back(&node);
      return;
    }
    for (const std::unique_ptr<detail::MountedNode>& child : node.children) {
      CollectTextNodes(owner, *child, nodes);
    }
  }

  static void CollectTextNodes(
      detail::MountedNode& owner,
      detail::MountedNode& node,
      std::unordered_map<std::uint64_t, detail::MountedNode*>& nodes
  ) {
    if (&node != &owner && node.kind == detail::NodeKind::SelectionArea) {
      return;
    }
    if (node.kind == detail::NodeKind::Text) {
      nodes.emplace(node.identity, &node);
      return;
    }
    for (const std::unique_ptr<detail::MountedNode>& child : node.children) {
      CollectTextNodes(owner, *child, nodes);
    }
  }

  void Rebuild(detail::MountedNode& node, PlatformHost& platform) {
    std::vector<detail::MountedNode*> nodes;
    CollectTextNodes(node, node, nodes);
    entries_.clear();
    document_.clear();
    total_length_ = 0;
    for (detail::MountedNode* text : nodes) {
      if (!entries_.empty()) {
        document_.push_back('\n');
        ++total_length_;
      }
      const TextOffset length = detail::Utf16Length(text->text).value_or(0);
      const float width = std::max(0.0F, text->measured_size.width - text->style.padding.Horizontal());
      entries_.push_back({
          text->identity,
          total_length_,
          length,
          platform.CreateTextLayout(text->text, ResolveFontSize(*text), width),
          {},
      });
      document_ += text->text;
      total_length_ += length;
    }
    selection_.anchor = std::clamp(selection_.anchor, TextOffset{0}, total_length_);
    selection_.active = std::clamp(selection_.active, TextOffset{0}, total_length_);
  }

  bool ResolveGeometry(detail::MountedNode& owner) {
    std::unordered_map<std::uint64_t, detail::MountedNode*> nodes;
    nodes.reserve(entries_.size());
    CollectTextNodes(owner, owner, nodes);
    const std::optional<Transform2D> host_to_owner = detail::InverseTransform(owner.presentation.resolved_transform);
    const float owner_opacity = owner.PresentationOpacity();
    bool changed = false;
    for (TextEntry& entry : entries_) {
      TextEntryGeometry geometry;
      const auto found = nodes.find(entry.node_identity);
      if (host_to_owner.has_value() && found != nodes.end()) {
        const detail::MountedNode& text = *found->second;
        geometry.content = ContentRect(text);
        geometry.to_owner = detail::ComposeTransform(*host_to_owner, text.presentation.resolved_transform);
        geometry.relative_opacity =
            owner_opacity > 0.0F ? std::clamp(text.PresentationOpacity() / owner_opacity, 0.0F, 1.0F) : 0.0F;
        geometry.valid = true;
      }
      if (entry.geometry != geometry) {
        entry.geometry = geometry;
        changed = true;
      }
    }
    const Color selection_color = detail::ResolveThemeSpec(owner.environment).colors.primary;
    if (selection_color_ != selection_color) {
      selection_color_ = selection_color;
      changed = true;
    }
    return changed;
  }

  TextOffset HitOffset(Point position) const {
    if (entries_.empty()) {
      return 0;
    }
    const TextEntry* best = nullptr;
    float best_distance = std::numeric_limits<float>::infinity();
    for (const TextEntry& entry : entries_) {
      if (!entry.geometry.valid) {
        continue;
      }
      const Rect content = detail::TransformBounds(entry.geometry.to_owner, entry.geometry.content);
      const float right = content.x + content.width;
      const float bottom = content.y + content.height;
      const float dx =
          position.x < content.x ? content.x - position.x : (position.x > right ? position.x - right : 0.0F);
      const float dy =
          position.y < content.y ? content.y - position.y : (position.y > bottom ? position.y - bottom : 0.0F);
      const float distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best = &entry;
        best_distance = distance;
      }
    }
    if (best == nullptr || !best->layout || !best->geometry.valid) {
      return 0;
    }
    const std::optional<Point> local = best->geometry.to_owner.Inverse(position);
    if (!local.has_value()) {
      return best->start;
    }
    const Rect content = best->geometry.content;
    const TextPosition hit = best->layout->HitTest({local->x - content.x, local->y - content.y});
    return std::clamp(best->start + hit.offset, best->start, best->start + best->length);
  }

  std::optional<Rect> CaretRect(TextOffset offset, bool prefer_previous) const {
    if (entries_.empty()) {
      return std::nullopt;
    }
    const TextEntry* selected = nullptr;
    for (const TextEntry& entry : entries_) {
      const TextOffset end = entry.start + entry.length;
      if (offset >= entry.start && offset <= end &&
          (!prefer_previous || offset != entry.start || selected == nullptr)) {
        selected = &entry;
        if (offset < end || !prefer_previous) {
          break;
        }
      }
      if (end <= offset) {
        selected = &entry;
      }
    }
    if (selected == nullptr || !selected->layout || !selected->geometry.valid) {
      return std::nullopt;
    }
    const TextOffset local = std::clamp(offset - selected->start, TextOffset{0}, selected->length);
    Rect rect = selected->layout->CaretRect(local, TextAffinity::Downstream);
    const Rect content = selected->geometry.content;
    rect.x += content.x;
    rect.y += content.y;
    return detail::TransformBounds(selected->geometry.to_owner, rect);
  }

  std::vector<TextEntry> entries_;
  std::string document_;
  TextSelection selection_;
  TextOffset total_length_ = 0;
  Color selection_color_ = Color::Rgb(103, 80, 164, 0.32F);
  bool pointer_active_ = false;
};

SelectionAreaExtension& FindSelectionAreaExtension(detail::MountedNode& node) {
  for (detail::NodeExtensionEntry& entry : node.extensions) {
    if (entry.descriptor == &detail::SelectionAreaModifier::Descriptor() && entry.extension) {
      return static_cast<SelectionAreaExtension&>(*entry.extension);
    }
  }
  throw std::logic_error("HuxerUI SelectionArea has no retained selection extension");
}

std::shared_ptr<detail::ViewSpec> MakeSelectionAreaSpec(View content) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::SelectionArea);
  spec->children.push_back(std::move(content));
  spec->focusable = true;
  spec->style.focus_ring_width = 0.0F;
  spec->modifiers.push_back(detail::MakeModifierSpec(detail::SelectionAreaModifier{}));
  return spec;
}

} // namespace

namespace detail {

const ModifierDescriptor& SelectionAreaModifier::Descriptor() {
  return ModifierDescriptorFor<SelectionAreaModifier, SelectionAreaExtension>();
}

Size MeasureSelectionArea(MountedNode& node, PlatformHost& platform, Runtime& runtime, const Constraints& constraints) {
  return FindSelectionAreaExtension(node).Measure(node, platform, runtime, constraints);
}

} // namespace detail

SelectionArea::SelectionArea(View content) : View(MakeSelectionAreaSpec(std::move(content))) {}

} // namespace huxerui
