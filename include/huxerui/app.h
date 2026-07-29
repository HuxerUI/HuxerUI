#pragma once

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/display_list.h>
#include <huxerui/event.h>
#include <huxerui/root.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>

namespace huxerui {

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

class PlatformHost {
public:
  virtual ~PlatformHost() = default;

  virtual void RequestFrame(double delay_seconds) = 0;
  virtual double Now() const noexcept = 0;
  virtual Size
  MeasureText(std::string_view text, float font_size, float max_width = std::numeric_limits<float>::infinity()) = 0;
  virtual std::unique_ptr<detail::TextLayout>
  CreateTextLayout(std::string_view text, float font_size, float max_width = std::numeric_limits<float>::infinity());
  virtual PlatformTextInput* TextInput() noexcept {
    return nullptr;
  }
  virtual PlatformClipboard* Clipboard() noexcept {
    return nullptr;
  }
};

namespace detail {

struct EnvironmentFrame;
struct NodeExtensionHandle;
struct MountedNode;
struct PointerSession;
struct RuntimeAccess;
struct SavedNodeState;
struct ViewSpec;
class RecomposeScope;
class ScrollConnection;
class VirtualMeasureSession;

} // namespace detail

class Runtime final {
public:
  Runtime(AppDefinition definition, PlatformHost& host);
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  void SetViewport(Size viewport);
  const DisplayList& BuildFrame();
  void HandlePointerEvent(const PointerEvent& event);
  void HandleScrollEvent(const ScrollEvent& event);
  void HandleKeyEvent(const KeyEvent& event);
  [[nodiscard]] bool CanPerformTextEditingAction(TextEditingAction action) const;
  bool PerformTextEditingAction(TextEditingAction action);
  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch);
  [[nodiscard]] TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const;
  [[nodiscard]] TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const;

private:
  struct State;

  LayerId
  AttachLayer(LayerOptions options, ViewFactory content, std::shared_ptr<const detail::EnvironmentFrame> environment);
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
  void UpdateHoveredExtension(Point position);
  void RefreshInteractionTree();
  [[nodiscard]] std::optional<LayerId> ActiveModalLayerId() const;
  detail::MountedNode* ActiveModalFocusRoot();
  void SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible = std::nullopt);
  void MoveFocus(bool reverse);
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
  bool UpdateNodeExtensions(
      detail::MountedNode& node,
      const FrameInfo& frame,
      bool& needs_frame,
      std::optional<double>& next_wakeup,
      bool rebuild_cache
  );
  const DisplayList& BuildFrame(FrameInfo frame);
  void InvalidateRoot();
  void InvalidateScope(std::uint64_t scope_id);
  void ComposeRoot();
  void ComposeScope(detail::MountedNode& mounted);
  void RecomposeDirtyScopes(detail::MountedNode& mounted);
  void Reconcile(std::unique_ptr<detail::MountedNode>& mounted, const std::shared_ptr<detail::ViewSpec>& incoming);
  std::unique_ptr<detail::MountedNode> Mount(const std::shared_ptr<detail::ViewSpec>& incoming);
  void ReconcileChildren(
      std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children
  );
  detail::SavedNodeState SaveNodeState(detail::MountedNode& mounted);
  void RestoreNodeState(detail::MountedNode& mounted, detail::SavedNodeState& saved);
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
