#include "internal.h"
#include "text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace huxerui {

namespace detail {

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

} // namespace detail

namespace {

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

bool Runtime::BringTextInputIntoView() {
  if (!state_->mounted_root_ || !state_->text_input_session_.has_value()) {
    return false;
  }

  detail::ActiveTextInputSession& session = *state_->text_input_session_;
  std::vector<detail::MountedNode*> path;
  if (!FindNodePath(*state_->mounted_root_, session.node_identity, path)) {
    return false;
  }

  const TextInputState current = session.client->State();
  if (!detail::IsValidTextInputState(current, session.session_id) ||
      !detail::IsValidTextInputStateTransition(session.state, current)) {
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
    NotifyScrollActivity(**ancestor, ScrollActivitySource::TextInputReveal);
    changed = true;
  }
  return changed;
}

void Runtime::StopTextInputSession(TextInputEndReason reason) {
  if (!state_->text_input_session_.has_value()) {
    return;
  }

  detail::ActiveTextInputSession session = std::move(*state_->text_input_session_);
  state_->text_input_session_.reset();

  std::exception_ptr failure;
  try {
    session.client->EndTextInput(session.session_id, reason);
  } catch (...) {
    failure = std::current_exception();
  }

  if (PlatformTextInput* text_input = state_->platform_->TextInput()) {
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

void Runtime::RefreshTextInputSession() {
  // Callers that build a frame invoke this after layout and presentation have settled. Event handlers may also invoke
  // it immediately to keep native editing state synchronized without waiting for the next frame.
  detail::MountedNode* focused = nullptr;
  std::shared_ptr<TextInputClient> client;
  if (state_->mounted_root_ && state_->focused_node_identity_.has_value()) {
    focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    if (focused && focused->enabled && focused->focusable) {
      client = FindTextInputClient(*focused);
    }
  }

  if (state_->text_input_session_.has_value()) {
    detail::ActiveTextInputSession& active = *state_->text_input_session_;
    if (focused && focused->identity == active.node_identity && client == active.client) {
      const TextInputConfiguration configuration = active.client->Configuration();
      const TextInputState current = active.client->State();
      if (!IsValidConfiguration(configuration) || !detail::IsValidTextInputState(current, active.session_id) ||
          !detail::IsValidTextInputStateTransition(active.state, current)) {
        throw std::logic_error("HuxerUI text input client returned invalid state");
      }
      if (configuration.read_only) {
        StopTextInputSession(TextInputEndReason::ReadOnly);
        return;
      }
      if (state_->text_selection_overlay_.state.visible && !state_->text_selection_overlay_.state.dismissing &&
          current.selection.IsCollapsed() && state_->text_selection_overlay_.state.show_handles) {
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
      if (PlatformTextInput* text_input = state_->platform_->TextInput()) {
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
    detail::MountedNode* previous =
        state_->mounted_root_ ? FindNode(*state_->mounted_root_, state_->text_input_session_->node_identity) : nullptr;
    if (!previous) {
      reason = TextInputEndReason::ClientRemoved;
    } else if (!previous->enabled) {
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
  if (state_->next_text_input_session_id_ == std::numeric_limits<TextInputSessionId>::max()) {
    throw std::overflow_error("HuxerUI text input session identity overflow");
  }
  const TextInputSessionId session_id = state_->next_text_input_session_id_++;
  const TextInputState initial = client->BeginTextInput(session_id);
  if (!detail::IsValidTextInputState(initial, session_id)) {
    client->EndTextInput(session_id, TextInputEndReason::ClientRemoved);
    throw std::logic_error("HuxerUI text input client returned invalid initial state");
  }

  state_->text_input_session_ = detail::ActiveTextInputSession{
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
    if (PlatformTextInput* text_input = state_->platform_->TextInput()) {
      text_input->Start(session_id, configuration, initial, geometry);
    }
    state_->text_input_session_->published_geometry = MakeGeometrySnapshot(initial, *focused, std::move(geometry));
  } catch (...) {
    state_->text_input_session_.reset();
    client->EndTextInput(session_id, TextInputEndReason::ClientRemoved);
    throw;
  }
}

TextInputApplyResult Runtime::HandleTextInputCommands(const TextInputCommandBatch& batch) {
  if (!state_->text_input_session_.has_value() || batch.session_id != state_->text_input_session_->session_id) {
    return {
        TextInputResultCode::SessionMismatch,
        TextInputSyncAction::None,
    };
  }

  detail::ActiveTextInputSession& active = *state_->text_input_session_;
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
  if (!IsValidConfiguration(configuration) || !detail::IsValidTextInputState(current, active.session_id) ||
      !detail::IsValidTextInputStateTransition(previous, current)) {
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
  detail::MountedNode* node = state_->mounted_root_ ? FindNode(*state_->mounted_root_, active.node_identity) : nullptr;
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

  if (PlatformTextInput* text_input = state_->platform_->TextInput()) {
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

void Runtime::InvalidateTextInputStateChange(
    std::uint64_t node_identity, const TextInputState& previous, const TextInputState& current
) {
  if (current.revision == previous.revision) {
    return;
  }
  if (state_->text_selection_overlay_.state.visible) {
    state_->text_selection_overlay_.state.paint_dirty = true;
  }
  detail::MountedNode* node = state_->mounted_root_ ? FindNode(*state_->mounted_root_, node_identity) : nullptr;
  if (!node) {
    RequestFrame();
    return;
  }
  node->foreground_paint_dirty = true;
  if (current.content_revision != previous.content_revision) {
    InvalidateLayout(*node);
  } else {
    RequestFrame();
  }
}

bool Runtime::PerformTextInputAction(TextInputSessionId session_id, TextInputAction action) {
  if (!IsKnown(action) || !state_->text_input_session_.has_value()) {
    return false;
  }
  const detail::ActiveTextInputSession& active = *state_->text_input_session_;
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

TextInputContext
Runtime::QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const {
  if (!state_->text_input_session_.has_value() || session_id != state_->text_input_session_->session_id) {
    return SessionMismatchContext(session_id);
  }
  if (start < 0 || length < 0) {
    TextInputContext result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  const TextInputContext result = state_->text_input_session_->client->QueryTextInputContext(session_id, start, length);
  if (!IsValidContext(result, session_id)) {
    throw std::logic_error("HuxerUI text input client returned invalid context");
  }
  return result;
}

TextInputGeometry Runtime::QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const {
  if (!state_->text_input_session_.has_value() || session_id != state_->text_input_session_->session_id) {
    return SessionMismatchGeometry(session_id);
  }
  if (!range.IsValid()) {
    TextInputGeometry result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  const detail::ActiveTextInputSession& active = *state_->text_input_session_;
  detail::MountedNode* node = state_->mounted_root_ ? FindNode(*state_->mounted_root_, active.node_identity) : nullptr;
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
    result.caret = detail::TransformBounds(node->presentation.resolved_transform, result.caret);
    for (Rect& rect : result.range_rects) {
      rect = detail::TransformBounds(node->presentation.resolved_transform, rect);
    }
    if (!IsValidGeometry(result, session_id)) {
      throw std::logic_error("HuxerUI text input geometry transform produced invalid geometry");
    }
  }
  return result;
}

TextInputPositionResult Runtime::QueryTextInputPosition(TextInputSessionId session_id, Point point) const {
  if (!state_->text_input_session_.has_value() || session_id != state_->text_input_session_->session_id) {
    return SessionMismatchPosition(session_id);
  }
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    TextInputPositionResult result;
    result.result_code = TextInputResultCode::Rejected;
    result.session_id = session_id;
    return result;
  }

  const detail::ActiveTextInputSession& active = *state_->text_input_session_;
  detail::MountedNode* node = state_->mounted_root_ ? FindNode(*state_->mounted_root_, active.node_identity) : nullptr;
  const std::optional<Point> local = node ? node->presentation.resolved_transform.Inverse(point) : std::nullopt;
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

} // namespace huxerui
