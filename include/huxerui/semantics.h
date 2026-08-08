#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/resource.h>
#include <huxerui/text_input.h>

namespace huxerui {

class Runtime;

namespace detail {
struct ModifierDescriptor;
struct SemanticBuilderState;
} // namespace detail

using SemanticNodeId = std::uint64_t;

enum class SemanticRole {
  Generic,
  Text,
  Heading,
  Image,
  Button,
  Link,
  Checkbox,
  RadioButton,
  Switch,
  Slider,
  ProgressIndicator,
  TextField,
  SearchField,
  Tab,
  TabList,
  Menu,
  MenuItem,
  Dialog,
  Navigation,
  List,
  ListItem,
  Grid,
  GridCell,
  ScrollView,
};

enum class SemanticCheckedState {
  Unchecked,
  Checked,
  Mixed,
};

enum class SemanticLiveRegion {
  None,
  Polite,
  Assertive,
};

enum class SemanticDescendantPolicy {
  Preserve,
  Exclude,
};

struct SemanticRange {
  double minimum = 0.0;
  double maximum = 0.0;
  double current = 0.0;
  std::optional<double> step;

  bool operator==(const SemanticRange&) const = default;
};

struct SemanticCollection {
  std::optional<std::size_t> item_count;
  std::optional<std::size_t> row_count;
  std::optional<std::size_t> column_count;

  bool operator==(const SemanticCollection&) const = default;
};

struct SemanticCollectionItem {
  std::optional<std::size_t> index;
  std::optional<std::size_t> row_index;
  std::optional<std::size_t> column_index;
  std::size_t row_span = 1;
  std::size_t column_span = 1;

  bool operator==(const SemanticCollectionItem&) const = default;
};

struct Semantics {
  static const detail::ModifierDescriptor& Descriptor();

  std::optional<SemanticRole> role;
  std::optional<StringVariant> label;
  std::optional<StringVariant> value;
  std::optional<StringVariant> placeholder;
  std::optional<StringVariant> hint;
  std::optional<StringVariant> state_description;
  std::optional<StringVariant> error;
  std::optional<std::string> identifier;
  std::optional<SemanticCheckedState> checked;
  std::optional<bool> selected;
  std::optional<bool> expanded;
  std::optional<bool> busy;
  std::optional<bool> read_only;
  std::optional<bool> required;
  std::optional<bool> invalid;
  std::optional<unsigned int> heading_level;
  std::optional<SemanticRange> range;
  std::optional<TextRange> text_selection;
  std::optional<SemanticCollection> collection;
  std::optional<SemanticCollectionItem> collection_item;
  std::optional<SemanticLiveRegion> live_region;
  std::optional<SemanticDescendantPolicy> descendants;
  std::optional<bool> hidden;

  bool operator==(const Semantics&) const = default;
};

enum class SemanticActionKind : std::uint8_t {
  Activate,
  Focus,
  SetText,
  SetSelection,
  SetValue,
  Increment,
  Decrement,
  Scroll,
  ShowOnScreen,
  Expand,
  Collapse,
  Dismiss,
  Custom,
};

[[nodiscard]] constexpr std::uint64_t SemanticActionMask(SemanticActionKind action) noexcept {
  const std::uint8_t index = static_cast<std::uint8_t>(action);
  return index <= static_cast<std::uint8_t>(SemanticActionKind::Custom) ? std::uint64_t{1} << index : 0;
}

struct SemanticAction {
  SemanticActionKind kind = SemanticActionKind::Activate;
  std::variant<std::monostate, std::string, TextRange, double, Point, std::uint64_t> value;

  bool operator==(const SemanticAction&) const = default;
};

struct SemanticNode {
  SemanticNodeId id = 0;
  std::optional<SemanticNodeId> parent;
  std::vector<SemanticNodeId> children;
  SemanticRole role = SemanticRole::Generic;
  std::string label;
  std::string value;
  std::string placeholder;
  std::string hint;
  std::string state_description;
  std::string error;
  std::string identifier;
  std::optional<SemanticCheckedState> checked;
  std::optional<bool> selected;
  std::optional<bool> expanded;
  std::optional<bool> busy;
  std::optional<bool> read_only;
  std::optional<bool> required;
  std::optional<bool> invalid;
  std::optional<unsigned int> heading_level;
  std::optional<SemanticRange> range;
  std::optional<TextRange> text_selection;
  std::optional<SemanticCollection> collection;
  std::optional<SemanticCollectionItem> collection_item;
  SemanticLiveRegion live_region = SemanticLiveRegion::None;
  bool enabled = true;
  bool focused = false;
  bool multiline = false;
  bool secure = false;
  std::uint64_t actions = 0;
  std::vector<std::pair<std::uint64_t, std::string>> custom_actions;
  Rect bounds;

  bool operator==(const SemanticNode&) const = default;
};

struct SemanticFrame {
  std::uint64_t revision = 0;
  SemanticNodeId root = 0;
  std::vector<SemanticNode> nodes;

  bool operator==(const SemanticFrame&) const = default;
};

class SemanticBuilder {
public:
  SemanticBuilder(const SemanticBuilder&) = delete;
  SemanticBuilder& operator=(const SemanticBuilder&) = delete;
  SemanticBuilder(SemanticBuilder&&) = delete;
  SemanticBuilder& operator=(SemanticBuilder&&) = delete;

  void SetOwner(Semantics semantics);
  void AddChild(std::uint64_t local_id, Rect local_bounds, Semantics semantics);
  void AddAction(std::uint64_t local_id, SemanticActionKind action);
  void AddCustomAction(std::uint64_t local_id, std::uint64_t action_id, StringVariant label);

private:
  explicit SemanticBuilder(detail::SemanticBuilderState& state) noexcept : state_(&state) {}

  detail::SemanticBuilderState* state_;

  friend class Runtime;
};

} // namespace huxerui
