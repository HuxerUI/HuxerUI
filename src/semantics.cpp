#include <huxerui/semantics.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include "internal.h"
#include "text_input_internal.h"

namespace huxerui::detail {

namespace {

template <class Value> void ApplyOptional(std::optional<Value>& target, const std::optional<Value>& source) {
  if (source.has_value()) {
    target = source;
  }
}

std::optional<std::string> ResolveString(const std::optional<StringVariant>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return ResolveStringVariant(*value);
}

void ValidateRange(const SemanticRange& range) {
  if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum) || !std::isfinite(range.current) ||
      range.minimum > range.maximum || range.current < range.minimum || range.current > range.maximum ||
      (range.step.has_value() && (!std::isfinite(*range.step) || *range.step <= 0.0))) {
    throw std::invalid_argument("HuxerUI semantic range must be finite, ordered, and contain its current value");
  }
}

void ValidateCollectionItem(const SemanticCollectionItem& item) {
  if (item.row_span == 0 || item.column_span == 0) {
    throw std::invalid_argument("HuxerUI semantic collection item spans must be greater than zero");
  }
}

SemanticBuilderItem& RequireItem(SemanticBuilderState& state, std::uint64_t local_id) {
  const auto found = std::ranges::find(state.items, local_id, &SemanticBuilderItem::local_id);
  if (found == state.items.end()) {
    throw std::logic_error("HuxerUI semantic action requires an existing owner or virtual child");
  }
  return *found;
}

} // namespace

SemanticPatch ResolveSemantics(const Semantics& semantics, std::shared_ptr<const Environment> environment) {
  if (semantics.heading_level.has_value() && (*semantics.heading_level == 0 || *semantics.heading_level > 6)) {
    throw std::invalid_argument("HuxerUI semantic heading level must be between one and six");
  }
  if (semantics.range.has_value()) {
    ValidateRange(*semantics.range);
  }
  if (semantics.text_selection.has_value() && !semantics.text_selection->IsValid()) {
    throw std::invalid_argument("HuxerUI semantic text selection must be a normalized nonnegative range");
  }
  if (semantics.collection_item.has_value()) {
    ValidateCollectionItem(*semantics.collection_item);
  }

  Composer composer(nullptr, environment);
  Composer::Guard guard(composer);
  return {
      .role = semantics.role,
      .label = ResolveString(semantics.label),
      .value = ResolveString(semantics.value),
      .placeholder = ResolveString(semantics.placeholder),
      .hint = ResolveString(semantics.hint),
      .state_description = ResolveString(semantics.state_description),
      .error = ResolveString(semantics.error),
      .identifier = semantics.identifier,
      .checked = semantics.checked,
      .selected = semantics.selected,
      .expanded = semantics.expanded,
      .busy = semantics.busy,
      .read_only = semantics.read_only,
      .required = semantics.required,
      .invalid = semantics.invalid,
      .heading_level = semantics.heading_level,
      .range = semantics.range,
      .text_selection = semantics.text_selection,
      .collection = semantics.collection,
      .collection_item = semantics.collection_item,
      .live_region = semantics.live_region,
      .descendants = semantics.descendants,
      .hidden = semantics.hidden,
  };
}

void ApplySemantics(SemanticPatch& target, const SemanticPatch& source) {
  ApplyOptional(target.role, source.role);
  ApplyOptional(target.label, source.label);
  ApplyOptional(target.value, source.value);
  ApplyOptional(target.placeholder, source.placeholder);
  ApplyOptional(target.hint, source.hint);
  ApplyOptional(target.state_description, source.state_description);
  ApplyOptional(target.error, source.error);
  ApplyOptional(target.identifier, source.identifier);
  ApplyOptional(target.checked, source.checked);
  ApplyOptional(target.selected, source.selected);
  ApplyOptional(target.expanded, source.expanded);
  ApplyOptional(target.busy, source.busy);
  ApplyOptional(target.read_only, source.read_only);
  ApplyOptional(target.required, source.required);
  ApplyOptional(target.invalid, source.invalid);
  ApplyOptional(target.heading_level, source.heading_level);
  ApplyOptional(target.range, source.range);
  ApplyOptional(target.text_selection, source.text_selection);
  ApplyOptional(target.collection, source.collection);
  ApplyOptional(target.collection_item, source.collection_item);
  ApplyOptional(target.live_region, source.live_region);
  ApplyOptional(target.descendants, source.descendants);
  ApplyOptional(target.hidden, source.hidden);
}

const ModifierDescriptor& BuiltInSemantics::Descriptor() {
  static const ModifierDescriptor descriptor{
      [](ViewSpec& spec, const void* value) {
        const auto& semantics = static_cast<const BuiltInSemantics*>(value)->value;
        ApplySemantics(spec.component_semantics, ResolveSemantics(semantics, spec.environment));
      },
      nullptr,
      nullptr,
      false,
      ErasedEqualsFor<BuiltInSemantics>(),
      nullptr,
  };
  return descriptor;
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

std::string SemanticString(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return {};
  }
  const bool only_whitespace =
      std::ranges::all_of(*value, [](unsigned char character) { return std::isspace(character) != 0; });
  return only_whitespace ? std::string{} : *value;
}

bool HasMeaning(
    const detail::SemanticPatch& semantics, std::uint64_t actions, bool has_virtual_children, bool explicitly_declared
) {
  return explicitly_declared || semantics.role.has_value() || !SemanticString(semantics.label).empty() ||
         !SemanticString(semantics.value).empty() || !SemanticString(semantics.placeholder).empty() ||
         !SemanticString(semantics.hint).empty() || !SemanticString(semantics.state_description).empty() ||
         !SemanticString(semantics.error).empty() || !semantics.identifier.value_or(std::string{}).empty() ||
         semantics.checked.has_value() || semantics.selected.has_value() || semantics.expanded.has_value() ||
         semantics.busy.has_value() || semantics.read_only.has_value() || semantics.required.has_value() ||
         semantics.invalid.has_value() || semantics.heading_level.has_value() || semantics.range.has_value() ||
         semantics.text_selection.has_value() || semantics.collection.has_value() ||
         semantics.collection_item.has_value() ||
         semantics.live_region.value_or(SemanticLiveRegion::None) != SemanticLiveRegion::None || actions != 0 ||
         has_virtual_children;
}

SemanticNode MakeSemanticNode(
    SemanticNodeId id,
    std::optional<SemanticNodeId> parent,
    const detail::SemanticPatch& semantics,
    bool enabled,
    bool focused,
    Rect bounds
) {
  return {
      .id = id,
      .parent = parent,
      .role = semantics.role.value_or(SemanticRole::Generic),
      .label = SemanticString(semantics.label),
      .value = SemanticString(semantics.value),
      .placeholder = SemanticString(semantics.placeholder),
      .hint = SemanticString(semantics.hint),
      .state_description = SemanticString(semantics.state_description),
      .error = SemanticString(semantics.error),
      .identifier = semantics.identifier.value_or(std::string{}),
      .checked = semantics.checked,
      .selected = semantics.selected,
      .expanded = semantics.expanded,
      .busy = semantics.busy,
      .read_only = semantics.read_only,
      .required = semantics.required,
      .invalid = semantics.invalid,
      .heading_level = semantics.heading_level,
      .range = semantics.range,
      .text_selection = semantics.text_selection,
      .collection = semantics.collection,
      .collection_item = semantics.collection_item,
      .live_region = semantics.live_region.value_or(SemanticLiveRegion::None),
      .enabled = enabled,
      .focused = focused,
      .bounds = bounds,
  };
}

bool IsParameterlessAction(SemanticActionKind kind) noexcept {
  return kind == SemanticActionKind::Activate || kind == SemanticActionKind::Focus ||
         kind == SemanticActionKind::Increment || kind == SemanticActionKind::Decrement ||
         kind == SemanticActionKind::ShowOnScreen || kind == SemanticActionKind::Expand ||
         kind == SemanticActionKind::Collapse || kind == SemanticActionKind::Dismiss;
}

bool HasValidPayload(const SemanticAction& action) noexcept {
  if (IsParameterlessAction(action.kind)) {
    return std::holds_alternative<std::monostate>(action.value);
  }
  if (action.kind == SemanticActionKind::SetText) {
    return std::holds_alternative<std::string>(action.value);
  }
  if (action.kind == SemanticActionKind::SetSelection) {
    const auto* range = std::get_if<TextRange>(&action.value);
    return range != nullptr && range->IsValid();
  }
  if (action.kind == SemanticActionKind::SetValue) {
    const auto* value = std::get_if<double>(&action.value);
    return value != nullptr && std::isfinite(*value);
  }
  if (action.kind == SemanticActionKind::Scroll) {
    const auto* offset = std::get_if<Point>(&action.value);
    return offset != nullptr && std::isfinite(offset->x) && std::isfinite(offset->y);
  }
  if (action.kind == SemanticActionKind::Custom) {
    return std::holds_alternative<std::uint64_t>(action.value);
  }
  return false;
}

void BindExtensionActions(
    detail::SemanticActionRoute& target, std::uint64_t actions, const detail::SemanticExtensionRoute& source
) {
  for (std::size_t index = 0; index < detail::semantic_standard_action_count; ++index) {
    const auto kind = static_cast<SemanticActionKind>(index);
    if ((actions & SemanticActionMask(kind)) == 0) {
      continue;
    }
    if (target.extension_actions[index].has_value()) {
      throw std::logic_error("HuxerUI semantic action has more than one extension route");
    }
    target.extension_actions[index] = source;
  }
}

} // namespace

const detail::ModifierDescriptor& Semantics::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, const void* value) {
        const auto& semantics = *static_cast<const Semantics*>(value);
        if (!spec.author_semantics.has_value()) {
          spec.author_semantics.emplace();
        }
        detail::ApplySemantics(*spec.author_semantics, detail::ResolveSemantics(semantics, spec.environment));
      },
      nullptr,
      nullptr,
      false,
      nullptr,
      nullptr,
  };
  return descriptor;
}

void SemanticBuilder::SetOwner(Semantics semantics) {
  auto found = std::ranges::find(state_->items, std::uint64_t{0}, &detail::SemanticBuilderItem::local_id);
  if (found == state_->items.end()) {
    state_->items.push_back({.local_id = 0});
    found = std::prev(state_->items.end());
  }
  detail::ApplySemantics(found->semantics, detail::ResolveSemantics(semantics, state_->environment));
}

void SemanticBuilder::AddChild(std::uint64_t local_id, Rect local_bounds, Semantics semantics) {
  if (local_id == 0) {
    throw std::invalid_argument("HuxerUI virtual semantic child local id must be nonzero");
  }
  if (!std::isfinite(local_bounds.x) || !std::isfinite(local_bounds.y) || !std::isfinite(local_bounds.width) ||
      !std::isfinite(local_bounds.height) || local_bounds.width < 0.0F || local_bounds.height < 0.0F) {
    throw std::invalid_argument("HuxerUI virtual semantic child bounds must be finite and nonnegative");
  }
  if (std::ranges::any_of(state_->items, [local_id](const auto& item) { return item.local_id == local_id; })) {
    throw std::logic_error("HuxerUI virtual semantic child local ids must be unique within an extension");
  }
  state_->items.push_back({
      .local_id = local_id,
      .local_bounds = local_bounds,
      .semantics = detail::ResolveSemantics(semantics, state_->environment),
  });
}

void SemanticBuilder::AddAction(std::uint64_t local_id, SemanticActionKind action) {
  if (action == SemanticActionKind::Custom) {
    throw std::invalid_argument("HuxerUI custom semantic actions must use AddCustomAction");
  }
  if (static_cast<std::uint8_t>(action) >= static_cast<std::uint8_t>(SemanticActionKind::Custom)) {
    throw std::invalid_argument("HuxerUI semantic action kind is invalid");
  }
  detail::SemanticBuilderItem& item = detail::RequireItem(*state_, local_id);
  item.actions |= SemanticActionMask(action);
}

void SemanticBuilder::AddCustomAction(std::uint64_t local_id, std::uint64_t action_id, StringVariant label) {
  detail::SemanticBuilderItem& item = detail::RequireItem(*state_, local_id);
  if (action_id == 0 ||
      std::ranges::any_of(item.custom_actions, [action_id](const auto& action) { return action.first == action_id; })) {
    throw std::logic_error("HuxerUI custom semantic action ids must be nonzero and unique within a semantic node");
  }
  detail::Composer composer(nullptr, state_->environment);
  detail::Composer::Guard guard(composer);
  std::string resolved = detail::ResolveStringVariant(std::move(label));
  if (resolved.empty() ||
      std::ranges::all_of(resolved, [](unsigned char character) { return std::isspace(character) != 0; })) {
    throw std::invalid_argument("HuxerUI custom semantic action label must not be empty");
  }
  item.actions |= SemanticActionMask(SemanticActionKind::Custom);
  item.custom_actions.emplace_back(action_id, std::move(resolved));
}

void Runtime::BuildSemantics() {
  if (state_->semantic_root_identity_ == 0) {
    state_->semantic_root_identity_ = state_->next_semantic_identity_++;
  }

  SemanticFrame next;
  next.root = state_->semantic_root_identity_;
  const Rect viewport{
      0.0F,
      0.0F,
      std::max(0.0F, state_->viewport_.width),
      std::max(0.0F, state_->viewport_.height),
  };
  next.nodes.push_back({
      .id = next.root,
      .role = SemanticRole::Generic,
      .bounds = viewport,
  });
  std::unordered_map<SemanticNodeId, detail::SemanticActionRoute> routes;

  using NodeIds = std::vector<SemanticNodeId>;
  const auto visit = [&](auto&& self, detail::MountedNode& mounted, SemanticNodeId parent) -> NodeIds {
    detail::SemanticPatch resolved = mounted.component_semantics;
    struct ExtensionContribution {
      std::size_t index = 0;
      detail::SemanticBuilderState state;
    };
    std::vector<ExtensionContribution> contributions;
    contributions.reserve(mounted.extensions.size());
    std::optional<TextInputConfiguration> text_input_configuration;
    std::optional<TextInputState> text_input_state;
    bool owner_extension_declared = false;
    bool has_virtual_children = false;
    std::uint64_t owner_extension_actions = 0;
    for (std::size_t index = 0; index < mounted.extensions.size(); ++index) {
      detail::NodeExtensionEntry& entry = mounted.extensions[index];
      if (!entry.extension) {
        continue;
      }
      ExtensionContribution contribution{
          .index = index,
          .state = {.environment = mounted.environment},
      };
      SemanticBuilder builder(contribution.state);
      entry.extension->BuildSemantics(builder);
      if (std::shared_ptr<TextInputClient> client = entry.extension->GetTextInputClient()) {
        text_input_configuration = client->Configuration();
        text_input_state = client->State();
      }
      for (const detail::SemanticBuilderItem& item : contribution.state.items) {
        if (item.local_id == 0) {
          owner_extension_declared = true;
          owner_extension_actions |= item.actions;
          detail::ApplySemantics(resolved, item.semantics);
        } else {
          has_virtual_children = true;
        }
      }
      contributions.push_back(std::move(contribution));
    }
    if (mounted.author_semantics.has_value()) {
      detail::ApplySemantics(resolved, *mounted.author_semantics);
    }

    if (resolved.hidden.value_or(false)) {
      return {};
    }

    std::uint64_t actions = owner_extension_actions;
    if (mounted.enabled && (mounted.activation || detail::HasEventBinding<ViewEvents::Click>(mounted.event_bindings))) {
      actions |= SemanticActionMask(SemanticActionKind::Activate);
    }
    if (mounted.enabled && mounted.focusable) {
      actions |= SemanticActionMask(SemanticActionKind::Focus);
    }
    if (!mounted.enabled) {
      actions = 0;
    }
    if (text_input_configuration.has_value()) {
      if (text_input_configuration->read_only) {
        actions &= ~SemanticActionMask(SemanticActionKind::SetText);
      }
      if (text_input_configuration->secure) {
        actions &= ~SemanticActionMask(SemanticActionKind::SetSelection);
      }
    }

    const bool emit_owner = HasMeaning(
        resolved,
        actions,
        has_virtual_children,
        owner_extension_declared || mounted.author_semantics.has_value()
    );
    const SemanticNodeId owner_id = [&] {
      if (!emit_owner) {
        return SemanticNodeId{0};
      }
      if (mounted.semantic_identity == 0) {
        mounted.semantic_identity = state_->next_semantic_identity_++;
      }
      return mounted.semantic_identity;
    }();
    const SemanticNodeId child_parent = emit_owner ? owner_id : parent;
    std::size_t owner_index = 0;
    if (emit_owner) {
      SemanticNode owner =
          MakeSemanticNode(owner_id, parent, resolved, mounted.enabled, mounted.focused, mounted.PresentationBounds());
      owner.actions = actions;
      if (text_input_configuration.has_value()) {
        owner.multiline = text_input_configuration->multiline;
        owner.secure = text_input_configuration->secure;
        owner.read_only = text_input_configuration->read_only;
        if (text_input_configuration->secure) {
          owner.value.clear();
          owner.text_selection.reset();
        } else if (text_input_state.has_value()) {
          owner.text_selection = text_input_state->selection.Range();
        }
      }
      owner_index = next.nodes.size();
      next.nodes.push_back(std::move(owner));
      routes[owner_id].node_identity = mounted.identity;
    }

    std::vector<SemanticNodeId> children;
    if (resolved.descendants.value_or(SemanticDescendantPolicy::Preserve) != SemanticDescendantPolicy::Exclude) {
      for (const std::unique_ptr<detail::MountedNode>& child : mounted.children) {
        std::vector<SemanticNodeId> child_ids = self(self, *child, child_parent);
        children.insert(children.end(), child_ids.begin(), child_ids.end());
      }
    }

    for (const ExtensionContribution& contribution : contributions) {
      const detail::NodeExtensionEntry& entry = mounted.extensions[contribution.index];
      const detail::NodeExtensionHandle handle{mounted.identity, contribution.index, entry.descriptor};
      for (const detail::SemanticBuilderItem& item : contribution.state.items) {
        if (item.local_id == 0) {
          if (!emit_owner || !mounted.enabled) {
            continue;
          }
          BindExtensionActions(routes[owner_id], item.actions, {handle, 0});
          for (const auto& custom : item.custom_actions) {
            if (!routes[owner_id]
                     .custom_actions.emplace(custom.first, detail::SemanticExtensionRoute{handle, 0})
                     .second) {
              throw std::logic_error("HuxerUI custom semantic action id must be unique within a semantic node");
            }
            next.nodes[owner_index].custom_actions.push_back(custom);
          }
          continue;
        }

        const detail::VirtualSemanticKey key{contribution.index, item.local_id};
        SemanticNodeId& id = mounted.virtual_semantic_identities[key];
        if (id == 0) {
          id = state_->next_semantic_identity_++;
        }
        const Rect local_bounds = item.local_bounds.value_or(mounted.bounds);
        SemanticNode child = MakeSemanticNode(
            id,
            child_parent,
            item.semantics,
            mounted.enabled,
            false,
            detail::TransformBounds(mounted.presentation.resolved_transform, local_bounds)
        );
        child.actions = mounted.enabled ? item.actions : 0;
        child.custom_actions = item.custom_actions;
        next.nodes.push_back(std::move(child));
        children.push_back(id);
        detail::SemanticActionRoute& route = routes[id];
        route.node_identity = mounted.identity;
        if (mounted.enabled) {
          BindExtensionActions(route, item.actions, {handle, item.local_id});
          for (const auto& custom : item.custom_actions) {
            route.custom_actions.insert_or_assign(custom.first, detail::SemanticExtensionRoute{handle, item.local_id});
          }
        }
      }
    }

    if (emit_owner) {
      next.nodes[owner_index].children = std::move(children);
      return {owner_id};
    }
    return children;
  };

  if (state_->mounted_root_ && !viewport.IsEmpty()) {
    next.nodes.front().children = visit(visit, *state_->mounted_root_, next.root);
  }

  bool changed = !state_->frame_commit_.semantic_frame || state_->frame_commit_.semantic_frame->root != next.root ||
                 state_->frame_commit_.semantic_frame->nodes != next.nodes;
  if (changed) {
    next.revision = ++state_->semantic_revision_;
    state_->frame_commit_.semantic_frame = std::make_shared<const SemanticFrame>(std::move(next));
  }
  state_->semantic_action_routes_ = std::move(routes);
}

bool Runtime::PerformSemanticAction(SemanticNodeId node_id, const SemanticAction& action) {
  if (!HasValidPayload(action) || !state_->frame_commit_.semantic_frame) {
    return false;
  }
  const auto node = std::ranges::find(state_->frame_commit_.semantic_frame->nodes, node_id, &SemanticNode::id);
  if (node == state_->frame_commit_.semantic_frame->nodes.end() || !node->enabled ||
      (node->actions & SemanticActionMask(action.kind)) == 0) {
    return false;
  }
  const auto route = state_->semantic_action_routes_.find(node_id);
  if (route == state_->semantic_action_routes_.end() || !state_->mounted_root_) {
    return false;
  }
  detail::MountedNode* owner = FindNode(*state_->mounted_root_, route->second.node_identity);
  if (owner == nullptr || !owner->enabled) {
    return false;
  }
  std::optional<detail::SemanticExtensionRoute> extension_route;
  if (action.kind == SemanticActionKind::Custom) {
    const std::uint64_t action_id = std::get<std::uint64_t>(action.value);
    const auto custom = route->second.custom_actions.find(action_id);
    if (custom == route->second.custom_actions.end()) {
      return false;
    }
    extension_route = custom->second;
  } else {
    const std::size_t index = static_cast<std::size_t>(action.kind);
    if (index < route->second.extension_actions.size()) {
      extension_route = route->second.extension_actions[index];
    }
  }
  if (extension_route.has_value()) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_route->extension);
    if (extension == nullptr) {
      return false;
    }
    const bool text_action =
        action.kind == SemanticActionKind::SetText || action.kind == SemanticActionKind::SetSelection;
    const std::shared_ptr<TextInputClient> text_client = text_action ? extension->GetTextInputClient() : nullptr;
    const std::optional<TextInputState> previous = text_client ? std::optional{text_client->State()} : std::nullopt;
    if (!extension->OnSemanticAction(extension_route->local_id, action)) {
      return false;
    }
    if (text_client) {
      const TextInputState current = text_client->State();
      if (!detail::IsValidTextInputState(current, previous->session_id) ||
          !detail::IsValidTextInputStateTransition(*previous, current)) {
        throw std::logic_error("HuxerUI text input client returned invalid state after a semantic action");
      }
      InvalidateTextInputStateChange(owner->identity, *previous, current);
      RefreshTextInputSession();
    }
    RequestFrame();
    return true;
  }
  if (action.kind == SemanticActionKind::Activate) {
    ActivateNode(*owner);
    RequestFrame();
    return true;
  }
  if (action.kind == SemanticActionKind::Focus) {
    SetFocusedNode(owner->identity, true);
    return state_->focused_node_identity_ == owner->identity;
  }
  return false;
}

} // namespace huxerui
