#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/environment.h>
#include <huxerui/modifier.h>
#include <huxerui/semantics.h>

namespace huxerui::detail {

class AppResources;
struct MountedNode;

struct SemanticExtensionRoute {
  NodeExtensionHandle extension;
  std::uint64_t local_id = 0;
};

inline constexpr std::size_t semantic_standard_action_count = static_cast<std::size_t>(SemanticActionKind::Custom);

struct SemanticActionRoute {
  std::uint64_t node_identity = 0;
  std::array<std::optional<SemanticExtensionRoute>, semantic_standard_action_count> extension_actions;
  std::unordered_map<std::uint64_t, SemanticExtensionRoute> custom_actions;
};

class SemanticTree final {
public:
  explicit SemanticTree(Runtime::State& runtime_state) : runtime_state_(runtime_state) {}
  void BuildSemantics();
  bool PerformSemanticAction(SemanticNodeId node_id, const SemanticAction& action);
  const std::shared_ptr<const SemanticFrame>& Frame() const noexcept { return frame_; }

private:
  Runtime::State& runtime_state_;
  std::shared_ptr<const SemanticFrame> frame_;
  SemanticNodeId next_semantic_identity_ = 1;
  SemanticNodeId semantic_root_identity_ = 0;
  std::uint64_t semantic_revision_ = 0;
  std::unordered_map<SemanticNodeId, SemanticActionRoute> semantic_action_routes_;
};

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
  std::optional<TextRange> text_selection;
  std::optional<ScrollMetrics> scroll;
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

SemanticPatch ResolveSemantics(
    const Semantics& semantics, std::shared_ptr<const Environment> environment, AppResources& resources
);
void ApplySemantics(SemanticPatch& target, const SemanticPatch& source);

struct SemanticBuilderItem {
  std::uint64_t local_id = 0;
  std::optional<Rect> local_bounds{};
  SemanticPatch semantics{};
  bool enabled = true;
  std::uint64_t actions = 0;
  std::vector<std::pair<std::uint64_t, std::string>> custom_actions{};
};

struct SemanticBuilderState {
  std::shared_ptr<const Environment> environment;
  AppResources* resources = nullptr;
  std::vector<SemanticBuilderItem> items{};
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
