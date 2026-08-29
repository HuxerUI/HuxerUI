#include "android_accessibility.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace huxerui::detail {

namespace {

class Writer final {
public:
  template <typename Value>
    requires std::is_integral_v<Value>
  void Integer(Value value) {
    using Unsigned = std::make_unsigned_t<Value>;
    std::uint64_t bits = static_cast<Unsigned>(value);
    for (std::size_t byte = 0; byte < sizeof(Value); ++byte) {
      bytes_.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
      bits >>= 8U;
    }
  }

  void Float(float value) {
    Integer(std::bit_cast<std::uint32_t>(value));
  }

  void Double(double value) {
    Integer(std::bit_cast<std::uint64_t>(value));
  }

  void String(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error("HuxerUI Android semantic string is too large");
    }
    Integer(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  std::vector<std::uint8_t> Finish() && {
    return std::move(bytes_);
  }

private:
  std::vector<std::uint8_t> bytes_;
};

std::int32_t AndroidNodeId(SemanticNodeId id) {
  if (id > static_cast<SemanticNodeId>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error("HuxerUI Android semantic node id exceeds the virtual view id range");
  }
  return static_cast<std::int32_t>(id);
}

std::int64_t OptionalSize(const std::optional<std::size_t>& value) {
  if (!value.has_value()) {
    return -1;
  }
  if (*value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::overflow_error("HuxerUI Android semantic collection size is too large");
  }
  return static_cast<std::int64_t>(*value);
}

void OptionalBool(Writer& writer, const std::optional<bool>& value) {
  writer.Integer<std::int8_t>(value.has_value() ? (*value ? 1 : 0) : -1);
}

void PlatformViewIdentity(Writer& writer, const std::optional<std::uint64_t>& identity) {
  writer.Integer<std::uint8_t>(identity.has_value() ? 1 : 0);
  if (!identity.has_value()) {
    return;
  }
  if (*identity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::overflow_error("HuxerUI Android PlatformView semantic identity exceeds the Java long range");
  }
  writer.Integer(static_cast<std::int64_t>(*identity));
}

std::int32_t AndroidRole(SemanticRole role) noexcept {
  switch (role) {
  case SemanticRole::Generic:
    return 0;
  case SemanticRole::Text:
    return 1;
  case SemanticRole::Heading:
    return 2;
  case SemanticRole::Image:
    return 3;
  case SemanticRole::Button:
    return 4;
  case SemanticRole::Link:
    return 5;
  case SemanticRole::Checkbox:
    return 6;
  case SemanticRole::RadioButton:
    return 7;
  case SemanticRole::Switch:
    return 8;
  case SemanticRole::Slider:
    return 9;
  case SemanticRole::ProgressIndicator:
    return 10;
  case SemanticRole::TextField:
    return 11;
  case SemanticRole::SearchField:
    return 12;
  case SemanticRole::Tab:
    return 13;
  case SemanticRole::TabList:
    return 14;
  case SemanticRole::Menu:
    return 15;
  case SemanticRole::MenuItem:
    return 16;
  case SemanticRole::Dialog:
    return 17;
  case SemanticRole::Navigation:
    return 18;
  case SemanticRole::List:
    return 19;
  case SemanticRole::ListItem:
    return 20;
  case SemanticRole::Grid:
    return 21;
  case SemanticRole::GridCell:
    return 22;
  case SemanticRole::ScrollView:
    return 23;
  case SemanticRole::ComboBox:
    return 24;
  }
  return 0;
}

std::uint64_t AndroidActionMask(AndroidSemanticAction action) noexcept {
  return std::uint64_t{1} << static_cast<std::uint32_t>(action);
}

std::uint64_t AndroidActions(const SemanticNode& node) noexcept {
  std::uint64_t result = 0;
  const auto include = [&node, &result](SemanticActionKind source, AndroidSemanticAction target) {
    if ((node.actions & SemanticActionMask(source)) != 0) {
      result |= AndroidActionMask(target);
    }
  };
  include(SemanticActionKind::Activate, AndroidSemanticAction::Activate);
  include(SemanticActionKind::Focus, AndroidSemanticAction::Focus);
  include(SemanticActionKind::SetText, AndroidSemanticAction::SetText);
  include(SemanticActionKind::SetSelection, AndroidSemanticAction::SetSelection);
  include(SemanticActionKind::SetValue, AndroidSemanticAction::SetValue);
  include(SemanticActionKind::Increment, AndroidSemanticAction::Increment);
  include(SemanticActionKind::Decrement, AndroidSemanticAction::Decrement);
  include(SemanticActionKind::Scroll, AndroidSemanticAction::Scroll);
  include(SemanticActionKind::ShowOnScreen, AndroidSemanticAction::ShowOnScreen);
  include(SemanticActionKind::Expand, AndroidSemanticAction::Expand);
  include(SemanticActionKind::Collapse, AndroidSemanticAction::Collapse);
  include(SemanticActionKind::Dismiss, AndroidSemanticAction::Dismiss);
  include(SemanticActionKind::Custom, AndroidSemanticAction::Custom);
  return result;
}

std::int8_t AndroidCheckedState(const std::optional<SemanticCheckedState>& checked) noexcept {
  if (!checked.has_value()) {
    return -1;
  }
  switch (*checked) {
  case SemanticCheckedState::Unchecked:
    return 0;
  case SemanticCheckedState::Checked:
    return 1;
  case SemanticCheckedState::Mixed:
    return 2;
  }
  return -1;
}

void EncodeNode(Writer& writer, const SemanticNode& node) {
  writer.Integer(AndroidNodeId(node.id));
  writer.Integer(node.parent.has_value() ? AndroidNodeId(*node.parent) : -1);
  PlatformViewIdentity(writer, node.platform_view_identity);
  writer.Integer(AndroidRole(node.role));
  if (node.children.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error("HuxerUI Android semantic node has too many children");
  }
  writer.Integer(static_cast<std::int32_t>(node.children.size()));
  for (SemanticNodeId child : node.children) {
    writer.Integer(AndroidNodeId(child));
  }

  writer.Integer(AndroidActions(node));
  std::uint8_t flags = 0;
  flags |= node.enabled ? 1U << 0U : 0;
  flags |= node.focused ? 1U << 1U : 0;
  flags |= node.multiline ? 1U << 2U : 0;
  flags |= node.secure ? 1U << 3U : 0;
  flags |= node.offscreen ? 1U << 4U : 0;
  writer.Integer(flags);

  writer.String(node.label);
  writer.String(node.value);
  writer.String(node.placeholder);
  writer.String(node.hint);
  writer.String(node.state_description);
  writer.String(node.error);
  writer.String(node.identifier);

  writer.Integer(AndroidCheckedState(node.checked));
  OptionalBool(writer, node.selected);
  OptionalBool(writer, node.expanded);
  OptionalBool(writer, node.busy);
  OptionalBool(writer, node.read_only);
  OptionalBool(writer, node.required);
  OptionalBool(writer, node.invalid);
  writer.Integer<std::int32_t>(node.heading_level.has_value() ? static_cast<std::int32_t>(*node.heading_level) : -1);

  writer.Integer<std::uint8_t>(node.range.has_value() ? 1 : 0);
  if (node.range.has_value()) {
    writer.Double(node.range->minimum);
    writer.Double(node.range->maximum);
    writer.Double(node.range->current);
    writer.Integer<std::uint8_t>(node.range->step.has_value() ? 1 : 0);
    if (node.range->step.has_value()) {
      writer.Double(*node.range->step);
    }
  }

  writer.Integer<std::uint8_t>(node.text_selection.has_value() ? 1 : 0);
  if (node.text_selection.has_value()) {
    writer.Integer<std::int64_t>(node.text_selection->start);
    writer.Integer<std::int64_t>(node.text_selection->end);
  }

  writer.Integer<std::uint8_t>(node.scroll.has_value() ? 1 : 0);
  if (node.scroll.has_value()) {
    writer.Integer<std::int32_t>(node.scroll->axis == Axis::Horizontal ? 0 : 1);
    writer.Float(node.scroll->offset);
    writer.Float(node.scroll->maximum_offset);
    writer.Float(node.scroll->viewport_extent);
    writer.Float(node.scroll->content_extent);
  }

  writer.Integer<std::uint8_t>(node.collection.has_value() ? 1 : 0);
  if (node.collection.has_value()) {
    writer.Integer(OptionalSize(node.collection->item_count));
    writer.Integer(OptionalSize(node.collection->row_count));
    writer.Integer(OptionalSize(node.collection->column_count));
  }

  writer.Integer<std::uint8_t>(node.collection_item.has_value() ? 1 : 0);
  if (node.collection_item.has_value()) {
    if (node.collection_item->row_span > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
        node.collection_item->column_span > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::overflow_error("HuxerUI Android semantic collection span is too large");
    }
    writer.Integer(OptionalSize(node.collection_item->index));
    writer.Integer(OptionalSize(node.collection_item->row_index));
    writer.Integer(OptionalSize(node.collection_item->column_index));
    writer.Integer<std::int64_t>(static_cast<std::int64_t>(node.collection_item->row_span));
    writer.Integer<std::int64_t>(static_cast<std::int64_t>(node.collection_item->column_span));
  }

  const std::int32_t live_region = node.live_region == SemanticLiveRegion::Polite      ? 1
                                   : node.live_region == SemanticLiveRegion::Assertive ? 2
                                                                                       : 0;
  writer.Integer(live_region);
  writer.Float(node.bounds.x);
  writer.Float(node.bounds.y);
  writer.Float(node.bounds.width);
  writer.Float(node.bounds.height);

  if (node.custom_actions.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error("HuxerUI Android semantic node has too many custom actions");
  }
  writer.Integer(static_cast<std::int32_t>(node.custom_actions.size()));
  for (const auto& [action_id, label] : node.custom_actions) {
    writer.Integer(action_id);
    writer.String(label);
  }
}

} // namespace

std::vector<std::uint8_t> EncodeAndroidSemanticFrame(const SemanticFrame& frame) {
  Writer writer;
  writer.Integer(android_semantics_magic);
  writer.Integer(android_semantics_version);
  writer.Integer(frame.revision);
  writer.Integer(AndroidNodeId(frame.root));
  if (frame.nodes.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error("HuxerUI Android semantic frame has too many nodes");
  }
  writer.Integer(static_cast<std::int32_t>(frame.nodes.size()));
  for (const SemanticNode& node : frame.nodes) {
    EncodeNode(writer, node);
  }
  return std::move(writer).Finish();
}

} // namespace huxerui::detail
