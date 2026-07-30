#include "selection_area_internal.h"

#include "text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <huxerui/theme.h>

namespace huxerui {
namespace {

struct TextEntry {
  detail::MountedNode* node = nullptr;
  TextOffset start = 0;
  TextOffset length = 0;
  std::unique_ptr<detail::TextLayout> layout;
};

float ResolveFontSize(const detail::MountedNode& node) {
  return node.style.font_size.value_or(TextStyleKey::Default().font_size);
}

Rect ContentRect(const detail::MountedNode& node) {
  return {
      node.frame.x + node.style.padding.left,
      node.frame.y + node.style.padding.top,
      std::max(0.0F, node.frame.width - node.style.padding.Horizontal()),
      std::max(0.0F, node.frame.height - node.style.padding.Vertical()),
  };
}

std::optional<Rect> TransformBoundsBetween(
    const detail::PresentationTransform& source, const detail::PresentationTransform& target, Rect rect
) {
  const auto transform = [&](Point point) { return target.Inverse(source.Apply(point)); };
  const std::optional<Point> top_left = transform({rect.x, rect.y});
  const std::optional<Point> top_right = transform({rect.x + rect.width, rect.y});
  const std::optional<Point> bottom_left = transform({rect.x, rect.y + rect.height});
  const std::optional<Point> bottom_right = transform({rect.x + rect.width, rect.y + rect.height});
  if (!top_left.has_value() || !top_right.has_value() || !bottom_left.has_value() || !bottom_right.has_value()) {
    return std::nullopt;
  }
  const float left = std::min({top_left->x, top_right->x, bottom_left->x, bottom_right->x});
  const float right = std::max({top_left->x, top_right->x, bottom_left->x, bottom_right->x});
  const float top = std::min({top_left->y, top_right->y, bottom_left->y, bottom_right->y});
  const float bottom = std::max({top_left->y, top_right->y, bottom_left->y, bottom_right->y});
  return Rect{
      left,
      top,
      right - left,
      bottom - top,
  };
}

class SelectionAreaExtension final : public NodeExtension {
public:
  SelectionAreaExtension(MountedNode& node, const detail::SelectionAreaModifier&) {
    Update(node);
  }

  void Update(MountedNode& node, const detail::SelectionAreaModifier&) {
    Update(node);
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Frame().Contains(position);
  }

  PointerResult OnPointer(MountedNode&, const PointerEvent& event) override {
    if (event.device_kind == PointerDeviceKind::Touch) {
      return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
    }
    const Point host_position = node_->presentation.resolved_transform.Apply(event.position);
    switch (event.type) {
    case PointerEventType::Down:
      pointer_active_ = true;
      selection_ = TextSelection{HitOffset(host_position), HitOffset(host_position)};
      return PointerResult::Capture;
    case PointerEventType::Move:
      if (pointer_active_) {
        selection_.active = HitOffset(host_position);
        return PointerResult::Handled;
      }
      break;
    case PointerEventType::Up:
      if (pointer_active_) {
        selection_.active = HitOffset(host_position);
        pointer_active_ = false;
        return PointerResult::Handled;
      }
      break;
    case PointerEventType::Cancel:
      pointer_active_ = false;
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

  void Paint(const MountedNode&, DisplayList& display_list) const override {
    const TextRange selection = selection_.Range();
    if (selection.IsCollapsed()) {
      return;
    }
    Color color = selection_color_;
    color.alpha = std::min(color.alpha, 0.32F);
    for (const TextEntry& entry : entries_) {
      const TextOffset start = std::max(selection.start, entry.start);
      const TextOffset end = std::min(selection.end, entry.start + entry.length);
      if (start >= end || !entry.layout) {
        continue;
      }
      const Point origin{ContentRect(*entry.node).x, ContentRect(*entry.node).y};
      Color entry_color = color;
      entry_color.alpha *= std::clamp(entry.node->PresentationOpacity(), 0.0F, 1.0F);
      for (Rect rect : entry.layout->RangeRects({start - entry.start, end - entry.start})) {
        rect.x += origin.x;
        rect.y += origin.y;
        const std::optional<Rect> transformed = TransformBoundsBetween(
            entry.node->presentation.resolved_transform,
            node_->presentation.resolved_transform,
            rect
        );
        if (transformed.has_value()) {
          display_list.DrawRect(*transformed, entry_color);
        }
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

  bool CanPerform(TextEditingAction action, PlatformClipboard* clipboard) const {
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

  bool Perform(TextEditingAction action, PlatformClipboard* clipboard) {
    if (!CanPerform(action, clipboard)) {
      return false;
    }
    if (action == TextEditingAction::SelectAll) {
      selection_ = TextSelection{0, total_length_};
      return true;
    }
    const std::optional<std::string> selected = detail::Utf8TextInRange(document_, selection_.Range());
    return selected.has_value() && clipboard->WriteText(*selected);
  }

  bool SelectWord(Point position) {
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

  bool Extend(Point position, bool start_handle) {
    const TextRange range = selection_.Range();
    if (range.IsCollapsed()) {
      return false;
    }
    const TextOffset hit = HitOffset(position);
    selection_ = start_handle ? TextSelection{range.end, std::min(hit, range.end)}
                              : TextSelection{range.start, std::max(hit, range.start)};
    return true;
  }

  bool QueryGeometry(Rect& start, Rect& end) const {
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

  Color HandleColor() const noexcept {
    Color color = selection_color_;
    color.alpha *= node_ == nullptr ? 1.0F : std::clamp(node_->PresentationOpacity(), 0.0F, 1.0F);
    return color;
  }

private:
  void Update(MountedNode& node) {
    node_ = &static_cast<detail::MountedNode&>(node);
  }

  void CollectTextNodes(detail::MountedNode& node, std::vector<detail::MountedNode*>& nodes) {
    if (&node != node_ && node.kind == detail::NodeKind::SelectionArea) {
      return;
    }
    if (node.kind == detail::NodeKind::Text) {
      nodes.push_back(&node);
      return;
    }
    for (const std::unique_ptr<detail::MountedNode>& child : node.children) {
      CollectTextNodes(*child, nodes);
    }
  }

  void Rebuild(detail::MountedNode& node, PlatformHost& platform) {
    std::vector<detail::MountedNode*> nodes;
    CollectTextNodes(node, nodes);
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
          text,
          total_length_,
          length,
          platform.CreateTextLayout(text->text, ResolveFontSize(*text), width),
      });
      document_ += text->text;
      total_length_ += length;
    }
    selection_.anchor = std::clamp(selection_.anchor, TextOffset{0}, total_length_);
    selection_.active = std::clamp(selection_.active, TextOffset{0}, total_length_);
    selection_color_ = detail::ResolveThemeSpec(node.environment).colors.primary;
  }

  TextOffset HitOffset(Point position) const {
    if (entries_.empty()) {
      return 0;
    }
    const TextEntry* best = nullptr;
    float best_distance = std::numeric_limits<float>::infinity();
    for (const TextEntry& entry : entries_) {
      const Rect content =
          detail::TransformBounds(entry.node->presentation.resolved_transform, ContentRect(*entry.node));
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
    if (best == nullptr || !best->layout) {
      return 0;
    }
    const std::optional<Point> local = best->node->presentation.resolved_transform.Inverse(position);
    if (!local.has_value()) {
      return best->start;
    }
    const Rect content = ContentRect(*best->node);
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
    if (selected == nullptr || !selected->layout) {
      return std::nullopt;
    }
    const TextOffset local = std::clamp(offset - selected->start, TextOffset{0}, selected->length);
    Rect rect = selected->layout->CaretRect(local, TextAffinity::Downstream);
    const Rect content = ContentRect(*selected->node);
    rect.x += content.x;
    rect.y += content.y;
    return detail::TransformBounds(selected->node->presentation.resolved_transform, rect);
  }

  detail::MountedNode* node_ = nullptr;
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

const SelectionAreaExtension& FindSelectionAreaExtension(const detail::MountedNode& node) {
  return FindSelectionAreaExtension(const_cast<detail::MountedNode&>(node));
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

bool CanPerformSelectionAreaAction(const MountedNode& node, TextEditingAction action, PlatformClipboard* clipboard) {
  return FindSelectionAreaExtension(node).CanPerform(action, clipboard);
}

bool PerformSelectionAreaAction(MountedNode& node, TextEditingAction action, PlatformClipboard* clipboard) {
  return FindSelectionAreaExtension(node).Perform(action, clipboard);
}

bool SelectSelectionAreaWord(MountedNode& node, Point position) {
  return FindSelectionAreaExtension(node).SelectWord(position);
}

bool ExtendSelectionArea(MountedNode& node, Point position, bool start_handle) {
  return FindSelectionAreaExtension(node).Extend(position, start_handle);
}

bool QuerySelectionAreaGeometry(const MountedNode& node, Rect& start, Rect& end) {
  return FindSelectionAreaExtension(node).QueryGeometry(start, end);
}

Color SelectionAreaHandleColor(const MountedNode& node) {
  return FindSelectionAreaExtension(node).HandleColor();
}

} // namespace detail

SelectionArea::SelectionArea(View content) : View(MakeSelectionAreaSpec(std::move(content))) {}

} // namespace huxerui
