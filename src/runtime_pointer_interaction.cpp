#include "internal.h"
#include "window_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <utility>

namespace huxerui::detail {

namespace {

bool HandlesPointerEvents(const MountedNode& node) {
  return static_cast<bool>(node.activation) || HasEventBinding<ViewEvents::Click>(node.event_bindings) ||
         HasEventBinding<ViewEvents::Pointer>(node.event_bindings);
}

float PointerDelta(Point previous, Point current, Axis axis) {
  return axis == Axis::Vertical ? previous.y - current.y : previous.x - current.x;
}

bool SupportsHover(PointerDeviceKind device_kind) {
  return device_kind == PointerDeviceKind::Mouse || device_kind == PointerDeviceKind::Pen;
}

PointerEvent NormalizePointerEvent(const PointerEvent& event) noexcept {
  PointerEvent normalized = event;
  if (normalized.type == PointerEventType::Cancel) {
    normalized.changed_button = PointerButton::None;
    normalized.pressed_buttons = PointerButton::None;
    return normalized;
  }
  if ((normalized.type == PointerEventType::Down || normalized.type == PointerEventType::Up) &&
      normalized.changed_button == PointerButton::None) {
    normalized.changed_button = PointerButton::Primary;
  }
  if (normalized.type == PointerEventType::Down && normalized.pressed_buttons == PointerButton::None) {
    normalized.pressed_buttons = normalized.changed_button;
  }
  return normalized;
}

PointerEvent CancellationEvent(const PointerEvent& event) noexcept {
  PointerEvent cancellation = event;
  cancellation.type = PointerEventType::Cancel;
  cancellation.changed_button = PointerButton::None;
  cancellation.pressed_buttons = PointerButton::None;
  return cancellation;
}

bool HasMultipleButtons(PointerButton buttons) noexcept {
  return std::popcount(static_cast<std::uint32_t>(buttons)) > 1;
}

void RecordScrollVelocitySample(PointerSession& session, Point position, double timestamp) {
  if (!std::isfinite(timestamp)) {
    return;
  }
  if (session.scroll_velocity_sample_count > 0) {
    ScrollVelocitySample& latest = session.scroll_velocity_samples[session.scroll_velocity_sample_count - 1];
    if (timestamp < latest.timestamp) {
      session.scroll_velocity_sample_count = 0;
    } else if (timestamp == latest.timestamp) {
      latest.position = position;
      return;
    }
  }

  if (session.scroll_velocity_sample_count == session.scroll_velocity_samples.size()) {
    for (std::size_t index = 1; index < session.scroll_velocity_samples.size(); ++index) {
      session.scroll_velocity_samples[index - 1] = session.scroll_velocity_samples[index];
    }
    --session.scroll_velocity_sample_count;
  }
  session.scroll_velocity_samples[session.scroll_velocity_sample_count++] = {position, timestamp};
}

std::optional<float> EstimateScrollVelocity(const PointerSession& session, Axis axis, double release_timestamp) {
  constexpr double maximum_sample_age = 0.1;
  if (session.scroll_velocity_sample_count < 2 || !std::isfinite(release_timestamp)) {
    return std::nullopt;
  }

  const ScrollVelocitySample& latest = session.scroll_velocity_samples[session.scroll_velocity_sample_count - 1];
  const double release_age = release_timestamp - latest.timestamp;
  if (!std::isfinite(release_age) || release_age < 0.0 || release_age > maximum_sample_age) {
    return std::nullopt;
  }

  const double cutoff = latest.timestamp - maximum_sample_age;
  std::size_t first = 0;
  while (first < session.scroll_velocity_sample_count && session.scroll_velocity_samples[first].timestamp < cutoff) {
    ++first;
  }
  const std::size_t sample_count = session.scroll_velocity_sample_count - first;
  if (sample_count < 2) {
    return std::nullopt;
  }

  const ScrollVelocitySample& previous = session.scroll_velocity_samples[session.scroll_velocity_sample_count - 2];
  const double latest_elapsed = latest.timestamp - previous.timestamp;
  if (!std::isfinite(latest_elapsed) || latest_elapsed <= 0.0) {
    return std::nullopt;
  }
  const double fallback_velocity =
      static_cast<double>(PointerDelta(previous.position, latest.position, axis)) / latest_elapsed;
  if (!std::isfinite(fallback_velocity)) {
    return std::nullopt;
  }
  if (sample_count == 2) {
    return static_cast<float>(fallback_velocity);
  }

  double weighted_velocity = 0.0;
  double weight_sum = 0.0;
  for (std::size_t index = first + 1; index < session.scroll_velocity_sample_count; ++index) {
    const ScrollVelocitySample& segment_start = session.scroll_velocity_samples[index - 1];
    const ScrollVelocitySample& segment_end = session.scroll_velocity_samples[index];
    const double elapsed = segment_end.timestamp - segment_start.timestamp;
    if (!std::isfinite(elapsed) || elapsed <= 0.0) {
      continue;
    }
    const double weight = static_cast<double>(index - first);
    weighted_velocity +=
        static_cast<double>(PointerDelta(segment_start.position, segment_end.position, axis)) / elapsed * weight;
    weight_sum += weight;
  }
  if (weight_sum <= 0.0) {
    return static_cast<float>(fallback_velocity);
  }
  const double trend_velocity = weighted_velocity / weight_sum;
  if (!std::isfinite(trend_velocity) || trend_velocity == 0.0) {
    return std::nullopt;
  }

  // A quadratic position fit estimates the derivative at release, so an accelerating gesture is not reduced to its
  // average speed. Time is normalized to the sampling window to keep the normal equation well-conditioned.
  double time_sum = 0.0;
  double time2_sum = 0.0;
  double time3_sum = 0.0;
  double time4_sum = 0.0;
  double position_sum = 0.0;
  double time_position_sum = 0.0;
  double time2_position_sum = 0.0;
  for (std::size_t index = first; index < session.scroll_velocity_sample_count; ++index) {
    const ScrollVelocitySample& sample = session.scroll_velocity_samples[index];
    const double time = (sample.timestamp - latest.timestamp) / maximum_sample_age;
    const double time2 = time * time;
    const double position = axis == Axis::Vertical ? -sample.position.y : -sample.position.x;
    time_sum += time;
    time2_sum += time2;
    time3_sum += time2 * time;
    time4_sum += time2 * time2;
    position_sum += position;
    time_position_sum += time * position;
    time2_position_sum += time2 * position;
  }

  const double count = static_cast<double>(sample_count);
  const double determinant = count * (time2_sum * time4_sum - time3_sum * time3_sum) -
                             time_sum * (time_sum * time4_sum - time2_sum * time3_sum) +
                             time2_sum * (time_sum * time3_sum - time2_sum * time2_sum);
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
    return static_cast<float>(trend_velocity);
  }
  const double velocity_determinant = count * (time_position_sum * time4_sum - time3_sum * time2_position_sum) -
                                      position_sum * (time_sum * time4_sum - time2_sum * time3_sum) +
                                      time2_sum * (time_sum * time2_position_sum - time_position_sum * time2_sum);
  const double velocity = velocity_determinant / determinant / maximum_sample_age;
  if (!std::isfinite(velocity)) {
    return static_cast<float>(trend_velocity);
  }
  if (std::signbit(velocity) != std::signbit(trend_velocity)) {
    return std::signbit(fallback_velocity) == std::signbit(trend_velocity)
               ? std::optional<float>{static_cast<float>(fallback_velocity)}
               : std::nullopt;
  }

  constexpr double maximum_extrapolation_ratio = 2.0;
  const double maximum_velocity = std::abs(trend_velocity) * maximum_extrapolation_ratio;
  return static_cast<float>(std::clamp(velocity, -maximum_velocity, maximum_velocity));
}

PointerEvent LocalPointerEvent(const MountedNode& node, const PointerEvent& event) {
  PointerEvent local = event;
  if (const auto position = node.presentation.resolved_transform.Inverse(event.position)) {
    local.position = *position;
  } else {
    local.position = {
        node.bounds.x + node.bounds.width * 0.5F,
        node.bounds.y + node.bounds.height * 0.5F,
    };
  }
  return local;
}

GestureRecognizerInput GestureInput(const GestureRecognitionState& retained, const PointerEvent& event,
                                    double timestamp) {
  PointerEvent local = event;
  if (const std::optional<Point> position = retained.frozen_node_to_window.Inverse(event.position)) {
    local.position = *position;
  }
  return {local, event.position, timestamp};
}

GestureRecognizerInput GestureInput(const DragSourceRecognitionState& retained, const PointerEvent& event,
                                    double timestamp) {
  PointerEvent local = event;
  if (const std::optional<Point> position = retained.frozen_node_to_window.Inverse(event.position)) {
    local.position = *position;
  }
  return {local, event.position, timestamp};
}

DropEvent LocalDropEvent(const MountedNode& node, const DragEvent& drag) {
  Point local = drag.window_position;
  if (const std::optional<Point> position = node.presentation.resolved_transform.Inverse(drag.window_position)) {
    local = *position;
  }
  return {drag.pointer_id, drag.device_kind, local, drag.window_position};
}

LayerPlacement DragPreviewPlacement(Point position, Point grab_offset) {
  return {
      .kind = LayerPlacementKind::Anchored,
      .anchor = {position.x, position.y, 0.0F, 0.0F},
      .preferred_side = LayerAnchorSide::Below,
      .alignment = LayerAnchorAlignment::Start,
      .gap = 0.0F,
      .viewport_margin = 8.0F,
      .offset = {-grab_offset.x, -grab_offset.y},
  };
}

std::optional<std::uint64_t> PointerRecognitionNodeIdentity(const PointerRecognition& recognition) {
  if (const auto* tap = std::get_if<TapRecognitionState>(&recognition.state)) {
    return tap->node_identity;
  }
  if (const auto* context_menu = std::get_if<ContextMenuRecognitionState>(&recognition.state)) {
    return context_menu->node_identity;
  }
  if (const auto* extension = std::get_if<ExtensionRecognitionState>(&recognition.state)) {
    return extension->extension.node_identity;
  }
  if (const auto* gesture = std::get_if<GestureRecognitionState>(&recognition.state)) {
    return gesture->extension.node_identity;
  }
  if (const auto* source = std::get_if<DragSourceRecognitionState>(&recognition.state)) {
    return source->extension.node_identity;
  }
  return std::nullopt;
}

std::optional<std::size_t> RecognitionOwnerIndex(const PointerSession& session) {
  if (!session.owner.has_value()) {
    return std::nullopt;
  }
  if (const auto* index = std::get_if<std::size_t>(&*session.owner)) {
    return *index;
  }
  return std::nullopt;
}

bool TextSelectionOverlayOwnsPointer(const PointerSession& session) {
  return session.owner.has_value() && std::holds_alternative<TextSelectionOverlayOwner>(*session.owner);
}

} // namespace

void UpdateInteraction(MountedNode& node, InteractionState state, std::optional<InteractionEvent> event) {
  if (node.interaction == state && !event.has_value()) {
    return;
  }
  node.interaction = state;
  for (NodeExtensionEntry& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->OnInteraction(node, node.interaction, event);
    }
  }
}

} // namespace huxerui::detail

namespace huxerui {

using namespace detail;

detail::MountedNode* Runtime::FindNode(detail::MountedNode& node, std::uint64_t identity) {
  if (node.identity == identity) {
    return &node;
  }
  for (auto& child : node.children) {
    if (detail::MountedNode* found = FindNode(*child, identity)) {
      return found;
    }
  }
  return nullptr;
}

NodeExtension* Runtime::FindExtension(detail::MountedNode& root, const NodeExtensionHandle& handle) {
  detail::MountedNode* node = FindNode(root, handle.node_identity);
  if (!node || handle.extension_index >= node->extensions.size()) {
    return nullptr;
  }
  NodeExtensionEntry& entry = node->extensions[handle.extension_index];
  if (entry.descriptor != handle.descriptor || !entry.extension) {
    return nullptr;
  }
  return entry.extension.get();
}

void Runtime::ActivateNode(detail::MountedNode& node) {
  if (node.activation) {
    node.activation(node.event_bindings);
  } else {
    EmitEvent<ViewEvents::Click>(node.event_bindings);
  }
}

std::uint64_t Runtime::BeginInteraction(detail::MountedNode& node, InteractionEvent::Source source,
                                        std::optional<Point> position) {
  const std::uint64_t press_id = state_->next_press_id_++;
  ++node.active_press_count;
  InteractionState interaction = node.interaction;
  interaction.pressed = true;
  UpdateInteraction(node, interaction, InteractionEvent{InteractionEvent::Type::Press, source, press_id, position});
  return press_id;
}

void Runtime::EndInteraction(detail::MountedNode& node, InteractionEvent::Type type,
                             InteractionEvent::Source source, std::uint64_t press_id,
                             std::optional<Point> position) {
  if (node.active_press_count > 0) {
    --node.active_press_count;
  }
  InteractionState interaction = node.interaction;
  interaction.pressed = node.active_press_count != 0;
  UpdateInteraction(node, interaction, InteractionEvent{type, source, press_id, position});
}

void Runtime::BeginPointerInteraction(PointerSession& session, std::uint64_t node_identity,
                                      const PointerEvent& event) {
  detail::MountedNode* node = FindNode(*state_->mounted_root_, node_identity);
  if (node == nullptr || !node->interaction.enabled) {
    return;
  }
  const PointerEvent local = LocalPointerEvent(*node, event);
  session.interaction = detail::ActivePointerInteraction{
      node_identity,
      BeginInteraction(*node, InteractionEvent::Source::Pointer, local.position),
  };
}

void Runtime::EndPointerInteraction(PointerSession& session, InteractionEvent::Type type,
                                    const PointerEvent& event) {
  if (!session.interaction.has_value()) {
    return;
  }
  const detail::ActivePointerInteraction interaction = *session.interaction;
  session.interaction.reset();
  if (detail::MountedNode* node = FindNode(*state_->mounted_root_, interaction.node_identity)) {
    const PointerEvent local = LocalPointerEvent(*node, event);
    EndInteraction(*node, type, InteractionEvent::Source::Pointer, interaction.press_id, local.position);
  }
}

void Runtime::CancelPointerSession(PointerSession& session, const PointerEvent& event) {
  const std::optional<std::uint64_t> raw_target =
      session.raw_target_started ? session.raw_target_identity : std::nullopt;
  const bool overlay_owned = TextSelectionOverlayOwnsPointer(session);
  session.owner.reset();
  session.raw_target_started = false;
  session.raw_target_identity.reset();
  session.focus_pending = false;
  session.pending_focus_identity.reset();

  const PointerEvent cancellation = CancellationEvent(event);
  EndPointerInteraction(session, InteractionEvent::Type::Cancel, cancellation);
  for (PointerRecognition& recognition : session.recognitions) {
    CancelPointerRecognition(recognition, cancellation);
  }
  if (session.drag_drop.has_value()) {
    const auto source = std::ranges::find_if(session.recognitions, [&](const PointerRecognition& recognition) {
      const auto* state = std::get_if<DragSourceRecognitionState>(&recognition.state);
      return state && state->extension == session.drag_drop->source;
    });
    if (source != session.recognitions.end()) {
      const auto& source_state = std::get<DragSourceRecognitionState>(source->state);
      if (source_state.recognizer) {
        CancelDragDrop(session, source_state.recognizer->CurrentEvent());
      }
    }
  }
  if (overlay_owned) {
    HandleTextSelectionOverlayPointer(cancellation);
  }
  if (raw_target.has_value()) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *raw_target)) {
      EmitEvent<ViewEvents::Pointer>(target->event_bindings, cancellation);
    }
  }
}

bool Runtime::BeginPointerChord(PointerSession& session, const PointerEvent& event) {
  session.chorded = true;
  session.focus_pending = false;
  session.pending_focus_identity.reset();
  if (session.quarantined) {
    return false;
  }

  const std::optional<std::size_t> owner = RecognitionOwnerIndex(session);
  if (owner.has_value() && *owner < session.recognitions.size() &&
      std::holds_alternative<PointerInterceptRecognitionState>(session.recognitions[*owner].state)) {
    return true;
  }
  const PointerEvent cancellation = CancellationEvent(event);
  if (owner.has_value()) {
    CancelPointerSession(session, cancellation);
    session.quarantined = true;
    return false;
  }

  EndPointerInteraction(session, InteractionEvent::Type::Cancel, cancellation);
  for (PointerRecognition& recognition : session.recognitions) {
    if (!std::holds_alternative<PointerInterceptRecognitionState>(recognition.state)) {
      CancelPointerRecognition(recognition, cancellation);
    }
  }
  return true;
}

void Runtime::DispatchChordPointerEvent(PointerSession& session, const PointerEvent& event) {
  const std::optional<std::size_t> owner = RecognitionOwnerIndex(session);
  if (owner.has_value()) {
    if (*owner < session.recognitions.size() &&
        std::holds_alternative<PointerInterceptRecognitionState>(session.recognitions[*owner].state)) {
      static_cast<void>(UpdatePointerRecognition(session, *owner, event));
    }
    return;
  }

  for (std::size_t index = 0; index < session.recognitions.size(); ++index) {
    if (!session.recognitions[index].started ||
        !std::holds_alternative<PointerInterceptRecognitionState>(session.recognitions[index].state)) {
      continue;
    }
    if (UpdatePointerRecognition(session, index, event) == GestureDecision::Accept) {
      ResolvePointerRecognition(session, index, event);
      break;
    }
  }
  if (session.owner.has_value() || !session.raw_target_started || !session.raw_target_identity.has_value()) {
    return;
  }

  detail::MountedNode* target = FindNode(*state_->mounted_root_, *session.raw_target_identity);
  if (target == nullptr) {
    if (event.type != PointerEventType::Up) {
      CancelPointerTarget(session, event);
    }
    return;
  }
  if (event.type != PointerEventType::Cancel &&
      (event.type != PointerEventType::Up || target->interaction.enabled)) {
    EmitEvent<ViewEvents::Pointer>(target->event_bindings, event);
  }
}

void Runtime::QuarantinePointerSession(std::int64_t pointer_id, const PointerEvent& event) {
  const auto found = state_->pointer_sessions_.find(pointer_id);
  if (found == state_->pointer_sessions_.end() || found->second.quarantined) {
    return;
  }
  found->second.quarantined = true;
  CancelPointerSession(found->second, event);
}

void Runtime::CancelPointerTarget(PointerSession& session, const PointerEvent& event) {
  const std::optional<std::uint64_t> raw_target =
      session.raw_target_started ? session.raw_target_identity : std::nullopt;
  session.raw_target_started = false;
  session.raw_target_identity.reset();
  session.focus_pending = false;
  session.pending_focus_identity.reset();

  const PointerEvent cancellation = CancellationEvent(event);
  EndPointerInteraction(session, InteractionEvent::Type::Cancel, cancellation);
  if (raw_target.has_value()) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *raw_target)) {
      EmitEvent<ViewEvents::Pointer>(target->event_bindings, cancellation);
    }
  }
  RequestFrame();
}

std::optional<ActiveDropTarget> Runtime::ResolveDropTarget(const DragDropSession& session,
                                                          Point window_position) const {
  std::vector<detail::MountedNode*> route;
  const bool has_route =
      state_->mounted_root_ && BuildPointerRoute(*state_->mounted_root_, window_position, route);
  if (!has_route) {
    return std::nullopt;
  }
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!(*node)->interaction.enabled) {
      continue;
    }
    for (std::size_t index = (*node)->extensions.size(); index > 0; --index) {
      NodeExtensionEntry& entry = (*node)->extensions[index - 1];
      if (!entry.extension) {
        continue;
      }
      const DropTargetCapability* capability = entry.extension->GetDropTargetCapability();
      if (!capability || capability->payload_type != session.payload_type || !capability->accepts ||
          !capability->accepts(session.payload.get())) {
        continue;
      }
      return ActiveDropTarget{
          NodeExtensionHandle{(*node)->identity, index - 1, entry.descriptor},
          capability->dispatch,
      };
    }
  }
  return std::nullopt;
}

void Runtime::BeginDragDrop(PointerSession& session, DragSourceRecognitionState& recognition) {
  const DragEvent drag = recognition.recognizer->CurrentEvent();
  session.drag_drop = DragDropSession{
      .source = recognition.extension,
      .payload_type = recognition.source.payload_type,
      .payload = recognition.source.payload,
      .drag = drag,
  };
  DragDropSession& drag_drop = *session.drag_drop;
  if (recognition.source.preview) {
    const std::function<View()> preview = recognition.source.preview;
    detail::MountedNode* source = FindNode(*state_->mounted_root_, recognition.extension.node_identity);
    const Point transformed_origin = recognition.frozen_node_to_window.Apply({});
    const Point transformed_grab = recognition.frozen_node_to_window.Apply(drag.origin);
    drag_drop.preview_grab_offset = transformed_grab - transformed_origin;
    drag_drop.preview_layer = state_->layer_controller_.AttachCaptured(
        LayerOptions{.level = LayerLevel::Notification, .pointer_policy = LayerPointerPolicy::PassThrough},
        [preview] {
          return preview().With(Semantics{
              .descendants = SemanticDescendantPolicy::Exclude,
              .hidden = true,
          });
        },
        recognition.environment ? recognition.environment
                                : (source ? source->environment : state_->root_environment_),
        DragPreviewPlacement(drag.window_position, drag_drop.preview_grab_offset)
    );
  }
  drag_drop.target = ResolveDropTarget(drag_drop, drag.window_position);

  if (detail::MountedNode* source = FindNode(*state_->mounted_root_, recognition.extension.node_identity)) {
    EmitEvent<DragSourceEvents::Started>(source->event_bindings, drag);
  }
  if (!session.drag_drop.has_value() || !session.drag_drop->target.has_value()) {
    return;
  }
  session.drag_drop->target_entered = true;
  const ActiveDropTarget active = *session.drag_drop->target;
  const NodeExtensionHandle target_handle = active.extension;
  NodeExtension* extension = FindExtension(*state_->mounted_root_, target_handle);
  detail::MountedNode* target = FindNode(*state_->mounted_root_, target_handle.node_identity);
  if (target && extension && active.dispatch.entered) {
    active.dispatch.entered(
        target->event_bindings, session.drag_drop->payload.get(), LocalDropEvent(*target, drag)
    );
  }
  RequestFrame();
}

void Runtime::UpdateDragDrop(PointerSession& session, const DragEvent& drag) {
  if (!session.drag_drop.has_value()) {
    return;
  }
  DragDropSession& drag_drop = *session.drag_drop;
  drag_drop.drag = drag;
  if (drag_drop.preview_layer.has_value()) {
    state_->layer_controller_.UpdatePlacement(
        *drag_drop.preview_layer, DragPreviewPlacement(drag.window_position, drag_drop.preview_grab_offset)
    );
  }

  if (detail::MountedNode* source = FindNode(*state_->mounted_root_, drag_drop.source.node_identity)) {
    EmitEvent<DragSourceEvents::Changed>(source->event_bindings, drag);
  }
  if (!session.drag_drop.has_value()) {
    return;
  }

  UpdateDropTarget(session, drag, true);
  if (session.drag_drop.has_value() && session.drag_drop->target.has_value()) {
    RequestFrame();
  }
}

void Runtime::UpdateDropTarget(PointerSession& session, const DragEvent& drag, bool emit_moved) {
  if (!session.drag_drop.has_value()) {
    return;
  }
  const std::optional<ActiveDropTarget> previous =
      session.drag_drop->target_entered ? session.drag_drop->target : std::nullopt;
  const std::optional<ActiveDropTarget> next = ResolveDropTarget(*session.drag_drop, drag.window_position);
  const std::shared_ptr<const void> payload = session.drag_drop->payload;
  session.drag_drop->target = next;
  session.drag_drop->target_entered = false;
  const auto dispatch = [&](const ActiveDropTarget& active, DropTargetDispatch::Function function) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, active.extension);
    detail::MountedNode* target = FindNode(*state_->mounted_root_, active.extension.node_identity);
    if (target && extension && function) {
      function(target->event_bindings, payload.get(), LocalDropEvent(*target, drag));
    }
  };
  if (previous == next) {
    if (next.has_value()) {
      session.drag_drop->target_entered = true;
    }
    if (emit_moved && next.has_value()) {
      dispatch(*next, next->dispatch.moved);
    }
    return;
  }
  if (previous.has_value()) {
    dispatch(*previous, previous->dispatch.exited);
  }
  if (next.has_value() && session.drag_drop.has_value()) {
    session.drag_drop->target_entered = true;
    dispatch(*next, next->dispatch.entered);
  }
}

void Runtime::AdvanceDragDrop(const FrameInfo& frame) {
  std::vector<std::int64_t> pointer_ids;
  pointer_ids.reserve(state_->pointer_sessions_.size());
  for (const auto& [pointer_id, session] : state_->pointer_sessions_) {
    if (!session.quarantined && session.drag_drop.has_value()) {
      pointer_ids.push_back(pointer_id);
    }
  }
  for (const std::int64_t pointer_id : pointer_ids) {
    try {
      AdvanceDragDropSession(pointer_id, frame);
    } catch (...) {
      const auto active = state_->pointer_sessions_.find(pointer_id);
      if (active != state_->pointer_sessions_.end() && !active->second.quarantined) {
        active->second.quarantined = true;
        try {
          CancelPointerSession(
              active->second,
              PointerEvent{
                  PointerEventType::Cancel,
                  pointer_id,
                  active->second.last_position,
                  active->second.device_kind,
              }
          );
        } catch (...) {
        }
      }
      throw;
    }
  }
}

void Runtime::AdvanceDragDropSession(std::int64_t pointer_id, const FrameInfo& frame) {
  constexpr float edge_extent = 32.0F;
  constexpr float maximum_speed = 640.0F;
  auto found = state_->pointer_sessions_.find(pointer_id);
  if (found == state_->pointer_sessions_.end()) {
    return;
  }
  PointerSession& session = found->second;
  if (session.quarantined || !session.drag_drop.has_value()) {
    return;
  }
  const DragEvent drag = session.drag_drop->drag;
  UpdateDropTarget(session, drag, false);
  found = state_->pointer_sessions_.find(pointer_id);
  if (found == state_->pointer_sessions_.end() || found->second.quarantined ||
      !found->second.drag_drop.has_value() || !found->second.drag_drop->target.has_value()) {
    return;
  }
  DragDropSession& drag_drop = *found->second.drag_drop;

  std::vector<detail::MountedNode*> route;
  const bool has_route = BuildPointerRoute(*state_->mounted_root_, drag_drop.drag.window_position, route);
  if (!has_route) {
    return;
  }
  const auto target = std::ranges::find(route, drag_drop.target->extension.node_identity,
                                        &detail::MountedNode::identity);
  if (target == route.end()) {
    return;
  }
  for (auto candidate = std::make_reverse_iterator(target + 1); candidate != route.rend(); ++candidate) {
    detail::MountedNode* node = *candidate;
    if (!node->interaction.enabled || !IsScrollContainer(*node)) {
      continue;
    }
    const std::optional<Point> local =
        node->presentation.resolved_transform.Inverse(drag_drop.drag.window_position);
    if (!local.has_value()) {
      continue;
    }
    const Axis axis = ScrollAxis(*node);
    const Rect viewport = ScrollViewport(*node);
    const float position = axis == Axis::Vertical ? local->y : local->x;
    const float start = axis == Axis::Vertical ? viewport.y : viewport.x;
    const float extent = axis == Axis::Vertical ? viewport.height : viewport.width;
    const float end = start + extent;
    const float edge = std::min(edge_extent, extent * 0.5F);
    float intensity = 0.0F;
    if (position < start + edge) {
      intensity = -std::clamp((start + edge - position) / edge, 0.0F, 1.0F);
    } else if (position > end - edge) {
      intensity = std::clamp((position - (end - edge)) / edge, 0.0F, 1.0F);
    }
    if (intensity == 0.0F || !AllowsScrollSource(*node, ScrollSource::DragDrop) ||
        !CanScrollNode(*node, intensity)) {
      continue;
    }
    if (frame.delta_time <= 0.0) {
      RequestFrame();
      break;
    }
    const float delta = intensity * maximum_speed * static_cast<float>(frame.delta_time);
    const float consumed = ScrollNodeBy(*node, delta, ScrollSource::DragDrop);
    if (consumed != 0.0F) {
      RequestFrame();
    }
    if (std::abs(consumed - delta) < 0.001F) {
      break;
    }
  }
}

void Runtime::FinishDragDrop(PointerSession& session, const DragEvent& drag) {
  if (!session.drag_drop.has_value()) {
    return;
  }
  const std::optional<ActiveDropTarget> previous =
      session.drag_drop->target_entered ? session.drag_drop->target : std::nullopt;
  const std::optional<ActiveDropTarget> target = ResolveDropTarget(*session.drag_drop, drag.window_position);
  DragDropSession completed = std::move(*session.drag_drop);
  completed.target = target;
  session.drag_drop.reset();
  if (completed.preview_layer.has_value()) {
    state_->layer_controller_.Dismiss(*completed.preview_layer);
  }

  const auto dispatch = [&](const ActiveDropTarget& active, DropTargetDispatch::Function function) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, active.extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, active.extension.node_identity);
    if (node && extension && function) {
      function(node->event_bindings, completed.payload.get(), LocalDropEvent(*node, drag));
    }
  };
  if (previous != target && previous.has_value()) {
    dispatch(*previous, previous->dispatch.exited);
  }
  if (previous != target && target.has_value()) {
    dispatch(*target, target->dispatch.entered);
  }

  bool dropped = false;
  if (completed.target.has_value()) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, completed.target->extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, completed.target->extension.node_identity);
    if (node && extension && completed.target->dispatch.dropped) {
      dropped = true;
      completed.target->dispatch.dropped(
          node->event_bindings, completed.payload.get(), LocalDropEvent(*node, drag)
      );
    }
  }
  if (detail::MountedNode* source = FindNode(*state_->mounted_root_, completed.source.node_identity)) {
    EmitEvent<DragSourceEvents::Ended>(source->event_bindings, DragDropResult{drag, dropped});
  }
}

void Runtime::CancelDragDrop(PointerSession& session, const DragEvent& drag) {
  if (!session.drag_drop.has_value()) {
    return;
  }
  DragDropSession canceled = std::move(*session.drag_drop);
  session.drag_drop.reset();
  if (canceled.preview_layer.has_value()) {
    state_->layer_controller_.Dismiss(*canceled.preview_layer);
  }
  if (canceled.target.has_value() && canceled.target_entered) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, canceled.target->extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, canceled.target->extension.node_identity);
    if (node && extension && canceled.target->dispatch.exited) {
      canceled.target->dispatch.exited(
          node->event_bindings, canceled.payload.get(), LocalDropEvent(*node, drag)
      );
    }
  }
  if (detail::MountedNode* source = FindNode(*state_->mounted_root_, canceled.source.node_identity)) {
    EmitEvent<DragSourceEvents::Canceled>(source->event_bindings, drag);
  }
}

void Runtime::CancelPointerRecognition(PointerRecognition& recognition, const PointerEvent& event) {
  if (!recognition.started) {
    return;
  }
  recognition.started = false;
  const PointerEvent cancellation = CancellationEvent(event);
  const double timestamp = state_->platform_->Now();
  const auto cancel_gesture = [&](GestureRecognitionState& retained) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, retained.extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, retained.extension.node_identity);
    if (extension && node && retained.recognizer) {
      retained.recognizer->Canceled(*node, *extension, GestureInput(retained, cancellation, timestamp));
    }
  };
  if (auto* extension_state = std::get_if<ExtensionRecognitionState>(&recognition.state)) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_state->extension);
    detail::MountedNode* node =
        FindNode(*state_->mounted_root_, extension_state->extension.node_identity);
    if (extension && node) {
      extension->OnPointer(*node, LocalPointerEvent(*node, cancellation));
      RequestFrame();
    }
  } else if (auto* scroll_state = std::get_if<ScrollRecognitionState>(&recognition.state)) {
    for (std::uint64_t identity : std::exchange(scroll_state->active_nodes, {})) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, identity)) {
        NotifyScrollNodeActivity(*node, ScrollSource::Drag, ScrollPhase::Cancel, 0.0F);
        if (node->scroll_state->motion.StartOverscrollSettlement(*node)) {
          state_->scroll_motion_active_ = true;
          RequestFrame();
        }
      }
    }
  } else if (auto* tap_state = std::get_if<TapRecognitionState>(&recognition.state)) {
    for (GestureRecognitionState& consumer : tap_state->consumers) {
      cancel_gesture(consumer);
    }
  } else if (auto* gesture_state = std::get_if<GestureRecognitionState>(&recognition.state)) {
    cancel_gesture(*gesture_state);
  } else if (auto* source_state = std::get_if<DragSourceRecognitionState>(&recognition.state)) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, source_state->extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, source_state->extension.node_identity);
    if (extension && node && source_state->recognizer) {
      source_state->recognizer->Canceled(*node, *extension, GestureInput(*source_state, cancellation, timestamp));
    }
  } else if (auto* intercept_state = std::get_if<PointerInterceptRecognitionState>(&recognition.state)) {
    if (detail::MountedNode* node = FindNode(*state_->mounted_root_, intercept_state->node_identity);
        node && node->interaction.enabled && HasEventBinding<ViewEvents::PointerIntercept>(node->event_bindings)) {
      static_cast<void>(EmitEvent<ViewEvents::PointerIntercept>(node->event_bindings, cancellation));
    }
  } else if (std::holds_alternative<TextSelectionRecognitionState>(recognition.state)) {
    TrackTextSelectionGesture(cancellation);
  }
}

bool Runtime::ResolveSharedGestureRecognition(const std::shared_ptr<GestureRecognizer>& recognizer,
                                              std::size_t index, const PointerEvent& event,
                                              std::optional<double> timestamp) {
  struct SharedRecognition {
    std::int64_t pointer_id = 0;
    std::size_t index = 0;
    bool newly_owned = false;
  };
  std::vector<SharedRecognition> shared;
  for (const auto& [pointer_id, pointer_session] : state_->pointer_sessions_) {
    if (const std::optional<std::size_t> owner = RecognitionOwnerIndex(pointer_session)) {
      if (*owner >= pointer_session.recognitions.size()) {
        continue;
      }
      const auto* gesture =
          std::get_if<GestureRecognitionState>(&pointer_session.recognitions[*owner].state);
      if (gesture && gesture->recognizer == recognizer) {
        shared.push_back({pointer_id, *owner, false});
      }
      continue;
    }
    for (std::size_t recognition_index = 0; recognition_index < pointer_session.recognitions.size();
         ++recognition_index) {
      const PointerRecognition& recognition = pointer_session.recognitions[recognition_index];
      const auto* gesture = std::get_if<GestureRecognitionState>(&recognition.state);
      if (recognition.started && gesture && gesture->recognizer == recognizer) {
        shared.push_back({pointer_id, recognition_index, true});
        break;
      }
    }
  }

  if (shared.size() <= 1) {
    return false;
  }

  std::ranges::sort(shared, {}, &SharedRecognition::pointer_id);
  // Commit every PointerSession owner before cancellation callbacks can observe or invalidate the mounted tree.
  for (const SharedRecognition& participant : shared) {
    const auto found = state_->pointer_sessions_.find(participant.pointer_id);
    if (found != state_->pointer_sessions_.end() && participant.newly_owned &&
        participant.index < found->second.recognitions.size()) {
      found->second.owner = participant.index;
      found->second.recognitions[participant.index].started = true;
    }
  }

  try {
    for (const SharedRecognition& participant : shared) {
      if (!participant.newly_owned) {
        continue;
      }
      const auto found = state_->pointer_sessions_.find(participant.pointer_id);
      if (found == state_->pointer_sessions_.end() || participant.index >= found->second.recognitions.size()) {
        continue;
      }
      const PointerEvent cancellation{
          PointerEventType::Cancel,
          participant.pointer_id,
          found->second.last_position,
          found->second.device_kind,
      };
      CancelPointerTarget(found->second, cancellation);
      for (std::size_t other_index = 0; other_index < found->second.recognitions.size(); ++other_index) {
        if (other_index != participant.index) {
          CancelPointerRecognition(found->second.recognitions[other_index], cancellation);
        }
      }
    }

    const auto current = state_->pointer_sessions_.find(event.pointer_id);
    if (current == state_->pointer_sessions_.end() || index >= current->second.recognitions.size() ||
        RecognitionOwnerIndex(current->second) != std::optional{index}) {
      return true;
    }
    auto* gesture = std::get_if<GestureRecognitionState>(&current->second.recognitions[index].state);
    if (!gesture || gesture->recognizer != recognizer) {
      return true;
    }
    NodeExtension* extension = FindExtension(*state_->mounted_root_, gesture->extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, gesture->extension.node_identity);
    if (extension && node) {
      gesture->recognizer->Accepted(*node, *extension,
                                    GestureInput(*gesture, event, timestamp.value_or(state_->platform_->Now())));
    }
  } catch (...) {
    for (const SharedRecognition& participant : shared) {
      const auto found = state_->pointer_sessions_.find(participant.pointer_id);
      if (found == state_->pointer_sessions_.end() || found->second.quarantined) {
        continue;
      }
      found->second.quarantined = true;
      try {
        const PointerEvent cancellation{
            PointerEventType::Cancel,
            participant.pointer_id,
            found->second.last_position,
            found->second.device_kind,
        };
        CancelPointerSession(found->second, cancellation);
      } catch (...) {
      }
    }
    throw;
  }
  return true;
}

void Runtime::ResolvePointerRecognition(PointerSession& session, std::size_t index, const PointerEvent& event,
                                        std::optional<double> timestamp) {
  if (session.owner.has_value() || index >= session.recognitions.size()) {
    return;
  }

  if (const auto* selected = std::get_if<GestureRecognitionState>(&session.recognitions[index].state);
      selected && selected->recognizer &&
      ResolveSharedGestureRecognition(selected->recognizer, index, event, timestamp)) {
    return;
  }

  session.owner = index;
  session.recognitions[index].started = true;
  const bool tap = std::holds_alternative<TapRecognitionState>(session.recognitions[index].state) ||
                   std::holds_alternative<ContextMenuRecognitionState>(session.recognitions[index].state);
  const bool immediate_extension = event.type == PointerEventType::Down &&
                                   std::holds_alternative<ExtensionRecognitionState>(session.recognitions[index].state);
  if (!tap) {
    if (immediate_extension) {
      EndPointerInteraction(session, InteractionEvent::Type::Cancel, event);
      session.raw_target_identity.reset();
      session.raw_target_started = false;
    } else {
      CancelPointerTarget(session, event);
    }
  }
  const PointerEvent cancellation = CancellationEvent(event);
  for (std::size_t other_index = 0; other_index < session.recognitions.size(); ++other_index) {
    if (other_index != index) {
      CancelPointerRecognition(session.recognitions[other_index], cancellation);
    }
  }
  if (const auto* extension = std::get_if<ExtensionRecognitionState>(&session.recognitions[index].state);
      extension && event.type == PointerEventType::Down && !session.interaction.has_value()) {
    BeginPointerInteraction(session, extension->extension.node_identity, event);
  }
  if (auto* gesture = std::get_if<GestureRecognitionState>(&session.recognitions[index].state)) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, gesture->extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, gesture->extension.node_identity);
    if (extension && node && gesture->recognizer) {
      gesture->recognizer->Accepted(*node, *extension,
                                    GestureInput(*gesture, event, timestamp.value_or(state_->platform_->Now())));
    }
  } else if (auto* source = std::get_if<DragSourceRecognitionState>(&session.recognitions[index].state)) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, source->extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, source->extension.node_identity);
    if (extension && node && source->recognizer) {
      source->recognizer->Accepted(*node, *extension,
                                   GestureInput(*source, event, timestamp.value_or(state_->platform_->Now())));
      BeginDragDrop(session, *source);
    }
  }
}

GestureDecision Runtime::UpdatePointerRecognition(PointerSession& session, std::size_t index,
                                                  const PointerEvent& event) {
  if (index >= session.recognitions.size()) {
    return GestureDecision::Reject;
  }
  PointerRecognition& recognition = session.recognitions[index];
  if (auto* intercept_state = std::get_if<PointerInterceptRecognitionState>(&recognition.state)) {
    detail::MountedNode* node = FindNode(*state_->mounted_root_, intercept_state->node_identity);
    if (!node || !node->interaction.enabled ||
        !HasEventBinding<ViewEvents::PointerIntercept>(node->event_bindings)) {
      return GestureDecision::Reject;
    }
    recognition.started = true;
    const std::optional<bool> intercept = EmitEvent<ViewEvents::PointerIntercept>(node->event_bindings, event);
    if (RecognitionOwnerIndex(session) == std::optional{index}) {
      return event.type == PointerEventType::Up || event.type == PointerEventType::Cancel
                 ? GestureDecision::Reject
                 : GestureDecision::Continue;
    }
    if (event.type == PointerEventType::Cancel) {
      return GestureDecision::Reject;
    }
    if (intercept.value_or(false)) {
      return GestureDecision::Accept;
    }
    return event.type == PointerEventType::Up ? GestureDecision::Reject : GestureDecision::Continue;
  }
  if (auto* context_menu = std::get_if<ContextMenuRecognitionState>(&recognition.state)) {
    recognition.started = true;
    if (event.type != PointerEventType::Up) {
      return event.type == PointerEventType::Cancel ? GestureDecision::Reject : GestureDecision::Continue;
    }
    std::vector<detail::MountedNode*> route;
    if (!BuildPointerRoute(*state_->mounted_root_, event.position, route)) {
      return GestureDecision::Reject;
    }
    const auto target = std::ranges::find(route, context_menu->node_identity, &detail::MountedNode::identity);
    return target != route.end() && (*target)->interaction.enabled &&
                   HasEventBinding<ViewEvents::ContextMenuRequested>((*target)->event_bindings)
               ? GestureDecision::Accept
               : GestureDecision::Reject;
  }
  if (auto* extension_state = std::get_if<ExtensionRecognitionState>(&recognition.state)) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, extension_state->extension);
    detail::MountedNode* node =
        FindNode(*state_->mounted_root_, extension_state->extension.node_identity);
    if (!extension || !node || !node->interaction.enabled) {
      return GestureDecision::Reject;
    }
    recognition.started = true;
    const NodeExtension::PointerResult result = extension->OnPointer(*node, LocalPointerEvent(*node, event));
    if (result != NodeExtension::PointerResult::Ignored) {
      RequestFrame();
    }
    if (event.type == PointerEventType::Down) {
      if (result == NodeExtension::PointerResult::Capture || result == NodeExtension::PointerResult::Handled ||
          result == NodeExtension::PointerResult::CancelTarget) {
        return GestureDecision::Accept;
      }
      return result == NodeExtension::PointerResult::Observe ? GestureDecision::Continue : GestureDecision::Reject;
    }
    if (result == NodeExtension::PointerResult::CancelTarget) {
      return GestureDecision::Accept;
    }
    return event.type == PointerEventType::Cancel || event.type == PointerEventType::Up
               ? GestureDecision::Reject
               : GestureDecision::Continue;
  }
  if (auto* gesture_state = std::get_if<GestureRecognitionState>(&recognition.state)) {
    GestureRecognitionState& retained = *gesture_state;
    NodeExtension* extension = FindExtension(*state_->mounted_root_, retained.extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, retained.extension.node_identity);
    if (!extension || !node || !node->interaction.enabled || !retained.recognizer) {
      return GestureDecision::Reject;
    }
    recognition.started = true;
    const GestureRecognizerInput input = GestureInput(retained, event, state_->platform_->Now());
    if (RecognitionOwnerIndex(session) == std::optional{index}) {
      retained.recognizer->UpdateAccepted(*node, *extension, input);
      return event.type == PointerEventType::Up || event.type == PointerEventType::Cancel
                 ? GestureDecision::Reject
                 : GestureDecision::Continue;
    }
    return retained.recognizer->Update(input);
  }
  if (auto* source_state = std::get_if<DragSourceRecognitionState>(&recognition.state)) {
    DragSourceRecognitionState& retained = *source_state;
    NodeExtension* extension = FindExtension(*state_->mounted_root_, retained.extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, retained.extension.node_identity);
    if (!extension || !node || !node->interaction.enabled || !retained.recognizer) {
      return GestureDecision::Reject;
    }
    recognition.started = true;
    const GestureRecognizerInput input = GestureInput(retained, event, state_->platform_->Now());
    if (RecognitionOwnerIndex(session) == std::optional{index}) {
      retained.recognizer->UpdateAccepted(*node, *extension, input);
      if (event.type == PointerEventType::Move) {
        UpdateDragDrop(session, retained.recognizer->CurrentEvent());
      } else if (event.type == PointerEventType::Up) {
        FinishDragDrop(session, retained.recognizer->CurrentEvent());
      }
      return event.type == PointerEventType::Up || event.type == PointerEventType::Cancel
                 ? GestureDecision::Reject
                 : GestureDecision::Continue;
    }
    return retained.recognizer->Update(input);
  }
  if (auto* scroll_state = std::get_if<ScrollRecognitionState>(&recognition.state)) {
    recognition.started = true;
    if (event.type != PointerEventType::Move) {
      return event.type == PointerEventType::Up || event.type == PointerEventType::Cancel
                 ? GestureDecision::Reject
                 : GestureDecision::Continue;
    }
    const float distance_x = event.position.x - session.down_position.x;
    const float distance_y = event.position.y - session.down_position.y;
    if (std::max(std::abs(distance_x), std::abs(distance_y)) < detail::touch_gesture_slop) {
      return GestureDecision::Continue;
    }
    const Axis dominant_axis = std::abs(distance_y) >= std::abs(distance_x) ? Axis::Vertical : Axis::Horizontal;
    if (dominant_axis != scroll_state->axis) {
      return GestureDecision::Continue;
    }
    detail::MountedNode* node = FindNode(*state_->mounted_root_, scroll_state->node_identity);
    const float delta = PointerDelta(session.down_position, event.position, scroll_state->axis);
    const bool can_overscroll =
        node && session.device_kind == PointerDeviceKind::Touch && CanOverscrollNode(*node, delta);
    return node && node->interaction.enabled && (CanScrollNode(*node, delta) || can_overscroll)
               ? GestureDecision::Accept
               : GestureDecision::Continue;
  }
  if (auto* tap_state = std::get_if<TapRecognitionState>(&recognition.state)) {
    recognition.started = true;
    if (event.type != PointerEventType::Up) {
      return event.type == PointerEventType::Cancel ? GestureDecision::Reject : GestureDecision::Continue;
    }

    detail::MountedNode* released = HitTestPointer(*state_->mounted_root_, event.position);
    tap_state->activates = tap_state->activates && released && released->interaction.enabled &&
                           released->identity == tap_state->node_identity;

    std::vector<detail::MountedNode*> route;
    const bool has_route = BuildPointerRoute(*state_->mounted_root_, event.position, route);
    const auto owner = has_route
                           ? std::ranges::find_if(route, [&](const detail::MountedNode* node) {
                               return node->identity == tap_state->node_identity;
                             })
                           : route.end();
    std::erase_if(tap_state->consumers, [&](const GestureRecognitionState& consumer) {
      NodeExtension* extension = FindExtension(*state_->mounted_root_, consumer.extension);
      detail::MountedNode* node = FindNode(*state_->mounted_root_, consumer.extension.node_identity);
      const bool remains_on_route = owner != route.end() && *owner == node;
      const auto local_position = remains_on_route
                                      ? node->presentation.resolved_transform.Inverse(event.position)
                                      : std::nullopt;
      const bool valid = remains_on_route && node->interaction.enabled && extension &&
                         local_position.has_value() && extension->HitTest(**owner, *local_position);
      if (!valid && extension && node && consumer.recognizer) {
        const PointerEvent cancellation = CancellationEvent(event);
        consumer.recognizer->Canceled(*node, *extension,
                                      GestureInput(consumer, cancellation, state_->platform_->Now()));
      }
      return !valid;
    });
    return tap_state->activates || !tap_state->consumers.empty() ? GestureDecision::Accept
                                                                 : GestureDecision::Reject;
  }
  recognition.started = true;
  return event.type == PointerEventType::Cancel ? GestureDecision::Reject : GestureDecision::Continue;
}

void Runtime::PublishTap(TapRecognitionState& tap, const PointerEvent& event) {
  const double timestamp = state_->platform_->Now();
  for (GestureRecognitionState& consumer : tap.consumers) {
    NodeExtension* extension = FindExtension(*state_->mounted_root_, consumer.extension);
    detail::MountedNode* node = FindNode(*state_->mounted_root_, consumer.extension.node_identity);
    if (extension && node && consumer.recognizer) {
      consumer.recognizer->TapAccepted(*node, *extension, GestureInput(consumer, event, timestamp));
    }
  }
  if (tap.activates) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, tap.node_identity);
        target && target->interaction.enabled) {
      ActivateNode(*target);
    }
  }
}

void Runtime::PublishContextMenu(ContextMenuRecognitionState& context_menu, const PointerEvent& event) {
  if (detail::MountedNode* target = FindNode(*state_->mounted_root_, context_menu.node_identity);
      target && target->interaction.enabled) {
    static_cast<void>(EmitEvent<ViewEvents::ContextMenuRequested>(target->event_bindings, event.position));
  }
}

void Runtime::AdvancePointerRecognition(double timestamp) {
  struct RecognitionDeadline {
    double deadline = 0.0;
    std::int64_t pointer_id = 0;
    std::size_t recognition_index = 0;
  };

  while (true) {
    std::optional<RecognitionDeadline> due;
    std::optional<double> next_deadline;
    for (const auto& [pointer_id, session] : state_->pointer_sessions_) {
      if (session.quarantined || session.owner.has_value()) {
        continue;
      }
      for (std::size_t index = 0; index < session.recognitions.size(); ++index) {
        const PointerRecognition& recognition = session.recognitions[index];
        const GestureRecognizer* recognizer = nullptr;
        if (const auto* gesture = std::get_if<GestureRecognitionState>(&recognition.state)) {
          recognizer = gesture->recognizer.get();
        } else if (const auto* source = std::get_if<DragSourceRecognitionState>(&recognition.state)) {
          recognizer = source->recognizer.get();
        }
        if (!recognition.started || !recognizer) {
          continue;
        }
        const std::optional<double> deadline = recognizer->Deadline();
        if (!deadline.has_value()) {
          continue;
        }
        if (*deadline <= timestamp) {
          const RecognitionDeadline current{*deadline, pointer_id, index};
          if (!due.has_value() || current.deadline < due->deadline ||
              (current.deadline == due->deadline && current.pointer_id < due->pointer_id) ||
              (current.deadline == due->deadline && current.pointer_id == due->pointer_id &&
               current.recognition_index < due->recognition_index)) {
            due = current;
          }
        } else if (!next_deadline.has_value() || *deadline < *next_deadline) {
          next_deadline = *deadline;
        }
      }
    }

    if (!due.has_value()) {
      if (next_deadline.has_value()) {
        RequestFrameAfter(*next_deadline - state_->platform_->Now());
      }
      return;
    }

    const auto session = state_->pointer_sessions_.find(due->pointer_id);
    if (session == state_->pointer_sessions_.end() || session->second.quarantined ||
        session->second.owner.has_value() || due->recognition_index >= session->second.recognitions.size()) {
      continue;
    }
    PointerRecognition& recognition = session->second.recognitions[due->recognition_index];
    GestureRecognizer* recognizer = nullptr;
    if (auto* gesture = std::get_if<GestureRecognitionState>(&recognition.state)) {
      recognizer = gesture->recognizer.get();
    } else if (auto* source = std::get_if<DragSourceRecognitionState>(&recognition.state)) {
      recognizer = source->recognizer.get();
    }
    if (!recognition.started || !recognizer) {
      continue;
    }
    const GestureDecision decision = recognizer->AdvanceDeadline(timestamp);
    PointerEvent event{
        PointerEventType::Move,
        due->pointer_id,
        session->second.last_position,
        session->second.device_kind,
    };
    if (decision == GestureDecision::Accept) {
      ResolvePointerRecognition(session->second, due->recognition_index, event, timestamp);
    } else if (decision == GestureDecision::Reject) {
      CancelPointerRecognition(recognition, CancellationEvent(event));
    } else if (recognizer->Deadline().has_value() && *recognizer->Deadline() <= timestamp) {
      throw std::logic_error("HuxerUI gesture recognizer did not consume its elapsed deadline");
    }
  }
}

void Runtime::ApplyDragScroll(const PointerSession& session, ScrollRecognitionState& scroll, float delta) {
  if (delta == 0.0F) {
    return;
  }

  const auto origin = std::ranges::find(session.route, scroll.node_identity);
  if (origin == session.route.end()) {
    return;
  }
  std::vector<detail::MountedNode*> route;
  route.reserve(static_cast<std::size_t>(origin - session.route.begin()) + 1);
  for (auto identity = session.route.begin(); identity != origin + 1; ++identity) {
    detail::MountedNode* candidate = FindNode(*state_->mounted_root_, *identity);
    if (candidate) {
      route.push_back(candidate);
    }
  }
  static_cast<void>(ApplyScrollTransaction(route, scroll.axis, delta, ScrollSource::Drag, &scroll.active_nodes,
                                           session.device_kind == PointerDeviceKind::Touch));
}

bool Runtime::HasContextMenuHandler(Point position) const {
  if (!state_->mounted_root_) {
    return false;
  }
  std::vector<detail::MountedNode*> route;
  if (!BuildPointerRoute(*state_->mounted_root_, position, route)) {
    return false;
  }
  return std::ranges::any_of(route.rbegin(), route.rend(), [](const detail::MountedNode* node) {
    return node->interaction.enabled && HasEventBinding<ViewEvents::ContextMenuRequested>(node->event_bindings);
  });
}

void Runtime::UpdatePointerCursor(std::optional<Point> position) {
  PointerCursorKind kind = PointerCursorKind::Default;
  if (position.has_value() && state_->mounted_root_ && state_->window_->metrics.viewport.width > 0.0F &&
      state_->window_->metrics.viewport.height > 0.0F) {
    std::vector<detail::MountedNode*> route;
    if (BuildPointerCursorRoute(*state_->mounted_root_, *position, route) && !route.empty() &&
        route.back()->kind != detail::NodeKind::PlatformView) {
      for (auto node = route.rbegin(); node != route.rend(); ++node) {
        if ((*node)->properties.pointer_cursor.has_value()) {
          kind = *(*node)->properties.pointer_cursor;
          break;
        }
      }
    }
  }
  if (kind == state_->pointer_cursor_kind_) {
    return;
  }
  state_->pointer_cursor_kind_ = kind;
  state_->platform_->SetPointerCursor(kind);
}

void Runtime::HandlePointerEvent(const PointerEvent& input_event) {
  if (!state_->mounted_root_) {
    return;
  }

  const PointerEvent event = NormalizePointerEvent(input_event);
  detail::InteractionOriginScope interaction_origin(state_->current_interaction_origin_, event.position, true);
  try {
    bool hover_moved = false;
    if (SupportsHover(event.device_kind)) {
      if (event.type == PointerEventType::Cancel) {
        ClearHover();
        UpdatePointerCursor(std::nullopt);
      } else {
        hover_moved = TrackHoverPointer(event);
      }
    }
    const auto active = state_->pointer_sessions_.find(event.pointer_id);
    if (active != state_->pointer_sessions_.end() && TextSelectionOverlayOwnsPointer(active->second)) {
      if (SupportsHover(event.device_kind) && event.type != PointerEventType::Cancel) {
        ClearHover();
        UpdatePointerCursor(std::nullopt);
      }
      PointerSession& session = active->second;
      session.pressed_buttons = event.pressed_buttons;
      const bool chorded = event.type == PointerEventType::Down &&
                           (event.changed_button != session.initiating_button ||
                            HasMultipleButtons(event.pressed_buttons));
      if (chorded) {
        CancelPointerSession(session, event);
        session.quarantined = true;
        RefreshTextInputSession();
        return;
      }
      HandleTextSelectionOverlayPointer(event);
      if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
        state_->pointer_sessions_.erase(active);
      }
      RefreshTextInputSession();
      return;
    }
    const bool primary_down = event.type != PointerEventType::Down ||
                              (event.changed_button == PointerButton::Primary &&
                               !HasMultipleButtons(event.pressed_buttons));
    const bool selection_overlay_handled =
        active == state_->pointer_sessions_.end() && primary_down && HandleTextSelectionOverlayPointer(event);
    if (selection_overlay_handled) {
      if (SupportsHover(event.device_kind) && event.type != PointerEventType::Cancel) {
        ClearHover();
        UpdatePointerCursor(std::nullopt);
      }
      if (event.type == PointerEventType::Down) {
        PointerSession session;
        session.down_position = event.position;
        session.last_position = event.position;
        session.device_kind = event.device_kind;
        session.initiating_button = event.changed_button;
        session.pressed_buttons = event.pressed_buttons;
        session.owner = TextSelectionOverlayOwner{};
        state_->pointer_sessions_.insert_or_assign(event.pointer_id, std::move(session));
      }
      RefreshTextInputSession();
      return;
    }

    switch (event.type) {
    case PointerEventType::Down:
      HandlePointerDown(event);
      break;
    case PointerEventType::Move:
      HandlePointerMove(event, hover_moved);
      break;
    case PointerEventType::Cancel:
      HandlePointerCancel(event);
      break;
    case PointerEventType::Up:
      HandlePointerUp(event);
      break;
    }
    RefreshTextInputSession();
  } catch (...) {
    const PointerEvent cancellation = CancellationEvent(event);
    try {
      QuarantinePointerSession(event.pointer_id, cancellation);
    } catch (...) {
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      state_->pointer_sessions_.erase(event.pointer_id);
    }
    throw;
  }
}

bool Runtime::CommitPendingTouchFocus(PointerSession& session, Point position) {
  if (!session.focus_pending) {
    return false;
  }
  session.focus_pending = false;
  const std::optional<std::uint64_t> pending = std::exchange(session.pending_focus_identity, std::nullopt);

  std::vector<detail::MountedNode*> route;
  const std::optional<std::uint64_t> released =
      state_->mounted_root_ && BuildPointerRoute(*state_->mounted_root_, position, route)
          ? ResolvePointerFocusTarget(route)
          : std::nullopt;
  if (released != pending) {
    return false;
  }

  std::optional<TextInputSessionId> session_to_show;
  if (pending.has_value() && state_->focused_node_identity_ == pending && state_->text_input_session_.has_value() &&
      state_->text_input_session_->node_identity == *pending) {
    session_to_show = state_->text_input_session_->session_id;
  }
  SetFocusedNode(pending, false);
  if (session_to_show.has_value()) {
    if (PlatformTextInput* text_input = state_->platform_->TextInput()) {
      text_input->RequestShow(*session_to_show);
    }
  }
  return true;
}

void Runtime::HandlePointerDown(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured != state_->pointer_sessions_.end()) {
    PointerSession& active = captured->second;
    const PointerButton changed_button = event.changed_button;
    const bool additional_button = changed_button != PointerButton::None &&
                                   (active.pressed_buttons & changed_button) == PointerButton::None;
    if (!additional_button) {
      if (!active.quarantined) {
        CancelPointerSession(active, event);
      }
      state_->pointer_sessions_.erase(captured);
    } else {
      active.pressed_buttons = event.pressed_buttons;
      active.last_position = event.position;
      if (!BeginPointerChord(active, event)) {
        return;
      }
      DispatchChordPointerEvent(active, event);
      return;
    }
  }

  std::vector<detail::MountedNode*> route;
  if (!BuildPointerRoute(*state_->mounted_root_, event.position, route)) {
    return;
  }
  const PointerButton initiating_button = event.changed_button;
  const PointerButton pressed_buttons = event.pressed_buttons;
  const bool primary = initiating_button == PointerButton::Primary && !HasMultipleButtons(pressed_buttons);
  const bool secondary = initiating_button == PointerButton::Secondary && !HasMultipleButtons(pressed_buttons);
  if (!primary) {
    state_->text_selection_gesture_.previous_tap_time.reset();
    state_->text_selection_gesture_.previous_tap_node.reset();
  }
  if (primary) {
    for (detail::MountedNode* node : route) {
      if (IsScrollContainer(*node)) {
        StopScrollNodeMotion(*node);
      }
    }
  }

  PointerSession session;
  session.route.reserve(route.size());
  for (const detail::MountedNode* node : route) {
    session.route.push_back(node->identity);
  }
  session.down_position = event.position;
  session.last_position = event.position;
  session.device_kind = event.device_kind;
  session.initiating_button = initiating_button;
  session.pressed_buttons = pressed_buttons;
  session.chorded = HasMultipleButtons(pressed_buttons);
  const double timestamp = state_->platform_->Now();
  if (event.device_kind == PointerDeviceKind::Touch) {
    RecordScrollVelocitySample(session, event.position, timestamp);
  }
  const std::optional<std::uint64_t> focus_target = primary ? ResolvePointerFocusTarget(route) : std::nullopt;
  if (primary) {
    if (event.device_kind == PointerDeviceKind::Touch) {
      session.focus_pending = true;
      session.pending_focus_identity = focus_target;
    } else {
      SetFocusedNode(focus_target, false);
    }
  }

  std::optional<std::uint64_t> raw_target;
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    if (!raw_target.has_value() && (*node)->interaction.enabled && HandlesPointerEvents(**node)) {
      raw_target = (*node)->identity;
    }
  }
  session.raw_target_identity = raw_target;

  bool context_menu_candidate = false;
  for (auto node = route.rbegin(); node != route.rend(); ++node) {
    TapRecognitionState tap{
        .node_identity = (*node)->identity,
        .activates = raw_target == std::optional{(*node)->identity} &&
                     (static_cast<bool>((*node)->activation) ||
                      HasEventBinding<ViewEvents::Click>((*node)->event_bindings)),
    };
    if ((*node)->interaction.enabled &&
        HasEventBinding<ViewEvents::PointerIntercept>((*node)->event_bindings)) {
      session.recognitions.push_back(PointerRecognition{
          PointerInterceptRecognitionState{.node_identity = (*node)->identity},
      });
    }
    if (secondary && !context_menu_candidate && (*node)->interaction.enabled &&
        HasEventBinding<ViewEvents::ContextMenuRequested>((*node)->event_bindings)) {
      session.recognitions.push_back(PointerRecognition{
          ContextMenuRecognitionState{.node_identity = (*node)->identity},
      });
      context_menu_candidate = true;
    }
    if (!primary) {
      continue;
    }
    for (std::size_t index = (*node)->extensions.size(); index > 0; --index) {
      if (!(*node)->interaction.enabled) {
        continue;
      }
      NodeExtensionEntry& entry = (*node)->extensions[index - 1];
      const auto local_position = (*node)->presentation.resolved_transform.Inverse(event.position);
      if (!entry.extension || !local_position.has_value() || !entry.extension->HitTest(**node, *local_position)) {
        continue;
      }
      const NodeExtensionHandle handle{
          (*node)->identity,
          index - 1,
          entry.descriptor,
      };
      PointerEvent local_event = event;
      local_event.position = *local_position;
      std::shared_ptr<GestureRecognizer> gesture = entry.extension->CreateGestureRecognizer(
          **node, local_event, timestamp, state_->gesture_settings_, (*node)->presentation.resolved_transform
      );
      if (gesture) {
        const DragSourceCapability* source = entry.extension->GetDragSourceCapability();
        std::shared_ptr<DragSourceRecognizer> source_recognizer =
            source ? std::dynamic_pointer_cast<DragSourceRecognizer>(gesture) : nullptr;
        if (source && source_recognizer) {
          session.recognitions.push_back(PointerRecognition{DragSourceRecognitionState{
              handle,
              std::move(source_recognizer),
              (*node)->presentation.resolved_transform,
              *source,
              (*node)->environment,
          }});
        } else {
          GestureRecognitionState retained{
              handle,
              std::move(gesture),
              (*node)->presentation.resolved_transform,
          };
          if (retained.recognizer->SharesTap()) {
            tap.consumers.push_back(std::move(retained));
          } else {
            session.recognitions.push_back(PointerRecognition{std::move(retained)});
          }
        }
      } else if (entry.extension->GetDropTargetCapability()) {
        continue;
      } else {
        session.recognitions.push_back(PointerRecognition{ExtensionRecognitionState{handle}});
      }
    }
    if (focus_target == std::optional{(*node)->identity} && detail::FindTextSelectionClient(**node)) {
      session.recognitions.push_back(PointerRecognition{
          TextSelectionRecognitionState{.node_identity = (*node)->identity},
      });
    }
    if (tap.activates || !tap.consumers.empty()) {
      session.recognitions.push_back(PointerRecognition{std::move(tap)});
    }
    if ((*node)->interaction.enabled && IsScrollContainer(**node) && AllowsScrollSource(**node, ScrollSource::Drag) &&
        (!(*node)->scroll_state->touch_drag_only || event.device_kind == PointerDeviceKind::Touch)) {
      session.recognitions.push_back(PointerRecognition{ScrollRecognitionState{
          .node_identity = (*node)->identity,
          .axis = ScrollAxis(**node),
      }});
    }
  }

  if (!session.raw_target_identity.has_value() && session.recognitions.empty() && !session.focus_pending) {
    if (primary) {
      state_->text_selection_gesture_.previous_tap_time.reset();
      state_->text_selection_gesture_.previous_tap_node.reset();
    }
    return;
  }

  auto [inserted, unused] = state_->pointer_sessions_.insert_or_assign(event.pointer_id, std::move(session));
  static_cast<void>(unused);
  if (primary) {
    HandleTextSelectionPointerDown(event);
  }
  inserted = state_->pointer_sessions_.find(event.pointer_id);
  if (inserted == state_->pointer_sessions_.end() || inserted->second.quarantined ||
      inserted->second.owner.has_value()) {
    return;
  }
  const std::size_t recognition_count = inserted->second.recognitions.size();
  for (std::size_t index = 0; index < recognition_count; ++index) {
    const GestureDecision decision = UpdatePointerRecognition(inserted->second, index, event);
    inserted = state_->pointer_sessions_.find(event.pointer_id);
    if (inserted == state_->pointer_sessions_.end() || inserted->second.quarantined) {
      return;
    }
    if (decision == GestureDecision::Accept) {
      ResolvePointerRecognition(inserted->second, index, event);
      break;
    }
    if (decision == GestureDecision::Reject) {
      inserted->second.recognitions[index].started = false;
    }
  }
  inserted = state_->pointer_sessions_.find(event.pointer_id);
  if (inserted == state_->pointer_sessions_.end() || inserted->second.quarantined) {
    return;
  }
  if (!inserted->second.owner.has_value()) {
    std::optional<std::uint64_t> interaction_identity = inserted->second.raw_target_identity;
    if (!interaction_identity.has_value()) {
      const auto recognition = std::ranges::find_if(
          inserted->second.recognitions,
          [](const PointerRecognition& recognition) {
            return recognition.started && PointerRecognitionNodeIdentity(recognition).has_value();
          }
      );
      if (recognition != inserted->second.recognitions.end()) {
        interaction_identity = PointerRecognitionNodeIdentity(*recognition);
      }
    }
    if (primary && interaction_identity.has_value()) {
      BeginPointerInteraction(inserted->second, *interaction_identity, event);
    }
    if (inserted->second.raw_target_identity.has_value()) {
      const std::uint64_t target_identity = *inserted->second.raw_target_identity;
      inserted->second.raw_target_started = true;
      if (detail::MountedNode* target = FindNode(*state_->mounted_root_, target_identity)) {
        EmitEvent<ViewEvents::Pointer>(target->event_bindings, event);
      }
    }
  }
  AdvancePointerRecognition(timestamp);
}

void Runtime::HandlePointerMove(const PointerEvent& event, bool hover_moved) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    if (SupportsHover(event.device_kind)) {
      RefreshHover(hover_moved);
    }
    if (detail::MountedNode* target = HitTestPointer(*state_->mounted_root_, event.position);
        target && target->interaction.enabled) {
      EmitEvent<ViewEvents::Pointer>(target->event_bindings, event);
    }
    return;
  }

  PointerSession& session = captured->second;
  session.pressed_buttons = event.pressed_buttons;
  if (!session.chorded && HasMultipleButtons(session.pressed_buttons) && !BeginPointerChord(session, event)) {
    return;
  }
  if (session.quarantined) {
    return;
  }

  if (session.chorded) {
    DispatchChordPointerEvent(session, event);
    session.last_position = event.position;
    return;
  }

  if (session.device_kind == PointerDeviceKind::Touch) {
    RecordScrollVelocitySample(session, event.position, state_->platform_->Now());
    const float distance_x = event.position.x - session.down_position.x;
    const float distance_y = event.position.y - session.down_position.y;
    if (!session.owner.has_value() &&
        std::max(std::abs(distance_x), std::abs(distance_y)) >= detail::touch_gesture_slop) {
      session.focus_pending = false;
      session.pending_focus_identity.reset();
    }
  }

  const auto apply_scroll = [&](ScrollRecognitionState& scroll, float delta) {
    ApplyDragScroll(session, scroll, delta);
  };

  if (const std::optional<std::size_t> owner = RecognitionOwnerIndex(session)) {
    PointerRecognition& recognition = session.recognitions[*owner];
    if (auto* scroll = std::get_if<ScrollRecognitionState>(&recognition.state)) {
      apply_scroll(*scroll, PointerDelta(session.last_position, event.position, scroll->axis));
    } else {
      static_cast<void>(UpdatePointerRecognition(session, *owner, event));
    }
    session.last_position = event.position;
    return;
  }

  if (std::ranges::any_of(session.recognitions, [](const PointerRecognition& recognition) {
        return recognition.started && std::holds_alternative<TextSelectionRecognitionState>(recognition.state);
      })) {
    TrackTextSelectionGesture(event);
  }
  for (std::size_t index = 0; index < session.recognitions.size(); ++index) {
    if (!session.recognitions[index].started) {
      continue;
    }
    const GestureDecision decision = UpdatePointerRecognition(session, index, event);
    if (decision == GestureDecision::Reject) {
      session.recognitions[index].started = false;
      continue;
    }
    if (decision != GestureDecision::Accept) {
      continue;
    }
    ResolvePointerRecognition(session, index, event);
    if (auto* scroll = std::get_if<ScrollRecognitionState>(&session.recognitions[index].state)) {
      apply_scroll(*scroll, PointerDelta(session.down_position, event.position, scroll->axis));
    }
    break;
  }
  if (!session.owner.has_value() && session.raw_target_started && session.raw_target_identity.has_value()) {
    if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *session.raw_target_identity)) {
      EmitEvent<ViewEvents::Pointer>(target->event_bindings, event);
    } else {
      CancelPointerTarget(session, event);
    }
  }
  session.last_position = event.position;
}

void Runtime::HandlePointerCancel(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    return;
  }
  if (!captured->second.quarantined) {
    CancelPointerSession(captured->second, event);
  }
  state_->pointer_sessions_.erase(captured);
}

void Runtime::HandlePointerUp(const PointerEvent& event) {
  auto captured = state_->pointer_sessions_.find(event.pointer_id);
  if (captured == state_->pointer_sessions_.end()) {
    if (detail::MountedNode* target = HitTestPointer(*state_->mounted_root_, event.position);
        target && target->interaction.enabled) {
      EmitEvent<ViewEvents::Pointer>(target->event_bindings, event);
    }
    if (SupportsHover(event.device_kind)) {
      RefreshHover(false);
    }
    return;
  }
  PointerSession& session = captured->second;
  session.pressed_buttons = event.pressed_buttons;
  const bool final_release = session.pressed_buttons == PointerButton::None;
  if (!session.chorded &&
      (session.pressed_buttons != PointerButton::None || event.changed_button != session.initiating_button) &&
      !BeginPointerChord(session, event)) {
    if (final_release) {
      state_->pointer_sessions_.erase(captured);
    }
    return;
  }
  if (captured->second.quarantined) {
    if (final_release) {
      state_->pointer_sessions_.erase(captured);
    }
    if (final_release && SupportsHover(event.device_kind)) {
      RefreshHover(false);
    }
    return;
  }

  if (session.chorded) {
    DispatchChordPointerEvent(session, event);
    session.last_position = event.position;
    if (final_release) {
      state_->pointer_sessions_.erase(captured);
      if (SupportsHover(event.device_kind)) {
        RefreshHover(false);
      }
    }
    return;
  }

  std::optional<float> scroll_velocity;
  std::optional<Axis> scroll_axis;
  if (const std::optional<std::size_t> owner = RecognitionOwnerIndex(session)) {
    PointerRecognition& recognition = session.recognitions[*owner];
    if (auto* scroll = std::get_if<ScrollRecognitionState>(&recognition.state)) {
      scroll_axis = scroll->axis;
      if (session.device_kind == PointerDeviceKind::Touch) {
        scroll_velocity = EstimateScrollVelocity(session, scroll->axis, state_->platform_->Now());
      }
      for (std::uint64_t identity : std::exchange(scroll->active_nodes, {})) {
        if (detail::MountedNode* node = FindNode(*state_->mounted_root_, identity)) {
          NotifyScrollNodeActivity(*node, ScrollSource::Drag, ScrollPhase::End, 0.0F);
          if (node->scroll_state->motion.StartOverscrollSettlement(*node)) {
            state_->scroll_motion_active_ = true;
            RequestFrame();
            scroll_velocity.reset();
          }
        }
      }
    } else {
      static_cast<void>(UpdatePointerRecognition(session, *owner, event));
      if (std::holds_alternative<ExtensionRecognitionState>(recognition.state)) {
        CommitPendingTouchFocus(session, event.position);
      }
    }
    EndPointerInteraction(session, InteractionEvent::Type::Release, event);
    recognition.started = false;
  } else {
    EndPointerInteraction(session, InteractionEvent::Type::Release, event);
    for (std::size_t index = 0; index < session.recognitions.size(); ++index) {
      PointerRecognition& recognition = session.recognitions[index];
      if (!recognition.started || std::holds_alternative<TapRecognitionState>(recognition.state) ||
          std::holds_alternative<ContextMenuRecognitionState>(recognition.state) ||
          std::holds_alternative<TextSelectionRecognitionState>(recognition.state) ||
          std::holds_alternative<ScrollRecognitionState>(recognition.state)) {
        continue;
      }
      const GestureDecision decision = UpdatePointerRecognition(session, index, event);
      if (decision == GestureDecision::Accept) {
        ResolvePointerRecognition(session, index, event);
        break;
      }
      recognition.started = false;
    }
    if (!session.owner.has_value()) {
      CommitPendingTouchFocus(session, event.position);
      const auto text_selection =
          std::ranges::find_if(session.recognitions, [](const PointerRecognition& recognition) {
            return recognition.started &&
                   std::holds_alternative<TextSelectionRecognitionState>(recognition.state);
          });
      if (text_selection != session.recognitions.end() && TrackTextSelectionGesture(event)) {
        const std::size_t index = static_cast<std::size_t>(text_selection - session.recognitions.begin());
        ResolvePointerRecognition(session, index, event);
        SelectFocusedTextWord(event.position);
      }
    }
    if (!session.owner.has_value()) {
      const std::optional<std::uint64_t> raw_target = session.raw_target_identity;
      if (session.raw_target_started && raw_target.has_value()) {
        if (detail::MountedNode* target = FindNode(*state_->mounted_root_, *raw_target);
            target && target->interaction.enabled) {
          EmitEvent<ViewEvents::Pointer>(target->event_bindings, event);
        }
      }
      for (std::size_t index = 0; index < session.recognitions.size(); ++index) {
        if (!session.recognitions[index].started ||
            (!std::holds_alternative<TapRecognitionState>(session.recognitions[index].state) &&
             !std::holds_alternative<ContextMenuRecognitionState>(session.recognitions[index].state))) {
          continue;
        }
        const GestureDecision decision = UpdatePointerRecognition(session, index, event);
        if (decision == GestureDecision::Accept) {
          ResolvePointerRecognition(session, index, event);
          if (auto* tap = std::get_if<TapRecognitionState>(&session.recognitions[index].state)) {
            PublishTap(*tap, event);
          } else {
            PublishContextMenu(std::get<ContextMenuRecognitionState>(session.recognitions[index].state), event);
          }
          break;
        } else {
          const PointerEvent cancellation = CancellationEvent(event);
          CancelPointerRecognition(session.recognitions[index], cancellation);
        }
      }
    }
  }

  if (final_release) {
    RecordTextSelectionTap(session, event);
  }
  if (scroll_velocity.has_value() && scroll_axis.has_value()) {
    std::vector<detail::MountedNode*> route;
    route.reserve(session.route.size());
    for (std::uint64_t identity : session.route) {
      if (detail::MountedNode* node = FindNode(*state_->mounted_root_, identity)) {
        route.push_back(node);
      }
    }
    const float velocity = ApplyPreFling(route, *scroll_axis, *scroll_velocity);
    for (auto candidate = route.rbegin(); candidate != route.rend(); ++candidate) {
      if (!(*candidate)->interaction.enabled || !IsScrollContainer(**candidate) ||
          ScrollAxis(**candidate) != *scroll_axis ||
          !AllowsScrollSource(**candidate, ScrollSource::Momentum) || !CanScrollNode(**candidate, velocity)) {
        continue;
      }
      if ((*candidate)->scroll_state->motion.StartMomentum(**candidate, velocity)) {
        state_->scroll_motion_active_ = true;
        RequestFrame();
      }
      break;
    }
  }
  if (final_release) {
    state_->pointer_sessions_.erase(captured);
  }
  if (final_release && SupportsHover(event.device_kind)) {
    RefreshHover(false);
  }
}

} // namespace huxerui
