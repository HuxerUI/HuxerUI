#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include <huxerui/app.h>

#include "gesture_internal.h"

namespace huxerui::detail {

class PointerInteraction final {
public:
  explicit PointerInteraction(Runtime::State& runtime_state) : runtime_state_(runtime_state) {}

  void HandlePointerEvent(const PointerEvent& event);
  bool HasContextMenuHandler(Point position) const;
  void RefreshHover(bool moved);
  void AdvancePointerRecognition(double timestamp);
  void AdvanceDragDrop(const FrameInfo& frame);
  void AdvanceTextSelectionLongPress(double timestamp);
  void ValidateTargets();
  void DeactivateSubtree(const MountedNode& root);
  void Disconnect() noexcept;
  void RefreshCursor();

private:
  void HandlePointerDown(const PointerEvent& event);
  void HandlePointerMove(const PointerEvent& event, bool hover_moved);
  void HandlePointerUp(const PointerEvent& event);
  void HandlePointerCancel(const PointerEvent& event);
  void BeginPointerInteraction(PointerSession& session, std::uint64_t node_identity, const PointerEvent& event);
  void EndPointerInteraction(PointerSession& session, InteractionEvent::Type type, const PointerEvent& event);
  void CancelPointerSession(PointerSession& session, const PointerEvent& event);
  void QuarantinePointerSession(std::int64_t pointer_id, const PointerEvent& event);
  void CancelPointerTarget(PointerSession& session, const PointerEvent& event);
  [[nodiscard]] bool BeginPointerChord(PointerSession& session, const PointerEvent& event);
  void DispatchChordPointerEvent(PointerSession& session, const PointerEvent& event);
  bool CommitPendingTouchFocus(PointerSession& session, Point position);

  [[nodiscard]] GestureDecision
  UpdatePointerRecognition(PointerSession& session, std::size_t index, const PointerEvent& event);
  bool AcceptPointerRecognition(PointerSession& session, std::size_t index, const PointerEvent& event,
                                std::optional<double> timestamp = std::nullopt);
  [[nodiscard]] bool AcceptSharedGestureRecognition(const std::shared_ptr<GestureRecognizer>& recognizer,
                                                   std::size_t index, const PointerEvent& event,
                                                   std::optional<double> timestamp);
  void CancelPointerRecognition(PointerRecognition& recognition, const PointerEvent& event);
  void PublishTap(TapRecognitionState& tap, const PointerEvent& event);
  void PublishContextMenu(ContextMenuRecognitionState& context_menu, const PointerEvent& event);
  void ApplyDragScroll(const PointerSession& session, ScrollRecognitionState& scroll, float delta);

  [[nodiscard]] bool TrackHoverPointer(const PointerEvent& event);
  void UpdateHoverRoute(PointerHoverState next, bool moved);
  void ClearHover();
  void UpdatePointerCursor(std::optional<Point> position);

  void HandleTextSelectionPointerDown(const PointerEvent& event);
  bool TrackTextSelectionGesture(const PointerEvent& event);
  void RecordTextSelectionTap(const PointerSession& session, const PointerEvent& event);

  void BeginDragDrop(PointerSession& session, DragSourceRecognitionState& recognition);
  void UpdateDragDrop(PointerSession& session, const DragEvent& drag);
  void UpdateDropTarget(PointerSession& session, const DragEvent& drag, bool emit_moved);
  [[nodiscard]] std::optional<ActiveDropTarget>
  ResolveDropTarget(const DragDropSession& session, Point window_position) const;
  void AdvanceDragDropSession(std::int64_t pointer_id, const FrameInfo& frame);
  void FinishDragDrop(PointerSession& session, const DragEvent& drag);
  void CancelDragDrop(PointerSession& session, const DragEvent& drag);

  Runtime::State& runtime_state_;
  std::optional<PointerHoverState> pointer_hover_;
  PointerCursorKind pointer_cursor_kind_ = PointerCursorKind::Default;
  std::unordered_map<std::int64_t, PointerSession> pointer_sessions_;

  friend class TextInteraction;
};

} // namespace huxerui::detail
