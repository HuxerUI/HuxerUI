#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <variant>
#include <vector>

#include <huxerui/event.h>
#include <huxerui/gesture.h>
#include <huxerui/layer.h>
#include <huxerui/modifier.h>

namespace huxerui::detail {

enum class GestureDecision {
  // Keep recognition pending and deliver later pointer or deadline updates.
  Continue,
  // Claim the pointer sequence. Runtime commits this recognition as the owner before publishing output.
  Accept,
  // Remove recognition from this sequence. Rejected recognition cannot become active later.
  Reject,
};

struct GestureRecognizerInput {
  // The event position stays in the node-local coordinate space captured when the recognizer was created.
  PointerEvent event;
  // Window position remains available for payloads and behavior that must cross node transforms.
  Point window_position;
  // Monotonic platform time in seconds, using the same clock as Deadline().
  double timestamp = 0.0;
};

// Recognition and output delivery are separate so Runtime can commit pointer ownership before application code runs.
// Update and AdvanceDeadline may mutate tentative recognizer state, but must not publish typed events or mutate the
// owning NodeExtension. Accepted, UpdateAccepted, Canceled, and TapAccepted are the output-delivery boundary.
class GestureRecognizer {
public:
  virtual ~GestureRecognizer() = default;

  // A shared-tap recognizer is attached to the node's single TapRecognitionState instead of entering ownership
  // resolution independently. Runtime then uses only Canceled and TapAccepted for that recognizer. This keeps Click and
  // MultiTap on the same movement, release-hit, disabled-state, and raw-target cancellation decision.
  [[nodiscard]] virtual bool SharesTap() const noexcept {
    return false;
  }

  // Runtime calls Update while this recognizer is pending. Down is delivered first, followed by each Move and the
  // terminal Up unless another recognizer wins. An explicit pointer Cancel uses Canceled instead. Continue preserves
  // pending recognition, Accept requests ownership, and Reject permanently removes this recognizer from the sequence.
  [[nodiscard]] virtual GestureDecision Update(const GestureRecognizerInput& input) = 0;

  // Return the next absolute platform timestamp at which a pending recognizer may change its decision. Runtime scans
  // all pending recognizers, coalesces their wake-ups, and does not poll recognizers that return no deadline.
  [[nodiscard]] virtual std::optional<double> Deadline() const noexcept {
    return std::nullopt;
  }

  // Runtime calls AdvanceDeadline only while the recognizer remains pending. The timestamp may be later than the
  // requested deadline, so implementations compare rather than require equality. Recognition therefore does not
  // depend on a synthetic pointer type or position; Runtime supplies the last committed position only if acceptance
  // needs to be delivered afterward.
  [[nodiscard]] virtual GestureDecision AdvanceDeadline(double timestamp) {
    static_cast<void>(timestamp);
    return GestureDecision::Continue;
  }

  // Runtime calls Accepted after storing this recognizer as the session owner, canceling every competing recognizer,
  // and canceling ordinary raw-pointer delivery. A recognizer shared by several PointerSessions may receive it again
  // after another session joins; all affected owners are committed before either callback form runs. The input is the
  // event that accepted the recognizer. For deadline acceptance, Runtime synthesizes a Move carrying the session's
  // last committed position and device kind. Implementations must commit bookkeeping before invoking handlers because
  // a handler may recompose or unmount the owner.
  virtual void Accepted(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) {
    static_cast<void>(node);
    static_cast<void>(extension);
    static_cast<void>(input);
  }

  // After acceptance, Runtime bypasses recognition and sends subsequent Move and Up events here. Delivery remains
  // bound to the owner even outside the original node bounds. Up is the normal completion path; device or ownership
  // cancellation uses Canceled instead. Implementations normally publish Changed and Ended from this callback.
  virtual void UpdateAccepted(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) {
    static_cast<void>(node);
    static_cast<void>(extension);
    static_cast<void>(input);
  }

  // Runtime calls Canceled when another recognizer wins, the platform cancels the pointer, or Runtime quarantines the
  // sequence after an exception. It may run before or after Accepted. A pending recognizer only discards tentative
  // state; a recognizer that published Started must publish at most one matching Canceled and clear ownership first.
  // If reconciliation removed the owning extension, its normal destruction is responsible for retained cleanup.
  virtual void Canceled(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) {
    static_cast<void>(node);
    static_cast<void>(extension);
    static_cast<void>(input);
  }

  // Runtime calls TapAccepted only when SharesTap returned true and the node's common Tap recognizer won on Up. A new
  // consumer recognizer is created for every pointer sequence, so accumulation across taps belongs to the compatible
  // NodeExtension rather than this per-sequence object.
  virtual void TapAccepted(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) {
    static_cast<void>(node);
    static_cast<void>(extension);
    static_cast<void>(input);
  }
};

// DragSource recognizers expose their latest immutable event snapshot so Runtime can order source and target output
// around session ownership, preview dismissal, and drop completion without adding callbacks to the generic recognizer.
class DragSourceRecognizer : public GestureRecognizer {
public:
  [[nodiscard]] virtual const DragEvent& CurrentEvent() const noexcept = 0;
};

struct DragSourceCapability {
  std::type_index payload_type = typeid(void);
  std::shared_ptr<const void> payload;
  std::function<View()> preview;
};

struct DropTargetCapability {
  std::type_index payload_type = typeid(void);
  std::function<bool(const void*)> accepts;
  DropTargetDispatch dispatch;
};

struct GestureRecognitionState {
  NodeExtensionHandle extension;
  std::shared_ptr<GestureRecognizer> recognizer;
  Transform2D frozen_node_to_window;
};

struct DragSourceRecognitionState {
  NodeExtensionHandle extension;
  std::shared_ptr<DragSourceRecognizer> recognizer;
  Transform2D frozen_node_to_window;
  DragSourceCapability source;
  std::shared_ptr<const Environment> environment;
};

struct TapRecognitionState {
  std::uint64_t node_identity = 0;
  bool activates = false;
  std::vector<GestureRecognitionState> consumers;
};

struct ScrollRecognitionState {
  std::uint64_t node_identity = 0;
  Axis axis = Axis::Vertical;
  std::optional<std::uint64_t> active_node;
};

struct ExtensionRecognitionState {
  NodeExtensionHandle extension;
};

struct PointerInterceptRecognitionState {
  std::uint64_t node_identity = 0;
};

struct ContextMenuRecognitionState {
  std::uint64_t node_identity = 0;
};

struct TextSelectionRecognitionState {
  std::uint64_t node_identity = 0;
  Point tap_position;
  double long_press_deadline = 0.0;
  bool double_tap_pending = false;
  bool long_press_pending = false;
};

// Mutable overlay drag state stays in the text-selection subsystem; this marker only identifies its pre-route owner.
struct TextSelectionOverlayOwner {};

using PointerRecognitionState = std::variant<
    TapRecognitionState,
    ScrollRecognitionState,
    PointerInterceptRecognitionState,
    ContextMenuRecognitionState,
    ExtensionRecognitionState,
    GestureRecognitionState,
    DragSourceRecognitionState,
    TextSelectionRecognitionState>;

struct PointerRecognition {
  PointerRecognitionState state;
  bool started = false;
};

// An index owns one entry in PointerSession::recognitions; the overlay owns the sequence without entering recognition.
using PointerOwner = std::variant<std::size_t, TextSelectionOverlayOwner>;

struct ActivePointerInteraction {
  std::uint64_t node_identity = 0;
  std::uint64_t press_id = 0;
};

struct ScrollVelocitySample {
  Point position;
  double timestamp = 0.0;
};

struct ActiveDropTarget {
  NodeExtensionHandle extension;
  // Exited must retain the event type that admitted this target even if compatible reconciliation changes its type.
  DropTargetDispatch dispatch;

  bool operator==(const ActiveDropTarget& other) const noexcept {
    return extension == other.extension;
  }
};

struct DragDropSession {
  NodeExtensionHandle source;
  std::type_index payload_type = typeid(void);
  std::shared_ptr<const void> payload;
  DragEvent drag;
  std::optional<ActiveDropTarget> target;
  // A target is committed before Entered is invoked; this flag prevents cancellation from publishing Exited when an
  // earlier source or target callback failed before Entered began.
  bool target_entered = false;
  std::optional<LayerId> preview_layer;
  // Layer placement is window-relative, so the frozen source transform resolves the local grab point once.
  Point preview_grab_offset;
};

struct PointerSession {
  // The committed Down route is retained for ownership validation and ancestor-based recognition.
  std::vector<std::uint64_t> route;
  std::optional<std::uint64_t> raw_target_identity;
  // Cancel is emitted only after the raw target has observed its matching Down.
  bool raw_target_started = false;
  // Recognitions retain deterministic deepest-node and reverse-modifier order; an indexed owner refers to this list.
  std::vector<PointerRecognition> recognitions;
  std::optional<PointerOwner> owner;
  std::optional<ActivePointerInteraction> interaction;
  std::optional<DragDropSession> drag_drop;
  std::optional<std::uint64_t> pending_focus_identity;
  Point down_position;
  Point last_position;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  PointerButton initiating_button = PointerButton::None;
  PointerButton pressed_buttons = PointerButton::None;
  std::array<ScrollVelocitySample, 8> scroll_velocity_samples;
  std::size_t scroll_velocity_sample_count = 0;
  bool focus_pending = false;
  bool chorded = false;
  // Deactivation preserves the host pointer sequence without retaining any mounted output target.
  bool quarantined = false;
};

void ValidateGestureSettings(const GestureSettings& settings);

} // namespace huxerui::detail
