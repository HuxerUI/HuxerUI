#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/event.h>
#include <huxerui/render_scene.h>
#include <huxerui/root.h>
#include <huxerui/text.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>

namespace huxerui {

class PlatformResources;
class Environment;
struct ResourceConfiguration;

namespace detail {
class TextLayout;
}

struct AppOptions {
  std::string title = "HuxerUI";
  float width = 520.0F;
  float height = 360.0F;
  std::vector<RootHook> root_hooks;
};

using RootFactory = View (*)();

struct AppDefinition {
  RootFactory root_factory = nullptr;
  AppOptions options;
};

class PlatformAdapter : public TextMeasurer {
public:
  virtual ~PlatformAdapter() = default;

  virtual void RequestFrameAt(double deadline) = 0;
  virtual double Now() const noexcept = 0;
  virtual std::unique_ptr<detail::TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  );
  virtual PlatformTextInput* TextInput() noexcept {
    return nullptr;
  }
  virtual PlatformClipboard* Clipboard() noexcept {
    return nullptr;
  }
  virtual PlatformResources* Resources() noexcept {
    return nullptr;
  }
};

namespace detail {

struct NodeExtensionHandle;
struct MountedNode;
struct PointerSession;
struct RuntimeAccess;
struct ViewSpec;
class RecomposeScope;
class ScrollConnection;
class VirtualMeasureSession;

} // namespace detail

class Runtime final {
public:
  Runtime(AppDefinition definition, PlatformAdapter& platform);
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  void SetViewport(Size viewport);
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  const FrameCommit& BuildFrame();
  void HandlePointerEvent(const PointerEvent& event);
  void HandleScrollEvent(const ScrollEvent& event);
  void HandleKeyEvent(const KeyEvent& event);
  bool PerformTextInputAction(TextInputSessionId session_id, TextInputAction action);
  [[nodiscard]] bool CanPerformTextEditingAction(TextEditingAction action) const;
  bool PerformTextEditingAction(TextEditingAction action);
  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch);
  [[nodiscard]] TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const;
  // Geometry is returned in logical coordinates relative to the HuxerUI host view.
  [[nodiscard]] TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const;
  // The point is expressed in logical coordinates relative to the HuxerUI host view.
  [[nodiscard]] TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const;

private:
  struct State;

  LayerId AttachLayer(LayerOptions options, ViewFactory content, std::shared_ptr<const Environment> environment);
  bool UpdateLayer(LayerId id, ViewFactory content);
  bool UpdateLayer(LayerId id, LayerOptions options, ViewFactory content);
  bool DismissLayer(LayerId id);
  void RequestFrame();
  void RequestFrameAfter(double delay_seconds);
  void NotifyScrollActivity(detail::MountedNode& node);
  static detail::MountedNode* FindNode(detail::MountedNode& node, std::uint64_t identity);
  static NodeExtension* FindExtension(detail::MountedNode& root, const detail::NodeExtensionHandle& handle);
  static void ActivateNode(detail::MountedNode& node);
  void ReleaseScrollGesture(detail::PointerSession& session);
  void DispatchExtensionObservers(detail::PointerSession& session, const PointerEvent& event, bool clear);
  [[nodiscard]] std::optional<std::size_t>
  FindScrollCandidate(const detail::PointerSession& session, Axis axis, float delta);
  std::vector<detail::MountedNode*> ApplyDragScroll(detail::PointerSession& session, float delta);
  void HandlePointerDown(const PointerEvent& event);
  void HandlePointerMove(const PointerEvent& event);
  void HandlePointerCancel(const PointerEvent& event);
  void HandlePointerUp(const PointerEvent& event);
  bool CommitPendingTouchFocus(detail::PointerSession& session, Point position, bool record_tap = false);
  void UpdateHoveredExtension(Point position);
  void RefreshInteractionTree();
  bool HandleFocusedTextInputKey(const KeyEvent& event);
  [[nodiscard]] std::optional<LayerId> ActiveModalLayerId() const;
  detail::MountedNode* ActiveModalFocusRoot();
  void SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible = std::nullopt);
  void MoveFocus(bool reverse, bool wrap = true);
  bool BringTextInputIntoView();
  bool SelectFocusedTextWord(Point position, bool show_overlay = true);
  bool ExtendFocusedTextSelection(Point position, bool start_handle);
  bool QueryFocusedTextSelectionGeometry(Rect& start, Rect& end) const;
  bool HandleTextSelectionOverlayPointer(const PointerEvent& event);
  void HandleTextSelectionClick(const PointerEvent& event);
  void TrackTouchTextSelectionGesture(const PointerEvent& event);
  void AdvanceTextSelectionLongPress(double timestamp);
  void AdvanceTextSelectionOverlay(const FrameInfo& frame);
  void PaintTextSelectionOverlay();
  void ShowTextSelectionOverlay(bool show_handles);
  void HideTextSelectionOverlay();
  void RefreshTextInputSession();
  void StopTextInputSession(TextInputEndReason reason);
  void InvalidateTextInputStateChange(
      std::uint64_t node_identity, const TextInputState& previous, const TextInputState& current
  );
  bool UpdateNodeExtensions(
      detail::MountedNode& node,
      const FrameInfo& frame,
      bool& needs_frame,
      std::optional<double>& next_wakeup,
      bool rebuild_cache
  );
  void BindExtensionPaintInvalidation(detail::MountedNode& node);
  const FrameCommit& BuildFrame(FrameInfo frame);
  void InvalidateRoot();
  void InvalidateScope(std::uint64_t scope_id);
  void InvalidateLayout(detail::MountedNode& mounted);
  void ComposeRoot();
  bool ComposeScope(detail::MountedNode& mounted);
  bool RecomposeDirtyScopes(detail::MountedNode& mounted);
  bool Reconcile(std::unique_ptr<detail::MountedNode>& mounted, const std::shared_ptr<detail::ViewSpec>& incoming);
  std::unique_ptr<detail::MountedNode> Mount(const std::shared_ptr<detail::ViewSpec>& incoming);
  bool ReconcileChildren(
      std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children
  );
  [[nodiscard]] const detail::MountedNode* RootNode() const noexcept;

  std::unique_ptr<State> state_;

  friend class LayerController;
  friend class detail::RecomposeScope;
  friend class detail::ScrollConnection;
  friend class detail::VirtualMeasureSession;
  friend struct detail::RuntimeAccess;
};

namespace detail {

void RegisterAppDefinition(AppDefinition definition);
const AppDefinition& RegisteredAppDefinition();

} // namespace detail

int RunApp(AppDefinition definition);

} // namespace huxerui

#if defined(__ANDROID__) || defined(HUXERUI_EXTERNAL_APP_HOST)
#define HUXERUI_APP(app_root, ...) \
  namespace { \
  [[maybe_unused]] const bool huxerui_app_registration = [] { \
    ::huxerui::detail::RegisterAppDefinition({ \
        .root_factory = (app_root), \
        .options = __VA_ARGS__, \
    }); \
    return true; \
  }(); \
  }
#else
#define HUXERUI_APP(app_root, ...) \
  int main() { \
    return ::huxerui::RunApp({ \
        .root_factory = (app_root), \
        .options = __VA_ARGS__, \
    }); \
  }
#endif
