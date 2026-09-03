#include "internal.h"
#include "text_input_internal.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace huxerui {

namespace detail {

TextSelectionClient* FindTextSelectionClient(MountedNode& node) {
  TextSelectionClient* result = nullptr;
  for (NodeExtensionEntry& entry : node.extensions) {
    if (!entry.extension) {
      continue;
    }
    TextSelectionClient* client = entry.extension->GetTextSelectionClient();
    if (!client) {
      continue;
    }
    if (result) {
      throw std::logic_error("HuxerUI focusable node must not expose multiple text selection clients");
    }
    result = client;
  }
  return result;
}

} // namespace detail

bool Runtime::CanPerformTextEditingAction(TextEditingAction action) const {
  if (!state_->text_input_session_.has_value()) {
    if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
      return false;
    }
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    TextSelectionClient* client = focused ? detail::FindTextSelectionClient(*focused) : nullptr;
    return client && client->CanPerformTextEditingAction(action, state_->platform_->Clipboard());
  }
  const detail::ActiveTextInputSession& session = *state_->text_input_session_;
  const TextRange selection = session.state.selection.Range();
  switch (action) {
  case TextEditingAction::Cut:
    return !session.configuration.read_only && !session.configuration.secure && !selection.IsCollapsed() &&
           state_->platform_->Clipboard() != nullptr;
  case TextEditingAction::Copy:
    return !session.configuration.secure && !selection.IsCollapsed() && state_->platform_->Clipboard() != nullptr;
  case TextEditingAction::Paste: {
    if (session.configuration.read_only) {
      return false;
    }
    PlatformClipboard* clipboard = state_->platform_->Clipboard();
    return clipboard != nullptr && clipboard->ReadText().has_value();
  }
  case TextEditingAction::SelectAll: {
    const TextInputContext context = session.client->QueryTextInputContext(session.session_id, 0, 0);
    return context.result_code == TextInputResultCode::Ok && context.total_length > 0 &&
           selection != TextRange{0, context.total_length};
  }
  }
  return false;
}

bool Runtime::PerformTextEditingAction(TextEditingAction action) {
  if (!state_->text_input_session_.has_value()) {
    if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
      return false;
    }
    detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
    TextSelectionClient* client = focused ? detail::FindTextSelectionClient(*focused) : nullptr;
    if (!client || !client->PerformTextEditingAction(action, state_->platform_->Clipboard())) {
      return false;
    }
    focused->foreground_paint_dirty = true;
    return true;
  }
  detail::ActiveTextInputSession& session = *state_->text_input_session_;
  const TextRange selection = session.state.selection.Range();
  PlatformClipboard* clipboard = state_->platform_->Clipboard();

  if (action == TextEditingAction::Copy || action == TextEditingAction::Cut) {
    if (!CanPerformTextEditingAction(action) || clipboard == nullptr) {
      return false;
    }
    const TextInputContext context =
        session.client->QueryTextInputContext(session.session_id, selection.start, selection.Length());
    if (context.result_code != TextInputResultCode::Ok || context.slice_start > selection.start) {
      return false;
    }
    const TextRange relative{
        selection.start - context.slice_start,
        selection.end - context.slice_start,
    };
    const std::optional<std::string> selected = detail::Utf8TextInRange(context.text, relative);
    if (!selected.has_value() || !clipboard->WriteText(*selected)) {
      return false;
    }
    if (action == TextEditingAction::Copy) {
      return true;
    }
  }

  TextInputCommand command;
  if (action == TextEditingAction::Paste) {
    if (session.configuration.read_only || clipboard == nullptr) {
      return false;
    }
    const std::optional<std::string> text = clipboard->ReadText();
    if (!text.has_value()) {
      return false;
    }
    command.kind = TextInputCommandKind::CommitText;
    command.text = *text;
    if (!session.state.composition.has_value()) {
      command.target = selection;
    }
  } else if (action == TextEditingAction::SelectAll) {
    const TextInputContext context = session.client->QueryTextInputContext(session.session_id, 0, 0);
    if (context.result_code != TextInputResultCode::Ok || context.total_length <= 0) {
      return false;
    }
    command.kind = TextInputCommandKind::SetSelection;
    command.selection_after = TextSelection{0, context.total_length};
  } else if (action == TextEditingAction::Cut) {
    command.kind = TextInputCommandKind::CommitText;
  } else {
    return false;
  }

  const TextInputApplyResult result = HandleTextInputCommands({session.session_id, {std::move(command)}});
  return result.result_code == TextInputResultCode::Ok;
}

bool Runtime::SelectFocusedTextWord(Point position, bool show_overlay) {
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused) {
    return false;
  }
  TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
  const std::optional<Point> local = client ? focused->WindowToLocal(position) : std::nullopt;
  const bool changed = local.has_value() && client->SelectWord(*local);
  if (changed) {
    focused->foreground_paint_dirty = true;
    RefreshTextInputSession();
    state_->text_selection_overlay_.state.paint_dirty = true;
    if (show_overlay) {
      ShowTextSelectionOverlay(true);
    }
    RequestFrame();
    return true;
  }
  if (show_overlay && client && CanPerformTextEditingAction(TextEditingAction::Paste)) {
    ShowTextSelectionOverlay(false);
    RequestFrame();
    return true;
  }
  return false;
}

bool Runtime::ExtendFocusedTextSelection(Point position, bool start_handle) {
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused) {
    return false;
  }
  TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
  const std::optional<Point> local = client ? focused->WindowToLocal(position) : std::nullopt;
  const bool changed = local.has_value() && client->ExtendSelection(*local, start_handle);
  if (changed) {
    focused->foreground_paint_dirty = true;
    RefreshTextInputSession();
    state_->text_selection_overlay_.state.paint_dirty = true;
    RequestFrame();
  }
  return changed;
}

bool Runtime::QueryFocusedTextSelectionGeometry(Rect& start, Rect& end) const {
  if (!state_->mounted_root_ || !state_->focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused = FindNode(*state_->mounted_root_, *state_->focused_node_identity_);
  if (!focused) {
    return false;
  }
  TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
  if (!client || !client->QuerySelectionGeometry(start, end)) {
    return false;
  }
  start = focused->LocalToWindowBounds(start);
  end = focused->LocalToWindowBounds(end);
  return true;
}

} // namespace huxerui
