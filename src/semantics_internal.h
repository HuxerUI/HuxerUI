#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/environment.h>
#include <huxerui/semantics.h>

namespace huxerui::detail {

struct ModifierDescriptor;

struct SemanticPatch {
  std::optional<SemanticRole> role;
  std::optional<std::string> label;
  std::optional<std::string> value;
  std::optional<std::string> placeholder;
  std::optional<std::string> hint;
  std::optional<std::string> state_description;
  std::optional<std::string> error;
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
  std::optional<SemanticCollection> collection;
  std::optional<SemanticCollectionItem> collection_item;
  std::optional<SemanticLiveRegion> live_region;
  std::optional<SemanticDescendantPolicy> descendants;
  std::optional<bool> hidden;
  bool operator==(const SemanticPatch&) const = default;
};

struct BuiltInSemantics {
  Semantics value;

  static const ModifierDescriptor& Descriptor();

  bool operator==(const BuiltInSemantics&) const = default;
};

SemanticPatch ResolveSemantics(const Semantics& semantics, std::shared_ptr<const Environment> environment);
void ApplySemantics(SemanticPatch& target, const SemanticPatch& source);

struct SemanticBuilderItem {
  std::uint64_t local_id = 0;
  std::optional<Rect> local_bounds;
  SemanticPatch semantics;
  std::uint64_t actions = 0;
  std::vector<std::pair<std::uint64_t, std::string>> custom_actions;
};

struct SemanticBuilderState {
  std::shared_ptr<const Environment> environment;
  std::vector<SemanticBuilderItem> items;
};

struct VirtualSemanticKey {
  std::size_t extension_index = 0;
  std::uint64_t local_id = 0;

  bool operator==(const VirtualSemanticKey&) const = default;
};

struct VirtualSemanticKeyHash {
  std::size_t operator()(const VirtualSemanticKey& key) const noexcept {
    const std::size_t first = std::hash<std::size_t>{}(key.extension_index);
    const std::size_t second = std::hash<std::uint64_t>{}(key.local_id);
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
  }
};

} // namespace huxerui::detail
