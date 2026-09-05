#include "selection_area_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <huxerui/gesture.h>
#include <huxerui/theme.h>

#include "runtime/mounted_node_internal.h"
#include "graphics/geometry_internal.h"
#include "text/text_input_internal.h"
#include "internal_access.h"
#include "text/text_internal.h"

namespace huxerui {
namespace detail {

namespace {

std::optional<std::size_t> FindBlock(const TextSelectionSource& source, TextBlockId id) {
  const auto index = source.IndexOf(id);
  if (index && (*index >= source.Count() || source.IdAt(*index) != id)) {
    throw std::invalid_argument("HuxerUI text selection source returned an inconsistent block index");
  }
  return index;
}

// Treat the changed middle as one replacement between scalar-aligned common prefix and suffix, not an edit history.
TextOffset RemapOffset(const AttributedText& old_text, const AttributedText& new_text, TextOffset offset) {
  if (InternalAccess::SameBody(old_text, new_text)) {
    return offset;
  }
  const auto& before = old_text.PlainText();
  const auto& after = new_text.PlainText();
  std::size_t prefix_bytes = 0;
  TextOffset prefix = 0;
  while (prefix_bytes < before.size() && prefix_bytes < after.size()) {
    Utf8CodePoint old_point;
    Utf8CodePoint new_point;
    DecodeCodePoint(before, prefix_bytes, old_point);
    DecodeCodePoint(after, prefix_bytes, new_point);
    if (old_point.value != new_point.value) {
      break;
    }
    prefix_bytes += old_point.byte_length;
    prefix += old_point.value > 0xFFFFU ? 2 : 1;
  }
  if (offset <= prefix) {
    // Existing endpoints stay before newly appended text, including combining marks delivered by a later delta.
    return offset;
  }
  std::size_t old_end = before.size();
  std::size_t new_end = after.size();
  TextOffset suffix = 0;
  while (old_end > prefix_bytes && new_end > prefix_bytes) {
    std::size_t old_start = old_end - 1;
    std::size_t new_start = new_end - 1;
    while ((static_cast<unsigned char>(before[old_start]) & 0xC0U) == 0x80U) {
      --old_start;
    }
    while ((static_cast<unsigned char>(after[new_start]) & 0xC0U) == 0x80U) {
      --new_start;
    }
    const auto old_piece = std::string_view(before).substr(old_start, old_end - old_start);
    const auto new_piece = std::string_view(after).substr(new_start, new_end - new_start);
    if (old_piece != new_piece) {
      break;
    }
    suffix += old_end - old_start == 4 ? 2 : 1;
    old_end = old_start;
    new_end = new_start;
  }
  return offset >= old_text.Length() - suffix ? new_text.Length() - (old_text.Length() - offset) : prefix;
}

void ValidatePosition(const TextSelectionSource& source, const LogicalTextPosition& position) {
  const auto index = FindBlock(source, position.block);
  if (!index) {
    throw std::invalid_argument("HuxerUI selection position must identify an existing block");
  }
  const auto text = source.BlockAt(*index).text;
  static_cast<void>(text.TextInRange({position.position.offset, position.position.offset}));
}

} // namespace

void LogicalTextSelection::SetSource(std::shared_ptr<const TextSelectionSource> source) {
  if (source == source_) {
    return;
  }
  // Remap only endpoint blocks, and keep the old snapshot intact if a source lookup rejects the replacement.
  auto anchor = anchor_;
  auto active = active_;
  const auto remap = [&](std::optional<LogicalTextPosition>& endpoint) {
    if (!endpoint || !source_ || !source) {
      endpoint.reset();
      return;
    }
    const auto old_index = FindBlock(*source_, endpoint->block);
    const auto new_index = FindBlock(*source, endpoint->block);
    if (!old_index || !new_index) {
      endpoint.reset();
      return;
    }
    endpoint->position.offset =
        RemapOffset(source_->BlockAt(*old_index).text, source->BlockAt(*new_index).text, endpoint->position.offset);
  };
  remap(anchor);
  remap(active);
  source_ = std::move(source);
  anchor_ = anchor;
  active_ = active;
  if (!anchor_ || !active_) {
    Clear();
  }
}

void LogicalTextSelection::Clear() noexcept {
  anchor_.reset();
  active_.reset();
}

void LogicalTextSelection::Select(LogicalTextPosition anchor, LogicalTextPosition active) {
  if (!source_) {
    throw std::logic_error("HuxerUI logical selection requires a committed text source");
  }
  ValidatePosition(*source_, anchor);
  ValidatePosition(*source_, active);
  anchor_ = anchor;
  active_ = active;
}

void LogicalTextSelection::Extend(LogicalTextPosition active) {
  Select(anchor_.value_or(active), active);
}

std::optional<LogicalTextRange> LogicalTextSelection::Range() const {
  if (!source_ || !anchor_ || !active_) {
    return std::nullopt;
  }
  // Resolve ordering from the current snapshot; retained block IDs are not document indexes.
  const auto anchor_index = FindBlock(*source_, anchor_->block);
  const auto active_index = FindBlock(*source_, active_->block);
  if (!anchor_index || !active_index ||
      (*anchor_index == *active_index && anchor_->position.offset == active_->position.offset)) {
    return std::nullopt;
  }
  if (std::pair{*anchor_index, anchor_->position.offset} < std::pair{*active_index, active_->position.offset}) {
    return LogicalTextRange{*anchor_, *active_, *anchor_index, *active_index};
  }
  return LogicalTextRange{*active_, *anchor_, *active_index, *anchor_index};
}

bool LogicalTextSelection::SelectAll() {
  if (!source_ || source_->Count() == 0) {
    return false;
  }
  // Select All needs only document endpoints, even when most blocks have no mounted representation.
  const auto last_index = source_->Count() - 1;
  const auto last = source_->BlockAt(last_index);
  const LogicalTextPosition start{source_->IdAt(0), {0, TextAffinity::Downstream}};
  const LogicalTextPosition end{source_->IdAt(last_index), {last.text.Length(), TextAffinity::Upstream}};
  const auto current = Range();
  if (current && current->start.block == start.block && current->start.position.offset == 0 &&
      current->end.block == end.block && current->end.position.offset == end.position.offset) {
    return false;
  }
  Select(start, end);
  return Range().has_value();
}

std::optional<std::string> LogicalTextSelection::Copy() const {
  const auto range = Range();
  if (!range) {
    return std::nullopt;
  }
  // Read logical content, including unmounted blocks; separators belong only between traversed blocks.
  std::string result;
  for (std::size_t index = range->start_index; index <= range->end_index; ++index) {
    const auto block = source_->BlockAt(index);
    const auto start = index == range->start_index ? range->start.position.offset : 0;
    const auto end = index == range->end_index ? range->end.position.offset : block.text.Length();
    result += block.text.TextInRange({start, end});
    if (index != range->end_index) {
      if (!Utf16Length(block.separator)) {
        throw std::invalid_argument("HuxerUI text selection separator must be valid UTF-8");
      }
      result += block.separator;
    }
  }
  return result;
}

} // namespace detail

namespace {

struct SelectionClip {
  Rect bounds;
  CornerRadii radii;
  Transform2D to_owner;

  bool operator==(const SelectionClip&) const = default;
};

struct TextEntryGeometry {
  Rect content;
  Point origin;
  Transform2D to_owner;
  std::vector<SelectionClip> clips;
  float relative_opacity = 1.0F;
  bool valid = false;

  bool operator==(const TextEntryGeometry&) const = default;
};

// Mounted identity keys geometry reuse; block identity maps that geometry into the independent logical selection.
struct TextEntry {
  std::uint64_t node_identity = 0;
  TextBlockId block = 0;
  AttributedText text;
  TextStyle style;
  TextLayoutOptions options;
  float width = 0.0F;
  std::shared_ptr<detail::TextLayout> layout;
  TextEntryGeometry geometry;
};

class DiscoveredTextSource final : public TextSelectionSource {
public:
  explicit DiscoveredTextSource(const std::vector<TextEntry>& entries) {
    blocks_.reserve(entries.size());
    for (const auto& entry : entries) {
      indexes_.emplace(entry.block, blocks_.size());
      blocks_.emplace_back(entry.block, entry.text);
    }
  }

  std::size_t Count() const noexcept override { return blocks_.size(); }
  TextBlockId IdAt(std::size_t index) const override { return blocks_.at(index).first; }
  std::optional<std::size_t> IndexOf(TextBlockId id) const override {
    const auto found = indexes_.find(id);
    return found == indexes_.end() ? std::nullopt : std::optional(found->second);
  }
  TextSelectionBlock BlockAt(std::size_t index) const override { return {blocks_.at(index).second, "\n"}; }

private:
  std::vector<std::pair<TextBlockId, AttributedText>> blocks_;
  std::unordered_map<TextBlockId, std::size_t> indexes_;
};

class SelectionAreaExtension final : public NodeExtension, public TextSelectionClient {
public:
  SelectionAreaExtension(MountedNode&, const detail::SelectionAreaModifier& modifier) : pending_source_(modifier.source) {}

  void Update(MountedNode&, const detail::SelectionAreaModifier& modifier) { pending_source_ = modifier.source; }

  PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer& measurer) override {
    auto* platform = dynamic_cast<PlatformAdapter*>(&measurer);
    if (!platform) {
      throw std::logic_error("HuxerUI selection geometry requires a platform text layout service");
    }
    enabled_ = node.IsEnabled();
    pointer_slop_ = platform->GestureDefaults().pointer_slop;
    if (!enabled_) {
      pointer_active_ = false;
    }
    return ReconcileEntries(static_cast<detail::MountedNode&>(node), *platform) ? PaintInvalidation::Foreground
                                                                            : PaintInvalidation::None;
  }

  TextSelectionClient* GetTextSelectionClient() noexcept override { return this; }

  bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (event.type == PointerEventType::Cancel || !node.IsEnabled()) {
      pointer_active_ = false;
      return PointerResult::Handled;
    }
    if (event.device_kind == PointerDeviceKind::Touch) {
      return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down && event.changed_button != PointerButton::None &&
        event.changed_button != PointerButton::Primary) {
      return PointerResult::Ignored;
    }
    const auto hit = HitPosition(event.position);
    if (!hit) {
      if (event.type == PointerEventType::Up) {
        pointer_active_ = false;
      }
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      pointer_active_ = true;
      dragging_ = false;
      down_ = event.position;
      const bool extending = event.modifiers.shift && selection_.Anchor().has_value();
      if (extending) {
        selection_.Extend(*hit);
      } else {
        selection_.Select(*hit, *hit);
      }
      InvalidatePaint();
      // Shift-click extends selection immediately; a plain click leaves a child link eligible until dragging wins.
      return extending ? PointerResult::CancelTarget : PointerResult::Observe;
    }
    if (pointer_active_) {
      if (!dragging_ && std::hypot(event.position.x - down_.x, event.position.y - down_.y) < pointer_slop_) {
        pointer_active_ = event.type != PointerEventType::Up;
        return PointerResult::Observe;
      }
      dragging_ = true;
      selection_.Extend(*hit);
      pointer_active_ = event.type != PointerEventType::Up;
      InvalidatePaint();
      return PointerResult::CancelTarget;
    }
    return PointerResult::Ignored;
  }

  void PaintAboveContent(const MountedNode&, PaintContext& context) const override {
    const auto selected = selection_.Range();
    if (!selected) {
      return;
    }
    for (const auto& entry : entries_) {
      const auto range = SelectedRange(entry, *selected);
      if (!range || !entry.geometry.valid || !entry.layout) {
        continue;
      }
      Color color = selection_color_;
      color.alpha = std::min(color.alpha, 0.32F) * entry.geometry.relative_opacity;
      // Install each clip in its original local space, then return to the owner space before drawing the text.
      for (const auto& clip : entry.geometry.clips) {
        context.PushTransform(clip.to_owner);
        context.PushClip(clip.bounds, clip.radii);
        context.PushTransform(*detail::InverseTransform(clip.to_owner));
      }
      context.PushTransform(entry.geometry.to_owner);
      context.PushClip(entry.geometry.content);
      for (auto rect : entry.layout->RangeRects(*range)) {
        rect.x += entry.geometry.origin.x;
        rect.y += entry.geometry.origin.y;
        context.DrawRect(rect, color);
      }
      context.PopClip();
      context.PopTransform();
      for (std::size_t index = 0; index < entry.geometry.clips.size(); ++index) {
        context.PopTransform();
        context.PopClip();
        context.PopTransform();
      }
    }
  }

  bool CanPerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) const override {
    if (!enabled_) {
      return false;
    }
    switch (action) {
    case TextEditingAction::Copy:
      return clipboard && selection_.Range().has_value();
    case TextEditingAction::SelectAll: {
      const auto& source = selection_.Source();
      if (!source || source->Count() == 0) {
        return false;
      }
      const auto last_index = source->Count() - 1;
      const auto last_length = source->BlockAt(last_index).text.Length();
      const auto range = selection_.Range();
      return (last_index != 0 || last_length != 0) &&
             (!range || range->start_index != 0 || range->start.position.offset != 0 ||
              range->end_index != last_index || range->end.position.offset != last_length);
    }
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
      const bool changed = selection_.SelectAll();
      if (changed) {
        InvalidatePaint();
      }
      return changed;
    }
    const auto text = selection_.Copy();
    return text && clipboard->WriteText(*text);
  }

  bool ClearSelection() override {
    if (!enabled_ || !selection_.Anchor()) {
      return false;
    }
    selection_.Clear();
    InvalidatePaint();
    return true;
  }

  bool SelectWord(Point position) override {
    const auto hit = enabled_ ? HitPosition(position) : std::nullopt;
    if (!hit) {
      return false;
    }
    const auto index = selection_.Source()->IndexOf(hit->block);
    const auto text = selection_.Source()->BlockAt(*index).text;
    const auto range = detail::WordRangeAt(text.PlainText(), hit->position.offset);
    if (!range) {
      return false;
    }
    selection_.Select({hit->block, {range->start}}, {hit->block, {range->end, TextAffinity::Upstream}});
    InvalidatePaint();
    return true;
  }

  bool ExtendSelection(Point position, bool start_handle) override {
    const auto range = selection_.Range();
    const auto hit = enabled_ ? HitPosition(position) : std::nullopt;
    if (!range || !hit) {
      return false;
    }
    const auto hit_index = *selection_.Source()->IndexOf(hit->block);
    const auto order = std::pair{hit_index, hit->position.offset};
    if (start_handle) {
      const auto end = std::pair{range->end_index, range->end.position.offset};
      selection_.Select(range->end, order <= end ? *hit : range->end);
    } else {
      const auto start = std::pair{range->start_index, range->start.position.offset};
      selection_.Select(range->start, order >= start ? *hit : range->start);
    }
    InvalidatePaint();
    return true;
  }

  std::optional<TextSelectionGeometry> QuerySelectionGeometry() const override {
    const auto selected = selection_.Range();
    if (!selected) {
      return std::nullopt;
    }
    // Either endpoint may be offscreen; the toolbar can still anchor to a visible fragment between them.
    TextSelectionGeometry result{CaretRect(selected->start), CaretRect(selected->end), {}};
    for (const auto& entry : entries_) {
      const auto range = SelectedRange(entry, *selected);
      if (!range || !entry.layout || !entry.geometry.valid) {
        continue;
      }
      for (const auto& rect : entry.layout->RangeRects(*range)) {
        const Rect visible = VisibleRect(entry, rect);
        if (!visible.IsEmpty()) {
          result.toolbar_anchor = visible;
          return result;
        }
      }
    }
    result.toolbar_anchor = result.start ? result.start : result.end;
    return result;
  }

  Color SelectionHandleColor() const noexcept override { return selection_color_; }

private:
  struct TextNode {
    detail::MountedNode* node;
    std::vector<SelectionClip> clips;
  };

  static void CollectTextNodes(detail::MountedNode& owner, detail::MountedNode& node, Transform2D to_owner,
      std::vector<SelectionClip> clips, std::vector<TextNode>& nodes) {
    // Nested areas own their own selection source; virtualized-away nodes contribute no retained geometry here.
    if (&node != &owner && (!node.participates_in_layout || node.kind == detail::NodeKind::SelectionArea)) {
      return;
    }
    if (node.kind == detail::NodeKind::Text) {
      nodes.push_back({&node, std::move(clips)});
      return;
    }
    const auto transform = detail::ComposeTransform(to_owner, node.presentation.resolved_transform);
    if (node.properties.clip_children) {
      clips.push_back({node.Bounds(), node.resolved_corner_radii, transform});
    }
    if (detail::IsScrollContainer(node)) {
      clips.push_back({node.ContentBounds(), {}, transform});
    }
    for (const auto& child : node.children) {
      CollectTextNodes(owner, *child, to_owner, clips, nodes);
    }
  }

  bool ReconcileEntries(detail::MountedNode& owner, PlatformAdapter& platform) {
    const auto inverse = detail::InverseTransform(owner.presentation.resolved_transform);
    std::vector<TextNode> nodes;
    CollectTextNodes(owner, owner, inverse.value_or(Transform2D{}), {}, nodes);
    std::unordered_map<std::uint64_t, std::size_t> previous;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      previous.emplace(entries_[index].node_identity, index);
    }
    std::vector<TextEntry> next;
    next.reserve(nodes.size());
    std::unordered_set<TextBlockId> bound_blocks;
    bool changed = pending_source_ != explicit_source_;
    bool document_changed = changed;
    for (auto& info : nodes) {
      auto& node = *info.node;
      const auto* binding = node.LayoutValue<detail::TextSelectionBlockKey>();
      // Explicit sources opt in complete Text blocks; unbound labels remain decorative rather than copied content.
      if (pending_source_ && !binding) {
        continue;
      }
      const TextBlockId block = pending_source_ ? *binding : node.identity;
      if (!bound_blocks.insert(block).second) {
        throw std::invalid_argument("HuxerUI selection source block must be bound to only one Text");
      }
      const auto found = previous.find(node.identity);
      TextEntry entry;
      if (found != previous.end()) {
        entry = entries_[found->second];
        document_changed = document_changed || found->second != next.size();
      } else {
        changed = true;
        document_changed = true;
      }
      if (pending_source_) {
        const auto index = pending_source_->IndexOf(block);
        if (!index || *index >= pending_source_->Count() || pending_source_->IdAt(*index) != block) {
          throw std::invalid_argument("HuxerUI Text selection block must exist in its source");
        }
        const auto source_text = pending_source_->BlockAt(*index).text;
        // Copy and hit testing must refer to the same body version, even when visual attributes differ.
        if (!detail::InternalAccess::SameBody(source_text, node.text)) {
          throw std::invalid_argument("HuxerUI Text body must match its committed selection source block");
        }
      }
      const bool body_changed = !detail::InternalAccess::SameBody(entry.text, node.text);
      const bool layout_changed =
          !detail::TextLayoutInputsEqual(entry.text, entry.style.font, node.text, node.properties.text_style.font);
      entry.text = node.text;
      document_changed = document_changed || body_changed || entry.block != block;
      const auto content = node.ContentBounds();
      if (!entry.layout || layout_changed ||
          entry.options != node.properties.text_layout_options || entry.width != content.width) {
        entry.style = node.properties.text_style;
        entry.options = node.properties.text_layout_options;
        entry.width = content.width;
        entry.layout = detail::GetParagraphLayout(node, platform);
        changed = true;
      }
      entry.node_identity = node.identity;
      entry.block = block;
      const auto transform =
          detail::ComposeTransform(inverse.value_or(Transform2D{}), node.presentation.resolved_transform);
      // Foreground painting already inherits owner opacity, so only the descendant's relative factor is applied.
      const float opacity = owner.PresentationOpacity();
      TextEntryGeometry geometry{
          content,
          {content.x, content.y},
          transform,
          std::move(info.clips),
          opacity > 0.0F ? std::clamp(node.PresentationOpacity() / opacity, 0.0F, 1.0F) : 0.0F,
          entry.layout && inverse.has_value() && detail::InverseTransform(transform).has_value()
      };
      if (entry.layout) {
        const float remaining = std::max(0.0F, content.height - entry.layout->Measure().height);
        if (entry.options.vertical_align == TextVerticalAlign::Center) {
          geometry.origin.y += remaining * 0.5F;
        } else if (entry.options.vertical_align == TextVerticalAlign::Bottom) {
          geometry.origin.y += remaining;
        }
      }
      changed = changed || geometry != entry.geometry;
      entry.geometry = std::move(geometry);
      next.push_back(std::move(entry));
    }
    document_changed = document_changed || next.size() != entries_.size();
    changed = changed || document_changed;
    // Publish entries and source together only after validation and endpoint remapping have both succeeded.
    auto next_selection = selection_;
    if (pending_source_) {
      next_selection.SetSource(pending_source_);
    } else if (document_changed || !selection_.Source()) {
      // Presentation-only changes skip this branch, preserving the discovered logical snapshot.
      next_selection.SetSource(std::make_shared<const DiscoveredTextSource>(next));
    }
    entries_ = std::move(next);
    explicit_source_ = pending_source_;
    selection_ = std::move(next_selection);
    const Color color = detail::ResolveThemeSpec(owner.environment).colors.primary;
    changed = changed || selection_color_ != color;
    selection_color_ = color;
    return changed;
  }

  std::optional<TextRange> SelectedRange(const TextEntry& entry, const detail::LogicalTextRange& selected) const {
    const auto index = selection_.Source()->IndexOf(entry.block);
    if (!index || *index < selected.start_index || *index > selected.end_index) {
      return std::nullopt;
    }
    const TextRange range{
        *index == selected.start_index ? selected.start.position.offset : 0,
        *index == selected.end_index ? selected.end.position.offset : entry.text.Length()
    };
    return range.IsCollapsed() ? std::nullopt : std::optional(range);
  }

  static Rect VisibleRect(const TextEntry& entry, Rect rect) {
    rect.x += entry.geometry.origin.x;
    rect.y += entry.geometry.origin.y;
    rect = rect.Intersection(entry.geometry.content);
    rect = detail::TransformBounds(entry.geometry.to_owner, rect);
    for (const auto& clip : entry.geometry.clips) {
      rect = rect.Intersection(detail::TransformBounds(clip.to_owner, clip.bounds));
    }
    return rect;
  }

  std::optional<detail::LogicalTextPosition> HitPosition(Point position) const {
    const TextEntry* best = nullptr;
    float best_distance = std::numeric_limits<float>::infinity();
    for (const auto& entry : entries_) {
      if (!entry.geometry.valid || entry.geometry.relative_opacity <= 0.0F) {
        continue;
      }
      Rect content = detail::TransformBounds(entry.geometry.to_owner, entry.geometry.content);
      for (const auto& clip : entry.geometry.clips) {
        content = content.Intersection(detail::TransformBounds(clip.to_owner, clip.bounds));
      }
      if (content.IsEmpty()) {
        continue;
      }
      const float dx = position.x - std::clamp(position.x, content.x, content.x + content.width);
      const float dy = position.y - std::clamp(position.y, content.y, content.y + content.height);
      const float distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best = &entry;
        best_distance = distance;
      }
    }
    if (!best) {
      return std::nullopt;
    }
    // Choose the nearest visible paragraph in owner space, then query its untransformed paragraph layout.
    const auto local = best->geometry.to_owner.Inverse(position);
    TextPosition hit = best->layout->HitTest(*local - best->geometry.origin);
    hit.offset = std::clamp(hit.offset, TextOffset{0}, best->text.Length());
    // Layout hits may land inside a cluster; style boundaries must not become extra selection stops.
    if (hit.offset > 0 && hit.offset < best->text.Length()) {
      const TextOffset previous = best->layout->PreviousCaretOffset(hit.offset);
      const TextOffset next = best->layout->NextCaretOffset(previous);
      if (next != hit.offset) {
        hit.offset = hit.affinity == TextAffinity::Upstream ? next : previous;
      }
    }
    return detail::LogicalTextPosition{best->block, hit};
  }

  std::optional<Rect> CaretRect(const detail::LogicalTextPosition& position) const {
    for (const auto& entry : entries_) {
      if (entry.block != position.block || !entry.layout || !entry.geometry.valid ||
          entry.geometry.relative_opacity <= 0.0F) {
        continue;
      }
      Rect rect = entry.layout->CaretRect(position.position.offset, position.position.affinity);
      // An end caret at the right edge still belongs to this paragraph's visible boundary.
      rect.x = std::min(rect.x, std::max(0.0F, entry.geometry.content.width - rect.width));
      const Rect visible = VisibleRect(entry, rect);
      return visible.IsEmpty() ? std::nullopt : std::optional(visible);
    }
    return std::nullopt;
  }

  std::vector<TextEntry> entries_;
  std::shared_ptr<const TextSelectionSource> pending_source_;
  std::shared_ptr<const TextSelectionSource> explicit_source_;
  detail::LogicalTextSelection selection_;
  Color selection_color_ = Color::Rgb(103, 80, 164, 0.32F);
  bool enabled_ = true;
  bool pointer_active_ = false;
  bool dragging_ = false;
  Point down_;
  float pointer_slop_ = 6.0F;
};

std::shared_ptr<detail::ViewSpec> MakeSelectionAreaSpec(View content) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::SelectionArea);
  spec->children.push_back(std::move(content));
  spec->focusable = true;
  spec->modifiers.push_back(detail::MakeModifierSpec(detail::SelectionAreaModifier{}));
  return spec;
}

} // namespace

namespace detail {

const ModifierDescriptor& SelectionAreaModifier::Descriptor() {
  return ModifierDescriptorFor<SelectionAreaModifier, SelectionAreaExtension>();
}

Size MeasureSelectionArea(MountedNode& node, PlatformAdapter& platform, Runtime& runtime,
    const Constraints& constraints, EdgeInsets safe_area, const WindowTitleBarMetrics* title_bar_metrics) {
  const Size size = node.children.empty()
                        ? Size{}
                        : MeasureNode(*node.children.front(), constraints, platform, runtime, safe_area, title_bar_metrics);
  return constraints.Constrain(size);
}

} // namespace detail

SelectionArea::SelectionArea(View content) : View(MakeSelectionAreaSpec(std::move(content))) {}

SelectionArea SelectionArea::Source(std::shared_ptr<const TextSelectionSource> source) && {
  SetModifier(detail::MakeModifierSpec(detail::SelectionAreaModifier{std::move(source)}));
  return std::move(*this);
}

} // namespace huxerui
