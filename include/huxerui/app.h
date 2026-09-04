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
#include <huxerui/file_drop.h>
#include <huxerui/layer.h>
#include <huxerui/lifecycle.h>
#include <huxerui/platform_adapter.h>
#include <huxerui/platform_registry.h>
#include <huxerui/presentation.h>
#include <huxerui/render_scene.h>
#include <huxerui/root.h>
#include <huxerui/semantics.h>
#include <huxerui/task.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>
#include <huxerui/window.h>

namespace huxerui {

struct DragEvent;

class FilePicker;
class ApplicationHandle;
struct ResourceConfiguration;

/// Represents an ordinary application launch without an external payload.
struct LaunchActivation {
  bool operator==(const LaunchActivation&) const = default;
};

/// Represents application activation through a URL.
struct UrlActivation {
  /// Validated URL supplied by the platform application shell.
  Uri url;

  bool operator==(const UrlActivation&) const = default;
};

/// Represents application activation through one or more platform-granted files.
///
/// `files` must not be empty when the value is submitted to Runtime. Each `FileReference` retains its capability
/// semantics; applications must not assume that every value is a directly accessible local path.
struct FileActivation {
  /// Files supplied together by one platform activation.
  std::vector<FileReference> files;
};

/// Contains the normalized startup or subsequent activation delivered by the platform application shell.
///
/// Inspect the closed alternatives without parallel optional fields:
/// @code
/// if (std::holds_alternative<LaunchActivation>(activation)) {
///   ShowHome();
/// } else if (const auto* url = std::get_if<UrlActivation>(&activation)) {
///   OpenUrl(url->url);
/// } else {
///   OpenFiles(std::get<FileActivation>(activation).files);
/// }
/// @endcode
using ApplicationActivation = std::variant<LaunchActivation, UrlActivation, FileActivation>;

/// Identifies the platform-owned lifecycle state of one Runtime.
enum class ApplicationLifecycleState {
  /// The application is visible and accepts user interaction.
  Active,
  /// The application remains visible but is not currently active for user input.
  Inactive,
  /// The application is no longer presented as an active foreground experience.
  Background,
};

/// Identifies an application-level runtime permission with shared cross-platform semantics.
enum class Permission {
  /// Access to cameras used for still-image or video capture.
  Camera,
  /// Access to microphones used for audio capture.
  Microphone,
};

/// Describes the current authorization state of an application-level runtime permission.
enum class PermissionStatus {
  /// The platform has not yet asked the user to decide.
  NotDetermined,
  /// The application currently has access.
  Granted,
  /// Access is currently denied, but the platform does not reliably expose whether another prompt is possible.
  Denied,
  /// Access is denied and the platform explicitly reports that an in-application request cannot prompt again.
  PermanentlyDenied,
  /// Access is blocked by system, parental, or administrative policy.
  Restricted,
  /// The host, platform API, or native application declaration cannot provide this permission capability.
  Unavailable,
};

namespace detail {
class ApplicationService;
class FileDropReceiver;
class PointerInteraction;
class SemanticTree;
class SystemTrayService;
class TaskScopeState;
class TextInteraction;
} // namespace detail

/// Configures one system tray presentation.
struct SystemTrayOptions {
  /// Optional localized tooltip displayed by the platform tray host.
  StringVariant tooltip;
  /// Optional native menu shown from the tray presentation.
  std::vector<MenuEntry> menu;
};

/// Controls the Runtime-owned system tray presentation for the declaring composition scope.
///
/// One Runtime owns at most one active tray presentation. Keep presentation lifetime explicit with `Lifecycle` so
/// unmounting the declaring component also hides its tray item:
/// @code
/// const ApplicationHandle application = UseApplication();
/// const SystemTrayHandle tray = application.SystemTray();
/// tray.OnActivate([] { ShowMainWindow(); });
/// Lifecycle([application, tray] {
///   tray.Show(
///       ImageAsset::FromFile("tray.png"),
///       {.tooltip = "Example", .menu = {MenuItem("Quit", [application] { application.Quit(); })}}
///   );
///   return [tray] { tray.Hide(); };
/// });
/// @endcode
class SystemTrayHandle final {
public:
  /// Returns whether the current platform tray host is available.
  ///
  /// Reading this value during composition subscribes the current scope to availability changes.
  [[nodiscard]] bool IsAvailable() const;
  /// Creates or replaces this owner's tray presentation.
  ///
  /// `icon` must resolve to an `ImageAsset`. The desired presentation is retained while the tray host is temporarily
  /// unavailable and is shown if availability returns.
  void Show(ImageVariant icon, SystemTrayOptions options = {}) const;
  /// Hides the tray presentation when it is still owned by this handle's declaring scope.
  void Hide() const;

  /// Connects the primary tray activation handler for the declaring component Lifecycle.
  ///
  /// Dependencies use the same replacement rules as `Lifecycle`: list captured ordinary values that must reconnect
  /// the handler after they change. One Runtime may have only one committed tray activation handler.
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

/// Configures the process-level application declaration and each Runtime created from it.
struct AppOptions {
  /// Initial native-window configuration.
  WindowOptions window;
  /// Width thresholds used by `UseViewportClass()`.
  ViewportBreakpoints viewport_breakpoints;
#if defined(NDEBUG)
  /// Whether Runtime installs the built-in debug overlay above application root hooks.
  bool show_debug_overlay = false;
#else
  /// Whether Runtime installs the built-in debug overlay above application root hooks.
  bool show_debug_overlay = true;
#endif
  /// Ordered application root extensions installed for every Runtime.
  std::vector<RootHook> root_hooks;
};

/// Function pointer that creates the application root View.
///
/// The root is composed by Runtime and must not be annotated as a reusable composable function.
using RootFactory = View (*)();

/// Declares the one process-level HuxerUI application used by the platform shell.
///
/// Declare one stable instance after defining the root factory:
/// @code
/// View App() {
///   return MaterialTheme {HomePage()};
/// }
///
/// const Application application{
///     App,
///     {.window = {.title = "Example", .initial_size = {960.0F, 640.0F}}},
/// };
/// @endcode
class Application final {
public:
  explicit Application(RootFactory root_factory, AppOptions options = {});
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;

  /// Root factory used by every Runtime for this declaration.
  const RootFactory root_factory;
  /// Immutable Runtime and platform-shell options.
  const AppOptions options;
};

/// Provides composition-bound access to application activation, lifecycle, tray, and termination capabilities.
///
/// `StartupActivation()` is immutable, while `OnActivation()` receives only later platform activations.
/// `LifecycleState()` exposes the coalesced current value, while `OnLifecycleChange()` preserves distinct mounted
/// transitions:
/// @code
/// const ApplicationHandle application = UseApplication();
/// HandleActivation(application.StartupActivation());
/// UpdateForLifecycle(application.LifecycleState());
/// application.OnActivation([](ApplicationActivation activation) {
///   HandleActivation(std::move(activation));
/// });
/// application.OnLifecycleChange([](ApplicationLifecycleState state) {
///   PersistForLifecycle(state);
/// });
/// @endcode
class ApplicationHandle final {
public:
  /// Returns the immutable activation used to create the current Runtime.
  [[nodiscard]] const ApplicationActivation& StartupActivation() const noexcept;
  /// Returns the current platform-owned lifecycle state.
  ///
  /// Reading the value during composition subscribes the current scope to later distinct state changes.
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  /// Returns the system tray handle owned by the current composition scope.
  [[nodiscard]] SystemTrayHandle SystemTray() const;
  /// Queries the current platform permission state without presenting system UI.
  [[nodiscard]] Task<PermissionStatus> CheckPermissionAsync(Permission permission) const;
  /// Requests a platform permission, presenting system UI when the platform permits it.
  [[nodiscard]] Task<PermissionStatus> RequestPermissionAsync(Permission permission) const;
  /// Opens the platform settings surface relevant to a permission when one is available.
  ///
  /// A true result means the platform accepted the request to open settings; it does not imply that the permission
  /// changed.
  [[nodiscard]] Task<bool> OpenPermissionSettingsAsync(Permission permission) const;
  /// Requests orderly whole-application termination from the platform adapter.
  void Quit() const;

  /// Connects an ordered lifecycle-transition handler while the declaring component Lifecycle is mounted.
  ///
  /// The current value is not replayed when the handler connects; read `LifecycleState()` for current state.
  /// Dependencies reconnect the handler after captured ordinary values change.
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

  /// Connects the handler for activations submitted after the current Runtime was created.
  ///
  /// Startup activation is never replayed through this handler. Subsequent activations retain FIFO order and are not
  /// deduplicated. Dependencies reconnect the handler after captured ordinary values change.
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

/// Returns the application handle installed for the current composition.
///
/// A reusable function that calls this composition-bound facility should be marked with the composable attribute.
ApplicationHandle UseApplication();

namespace detail {

const Application& CurrentApplication();
int RunPlatformApplication(const Application& application);

struct MountedNode;
struct RuntimeAccess;
class SceneTransitionService;
struct ViewSpec;
class RecomposeScope;
class ScrollConnection;
class VirtualMeasureSession;

} // namespace detail

/// Owns one mounted HuxerUI surface and the shared state used by its platform host.
///
/// Platform adapters create Runtime on their UI thread, submit normalized host input, and consume `BuildFrame()`
/// commits. Application components normally use public hooks and handles instead of calling Runtime directly.
/// @code
/// Runtime runtime(application, adapter, LaunchActivation{});
/// runtime.SetWindowMetrics({.viewport = {960.0F, 640.0F}});
/// const FrameCommit& commit = runtime.BuildFrame();
/// // The platform renderer consumes commit.render_frame before the next build.
/// @endcode
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

  /// Updates the native-window metrics in logical coordinates and invalidates affected layout or composition state.
  void SetWindowMetrics(WindowMetrics metrics);
  /// Returns whether the current mounted tree claims a window-local point as a native drag region.
  ///
  /// Native non-client hit testing queries this without rebuilding the mounted tree.
  [[nodiscard]] bool IsWindowDragRegion(Point position) const;
  /// Updates platform resource configuration such as locale, display scale, and accessibility preferences.
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  /// Builds and commits pending composition, layout, semantics, animation, and rendering work.
  ///
  /// The returned reference is owned by Runtime and remains valid until the next frame build or Runtime destruction.
  const FrameCommit& BuildFrame();
  /// Delivers one normalized pointer event in logical window-local coordinates.
  void HandlePointerEvent(const PointerEvent& event);
  /// Starts a host-owned file drag. Session identifiers must be nonzero and increase within this Runtime.
  /// Positions are host-local DIPs. The result indicates provisional Copy eligibility, not completed file reception.
  [[nodiscard]] bool HandleFileDragEntered(std::uint64_t session, FileDropOffer offer, Point position);
  /// Updates the active host file drag; stale session identifiers are ignored.
  [[nodiscard]] bool HandleFileDragMoved(std::uint64_t session, FileDropOffer offer, Point position);
  /// Ends matching host hover without canceling previously accepted asynchronous deliveries.
  void HandleFileDragExited(std::uint64_t session);
  /// Accepts a host drop and begins private platform preparation after committing the target and ending hover.
  /// Platform implementations provide the source through the private file-drop boundary; applications use events.
  [[nodiscard]] bool HandleFileDrop(std::uint64_t session, FileDropOffer offer, Point position,
                                    detail::FileDropPreparation prepare);
  /// Returns whether the window-local position has a HuxerUI context-menu handler.
  /// Platform hosts use this to preserve their native context menu outside claimed HuxerUI content.
  [[nodiscard]] bool HasContextMenuHandler(Point position) const;
  /// Delivers normalized wheel or trackpad input and returns the actual delta consumed by HuxerUI.
  [[nodiscard]] Point HandleScrollInput(const ScrollInputEvent& event);
  /// Dispatches a normalized keyboard event and returns whether HuxerUI consumed it.
  bool HandleKeyEvent(const KeyEvent& event);
  /// Queues a subsequent platform activation for ordered delivery on a later frame.
  ///
  /// Platform hosts call this on the Runtime UI thread. Startup input is supplied to the constructor instead.
  void HandleApplicationActivation(ApplicationActivation activation);
  /// Updates the coalesced lifecycle value and queues a distinct transition for any mounted handler.
  void UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state);
  /// Dispatches a platform minimize or close request and returns whether application code handled it.
  [[nodiscard]] bool HandleWindowRequest(WindowCommand command);
  /// Commits an immediate platform Back request and returns whether HuxerUI consumed it.
  bool HandleBack();
  /// Dispatches one phase of a predictive or immediate Back request and returns whether HuxerUI consumed it.
  bool HandleBack(const BackEvent& event);
  /// Performs a platform text-input action when the session and configured action still match.
  bool PerformTextInputAction(TextInputSessionId session_id, TextInputAction action);
  /// Returns whether the focused text editing or selection client can currently perform an action.
  [[nodiscard]] bool CanPerformTextEditingAction(TextEditingAction action) const;
  /// Performs a supported editing action for the focused text editing or selection client.
  bool PerformTextEditingAction(TextEditingAction action);
  /// Applies an ordered platform text-input command batch to its active session.
  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch);
  /// Queries UTF-16 text context from an active text-input session.
  ///
  /// `start` and `length` are UTF-16 code-unit offsets. The result reports mismatch or rejection without throwing;
  /// an invalid result returned by a TextInputClient is a framework invariant failure.
  [[nodiscard]] TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const;
  /// Queries caret and range geometry for UTF-16 offsets in an active text-input session.
  ///
  /// Geometry is returned in logical coordinates relative to the HuxerUI host view.
  [[nodiscard]] TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const;
  /// Resolves a logical host-view point to the nearest text position in an active text-input session.
  [[nodiscard]] TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const;
  /// Dispatches one platform accessibility action and returns whether the current semantic tree accepted it.
  bool PerformSemanticAction(SemanticNodeId node_id, const SemanticAction& action);

private:
  struct State;

  void RequestFrame();
  void RequestApplicationQuit();
  void RequestFrameAfter(double delay_seconds);
  void NotifyScrollActivity(detail::MountedNode& node, const ScrollActivity& activity);
  [[nodiscard]] std::optional<std::uint64_t> HitTestPlatformView(Point position) const;
  [[nodiscard]] std::optional<std::uint64_t> FocusedPlatformView() const;
  void SynchronizePlatformViewFocus(std::optional<std::uint64_t> identity, bool focus_visible);
  bool MoveFocusFromPlatformView(std::uint64_t identity, bool reverse);
  std::optional<PlatformPayload> DispatchPlatformViewEvent(
      std::uint64_t identity, std::string_view name, const PlatformPayload& payload
  );
  std::optional<PlatformValue> DispatchPlatformViewEvent(
      std::uint64_t identity, std::type_index key, const PlatformValue& value
  );
  std::uint64_t BeginInteraction(detail::MountedNode& node, InteractionEvent::Source source,
                                 std::optional<Point> position = std::nullopt);
  void EndInteraction(detail::MountedNode& node, InteractionEvent::Type type, InteractionEvent::Source source,
                      std::uint64_t press_id, std::optional<Point> position = std::nullopt);
  [[nodiscard]] std::optional<std::uint64_t> ResolvePointerFocusTarget(const std::vector<detail::MountedNode*>& route);
  void RefreshInteractionTree();
  bool DispatchKeyboardContextMenu();
  detail::MountedNode* ActiveFocusTrapRoot();
  void SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible = std::nullopt);
  void MoveFocus(bool reverse, bool wrap = true);
  bool UpdateNodeExtensions(
      detail::MountedNode& node,
      const FrameInfo& frame,
      bool& needs_frame,
      std::optional<double>& next_wakeup,
      bool rebuild_cache
  );
  void BindExtensions(detail::MountedNode& node);
  void BuildSemantics();
  const FrameCommit& BuildFrame(FrameInfo frame);
  void InvalidateRoot();
  void InvalidateLayers();
  void DeactivateLayerInput(LayerId id);
  void InvalidateLayerPlacement(LayerId id);
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
  friend class detail::FileDropReceiver;
  friend class detail::PointerInteraction;
  friend class detail::RecomposeScope;
  friend class detail::SceneTransitionService;
  friend class detail::SemanticTree;
  friend class detail::ScrollConnection;
  friend class detail::TextInteraction;
  friend class detail::VirtualMeasureSession;
  friend struct detail::RuntimeAccess;
};

/// Runs the platform application shell for the unique process-level `Application` declaration.
///
/// Desktop and Apple platform entry points call this after static application initialization. Android and Web own
/// their platform entry lifecycle and do not expose this operation.
/// @code
/// int main() {
///   return RunApplication();
/// }
/// @endcode
int RunApplication();

} // namespace huxerui
