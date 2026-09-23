#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/environment.h>
#include <huxerui/state.h>
#include <huxerui/system.h>
#include <huxerui/task.h>

namespace huxerui::detail {

class AppResources;
class HttpTransport;
class HttpOperationState;

/// Window identity and observable resource overrides retained independently of the native window.
/// Callbacks capture this state weakly. Retirement clears ui_window before native facilities disappear, so keeping
/// an Environment or task alive cannot silently select a replacement attachment.
struct UiExecutionState {
  UiWindow* ui_window = nullptr;
  std::optional<ResourceConfiguration> configuration;
  std::shared_ptr<CompositionDependency> configuration_dependency = std::make_shared<CompositionDependency>();
};

/// Callback provenance: the original application, optional original window, and captured Environment.
/// Application/window references are weak; the Environment is retained for service and resource resolution after
/// composition returns. Installing this context neither switches threads nor grants permission to use composition
/// hooks.
struct ExecutionContext {
  std::weak_ptr<ApplicationRuntimeState> application;
  std::weak_ptr<UiExecutionState> ui;
  std::shared_ptr<const Environment> environment;
  // Distinguishes an application-only context from a window context whose weak window has expired.
  bool requires_ui = false;
};

/// Reads the calling thread's currently installed callback provenance without validating its lifetime.
/// @return The current context, or null outside a framework callback boundary.
std::shared_ptr<ExecutionContext> CurrentExecutionContext();
/// Checks capability thread and application identity without deciding its disconnected-result policy.
/// @param application_source Non-null original capability context; its weak application may already have expired.
/// @throws std::logic_error If the live owner uses another thread or the current context names another application.
/// Each service separately decides whether disconnection returns an unavailable result, does nothing, or throws.
void ValidateApplicationCall(const std::shared_ptr<ExecutionContext>& application_source);
/// Captures this call's original presentation source without retaining its Environment.
/// @param application_source Non-null application context used when no callback context is installed.
/// @return An independent context snapshot preserving application/window identity and the requires_ui distinction.
/// Validates the application/thread first; a queued interaction never follows a later window replacement.
std::shared_ptr<ExecutionContext> CapturePresentationContext(const std::shared_ptr<ExecutionContext>& application_source);
/// Checks whether the original application and any explicitly required window remain usable for presentation.
/// @param source Non-null captured presentation context.
/// @return False for an expired/stopped application or retired required window; true permits native endpoint checks.
/// @throws std::logic_error If a live source is checked from another application thread.
/// A true application-only result does not guarantee that the platform has a foreground Activity or native presenter.
bool IsPresentationSourceAvailable(const std::shared_ptr<ExecutionContext>& source);
/// Resolves application state from callback provenance or the published hosted service table.
/// @return The original non-stopped application state on its owning thread, including during installation/cleanup.
/// @throws std::logic_error If missing, stopped, on another thread, or associated with a retired required window.
/// Never falls back to a replacement hosted application when an existing execution context has expired.
std::shared_ptr<ApplicationRuntimeState> CurrentApplicationRuntime();

/// Exact-type service table installed on the application thread and published once after installation.
/// Keys stay fixed after publication. values_mutex protects shared_ptr acquisition against shutdown release, not
/// arbitrary cross-thread service method calls. Shutdown releases retained instances in reverse installation order.
struct ApplicationServiceTable {
  std::unordered_map<std::type_index, std::shared_ptr<void>> entries;
  std::vector<std::type_index> order;
  std::weak_ptr<ApplicationRuntimeState> runtime;
  std::atomic<bool> ready = false;
  mutable std::mutex values_mutex;

  /// Adds a service during the application installation phase.
  /// @param type Exact C++ lookup key, unique within this table.
  /// @param service Non-null instance already checked by the typed installation boundary.
  /// @throws std::logic_error If published/frozen or the key already exists.
  void Provide(std::type_index type, std::shared_ptr<void> service);
  /// Acquires a retained service while synchronizing with shutdown release.
  /// @param type Exact registered type key.
  /// @return The current instance, or null for an absent or released entry; publication is checked by the caller.
  std::shared_ptr<void> Find(std::type_index type) const;
  /// Releases service values in reverse installation order while leaving the frozen key set intact.
  /// Runs on the application thread and destroys captures outside values_mutex so destructors may re-enter lookup.
  void Clear() noexcept;
};

/// Shared application coordination state that may outlive the native Runtime during in-flight wake delivery.
/// The owner pointer and service/lifecycle fields belong to the application thread. Only explicit publication and
/// queue operations synchronize foreign threads. Retirement clears owner and stops dispatch before native teardown.
struct ApplicationRuntimeState final : std::enable_shared_from_this<ApplicationRuntimeState> {
  /// Installation/admission state, separate from native foreground lifecycle.
  /// Installing admits startup work without delivering it; Running admits normal work; Stopping closes new Tasks;
  /// Stopped invalidates the native owner and retained capability access.
  enum class Phase { Installing, Running, Stopping, Stopped };

  ApplicationRuntimeState(Runtime& owner, const Application& application, UiThreadDispatcher native_dispatcher);
  /// Requires this application's single owning thread.
  /// @throws std::logic_error If invoked from any other thread; does not itself validate the application's phase.
  void RequireThread() const;
  /// Creates a posting function weakly bound to this application state.
  /// @return A copyable dispatcher safe to retain after Runtime destruction; expired/closed owners discard callbacks.
  /// It uses Post's queue and application execution context, without capturing a window or keeping the owner alive.
  UiThreadDispatcher Dispatcher();
  /// Enqueues owned work from any thread, coalescing native wakes and holding work during installation.
  /// @param callback Nonempty callback to invoke on the application thread, or discard after queue closure.
  /// @throws std::invalid_argument If callback is empty; native posting errors propagate.
  /// If native enqueue rejects a wake, only this submission is withdrawn, concurrent additions remain queued, and
  /// the rejected callback's captures are released outside the queue lock. This is not automatic native failure
  /// recovery.
  void Post(std::function<void()> callback);
  /// Opens queued delivery after installation succeeds and requests a wake for accumulated startup work.
  void StartDispatch();
  /// Closes admission to the queue and drops pending callbacks before native teardown.
  /// Captures are released after unlocking, so their destructors may safely attempt another Post.
  void CloseDispatch() noexcept;
  /// Delivers one FIFO batch with application-only execution context on the owning thread.
  /// Reentrant posts wait for another batch. Remaining callbacks in the batch run after an individual callback throws;
  /// the first exception is rethrown after rearming queued work. No recovery from a failed native wake is promised.
  void Drain();

  Runtime* owner;
  const Application* declaration;
  std::thread::id thread;
  const UiThreadDispatcher native_dispatcher;
  Phase phase = Phase::Installing;
  std::atomic<bool> accepts_tasks = true;
  PlatformRegistry registry;
  std::shared_ptr<ApplicationServiceTable> services = std::make_shared<ApplicationServiceTable>();
  std::shared_ptr<ExecutionContext> execution;
  std::shared_ptr<AppResources> resources;
  std::shared_ptr<ApplicationService> application_service;
  std::shared_ptr<HttpTransport> http_transport;
  std::vector<std::weak_ptr<HttpOperationState>> http_operations;
  std::shared_ptr<TaskScopeState> tasks;
  std::vector<std::weak_ptr<UiExecutionState>> uis;
  std::vector<std::function<void()>> handler_cleanups;
  bool event_dispatch_pending = false;
  bool hosted = false;

private:
  std::mutex queue_mutex_;
  std::deque<std::function<void()>> queue_;
  bool dispatch_enabled_ = false;
  bool dispatch_closed_ = false;
  bool wake_posted_ = false;
};

/// Allocates an observation identity shared by all windows in the process.
/// @return A new monotonically allocated identity, so one window cannot erase another window's subscriptions.
std::uint64_t NextCompositionIdentity();

using PermissionStatusCompletion = std::function<void(PermissionStatus)>;
using PermissionSettingsCompletion = std::function<void(bool)>;

class PermissionTransport {
public:
  virtual ~PermissionTransport() = default;

  /// Reads the current native presentation endpoint generation for queued interactive requests.
  /// @return An identity that changes when the endpoint is replaced; zero is the default for hosts without such
  /// tracking.
  /// Query on the application thread; the shared request layer compares it again before starting native presentation.
  virtual std::uint64_t PresentationIdentity() const { return 0; }

  virtual std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) = 0;
};

class PermissionController final {
public:
  struct State;

  PermissionController(std::shared_ptr<PermissionTransport> transport, UiThreadDispatcher dispatch_to_ui_thread);
  ~PermissionController();

  PermissionController(const PermissionController&) = delete;
  PermissionController& operator=(const PermissionController&) = delete;
  PermissionController(PermissionController&&) = delete;
  PermissionController& operator=(PermissionController&&) = delete;

  [[nodiscard]] Task<PermissionStatus> Check(Permission permission) const;
  [[nodiscard]] Task<PermissionStatus> Request(Permission permission) const;
  [[nodiscard]] Task<bool> OpenSettings(Permission permission) const;
  void Disconnect() noexcept;

private:
  std::shared_ptr<State> state_;
};

struct ResolvedLocalNotification {
  std::string identifier;
  std::string title;
  std::string body;
  LocalNotificationPresentation presentation;
  Bytes data;
};

inline constexpr std::size_t max_local_notification_data_bytes = 64U * 1024U;

// Only self-contained envelopes can survive scheduled delivery after the originating process exits.
[[nodiscard]] Bytes EncodeLocalNotificationData(const PlatformPayload& data);
[[nodiscard]] PlatformPayload DecodeLocalNotificationData(std::span<const std::byte> bytes);

using LocalNotificationOperationCompletion = std::function<void(LocalNotificationOperationStatus)>;

class LocalNotificationTransport {
public:
  virtual ~LocalNotificationTransport() = default;

  /// Reads the current native presentation endpoint generation for queued interactive requests.
  /// @return An identity that changes when the endpoint is replaced; zero is the default for hosts without such
  /// tracking.
  /// Query on the application thread; the shared request layer compares it again before starting native presentation.
  virtual std::uint64_t PresentationIdentity() const { return 0; }

  [[nodiscard]] virtual LocalNotificationCapabilities Capabilities() const noexcept = 0;

  // An empty cancellation callback means an in-flight operation must finish before the ordered queue advances.
  // A non-empty callback stops the operation; it does not withdraw already accepted native notifications.
  virtual std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) = 0;
  virtual std::function<void()> Show(ResolvedLocalNotification notification,
                                     LocalNotificationOperationCompletion completion) = 0;
  virtual std::function<void()> Schedule(ResolvedLocalNotification notification,
                                         std::chrono::system_clock::time_point delivery_time,
                                         LocalNotificationOperationCompletion completion) = 0;
  virtual std::function<void()> Cancel(std::string identifier, LocalNotificationOperationCompletion completion) = 0;
};

class LocalNotificationService final {
public:
  struct State;

  static std::shared_ptr<LocalNotificationService> Create(std::shared_ptr<LocalNotificationTransport> transport,
                                                          UiThreadDispatcher dispatch_to_ui_thread,
                                                          std::shared_ptr<AppResources> resources);
  ~LocalNotificationService();

  [[nodiscard]] LocalNotificationCapabilities Capabilities() const;
  [[nodiscard]] Task<PermissionStatus> CheckAuthorization() const;
  [[nodiscard]] Task<PermissionStatus> RequestAuthorization() const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> Show(LocalNotification notification) const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> Schedule(LocalNotification notification,
                                                                std::chrono::system_clock::time_point delivery_time) const;
  [[nodiscard]] Task<LocalNotificationOperationStatus> Cancel(std::string_view identifier) const;
  void Disconnect() noexcept;

private:
  LocalNotificationService(std::shared_ptr<LocalNotificationTransport> transport,
                           UiThreadDispatcher dispatch_to_ui_thread, std::shared_ptr<AppResources> resources);

  [[nodiscard]] ResolvedLocalNotification Resolve(LocalNotification notification) const;

  std::shared_ptr<State> state_;
  std::shared_ptr<AppResources> resources_;
};

class ApplicationService final : public std::enable_shared_from_this<ApplicationService> {
public:
  ApplicationService(Runtime& runtime, std::optional<ApplicationActivation> startup_activation, ApplicationLifecycleState initial_lifecycle,
                     std::shared_ptr<PermissionController> permissions,
                     std::shared_ptr<LocalNotificationService> local_notifications,
                     std::shared_ptr<SystemTrayService> system_tray, PlatformClipboard* platform_clipboard,
                     std::optional<AppDirectories> directories);

  [[nodiscard]] const std::optional<ApplicationActivation>& StartupActivation() const noexcept;
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  [[nodiscard]] std::function<void()> ConnectActivation(std::function<void(ApplicationActivation)> handler);
  /// Adds an application transition observer without replaying the current state.
  /// @param handler Nonempty callback retained until disconnection or Runtime shutdown.
  /// @return An idempotent cleanup bound to this connection identity; multiple observers may coexist.
  [[nodiscard]] std::function<void()> ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler);
  [[nodiscard]] Task<PermissionStatus> CheckPermission(Permission permission) const;
  [[nodiscard]] Task<PermissionStatus> RequestPermission(Permission permission) const;
  [[nodiscard]] Task<bool> OpenPermissionSettings(Permission permission) const;
  [[nodiscard]] const std::shared_ptr<LocalNotificationService>& LocalNotifications() const noexcept;
  [[nodiscard]] const std::shared_ptr<SystemTrayService>& SystemTray() const noexcept;
  [[nodiscard]] const std::shared_ptr<huxerui::Clipboard>& Clipboard() const noexcept;
  [[nodiscard]] const AppDirectories& Directories() const;
  void Quit() const;
  void Enqueue(ApplicationActivation activation);
  void UpdateLifecycleState(ApplicationLifecycleState lifecycle_state);
  void DispatchPending();
  void Disconnect() noexcept;

private:
  void DisconnectActivationHandler(std::uint64_t connection) noexcept;
  void DisconnectLifecycleHandler(std::uint64_t connection) noexcept;

  Runtime* runtime_;
  std::optional<ApplicationActivation> startup_activation_;
  std::shared_ptr<StateCell<ApplicationLifecycleState>> lifecycle_state_;
  std::shared_ptr<PermissionController> permissions_;
  std::shared_ptr<LocalNotificationService> local_notifications_;
  std::shared_ptr<SystemTrayService> system_tray_;
  std::shared_ptr<huxerui::Clipboard> clipboard_;
  std::optional<AppDirectories> directories_;
  std::deque<ApplicationActivation> pending_activations_;
  /// One distinct lifecycle transition and the observers connected when it occurred.
  /// Captured recipient identities prevent newly mounted observers from receiving transitions queued before them.
  struct LifecycleDelivery {
    ApplicationLifecycleState state;
    std::vector<std::uint64_t> recipients;
  };
  std::deque<LifecycleDelivery> pending_lifecycle_states_;
  std::function<void(ApplicationActivation)> activation_handler_;
  std::map<std::uint64_t, std::function<void(ApplicationLifecycleState)>> lifecycle_handlers_;
  std::uint64_t activation_connection_ = 0;
  std::uint64_t next_connection_ = 1;
};

} // namespace huxerui::detail
