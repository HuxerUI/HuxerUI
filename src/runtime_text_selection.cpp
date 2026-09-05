#include "runtime_internal.h"
#include "runtime_pointer_internal.h"
#include "runtime_text_internal.h"
#include "text_input_internal.h"
#include "window_internal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace huxerui {

namespace detail {

// Keyboard focus may belong to a descendant link; resolve the nearest selection capability without node-kind checks.
MountedNode* FindTextSelectionOwner(MountedNode& root, std::uint64_t identity) {
  const auto find = [&](auto&& self, MountedNode& node, MountedNode* owner) -> MountedNode* {
    if (FindTextSelectionClient(node)) {
      owner = &node;
    }
    if (node.identity == identity) {
      return owner;
    }
    for (const auto& child : node.children) {
      if (auto* found = self(self, *child, owner)) {
        return found;
      }
    }
    return nullptr;
  };
  return find(find, root, nullptr);
}

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

// An active editor owns clipboard actions and secure-text policy before any enclosing read-only selection area.
bool detail::TextInteraction::CanPerformTextEditingAction(TextEditingAction action) const {
  if (!text_input_session_.has_value()) {
    if (!runtime_state_.mounted_root_ || !runtime_state_.focused_node_identity_.has_value()) {
      return false;
    }
    detail::MountedNode* focused =
        detail::FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
    TextSelectionClient* client = focused ? detail::FindTextSelectionClient(*focused) : nullptr;
    return client && client->CanPerformTextEditingAction(action, runtime_state_.platform_->Clipboard());
  }
  const detail::ActiveTextInputSession& session = *text_input_session_;
  const TextRange selection = session.state.selection.Range();
  switch (action) {
  case TextEditingAction::Cut:
    return !session.configuration.read_only && !session.configuration.secure && !selection.IsCollapsed() &&
           runtime_state_.platform_->Clipboard() != nullptr;
  case TextEditingAction::Copy:
    return !session.configuration.secure && !selection.IsCollapsed() &&
           runtime_state_.platform_->Clipboard() != nullptr;
  case TextEditingAction::Paste: {
    if (session.configuration.read_only) {
      return false;
    }
    PlatformClipboard* clipboard = runtime_state_.platform_->Clipboard();
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

bool detail::TextInteraction::PerformTextEditingAction(TextEditingAction action) {
  if (!text_input_session_.has_value()) {
    if (!runtime_state_.mounted_root_ || !runtime_state_.focused_node_identity_.has_value()) {
      return false;
    }
    detail::MountedNode* focused =
        detail::FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
    TextSelectionClient* client = focused ? detail::FindTextSelectionClient(*focused) : nullptr;
    if (!client || !client->PerformTextEditingAction(action, runtime_state_.platform_->Clipboard())) {
      return false;
    }
    focused->foreground_paint_dirty = true;
    return true;
  }
  detail::ActiveTextInputSession& session = *text_input_session_;
  const TextRange selection = session.state.selection.Range();
  PlatformClipboard* clipboard = runtime_state_.platform_->Clipboard();

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

void detail::TextInteraction::HandleTextSelectionTap(std::optional<std::uint64_t> target) {
  HideTextSelectionOverlay();
  runtime_state_.owner_.RequestFrame();
  if (!runtime_state_.mounted_root_ || !runtime_state_.focused_node_identity_) {
    return;
  }
  // A tap inside the active editor already places its caret through the extension's pointer handling.
  if (text_input_session_ && target == text_input_session_->node_identity) {
    return;
  }
  auto* owner = FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
  auto* client = owner ? FindTextSelectionClient(*owner) : nullptr;
  if (client && owner->interaction.enabled) {
    owner->foreground_paint_dirty = true;
    client->ClearSelection();
  }
}

bool detail::TextInteraction::SelectTextWord(std::uint64_t node, Point position, bool show_overlay) {
  const auto selection_owner = [&]() -> detail::MountedNode* {
    return runtime_state_.mounted_root_ && runtime_state_.focused_node_identity_
               ? detail::FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_)
               : nullptr;
  };
  detail::MountedNode* focused = selection_owner();
  if (!focused || focused->identity != node || !focused->interaction.enabled) {
    return false;
  }
  TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
  const std::optional<Point> local = client ? focused->WindowToLocal(position) : std::nullopt;
  const bool changed = local.has_value() && client->SelectWord(*local);
  focused = selection_owner();
  if (!focused || focused->identity != node || !focused->interaction.enabled) {
    return changed;
  }
  if (changed) {
    focused->foreground_paint_dirty = true;
    RefreshTextInputSession();
    focused = selection_owner();
    if (!focused || focused->identity != node || !focused->interaction.enabled) {
      return true;
    }
    text_selection_overlay_.state.paint_dirty = true;
    if (show_overlay) {
      ShowTextSelectionOverlay(true);
    }
    runtime_state_.owner_.RequestFrame();
    return true;
  }
  if (show_overlay && client && CanPerformTextEditingAction(TextEditingAction::Paste)) {
    ShowTextSelectionOverlay(false);
    runtime_state_.owner_.RequestFrame();
    return true;
  }
  return false;
}

bool detail::TextInteraction::ExtendFocusedTextSelection(Point position, bool start_handle) {
  if (!runtime_state_.mounted_root_ || !runtime_state_.focused_node_identity_.has_value()) {
    return false;
  }
  detail::MountedNode* focused =
      detail::FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
  if (!focused) {
    return false;
  }
  TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
  const std::optional<Point> local = client ? focused->WindowToLocal(position) : std::nullopt;
  const bool changed = local.has_value() && client->ExtendSelection(*local, start_handle);
  if (changed) {
    focused->foreground_paint_dirty = true;
    RefreshTextInputSession();
    text_selection_overlay_.state.paint_dirty = true;
    runtime_state_.owner_.RequestFrame();
  }
  return changed;
}

void detail::TextInteraction::AdvanceTextSelectionDrag(const FrameInfo& frame) {
  if (!runtime_state_.mounted_root_) {
    return;
  }
  // The previous scroll is laid out before this call; a stationary pointer must hit the newly realized paragraphs.
  const bool retest = std::exchange(selection_scroll_pending_, false);
  const auto scroll = [&](detail::MountedNode& owner, Point position) {
    const Size viewport_size = runtime_state_.window_->metrics.viewport;
    const Point route_position{
        std::clamp(position.x, 0.0F, std::max(0.0F, viewport_size.width - 0.01F)),
        std::clamp(position.y, 0.0F, std::max(0.0F, viewport_size.height - 0.01F))
    };
    std::vector<detail::MountedNode*> route;
    if (!BuildPointerRoute(*runtime_state_.mounted_root_, route_position, route) ||
        std::find(route.begin(), route.end(), &owner) == route.end()) {
      return;
    }
    // Prefer the deepest eligible viewport; when it is exhausted, an ancestor may continue the selection reveal.
    for (auto candidate = route.rbegin(); candidate != route.rend(); ++candidate) {
      auto& node = **candidate;
      if (!node.IsEnabled() || !detail::IsScrollContainer(node) ||
          !detail::AllowsScrollSource(node, ScrollSource::FocusReveal)) {
        continue;
      }
      const auto local = node.WindowToLocal(position);
      if (!local) {
        continue;
      }
      const Rect viewport = detail::ScrollViewport(node);
      const bool vertical = detail::ScrollAxis(node) == Axis::Vertical;
      const float start = vertical ? viewport.y : viewport.x;
      const float extent = vertical ? viewport.height : viewport.width;
      const float value = vertical ? local->y : local->x;
      const float edge = std::min(32.0F, extent * 0.5F);
      if (edge <= 0.0F) {
        continue;
      }
      const float intensity = value < start + edge ? -std::clamp((start + edge - value) / edge, 0.0F, 1.0F)
                              : value > start + extent - edge
                                  ? std::clamp((value - start - extent + edge) / edge, 0.0F, 1.0F) : 0.0F;
      if (intensity == 0.0F || !detail::CanScrollNode(node, intensity)) {
        continue;
      }
      if (frame.delta_time <= 0.0) {
        runtime_state_.owner_.RequestFrame();
        return;
      }
      const float delta = intensity * 600.0F * static_cast<float>(frame.delta_time);
      if (detail::ScrollNodeBy(node, delta, ScrollSource::FocusReveal) != 0.0F) {
        selection_scroll_pending_ = true;
        runtime_state_.owner_.RequestFrame();
        return;
      }
    }
  };
  auto& overlay = text_selection_overlay_.state;
  if (overlay.dragging && overlay.pointer_id && runtime_state_.focused_node_identity_) {
    auto* owner = detail::FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
    if (owner && owner->IsEnabled()) {
      if (retest) {
        ExtendFocusedTextSelection(overlay.drag_position, overlay.dragging_start_handle);
      }
      scroll(*owner, overlay.drag_position);
    }
    return;
  }
  // Auto-scroll follows only the winning selection extension, never a still-observing link or canceled session.
  for (auto& [pointer_id, session] : runtime_state_.pointer_->pointer_sessions_) {
    if (session.quarantined || !session.owner || session.device_kind == PointerDeviceKind::Touch) {
      continue;
    }
    const auto* index = std::get_if<std::size_t>(&*session.owner);
    if (!index || *index >= session.recognitions.size()) {
      continue;
    }
    const auto* recognition = std::get_if<detail::ExtensionRecognitionState>(&session.recognitions[*index].state);
    if (!recognition) {
      continue;
    }
    NodeExtension* extension = FindExtension(*runtime_state_.mounted_root_, recognition->extension);
    auto* owner = FindNode(*runtime_state_.mounted_root_, recognition->extension.node_identity);
    if (!owner || !owner->IsEnabled() || !extension || !extension->GetTextSelectionClient() ||
        std::hypot(session.last_position.x - session.down_position.x,
            session.last_position.y - session.down_position.y) < runtime_state_.gesture_settings_.pointer_slop) {
      continue;
    }
    if (retest) {
      if (const auto local = owner->WindowToLocal(session.last_position)) {
        extension->OnPointer(
            *owner,
            {PointerEventType::Move,
             pointer_id,
             *local,
             session.device_kind,
             PointerButton::None,
             PointerButton::Primary}
        );
      }
    }
    scroll(*owner, session.last_position);
  }
}

std::optional<TextSelectionGeometry> detail::TextInteraction::QueryFocusedTextSelectionGeometry() const {
  if (!runtime_state_.mounted_root_ || !runtime_state_.focused_node_identity_.has_value()) {
    return {};
  }
  detail::MountedNode* focused =
      detail::FindTextSelectionOwner(*runtime_state_.mounted_root_, *runtime_state_.focused_node_identity_);
  if (!focused) {
    return {};
  }
  TextSelectionClient* client = detail::FindTextSelectionClient(*focused);
  if (!client) {
    return {};
  }
  // Preserve missing endpoints while translating available geometry to the overlay's window coordinate space.
  auto geometry = client->QuerySelectionGeometry();
  if (!geometry) {
    return std::nullopt;
  }
  const auto transform = [&](std::optional<Rect>& rect) {
    if (rect) {
      rect = focused->LocalToWindowBounds(*rect);
    }
  };
  transform(geometry->start);
  transform(geometry->end);
  transform(geometry->toolbar_anchor);
  return geometry;
}

} // namespace huxerui
