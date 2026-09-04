#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/render_scene.h>

namespace huxerui::detail {

class IndicationState;
struct MountedNode;
class GestureRecognizer;

struct ActiveTextInputSession {
  struct GeometrySnapshot {
    std::uint64_t client_revision = 0;
    std::uint64_t layout_revision = 0;
    Transform2D node_to_host;
    TextInputGeometry geometry;
  };

  std::uint64_t node_identity = 0;
  TextInputSessionId session_id = 0;
  std::shared_ptr<TextInputClient> client;
  TextInputConfiguration configuration;
  TextInputState state;
  std::optional<GeometrySnapshot> published_geometry;
  std::optional<GeometrySnapshot> prepared_geometry;
};

struct TextSelectionGestureState {
  std::optional<double> previous_tap_time;
  Point previous_tap_position;
  std::optional<std::uint64_t> previous_tap_node;
  PointerDeviceKind previous_tap_device = PointerDeviceKind::Mouse;
};

struct TextSelectionOverlayState {
  bool visible = false;
  bool paint_dirty = true;
  bool indication_frame_active = false;
  bool has_painted_geometry = false;
  bool dragging = false;
  bool dragging_start_handle = false;
  bool show_handles = false;
  bool dismissing = false;
  std::optional<std::int64_t> pointer_id;
  std::optional<std::uint64_t> press_id;
  std::optional<std::size_t> pressed_action;
  std::optional<std::size_t> hovered_action;
  Rect start_handle_hit_rect;
  Rect end_handle_hit_rect;
  Rect painted_start;
  Rect painted_end;
  Rect toolbar_rect;
  Color toolbar_background;
  Color toolbar_separator;
  Shadow toolbar_shadow;
  EdgeInsets toolbar_separator_padding;
  float toolbar_corner_radius = 0.0F;
  float toolbar_separator_thickness = 0.0F;
  bool toolbar_separators = false;
  TextStyle toolbar_text_style;
  TextShapingOptions toolbar_text_shaping;
  std::vector<TextEditingAction> actions;
  std::vector<Rect> action_rects;
  std::vector<std::string> action_labels;
  std::vector<InteractionState> action_interactions;
  std::vector<std::shared_ptr<IndicationState>> action_indications;
};

struct TextSelectionOverlay {
  // The framework-owned selection overlay is rendered above application layers without becoming part of their tree.
  RenderNode render_node;
  TextSelectionOverlayState state;
};

class TextInteraction final {
public:
  explicit TextInteraction(Runtime::State& runtime_state) : runtime_state_(runtime_state) {}

  bool BringTextInputIntoView();
  void StopTextInputSession(TextInputEndReason reason);
  void RefreshTextInputSession();
  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch);
  bool PerformSemanticTextAction(std::uint64_t node, NodeExtension& extension, std::uint64_t local_id,
                                 const SemanticAction& action);
  bool HandleFocusedTextInputKey(const KeyEvent& event);
  bool PerformTextInputAction(TextInputSessionId session, TextInputAction action);
  TextInputContext QueryTextInputContext(TextInputSessionId session, TextOffset start, TextOffset length) const;
  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session, TextRange range) const;
  TextInputPositionResult QueryTextInputPosition(TextInputSessionId session, Point point) const;
  bool CanPerformTextEditingAction(TextEditingAction action) const;
  bool PerformTextEditingAction(TextEditingAction action);
  bool SelectTextWord(std::uint64_t node, Point position, bool show_overlay);
  void HideTextSelectionOverlay();
  void AdvanceTextSelectionOverlay(const FrameInfo& frame);
  bool HandleTextSelectionOverlayPointer(const PointerEvent& event);
  void PaintTextSelectionOverlay();

  bool HasSession() const noexcept;
  void RequestShowForNode(std::uint64_t node);
  bool SessionBelongsTo(const MountedNode& root) const;
  void NotifyScrollActivity(MountedNode& node, const ScrollActivity& activity);
  bool OverlayVisible() const noexcept;
  void InvalidateOverlay() noexcept;
  const RenderNode& Overlay() const noexcept;
  void ResetSelectionGesture() noexcept;
  std::shared_ptr<GestureRecognizer> CreateSelectionRecognizer(std::uint64_t node, const PointerEvent& event,
                                                              double timestamp, const GestureSettings& settings);
  void RememberSelectionTap(const PointerEvent& event, std::uint64_t node, double timestamp);

private:
  void InvalidateTextInputStateChange(std::uint64_t node, const TextInputState& previous, const TextInputState& current);
  bool ExtendFocusedTextSelection(Point position, bool start_handle);
  bool QueryFocusedTextSelectionGeometry(Rect& start, Rect& end) const;
  void ShowTextSelectionOverlay(bool show_handles);

  Runtime::State& runtime_state_;
  std::optional<ActiveTextInputSession> text_input_session_;
  TextSelectionGestureState text_selection_gesture_;
  TextSelectionOverlay text_selection_overlay_;
  TextInputSessionId next_text_input_session_id_ = 1;
};

} // namespace huxerui::detail
