#include "runtime_internal.h"
#include "runtime_text_internal.h"
#include "text/text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace huxerui {

namespace {

bool IsValidTextInputState(const TextInputState& state, TextInputSessionId session_id) noexcept {
  const bool affinity_known =
      state.selection.affinity == TextAffinity::Upstream || state.selection.affinity == TextAffinity::Downstream;
  return state.session_id == session_id && state.selection.anchor >= 0 && state.selection.active >= 0 &&
         affinity_known && (!state.composition.has_value() || state.composition->IsValid());
}

bool IsValidTextInputStateTransition(const TextInputState& previous, const TextInputState& current) noexcept {
  if (current.session_id != previous.session_id || current.revision < previous.revision ||
      current.content_revision < previous.content_revision) {
    return false;
  }
  const bool observable_change = current.selection != previous.selection ||
                                 current.composition != previous.composition ||
                                 current.content_revision != previous.content_revision;
  return !observable_change || current.revision > previous.revision;
}

std::shared_ptr<TextInputClient> FindTextInputClient(detail::MountedNode& node) {
  std::shared_ptr<TextInputClient> result;
  for (detail::NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    std::shared_ptr<TextInputClient> client = entry.extension->GetTextInputClient();
    if (!client) {
      continue;
    }
    if (result) {
      throw std::logic_error("HuxerUI focusable node must not expose multiple text input clients");
    }
    result = std::move(client);
  }
  return result;
}

bool FindNodePath(detail::MountedNode& node, std::uint64_t identity, std::vector<detail::MountedNode*>& path) {
  path.push_back(&node);
  if (node.identity == identity) {
    return true;
  }
  for (const std::unique_ptr<detail::MountedNode>& child : node.children) {
    if (FindNodePath(*child, identity, path)) {
      return true;
    }
  }
  path.pop_back();
  return false;
}

bool IsKnown(TextAffinity affinity) noexcept {
  return affinity == TextAffinity::Upstream || affinity == TextAffinity::Downstream;
}

bool IsKnown(TextInputType type) noexcept {
  switch (type) {
  case TextInputType::Text:
  case TextInputType::Email:
  case TextInputType::Number:
  case TextInputType::Decimal:
  case TextInputType::Phone:
  case TextInputType::Url:
    return true;
  }
  return false;
}

bool IsKnown(TextCapitalization capitalization) noexcept {
  switch (capitalization) {
  case TextCapitalization::None:
  case TextCapitalization::Characters:
  case TextCapitalization::Words:
  case TextCapitalization::Sentences:
    return true;
  }
  return false;
}

bool IsKnown(TextInputAction action) noexcept {
  switch (action) {
  case TextInputAction::Default:
  case TextInputAction::Done:
  case TextInputAction::Go:
  case TextInputAction::Next:
  case TextInputAction::Search:
  case TextInputAction::Send:
  case TextInputAction::Newline:
    return true;
  }
  return false;
}

bool IsKnown(TextInputResultCode result_code) noexcept {
  switch (result_code) {
  case TextInputResultCode::Ok:
  case TextInputResultCode::SessionMismatch:
  case TextInputResultCode::Rejected:
  case TextInputResultCode::ReadOnly:
    return true;
  }
  return false;
}

bool IsKnown(TextInputSyncAction sync_action) noexcept {
  return sync_action == TextInputSyncAction::None || sync_action == TextInputSyncAction::Update ||
         sync_action == TextInputSyncAction::Restart;
}

bool IsValidConfiguration(const TextInputConfiguration& configuration) noexcept {
  return IsKnown(configuration.type) && IsKnown(configuration.capitalization) && IsKnown(configuration.action);
}

bool MatchesConfiguredAction(const TextInputConfiguration& configuration, TextInputAction requested) noexcept {
  if (configuration.action == requested) {
    return true;
  }
  if (configuration.action != TextInputAction::Default) {
    return false;
  }
  return requested == (configuration.multiline ? TextInputAction::Newline : TextInputAction::Done);
}

bool IsValidSelectionShape(const TextSelection& selection) noexcept {
  return selection.anchor >= 0 && selection.active >= 0 && IsKnown(selection.affinity);
}

bool IsValidContext(const TextInputContext& context, TextInputSessionId session_id) noexcept {
  if (!IsKnown(context.result_code) || context.session_id != session_id) {
    return false;
  }
  if (context.result_code != TextInputResultCode::Ok) {
    return true;
  }
  if (context.slice_start < 0 || context.total_length < 0 || context.slice_start > context.total_length ||
      !IsValidSelectionShape(context.selection) || context.selection.anchor > context.total_length ||
      context.selection.active > context.total_length ||
      (context.composition.has_value() &&
       (!context.composition->IsValid() || context.composition->end > context.total_length))) {
    return false;
  }
  const std::optional<TextOffset> slice_length = detail::Utf16Length(context.text);
  return slice_length.has_value() && *slice_length <= context.total_length - context.slice_start;
}

bool IsValidRect(const Rect& rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width >= 0.0F && rect.height >= 0.0F;
}

bool IsValidGeometry(const TextInputGeometry& geometry, TextInputSessionId session_id) noexcept {
  if (!IsKnown(geometry.result_code) || geometry.session_id != session_id) {
    return false;
  }
  if (geometry.result_code != TextInputResultCode::Ok) {
    return true;
  }
  return IsValidRect(geometry.caret) &&
         std::ranges::all_of(geometry.range_rects, [](const Rect& rect) { return IsValidRect(rect); });
}

bool GeometrySnapshotMatches(
    const detail::ActiveTextInputSession::GeometrySnapshot& snapshot,
    const TextInputState& state,
    const detail::MountedNode& node
) noexcept {
  return snapshot.client_revision == state.revision && snapshot.layout_revision == node.layout_revision &&
         snapshot.node_to_host == node.presentation.resolved_transform;
}

detail::ActiveTextInputSession::GeometrySnapshot
MakeGeometrySnapshot(const TextInputState& state, const detail::MountedNode& node, TextInputGeometry geometry) {
  return {
      state.revision,
      node.layout_revision,
      node.presentation.resolved_transform,
      std::move(geometry),
  };
}

struct ResolvedGeometrySnapshot {
  detail::ActiveTextInputSession::GeometrySnapshot* snapshot = nullptr;
  bool prepared = false;
};

template <typename Query>
ResolvedGeometrySnapshot ResolveGeometrySnapshot(
    detail::ActiveTextInputSession& session, const TextInputState& state, const detail::MountedNode& node, Query&& query
) {
  if (session.prepared_geometry.has_value() && GeometrySnapshotMatches(*session.prepared_geometry, state, node)) {
    return {&*session.prepared_geometry, true};
  }
  if (session.published_geometry.has_value() && GeometrySnapshotMatches(*session.published_geometry, state, node)) {
    return {&*session.published_geometry, false};
  }
  session.prepared_geometry = MakeGeometrySnapshot(state, node, std::forward<Query>(query)());
  return {&*session.prepared_geometry, true};
}

void PublishGeometrySnapshot(detail::ActiveTextInputSession& session, const ResolvedGeometrySnapshot& resolved) {
  if (resolved.prepared) {
    session.published_geometry = std::move(session.prepared_geometry);
  }
  session.prepared_geometry.reset();
}

TextInputContext SessionMismatchContext(TextInputSessionId session_id) {
  TextInputContext result;
  result.result_code = TextInputResultCode::SessionMismatch;
  result.session_id = session_id;
  return result;
}

TextInputGeometry SessionMismatchGeometry(TextInputSessionId session_id) {
  TextInputGeometry result;
  result.result_code = TextInputResultCode::SessionMismatch;
  result.session_id = session_id;
  return result;
}

TextInputPositionResult SessionMismatchPosition(TextInputSessionId session_id) {
  TextInputPositionResult result;
  result.result_code = TextInputResultCode::SessionMismatch;
  result.session_id = session_id;
  return result;
}

} // namespace

bool detail::TextInteraction::HasSession() const noexcept {
  return text_input_session_.has_value();
}

void detail::TextInteraction::RequestShowForNode(std::uint64_t node) {
  if (runtime_state_.focused_node_identity_ != node || !text_input_session_ ||
      text_input_session_->node_identity != node) {
    return;
  }
  if (PlatformTextInput* text_input = runtime_state_.platform_->TextInput()) {
    text_input->RequestShow(text_input_session_->session_id);
  }
}

bool detail::TextInteraction::PerformSemanticTextAction(
    std::uint64_t node, NodeExtension& extension, std::uint64_t local_id, const SemanticAction& action
) {
  const std::shared_ptr<TextInputClient> client = extension.GetTextInputClient();
  const std::optional<TextInputState> previous = client ? std::optional{client->State()} : std::nullopt;
  if (!extension.OnSemanticAction(local_id, action)) {
    return false;
  }
  if (client) {
    const TextInputState current = client->State();
    if (!IsValidTextInputState(current, previous->session_id) || !IsValidTextInputStateTransition(*previous, current)) {
      throw std::logic_error("HuxerUI text input client returned invalid state after a semantic action");
    }
    InvalidateTextInputStateChange(node, *previous, current);
    RefreshTextInputSession();
  }
  return true;
}

bool detail::TextInteraction::SessionBelongsTo(const detail::MountedNode& root) const {
  return text_input_session_ && ContainsNodeIdentity(root, text_input_session_->node_identity);
}

void detail::TextInteraction::NotifyScrollActivity(detail::MountedNode& node, const ScrollActivity& activity) {
  if (activity.source != ScrollSource::FocusReveal && text_input_session_ &&
      text_input_session_->node_identity != node.identity && FindNode(node, text_input_session_->node_identity)) {
    text_input_session_->client->ViewportScrolled();
  }
  auto& overlay = text_selection_overlay_.state;
  if (overlay.visible && !overlay.dismissing) {
    const auto key = std::pair{node.identity, activity.source};
    if (activity.phase == ScrollPhase::End || activity.phase == ScrollPhase::Cancel) {
      std::erase(overlay.scroll_activities, key);
    } else if (activity.phase == ScrollPhase::Begin && runtime_state_.mounted_root_ &&
               runtime_state_.focused_node_identity_) {
      auto* owner = FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
      if (owner && (ContainsNodeIdentity(node, owner->identity) || ContainsNodeIdentity(*owner, node.identity)) &&
          std::ranges::find(overlay.scroll_activities, key) == overlay.scroll_activities.end()) {
        overlay.scroll_activities.push_back(key);
      }
    }
  }
  InvalidateOverlay();
}

bool detail::TextInteraction::OverlayVisible() const noexcept {
  return text_selection_overlay_.state.visible;
}

void detail::TextInteraction::InvalidateOverlay() noexcept {
  if (OverlayVisible()) {
    text_selection_overlay_.state.paint_dirty = true;
  }
}

const RenderNode& detail::TextInteraction::Overlay() const noexcept {
  return text_selection_overlay_.render_node;
}

void detail::TextInteraction::ResetSelectionGesture() noexcept {
  text_selection_gesture_ = {};
}

void detail::TextInteraction::RememberSelectionTap(const PointerEvent& event, std::uint64_t node, double timestamp) {
  text_selection_gesture_.previous_tap_time = timestamp;
  text_selection_gesture_.previous_tap_position = event.position;
  text_selection_gesture_.previous_tap_node = node;
  text_selection_gesture_.previous_tap_device = event.device_kind;
}

bool detail::TextInteraction::BringTextInputIntoView() {
  if (!runtime_state_.mounted_root_ || !text_input_session_.has_value()) {
    return false;
  }

  detail::ActiveTextInputSession& session = *text_input_session_;
  std::vector<detail::MountedNode*> path;
  if (!FindNodePath(*runtime_state_.mounted_root_, session.node_identity, path)) {
    return false;
  }

  const TextInputState current = session.client->State();
  if (!IsValidTextInputState(current, session.session_id) ||
      !IsValidTextInputStateTransition(session.state, current)) {
    throw std::logic_error("HuxerUI text input client returned invalid state");
  }
  const detail::MountedNode& node = *path.back();
  // Clients report caret geometry in node-local coordinates. Resolving the snapshot applies layout, presentation, and
  // ancestor child transforms so scrolling decisions are made in host-view coordinates.
  const ResolvedGeometrySnapshot resolved = ResolveGeometrySnapshot(session, current, node, [&] {
    return QueryTextInputGeometry(session.session_id, current.selection.Range());
  });
  const TextInputGeometry& geometry = resolved.snapshot->geometry;
  if (geometry.result_code != TextInputResultCode::Ok) {
    return false;
  }

  constexpr float margin = 8.0F;
  Rect target{
      geometry.caret.x - margin,
      geometry.caret.y - margin,
      geometry.caret.width + margin * 2.0F,
      geometry.caret.height + margin * 2.0F,
  };
  bool changed = false;
  for (auto ancestor = path.rbegin(); ancestor != path.rend(); ++ancestor) {
    if ((*ancestor)->scroll_state && !(*ancestor)->scroll_state->allows_automatic_reveal) {
      break;
    }
    if (!detail::ScrollNodeRectIntoView(**ancestor, target)) {
      continue;
    }
    changed = true;
  }
  return changed;
}

void detail::TextInteraction::StopTextInputSession(TextInputEndReason reason) {
  if (!text_input_session_.has_value()) {
    return;
  }

  detail::ActiveTextInputSession session = std::move(*text_input_session_);
  text_input_session_.reset();

  std::exception_ptr failure;
  try {
    session.client->EndTextInput(session.session_id, reason);
  } catch (...) {
    failure = std::current_exception();
  }

  if (PlatformTextInput* text_input = runtime_state_.platform_->TextInput()) {
    try {
      text_input->Stop(session.session_id);
    } catch (...) {
      if (!failure) {
        failure = std::current_exception();
      }
    }
  }

  if (failure) {
    std::rethrow_exception(failure);
  }
}

void detail::TextInteraction::RefreshTextInputSession() {
  // Callers that build a frame invoke this after layout and presentation have settled. Event handlers may also invoke
  // it immediately to keep native editing state synchronized without waiting for the next frame.
  detail::MountedNode* focused = nullptr;
  std::shared_ptr<TextInputClient> client;
  if (runtime_state_.mounted_root_ && runtime_state_.focused_node_identity_.has_value()) {
    focused = FindNode(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
    if (focused && focused->interaction.enabled && focused->focusable) {
      client = FindTextInputClient(*focused);
    }
  }

  if (text_input_session_.has_value()) {
    detail::ActiveTextInputSession& active = *text_input_session_;
    if (focused && focused->identity == active.node_identity && client == active.client) {
      const TextInputConfiguration configuration = active.client->Configuration();
      const TextInputState current = active.client->State();
      if (!IsValidConfiguration(configuration) || !IsValidTextInputState(current, active.session_id) ||
          !IsValidTextInputStateTransition(active.state, current)) {
        throw std::logic_error("HuxerUI text input client returned invalid state");
      }
      if (configuration.read_only) {
        StopTextInputSession(TextInputEndReason::ReadOnly);
        return;
      }
      if (text_selection_overlay_.state.visible && !text_selection_overlay_.state.dismissing &&
          current.selection.IsCollapsed() && text_selection_overlay_.state.show_handles) {
        HideTextSelectionOverlay();
      }

      const bool editing_state_changed = current.selection != active.state.selection ||
                                         current.composition != active.state.composition ||
                                         current.content_revision != active.state.content_revision;
      const bool restart =
          configuration != active.configuration || (active.state.composition.has_value() && editing_state_changed);
      const bool state_changed = current != active.state;
      const ResolvedGeometrySnapshot resolved = ResolveGeometrySnapshot(active, current, *focused, [&] {
        return QueryTextInputGeometry(active.session_id, current.selection.Range());
      });
      const bool geometry_changed =
          !active.published_geometry.has_value() || resolved.snapshot->geometry != active.published_geometry->geometry;
      active.configuration = configuration;
      active.state = current;
      if (PlatformTextInput* text_input = runtime_state_.platform_->TextInput()) {
        if (restart) {
          text_input->Restart(active.session_id, active.configuration, active.state, resolved.snapshot->geometry);
        } else if (state_changed || geometry_changed) {
          text_input->Update(active.session_id, active.state, resolved.snapshot->geometry);
        }
      }
      PublishGeometrySnapshot(active, resolved);
      return;
    }

    TextInputEndReason reason = TextInputEndReason::FocusLost;
    detail::MountedNode* previous = runtime_state_.mounted_root_
                                       ? FindNode(*runtime_state_.mounted_root_, text_input_session_->node_identity)
                                       : nullptr;
    if (!previous) {
      reason = TextInputEndReason::ClientRemoved;
    } else if (!previous->interaction.enabled) {
      reason = TextInputEndReason::Disabled;
    } else if (focused && focused->identity == previous->identity) {
      reason = TextInputEndReason::ClientRemoved;
    }
    StopTextInputSession(reason);
  }

  if (!focused || !client) {
    return;
  }

  const TextInputConfiguration configuration = client->Configuration();
  if (!IsValidConfiguration(configuration)) {
    throw std::logic_error("HuxerUI text input client returned invalid configuration");
  }
  if (configuration.read_only) {
    return;
  }
  if (next_text_input_session_id_ == std::numeric_limits<TextInputSessionId>::max()) {
    throw std::overflow_error("HuxerUI text input session identity overflow");
  }
  const TextInputSessionId session_id = next_text_input_session_id_++;
  const TextInputState initial = client->BeginTextInput(session_id);
  if (!IsValidTextInputState(initial, session_id)) {
    client->EndTextInput(session_id, TextInputEndReason::ClientRemoved);
    throw std::logic_error("HuxerUI text input client returned invalid initial state");
  }

  text_input_session_ = detail::ActiveTextInputSession{
      focused->identity,
      session_id,
      client,
      configuration,
      initial,
      std::nullopt,
      std::nullopt,
  };
  try {
    TextInputGeometry geometry = QueryTextInputGeometry(session_id, initial.selection.Range());
    if (PlatformTextInput* text_input = runtime_state_.platform_->TextInput()) {
      text_input->Start(session_id, configuration, initial, geometry);
    }
    text_input_session_->published_geometry = MakeGeometrySnapshot(initial, *focused, std::move(geometry));
  } catch (...) {
    text_input_session_.reset();
    client->EndTextInput(session_id, TextInputEndReason::ClientRemoved);
    throw;
  }
}

TextInputApplyResult detail::TextInteraction::HandleTextInputCommands(const TextInputCommandBatch& batch) {
  if (!text_input_session_.has_value() || batch.session_id != text_input_session_->session_id) {
    return {
        TextInputResultCode::SessionMismatch,
        TextInputSyncAction::None,
    };
  }

  detail::ActiveTextInputSession& active = *text_input_session_;
  const TextInputState previous = active.state;
  TextInputApplyResult result = active.client->ApplyTextInput(batch);
  if (!IsKnown(result.result_code) || !IsKnown(result.sync_action) ||
      (result.result_code != TextInputResultCode::Ok && result.sync_action != TextInputSyncAction::None)) {
    throw std::logic_error("HuxerUI text input client returned invalid apply result");
  }
  if (result.result_code != TextInputResultCode::Ok) {
    return result;
  }

  const TextInputConfiguration configuration = active.client->Configuration();
  const TextInputState current = active.client->State();
  if (!IsValidConfiguration(configuration) || !IsValidTextInputState(current, active.session_id) ||
      !IsValidTextInputStateTransition(previous, current)) {
    throw std::logic_error("HuxerUI text input client returned invalid state after applying commands");
  }
  if (configuration.read_only) {
    InvalidateTextInputStateChange(active.node_identity, previous, current);
    StopTextInputSession(TextInputEndReason::ReadOnly);
    return result;
  }

  const bool state_changed = current.revision != previous.revision;
  const bool restart = result.sync_action == TextInputSyncAction::Restart || configuration != active.configuration;
  const bool update = result.sync_action == TextInputSyncAction::Update || state_changed;
  detail::MountedNode* node =
      runtime_state_.mounted_root_ ? FindNode(*runtime_state_.mounted_root_, active.node_identity) : nullptr;
  std::optional<ResolvedGeometrySnapshot> resolved;
  std::optional<TextInputGeometry> uncached_geometry;
  if (node != nullptr) {
    resolved = ResolveGeometrySnapshot(active, current, *node, [&] {
      return QueryTextInputGeometry(active.session_id, current.selection.Range());
    });
  } else {
    uncached_geometry = QueryTextInputGeometry(active.session_id, current.selection.Range());
  }
  const TextInputGeometry& geometry = resolved.has_value() ? resolved->snapshot->geometry : *uncached_geometry;
  const bool geometry_changed =
      !active.published_geometry.has_value() || geometry != active.published_geometry->geometry;
  active.configuration = configuration;
  active.state = current;

  if (PlatformTextInput* text_input = runtime_state_.platform_->TextInput()) {
    if (restart) {
      text_input->Restart(active.session_id, active.configuration, active.state, geometry);
    } else if (update || geometry_changed) {
      text_input->Update(active.session_id, active.state, geometry);
    }
  }
  if (resolved.has_value()) {
    PublishGeometrySnapshot(active, *resolved);
  } else {
    active.prepared_geometry.reset();
  }
  InvalidateTextInputStateChange(active.node_identity, previous, current);
  return result;
}

void detail::TextInteraction::InvalidateTextInputStateChange(
    std::uint64_t node_identity, const TextInputState& previous, const TextInputState& current
) {
  if (current.revision == previous.revision) {
    return;
  }
  if (text_selection_overlay_.state.visible) {
    text_selection_overlay_.state.paint_dirty = true;
  }
  detail::MountedNode* node =
      runtime_state_.mounted_root_ ? FindNode(*runtime_state_.mounted_root_, node_identity) : nullptr;
  if (!node) {
    runtime_state_.owner_.RequestFrame();
    return;
  }
  node->foreground_paint_dirty = true;
  if (current.content_revision != previous.content_revision) {
    runtime_state_.owner_.InvalidateLayout(*node);
  } else {
    runtime_state_.owner_.RequestFrame();
  }
}

bool detail::TextInteraction::PerformTextInputAction(TextInputSessionId session_id, TextInputAction action) {
  if (!IsKnown(action) || !text_input_session_.has_value()) {
    return false;
  }
  const detail::ActiveTextInputSession& active = *text_input_session_;
  if (session_id != active.session_id || !MatchesConfiguredAction(active.configuration, action)) {
    return false;
  }
  return HandleFocusedTextInputKey({
      KeyEventType::Down,
      Key::Enter,
      {},
      {},
      false,
  });
}

TextInputContext detail::TextInteraction::QueryTextInputContext(
    TextInputSessionId session_id, TextOffset start, TextOffset length
) const {
  if (!text_input_session_.has_value() || session_id != text_input_session_->session_id) {
    return SessionMismatchContext(session_id);
  }
  if (start < 0 || length < 0) {
    TextInputContext result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  const TextInputContext result = text_input_session_->client->QueryTextInputContext(session_id, start, length);
  if (!IsValidContext(result, session_id)) {
    throw std::logic_error("HuxerUI text input client returned invalid context");
  }
  return result;
}

TextInputGeometry
detail::TextInteraction::QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const {
  if (!text_input_session_.has_value() || session_id != text_input_session_->session_id) {
    return SessionMismatchGeometry(session_id);
  }
  if (!range.IsValid()) {
    TextInputGeometry result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  const detail::ActiveTextInputSession& active = *text_input_session_;
  detail::MountedNode* node =
      runtime_state_.mounted_root_ ? FindNode(*runtime_state_.mounted_root_, active.node_identity) : nullptr;
  if (!node) {
    TextInputGeometry result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  TextInputGeometry result = active.client->QueryTextInputGeometry(session_id, range);
  if (!IsValidGeometry(result, session_id)) {
    throw std::logic_error("HuxerUI text input client returned invalid geometry");
  }
  if (result.result_code == TextInputResultCode::Ok) {
    result.caret = node->LocalToWindowBounds(result.caret);
    for (Rect& rect : result.range_rects) {
      rect = node->LocalToWindowBounds(rect);
    }
    if (!IsValidGeometry(result, session_id)) {
      throw std::logic_error("HuxerUI text input geometry transform produced invalid geometry");
    }
  }
  return result;
}

TextInputPositionResult
detail::TextInteraction::QueryTextInputPosition(TextInputSessionId session_id, Point point) const {
  if (!text_input_session_.has_value() || session_id != text_input_session_->session_id) {
    return SessionMismatchPosition(session_id);
  }
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    TextInputPositionResult result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  const detail::ActiveTextInputSession& active = *text_input_session_;
  detail::MountedNode* node =
      runtime_state_.mounted_root_ ? FindNode(*runtime_state_.mounted_root_, active.node_identity) : nullptr;
  const std::optional<Point> local = node ? node->WindowToLocal(point) : std::nullopt;
  if (!local.has_value()) {
    TextInputPositionResult result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  TextInputPositionResult result = active.client->QueryTextInputPosition(session_id, *local);
  if (!IsKnown(result.result_code) || result.session_id != session_id ||
      (result.result_code == TextInputResultCode::Ok &&
       (result.position.offset < 0 || !IsKnown(result.position.affinity)))) {
    throw std::logic_error("HuxerUI text input client returned an invalid position result");
  }
  return result;
}

bool detail::TextInteraction::HandleFocusedTextInputKey(const KeyEvent& event) {
  if (!text_input_session_.has_value() || !runtime_state_.focused_node_identity_.has_value()) {
    return false;
  }
  const detail::ActiveTextInputSession& active = *text_input_session_;
  if (active.node_identity != *runtime_state_.focused_node_identity_) {
    return false;
  }
  const std::uint64_t node_identity = active.node_identity;
  const TextInputSessionId session_id = active.session_id;
  const std::shared_ptr<TextInputClient> client = active.client;
  const TextInputState previous = active.state;

  const bool next_action = event.type == KeyEventType::Down && event.key == Key::Enter && !event.modifiers.shift &&
                           !event.modifiers.control && !event.modifiers.alt && !event.modifiers.meta &&
                           active.configuration.action == TextInputAction::Next;
  if (next_action && event.repeat) {
    return true;
  }
  if (client->HandleTextKey(event) != TextInputKeyResult::Handled) {
    return false;
  }

  const TextInputState current = client->State();
  if (!IsValidTextInputState(current, session_id) ||
      !IsValidTextInputStateTransition(previous, current)) {
    throw std::logic_error("HuxerUI text input client returned invalid state after handling a key event");
  }
  InvalidateTextInputStateChange(node_identity, previous, current);
  if (next_action) {
    runtime_state_.owner_.MoveFocus(false, false);
  }
  RefreshTextInputSession();
  return true;
}

} // namespace huxerui
