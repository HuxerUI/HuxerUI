#include "internal.h"
#include "text_input_internal.h"

#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace huxerui {
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

bool IsValidState(const TextInputState& state, TextInputSessionId session_id) noexcept {
  return state.session_id == session_id && IsValidSelectionShape(state.selection) &&
         (!state.composition.has_value() || state.composition->IsValid());
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

  const detail::ActiveTextInputSession& session = *state_->text_input_session_;
  std::vector<detail::MountedNode*> path;
  if (!FindNodePath(*state_->mounted_root_, session.node_identity, path)) {
    return false;
  }

  const TextInputGeometry geometry = QueryTextInputGeometry(session.session_id, session.state.selection.Range());
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
    if ((*ancestor)->scroll && !(*ancestor)->scroll->allows_automatic_reveal) {
      break;
    }
    if (!detail::ScrollNodeRectIntoView(**ancestor, target)) {
      continue;
    }
    NotifyScrollActivity(**ancestor);
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

  if (PlatformTextInput* platform = state_->platform_->TextInput()) {
    try {
      platform->Stop(session.session_id);
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
      if (!IsValidConfiguration(configuration) || !IsValidState(current, active.session_id)) {
        throw std::logic_error("HuxerUI text input client returned invalid state");
      }
      if (configuration.read_only) {
        StopTextInputSession(TextInputEndReason::ReadOnly);
        return;
      }
      if (state_->text_selection_overlay_.visible && !state_->text_selection_overlay_.dismissing &&
          current.selection.IsCollapsed() && state_->text_selection_overlay_.show_handles) {
        HideTextSelectionOverlay();
      }

      const bool restart = configuration != active.configuration ||
                           (active.state.composition.has_value() && current.revision != active.state.revision);
      const bool update = current != active.state;
      active.configuration = configuration;
      active.state = current;
      if (PlatformTextInput* platform = state_->platform_->TextInput()) {
        if (restart) {
          platform->Restart(active.session_id, active.configuration, active.state);
        } else if (update) {
          const TextInputGeometry geometry = QueryTextInputGeometry(active.session_id, active.state.selection.Range());
          platform->Update(active.session_id, active.state, geometry);
        }
      }
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
  if (!IsValidState(initial, session_id)) {
    client->EndTextInput(session_id, TextInputEndReason::ClientRemoved);
    throw std::logic_error("HuxerUI text input client returned invalid initial state");
  }

  state_->text_input_session_ = detail::ActiveTextInputSession{
      focused->identity,
      session_id,
      client,
      configuration,
      initial,
  };
  try {
    if (PlatformTextInput* platform = state_->platform_->TextInput()) {
      platform->Start(session_id, configuration, initial);
    }
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
        false,
    };
  }

  detail::ActiveTextInputSession& active = *state_->text_input_session_;
  TextInputApplyResult result = active.client->ApplyTextInput(batch);
  if (!IsKnown(result.result_code) || !IsKnown(result.sync_action) ||
      (result.result_code != TextInputResultCode::Ok &&
       (result.sync_action != TextInputSyncAction::None || result.changed))) {
    throw std::logic_error("HuxerUI text input client returned invalid apply result");
  }
  if (result.result_code != TextInputResultCode::Ok) {
    return result;
  }

  const TextInputConfiguration configuration = active.client->Configuration();
  const TextInputState current = active.client->State();
  if (!IsValidConfiguration(configuration) || !IsValidState(current, active.session_id)) {
    throw std::logic_error("HuxerUI text input client returned invalid state after applying commands");
  }
  if (configuration.read_only) {
    if (result.changed) {
      RequestFrame();
    }
    StopTextInputSession(TextInputEndReason::ReadOnly);
    return result;
  }

  const bool restart = result.sync_action == TextInputSyncAction::Restart || configuration != active.configuration;
  const bool update = result.sync_action == TextInputSyncAction::Update || result.changed || current != active.state;
  active.configuration = configuration;
  active.state = current;

  if (PlatformTextInput* platform = state_->platform_->TextInput()) {
    if (restart) {
      platform->Restart(active.session_id, active.configuration, active.state);
    } else if (update) {
      const TextInputGeometry geometry = QueryTextInputGeometry(active.session_id, active.state.selection.Range());
      platform->Update(active.session_id, active.state, geometry);
    }
  }
  if (result.changed) {
    RequestFrame();
  }
  return result;
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

  TextInputGeometry result = state_->text_input_session_->client->QueryTextInputGeometry(session_id, range);
  if (!IsKnown(result.result_code) || result.session_id != session_id) {
    throw std::logic_error("HuxerUI text input client returned geometry for another session");
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

  TextInputPositionResult result = state_->text_input_session_->client->QueryTextInputPosition(session_id, point);
  if (!IsKnown(result.result_code) || result.session_id != session_id ||
      (result.result_code == TextInputResultCode::Ok &&
       (result.position.offset < 0 || !IsKnown(result.position.affinity)))) {
    throw std::logic_error("HuxerUI text input client returned an invalid position result");
  }
  return result;
}

} // namespace huxerui
