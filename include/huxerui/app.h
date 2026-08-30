#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/environment.h>
#include <huxerui/event.h>
#include <huxerui/file.h>
#include <huxerui/layer.h>
#include <huxerui/lifecycle.h>
#include <huxerui/platform_adapter.h>
#include <huxerui/platform_registry.h>
#include <huxerui/presentation.h>
#include <huxerui/render_scene.h>
#include <huxerui/root.h>
#include <huxerui/semantics.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>
#include <huxerui/window.h>

namespace huxerui {

struct DragEvent;

class FilePicker;
class ApplicationHandle;
struct ResourceConfiguration;

struct LaunchActivation {
  bool operator==(const LaunchActivation&) const = default;
};

struct UrlActivation {
  Uri url;

  bool operator==(const UrlActivation&) const = default;
};

struct FileActivation {
  std::vector<FileReference> files;
};

using ApplicationActivation = std::variant<LaunchActivation, UrlActivation, FileActivation>;

enum class ApplicationLifecycleState {
  Active,
  Inactive,
  Background,
};

namespace detail {
class ApplicationService;
class SystemTrayService;
class TaskScopeState;
} // namespace detail

struct SystemTrayOptions {
  StringVariant tooltip;
  std::vector<MenuEntry> menu;
};

class SystemTrayHandle final {
public:
  [[nodiscard]] bool IsAvailable() const;
  void Show(ImageVariant icon, SystemTrayOptions options = {}) const;
  void Hide() const;

  template <class... Dependencies>
  void OnActivate(std::function<void()> handler, Dependencies&&... dependencies) const {
    if (!handler) {
      throw std::invalid_argument("HuxerUI system tray activation handler must not be empty");
    }
    Lifecycle(
        [tray = *this, handler = std::move(handler)]() mutable {
          return tray.ConnectActivate(std::move(handler));
        },
        std::forward<Dependencies>(dependencies)...
    );
  }

private:
  SystemTrayHandle(
      std::shared_ptr<detail::SystemTrayService> service,
      std::shared_ptr<const Environment> environment,
      std::uint64_t owner
  )
      : service_(std::move(service)), environment_(std::move(environment)), owner_(owner) {}

  [[nodiscard]] std::function<void()> ConnectActivate(std::function<void()> handler) const;

  std::shared_ptr<detail::SystemTrayService> service_;
  std::shared_ptr<const Environment> environment_;
  std::uint64_t owner_ = 0;

  friend class ApplicationHandle;
};

struct AppOptions {
  WindowOptions window;
  ViewportBreakpoints viewport_breakpoints;
#if defined(NDEBUG)
  bool show_debug_overlay = false;
#else
  bool show_debug_overlay = true;
#endif
  std::vector<RootHook> root_hooks;
};

using RootFactory = View (*)();

class Application final {
public:
  explicit Application(RootFactory root_factory, AppOptions options = {});
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;

  const RootFactory root_factory;
  const AppOptions options;
};

class ApplicationHandle final {
public:
  [[nodiscard]] const ApplicationActivation& StartupActivation() const noexcept;
  // Reading the current platform-owned state during composition subscribes that scope to later changes.
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  [[nodiscard]] SystemTrayHandle SystemTray() const;
  void Quit() const;

  // Delivers each distinct platform transition while the declaring component Lifecycle is mounted.
  template <class... Dependencies>
  void OnLifecycleChange(
      std::function<void(ApplicationLifecycleState)> handler, Dependencies&&... dependencies
  ) const {
    if (!handler) {
      throw std::invalid_argument("HuxerUI application lifecycle handler must not be empty");
    }
    Lifecycle(
        [application = *this, handler = std::move(handler)]() mutable {
          return application.ConnectLifecycle(std::move(handler));
        },
        std::forward<Dependencies>(dependencies)...
    );
  }

  // Receives only activations submitted after this Runtime was created; StartupActivation remains separate.
  template <class... Dependencies>
  void OnActivation(std::function<void(ApplicationActivation)> handler, Dependencies&&... dependencies) const {
    if (!handler) {
      throw std::invalid_argument("HuxerUI application activation handler must not be empty");
    }
    Lifecycle(
        [application = *this, handler = std::move(handler)]() mutable {
          return application.ConnectActivation(std::move(handler));
        },
        std::forward<Dependencies>(dependencies)...
    );
  }

private:
  explicit ApplicationHandle(std::shared_ptr<detail::ApplicationService> service) : service_(std::move(service)) {}
  [[nodiscard]] std::function<void()> ConnectActivation(std::function<void(ApplicationActivation)> handler) const;
  [[nodiscard]] std::function<void()>
  ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler) const;

  std::shared_ptr<detail::ApplicationService> service_;

  friend ApplicationHandle UseApplication();
};

ApplicationHandle UseApplication();

namespace detail {

const Application& CurrentApplication();
int RunPlatformApplication(const Application& application);

enum class GestureDecision;
class GestureRecognizer;
struct NodeExtensionHandle;
struct ActiveDropTarget;
struct ContextMenuRecognitionState;
struct DragDropSession;
struct DragSourceRecognitionState;
struct MountedNode;
struct PointerRecognition;
struct PointerSession;
struct ScrollRecognitionState;
struct TapRecognitionState;
struct RuntimeAccess;
struct SceneTransitionRequest;
class SceneTransitionService;
struct ViewSpec;
class RecomposeScope;
class ScrollConnection;
class VirtualMeasureSession;

} // namespace detail

class Runtime final {
public:
  Runtime(
      const Application& application,
      PlatformAdapter& platform,
      ApplicationActivation startup_activation = LaunchActivation{}
  );
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  void SetWindowMetrics(WindowMetrics metrics);
  // Native non-client hit testing queries the geometry represented by the currently mounted tree.
  [[nodiscard]] bool IsWindowDragRegion(Point position) const;
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  const FrameCommit& BuildFrame();
  void HandlePointerEvent(const PointerEvent& event);
  /// Returns whether the window-local position has a HuxerUI context-menu handler.
  /// Platform hosts use this to preserve their native context menu outside claimed HuxerUI content.
  [[nodiscard]] bool HasContextMenuHandler(Point position) const;
  void HandleScrollEvent(const ScrollEvent& event);
  void HandleKeyEvent(const KeyEvent& event);
  // Platform hosts submit subsequent activation on the Runtime's UI thread; startup input is a constructor argument.
  void HandleApplicationActivation(ApplicationActivation activation);
  // Platform updates feed both the coalescing current value and any mounted ordered transition handler.
  void UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state);
  // Returns true when application code handled the platform minimize or close request.
  [[nodiscard]] bool HandleWindowRequest(WindowCommand command);
  bool HandleBack();
  bool HandleBack(const BackEvent& event);
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
  bool PerformSemanticAction(SemanticNodeId node_id, const SemanticAction& action);

private:
  struct State;

  enum class ScrollActivitySource {
    External,
    TextInputReveal,
  };

  void RequestFrame();
  void RequestApplicationQuit();
  void RequestFrameAfter(double delay_seconds);
  void NotifyScrollActivity(detail::MountedNode& node, ScrollActivitySource source);
  [[nodiscard]] std::optional<std::uint64_t> HitTestPlatformView(Point position) const;
  [[nodiscard]] std::optional<std::uint64_t> FocusedPlatformView() const;
  void SynchronizePlatformViewFocus(std::optional<std::uint64_t> identity, bool focus_visible);
  void MoveFocusFromPlatformView(std::uint64_t identity, bool reverse);
  std::optional<PlatformPayload> DispatchPlatformViewEvent(
      std::uint64_t identity, std::string_view name, const PlatformPayload& payload
  );
  std::optional<PlatformValue> DispatchPlatformViewEvent(
      std::uint64_t identity, std::type_index key, const PlatformValue& value
  );
  static detail::MountedNode* FindNode(detail::MountedNode& node, std::uint64_t identity);
  static NodeExtension* FindExtension(detail::MountedNode& root, const detail::NodeExtensionHandle& handle);
  static void ActivateNode(detail::MountedNode& node);
  std::uint64_t BeginInteraction(detail::MountedNode& node, InteractionEvent::Source source,
                                 std::optional<Point> position = std::nullopt);
  void EndInteraction(detail::MountedNode& node, InteractionEvent::Type type, InteractionEvent::Source source,
                      std::uint64_t press_id, std::optional<Point> position = std::nullopt);
  void BeginPointerInteraction(detail::PointerSession& session, std::uint64_t node_identity,
                               const PointerEvent& event);
  void EndPointerInteraction(detail::PointerSession& session, InteractionEvent::Type type,
                             const PointerEvent& event);
  void CancelPointerSession(detail::PointerSession& session, const PointerEvent& event);
  [[nodiscard]] bool BeginPointerChord(detail::PointerSession& session, const PointerEvent& event);
  void DispatchChordPointerEvent(detail::PointerSession& session, const PointerEvent& event);
  void QuarantinePointerSession(std::int64_t pointer_id, const PointerEvent& event);
  void CancelPointerTarget(detail::PointerSession& session, const PointerEvent& event);
  void CancelPointerRecognition(detail::PointerRecognition& recognition, const PointerEvent& event);
  void BeginDragDrop(detail::PointerSession& session, detail::DragSourceRecognitionState& recognition);
  void UpdateDragDrop(detail::PointerSession& session, const DragEvent& drag);
  void UpdateDropTarget(detail::PointerSession& session, const DragEvent& drag, bool emit_moved);
  void FinishDragDrop(detail::PointerSession& session, const DragEvent& drag);
  void CancelDragDrop(detail::PointerSession& session, const DragEvent& drag);
  void AdvanceDragDrop(const FrameInfo& frame);
  void AdvanceDragDropSession(std::int64_t pointer_id, const FrameInfo& frame);
  [[nodiscard]] std::optional<detail::ActiveDropTarget>
  ResolveDropTarget(const detail::DragDropSession& session, Point window_position) const;
  [[nodiscard]] bool ResolveSharedGestureRecognition(const std::shared_ptr<detail::GestureRecognizer>& recognizer,
    std::size_t index, const PointerEvent& event, std::optional<double> timestamp);
  void ResolvePointerRecognition(detail::PointerSession& session, std::size_t index, const PointerEvent& event,
                                 std::optional<double> timestamp = std::nullopt);
  void PublishTap(detail::TapRecognitionState& tap, const PointerEvent& event);
  void PublishContextMenu(detail::ContextMenuRecognitionState& context_menu, const PointerEvent& event);
  void AdvancePointerRecognition(double timestamp);
  [[nodiscard]] detail::GestureDecision
  UpdatePointerRecognition(detail::PointerSession& session, std::size_t index, const PointerEvent& event);
  std::vector<detail::MountedNode*> ApplyDragScroll(const detail::PointerSession& session,
                                                    detail::ScrollRecognitionState& scroll, float delta);
  void HandlePointerDown(const PointerEvent& event);
  void HandlePointerMove(const PointerEvent& event);
  void HandlePointerCancel(const PointerEvent& event);
  void HandlePointerUp(const PointerEvent& event);
  bool CommitPendingTouchFocus(detail::PointerSession& session, Point position);
  [[nodiscard]] std::optional<std::uint64_t> ResolvePointerFocusTarget(const std::vector<detail::MountedNode*>& route);
  void UpdatePointerCursor(std::optional<Point> position);
  void UpdateHoveredExtensions(Point position);
  void RefreshInteractionTree();
  bool DispatchKeyboardContextMenu();
  bool HandleFocusedTextInputKey(const KeyEvent& event);
  detail::MountedNode* ActiveFocusTrapRoot();
  void SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible = std::nullopt);
  void MoveFocus(bool reverse, bool wrap = true);
  bool BringTextInputIntoView();
  bool SelectFocusedTextWord(Point position, bool show_overlay = true);
  bool ExtendFocusedTextSelection(Point position, bool start_handle);
  bool QueryFocusedTextSelectionGeometry(Rect& start, Rect& end) const;
  bool HandleTextSelectionOverlayPointer(const PointerEvent& event);
  void HandleTextSelectionClick(const PointerEvent& event);
  bool TrackTouchTextSelectionGesture(const PointerEvent& event);
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
  void BindExtensionInvalidation(detail::MountedNode& node);
  void BuildSemantics();
  const FrameCommit& BuildFrame(FrameInfo frame);
  void InvalidateRoot();
  void InvalidateLayers();
  void DeactivateLayerInput(LayerId id);
  void InvalidateLayerPlacement(LayerId id);
  void BeginSceneTransition(
      detail::SceneTransitionRequest request, std::function<void()> mutation, bool reduced_motion
  );
  [[nodiscard]] std::optional<Point> CurrentInteractionOrigin() const noexcept;
  void InvalidateScope(std::uint64_t scope_id);
  void InvalidateLayout(detail::MountedNode& mounted);
  void QueueLifecycleCommit(const std::shared_ptr<detail::RecomposeScope>& scope);
  void RetireLifecycles(detail::RecomposeScope& scope) noexcept;
  void CommitLifecycles();
  void DiscardLifecycleCommits() noexcept;
  std::shared_ptr<detail::TaskScopeState> CreateTaskScope();
  void RetireTaskScope(std::shared_ptr<detail::TaskScopeState> scope) noexcept;
  void CommitTaskScopes() noexcept;
  void EnsureRootStructure();
  void ReconcileWindowControls();
  void CommitWindowAppearance();
  void ComposeApplication();
  void ComposeLayers();
  bool ComposeScope(detail::MountedNode& mounted);
  bool RecomposeDirtyScopes(detail::MountedNode& mounted);
  bool Reconcile(
      std::unique_ptr<detail::MountedNode>& mounted,
      const std::shared_ptr<detail::ViewSpec>& incoming,
      const std::shared_ptr<const Environment>& environment
  );
  std::unique_ptr<detail::MountedNode>
  Mount(const std::shared_ptr<detail::ViewSpec>& incoming, const std::shared_ptr<const Environment>& environment);
  bool ReconcileChildren(
      std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children,
      const std::vector<View>& incoming_children,
      const std::shared_ptr<const Environment>& environment
  );
  bool ReconcileLayerChildren(
      std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children,
      const std::vector<std::pair<View, std::shared_ptr<const Environment>>>& incoming_children
  );
  [[nodiscard]] const detail::MountedNode* RootNode() const noexcept;

  std::unique_ptr<State> state_;

  friend class LayerController;
  friend class detail::ApplicationService;
  friend class detail::RecomposeScope;
  friend class detail::SceneTransitionService;
  friend class detail::ScrollConnection;
  friend class detail::VirtualMeasureSession;
  friend struct detail::RuntimeAccess;
};

int RunApplication();

} // namespace huxerui
