#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <any>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/data.h>
#include <huxerui/environment.h>
#include <huxerui/event.h>
#include <huxerui/file.h>
#include <huxerui/file_drop.h>
#include <huxerui/layer.h>
#include <huxerui/lifecycle.h>
#include <huxerui/platform_registry.h>
#include <huxerui/presentation.h>
#include <huxerui/render_scene.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/semantics.h>
#include <huxerui/system.h>
#include <huxerui/task.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>
#include <huxerui/window.h>

namespace huxerui {

namespace detail {
/// Finds an application service without falling back to a window Environment.
/// @param type Exact registered C++ service type.
/// @return A retained service, or null if no published service of that type exists.
/// With an execution context, validates the original application and its thread; otherwise reads the frozen table.
std::shared_ptr<void> FindApplicationService(std::type_index type);
} // namespace detail

/// Acquires an installed application service or a service belonging to the current window context.
/// @tparam Service Exact type passed to ApplicationContext::Provide or WindowContext::Provide.
/// @return The retained service instance; repeated acquisition does not construct another instance.
/// @throws std::logic_error If the service is absent, the stored type is invalid, or the original context has retired.
///
/// Does not allocate composition slots. Frozen application services may be acquired from other threads when no
/// window execution context is active; each service method keeps its own thread rules. Window services require the
/// original live window context, including in its events. Application and window services cannot share a type.
/// @code{.cpp}
/// auto model = UseService<DemoViewModel>();
/// return Button("Refresh").OnClick([model] { model->Refresh(); });
/// @endcode
template <class Service> std::shared_ptr<Service> UseService() {
  if (auto service = detail::FindApplicationService(typeid(Service))) {
    return std::static_pointer_cast<Service>(std::move(service));
  }
  const std::any* value = detail::FindEnvironmentValue(detail::CurrentEnvironment(), typeid(Service));
  if (!value) {
    throw std::logic_error("HuxerUI requested root service is not installed");
  }
  const auto* service = std::any_cast<std::shared_ptr<Service>>(value);
  if (!service || !*service) {
    throw std::logic_error("HuxerUI root service environment value has an invalid stored type");
  }
  return *service;
}


/// Queued delivery of an owned callback on the application's UI thread.
/// The function accepts one owned std::function<void()> and may be invoked from another thread. Implementations must
/// enqueue rather than invoke inline, preserve submission order, and let retained copies safely discard work after
/// native shutdown. Captures must own their data; this callable does not capture a window Environment or TaskScope.
using UiThreadDispatcher = std::function<void(std::function<void()>)>;

class UiWindow;
class Runtime;
class PlatformClipboard;
class PlatformResources;
class PlatformTextInput;
struct GestureSettings;
enum class PointerCursorKind;
enum class SystemBarContentBrightness;
enum class WindowCommand;

namespace detail {

class ExternalTextureFrameRequester;
class ApplicationTimerQueue;
class FilePickerTransport;
class HttpTransport;
class LocalNotificationTransport;
class PlatformChannelEndpoint;
class PlatformRegistry;
class PermissionTransport;
class SystemTrayTransport;
class TextLayout;

PlatformChannelEndpoint MakePlatformChannelEndpoint(UiWindow& ui_window);
PlatformChannelEndpoint MakePlatformChannelEndpoint(Runtime& runtime);

} // namespace detail

/// One optional native process-usage sample used by performance diagnostics.
/// CPU time is cumulative, memory is measured in bytes, and processor_count normalizes CPU utilization between samples.
/// The platform may decline a sample by returning std::nullopt from Runtime::QueryProcessMetrics.
struct ProcessMetrics {
  // CPU time is cumulative; consumers derive utilization from two samples and the logical processor count.
  double cpu_time_seconds = 0.0;
  // Memory usage is the platform's preferred current process-footprint estimate, expressed in bytes.
  std::uint64_t memory_usage_bytes = 0;
  /// Logical processor count used to normalize cumulative CPU time; defaults to one.
  std::uint32_t processor_count = 1;

  bool operator==(const ProcessMetrics&) const = default;
};

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
/// } else if (const auto* files = std::get_if<FileActivation>(&activation)) {
///   OpenFiles(files->files);
/// } else {
///   OpenNotification(std::get<NotificationActivation>(activation).identifier);
/// }
/// @endcode
using ApplicationActivation = std::variant<LaunchActivation, UrlActivation, FileActivation, NotificationActivation>;

/// Identifies the aggregate platform-owned application lifecycle state.
enum class ApplicationLifecycleState {
  /// The application is visible and accepts user interaction.
  Active,
  /// The application remains visible but is not currently active for user input.
  Inactive,
  /// The application is no longer presented as an active foreground experience.
  Background,
};

namespace detail {
class ApplicationService;
class FileDropReceiver;
class PointerInteraction;
class SemanticTree;
class SystemTrayService;
class TaskScopeState;
class TextInteraction;
struct ApplicationRuntimeState;
struct InternalAccess;
} // namespace detail

/// Installs services, platform factories, and application-lifetime handlers during AppOptions::application_hooks.
/// Use this temporary context only on the application thread during that callback. Successful installation freezes
/// the single service table and factory catalog before queued application work or window initialization begins.
///
/// Application-owned models can hold State directly and survive window retirement:
/// @code{.cpp}
/// struct DemoViewModel {
///   State<int> activation_count{0};
/// };
///
/// void InstallApplication(ApplicationContext& context) {
///   auto model = std::make_shared<DemoViewModel>();
///   context.Provide(model);
///   context.OnActivation([model](ApplicationActivation) { ++model->activation_count; });
/// }
///
/// View App() {
///   const auto model = UseService<DemoViewModel>();
///   return Text(std::to_string(model->activation_count.Get()));
/// }
///
/// const Application application{App, {.application_hooks = {InstallApplication}}};
/// @endcode
class ApplicationContext final {
public:
  /// Installs one shared application service for its exact C++ type.
  /// @tparam Service Type used later with UseService; base classes are not registered implicitly.
  /// @param service Non-null instance retained until application shutdown; callers may retain their own copies.
  /// @throws std::invalid_argument If service is null.
  /// @throws std::logic_error If the type is already registered or installation has finished.
  template <class Service> void Provide(std::shared_ptr<Service> service) {
    if (!service) {
      throw std::invalid_argument("HuxerUI application service must not be empty");
    }
    ProvideService(typeid(Service), std::move(service));
  }

  /// Registers a lazily invoked module factory without constructing a module during installation.
  /// @tparam Module Exact value type returned by the factory and requested by OpenPlatformModule.
  /// @tparam Factory Callable accepting either Runtime& or UiWindow&, with an unambiguous host signature.
  /// @param name Nonempty UTF-8 registration name, unique among module and view registrations.
  /// @param factory Factory retained in the frozen application catalog and invoked once per module opening.
  /// Runtime factories may open without UI. UiWindow factories require the original live window execution context.
  /// @throws std::invalid_argument If name is invalid.
  /// @throws std::logic_error If name is duplicated or the catalog is frozen.
  template <class Module, class Factory> void RegisterPlatformModule(std::string name, Factory factory) {
    registry_->template RegisterModule<Module>(std::move(name), std::move(factory));
  }

  /// Registers a module factory with a strongly typed options value.
  /// @tparam Module Exact module result type.
  /// @tparam Options Exact options type supplied to OpenPlatformModule; no implicit registry conversion is performed.
  /// @tparam Factory Callable accepting Runtime& or UiWindow& followed by const Options&.
  /// @param name Nonempty UTF-8 name unique in this application's factory catalog.
  /// @param factory Retained factory invoked when an instance is requested; options are borrowed during that
  /// invocation.
  /// Uses the same host selection, installation lifetime, and registration errors as the overload without options.
  template <class Module, class Options, class Factory>
  void RegisterPlatformModule(std::string name, Factory factory) {
    registry_->template RegisterModule<Module, Options>(std::move(name), std::move(factory));
  }

  /// Registers a view factory whose native instances are created for their mounting UiWindow.
  /// @tparam Properties Exact declarative properties type, or void for a view without properties.
  /// @tparam Factory Copyable platform factory exposing Erase(UiWindow&).
  /// @param name Nonempty UTF-8 name unique in the application factory catalog.
  /// @param factory Retained factory copied for preparation in the actual mounting window.
  /// Registration does not construct native views; duplicate names or a frozen catalog throw std::logic_error.
  template <class Properties, class Factory> void RegisterPlatformView(std::string name, Factory factory) {
    registry_->template RegisterView<Properties>(std::move(name), std::move(factory));
  }

  /// Registers a native view with explicit properties and controller types.
  /// @tparam Properties Exact declarative properties type, or void; non-void values support movement and equality.
  /// @tparam Controller Exact public controller type; non-void values support movement and equality.
  /// @tparam Factory Copyable platform factory exposing Erase(UiWindow&).
  /// @param name Nonempty UTF-8 name unique in the application factory catalog.
  /// @param factory Retained factory prepared for each mounting window, without creating native views during
  /// registration.
  /// Uses the same name validation and frozen-catalog rules as the overload without a controller.
  template <class Properties, class Controller, class Factory>
  void RegisterPlatformView(std::string name, Factory factory) {
    registry_->template RegisterView<Properties, Controller>(std::move(name), std::move(factory));
  }

  /// Installs the application's single handler for subsequent platform activations.
  /// @param handler Nonempty callback receiving each owned activation in FIFO order on the application thread.
  /// StartupActivation is read separately and is never replayed. The handler remains connected until shutdown.
  /// @throws std::invalid_argument If handler is empty.
  /// @throws std::logic_error If an activation handler is already connected.
  void OnActivation(std::function<void(ApplicationActivation)> handler);
  /// Observes distinct application lifecycle transitions until shutdown.
  /// @param handler Nonempty callback receiving each queued transition on the application thread.
  /// Supports multiple observers and does not replay the current state; read UseApplication().LifecycleState() for it.
  /// @throws std::invalid_argument If handler is empty.
  void OnLifecycleChanged(std::function<void(ApplicationLifecycleState)> handler);

private:
  explicit ApplicationContext(detail::ApplicationRuntimeState& state);
  /// Stores a type-erased service in the same application table used by typed Provide.
  /// @param type Exact service key.
  /// @param service Non-null instance already validated by the typed entry point.
  void ProvideService(std::type_index type, std::shared_ptr<void> service);

  detail::ApplicationRuntimeState* state_;
  detail::PlatformRegistry* registry_;

  friend class Runtime;
};

/// A callback borrowing ApplicationContext during installation of one Runtime.
/// Hooks run in AppOptions declaration order on the application thread before windows or queued work. Do not retain
/// the context argument. Services and factories freeze after all hooks complete. Throwing stops later hooks and
/// aborts initialization, releasing services already installed by earlier hooks.
using ApplicationHook = std::function<void(ApplicationContext&)>;

/// Configures one system tray presentation.
struct SystemTrayOptions {
  /// Optional localized tooltip displayed by the platform tray host.
  StringVariant tooltip;
  /// Optional native menu shown from the tray presentation.
  std::vector<MenuEntry> menu;
};

/// Controls the application's shared tray presentation independently of any window lifetime.
/// All operations run on the original application's thread. The latest Show replaces the shared presentation;
/// Hide removes it regardless of which copied handle last called Show. Register primary activation once through
/// OnActivate, normally in an ApplicationHook.
/// @code{.cpp}
/// void InstallTray(ApplicationContext&) {
///   const auto application = UseApplication();
///   const auto tray = application.SystemTray();
///   tray.OnActivate([] { HandleTrayActivation(); });
///   tray.Show(
///       ImageAsset::FromFile("tray.png"),
///       {.tooltip = "Demo", .menu = {MenuItem("Quit", [application] { application.Quit(); })}}
///   );
/// }
/// @endcode
class SystemTrayHandle final {
public:
  /// Queries whether the native tray host is currently available.
  /// @return False when unsupported or disconnected; a composition read subscribes to availability changes.
  /// @throws std::logic_error If called on the wrong application thread or from a different application context.
  [[nodiscard]] bool IsAvailable() const;
  /// Creates or replaces the application tray presentation.
  /// @param icon Raster image asset or resource resolving to one; vector images are rejected.
  /// @param options Tooltip and native menu resolved using the application's resource configuration.
  /// The desired presentation survives temporary host unavailability and UI retirement while the application is live.
  /// @throws std::invalid_argument If the icon is empty, invalid, or resolves to a vector.
  /// @throws std::logic_error If the original application has expired or the application thread/context is wrong.
  void Show(ImageVariant icon, SystemTrayOptions options = {}) const;
  /// Removes the current application tray presentation and its desired contents.
  /// Repeated calls and calls after service disconnection are harmless on the owning application thread.
  /// @throws std::logic_error If the live application's thread or current application context does not match.
  void Hide() const;
  /// Registers the application's single primary tray activation handler until Runtime shutdown.
  /// @param handler Nonempty callback executed in the application's context on its thread while the tray is active.
  /// Registration runs on the original application's thread and does not require composition or show a tray item.
  /// The handler survives Show, Hide, and window retirement; this is not a composition-lifetime subscription.
  /// No window context is captured. Capture a WindowHandle for a fixed window or an application service that selects
  /// the target window. Native menu commands retain their own MenuEntry callbacks.
  /// @throws std::invalid_argument If handler is empty.
  /// @throws std::logic_error If already registered, the original application is disconnected, or the application
  /// thread/context does not match.
  void OnActivate(std::function<void()> handler) const;

private:
  explicit SystemTrayHandle(std::shared_ptr<detail::SystemTrayService> service) : service_(std::move(service)) {}
  std::shared_ptr<detail::SystemTrayService> service_;

  friend class ApplicationHandle;
};

/// Configures application installation and the initial defaults shared by its UiWindow attachments.
struct AppOptions {
  /// Initial native-window configuration.
  WindowOptions window{};
  /// Width thresholds used by `UseViewportClass()`.
  ViewportBreakpoints viewport_breakpoints{};
#if defined(NDEBUG)
  /// Whether UiWindow installs the built-in debug overlay above application root hooks.
  bool show_debug_overlay = false;
#else
  /// Whether UiWindow installs the built-in debug overlay above application root hooks.
  bool show_debug_overlay = true;
#endif
  /// Application installers, called once per Runtime in declaration order before windows or queued work.
  /// The list may be empty; an empty callback throws std::invalid_argument during application initialization.
  std::vector<ApplicationHook> application_hooks{};
  /// Window service/layer installers, called in declaration order before each window first composes.
  std::vector<WindowHook> window_hooks{};
};

/// Function pointer that creates the application root View.
///
/// The root is composed by UiWindow and must not be annotated as a reusable composable function.
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

  /// Root factory used by every UiWindow for this declaration.
  const RootFactory root_factory;
  /// Immutable UiWindow and platform-shell options.
  const AppOptions options;
};

/// Owns one application execution lifetime, including operation without a mounted UiWindow.
/// The native subclass supplies services and queued dispatch, calls InitializeApplication after native facilities are
/// ready, and calls Retire before destroying them. Application declarations must outlive their Runtime.
/// Shared state, service methods, and attached UIs use one application thread; native posting is the explicit
/// cross-thread entry. OnRuntimeStopped is delivered only after orderly shared shutdown completes.
class Runtime {
public:
  virtual ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  /// Begins orderly whole-application shutdown on the owning thread.
  /// Closes new Task admission immediately, then queues the same shared retirement used by Retire.
  /// Windows retire before application Tasks and services; native stop notification follows shared retirement.
  /// @throws std::logic_error If called during installation or from another thread. Repeated requests after stopping
  /// begins are harmless; the Runtime must already have been initialized.
  void RequestShutdown();
  /// Submits a normalized activation for later application-thread delivery without requiring a UI frame.
  /// @param activation Owned launch, URL, granted files, or notification payload; file lists must be nonempty.
  /// Requires initialization and the application thread; input after shutdown begins is ignored.
  /// @throws std::invalid_argument If a running application receives an invalid activation payload.
  void HandleApplicationActivation(ApplicationActivation activation);
  /// Publishes the native aggregate application lifecycle state on the owning thread.
  /// @param state Current process/application foreground state, not the state of an arbitrary attached window.
  /// Repeated values are ignored; distinct transitions are queued in order without requiring composition.
  void UpdateApplicationLifecycleState(ApplicationLifecycleState state);
  /// Publishes application resource defaults and refreshes application-owned presentation such as the tray.
  /// @param configuration Application locale and display density; display_scale must be finite and positive.
  /// Call on the initialized application's thread. Per-window overrides are submitted to their UiWindow instead.
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  /// Queues native work on the application's thread from any thread.
  /// @param task Nonempty owned callback; captured data must remain valid until execution or discard.
  /// @throws std::invalid_argument If task is empty. Native posting failures may propagate.
  /// Keep Runtime alive during this call. The callback acquires no captured Environment or scope cancellation;
  /// use TaskScope::Post when application work requires those lifetime and context rules.
  void DispatchToApplicationThread(std::function<void()> task) const;
  /// Returns the monotonic clock used for this Runtime's ordinary timer deadlines.
  /// @return A steady-clock time point in the same clock domain accepted by ScheduleTimerAt.
  /// Native subclasses and deterministic test hosts must keep their clock and scheduling implementation consistent.
  virtual std::chrono::steady_clock::time_point TimerNow() const noexcept {
    return std::chrono::steady_clock::now();
  }
  /// Schedules one callback independently of UI frame production.
  /// @param deadline Absolute monotonic deadline in TimerNow's clock domain; elapsed deadlines still enqueue delivery.
  /// @param callback Nonempty callback delivered at most once on the owning application thread.
  /// @return An idempotent cancellation function, also invoked on the application thread, that suppresses pending
  /// delivery.
  /// Scheduling and cancellation require that thread. The default native implementation uses one lazy timer worker;
  /// Web subclasses provide browser timers. This mechanism grants no operating-system background execution rights.
  virtual std::function<void()> ScheduleTimerAt(std::chrono::steady_clock::time_point deadline,
                                                std::function<void()> callback);
  /// Borrows the native installed-package resource capability during shared application initialization.
  /// @return A capability kept alive by the platform Runtime until shared retirement, or null if unavailable.
  virtual PlatformResources* Resources() noexcept {
    return nullptr;
  }
  /// Borrows the native plain-text clipboard capability during application initialization.
  /// @return A capability kept alive until shared retirement, or null for an unsupported clipboard host.
  virtual PlatformClipboard* Clipboard() noexcept {
    return nullptr;
  }
  /// Samples native process usage for diagnostics without throwing.
  /// @return Cumulative CPU time, current footprint, and processor count, or std::nullopt when unavailable.
  virtual std::optional<ProcessMetrics> QueryProcessMetrics() noexcept {
    return std::nullopt;
  }

protected:
  explicit Runtime(UiThreadDispatcher dispatcher);
  /// Reports whether shared application initialization completed and the application is still running.
  /// @return False before successful installation and after orderly stopping begins. Query on the application thread.
  bool IsInitialized() const noexcept;
  /// Installs shared application state once, after the derived native facilities are ready.
  /// @param application Stable declaration retained by reference for the Runtime's lifetime.
  /// @param startup_activation Immutable startup payload; std::nullopt represents startup without foreground
  /// activation.
  /// @param initial_lifecycle Native application state before any later transition is submitted.
  /// Runs on the native application thread, invokes application_hooks, freezes services/factories, then enables work.
  /// @throws std::logic_error If already initialized or another hosted application is installed.
  /// Invalid activation and installer exceptions propagate after shared initialization cleanup.
  void InitializeApplication(const Application& application,
                             std::optional<ApplicationActivation> startup_activation = LaunchActivation{},
                             ApplicationLifecycleState initial_lifecycle = ApplicationLifecycleState::Background);
  /// Synchronously disconnects shared application state before native facilities are released.
  /// Idempotently retires attached windows, closes tasks/timers/dispatch, disconnects services, and invalidates
  /// handles.
  /// Must run on the application thread once state exists; this teardown does not invoke OnRuntimeStopped.
  void Retire() noexcept;
  /// Captures platform-selected application storage directories during initialization.
  /// @return Directory values without performing application I/O, or std::nullopt if unsupported.
  virtual std::optional<AppDirectories> CreateAppDirectories();
  /// Creates the application's shared native HTTP capability during initialization.
  /// @return A transport shared by default HttpClient instances, or null when native HTTP is unavailable.
  virtual std::shared_ptr<detail::HttpTransport> CreateHttpTransport();
  /// Creates the native application notification capability during initialization.
  /// @return An application-owned transport, or null on hosts without local-notification support.
  virtual std::shared_ptr<detail::LocalNotificationTransport> CreateLocalNotificationTransport();
  /// Creates the native permission capability independently of a HuxerUI window.
  /// @return An application-owned transport, or null if unsupported. Interactive operations select a valid native
  /// presentation endpoint when requested, rather than retaining an Activity or window for the whole application.
  virtual std::shared_ptr<detail::PermissionTransport> CreatePermissionTransport();
  /// Creates the application's native tray capability during initialization.
  /// @return An application-owned transport, or null if the platform has no tray support.
  virtual std::shared_ptr<detail::SystemTrayTransport> CreateSystemTrayTransport();
  /// Notifies the native host after RequestShutdown has retired shared state.
  /// Runs on the application thread. The host may now stop its event loop and release native application facilities;
  /// the callback must not attempt to resume shared application or window work.
  virtual void OnRuntimeStopped() = 0;

private:
  /// Shared installation implementation used by platform hosts and isolated testing hosts.
  /// @param application Declaration retained for this Runtime's lifetime.
  /// @param startup_activation Optional immutable platform startup payload.
  /// @param initial_lifecycle Initial native application foreground state.
  /// @param hosted Whether to publish this application's services for context-free acquisition; false isolates test
  /// hosts.
  void InitializeApplication(const Application& application, std::optional<ApplicationActivation> startup_activation,
                             ApplicationLifecycleState initial_lifecycle, bool hosted);
  /// Coalesces application event delivery into the existing application queue without scheduling a UI frame.
  void RequestEventDispatch();
  /// Retires shared state and then invokes the native stop hook on the application thread.
  void FinishShutdown();
  /// Idempotently closes the ordinary timer queue before its dispatcher or native owner can disappear.
  void CloseTimers() noexcept;
  UiThreadDispatcher application_dispatcher_;
  std::shared_ptr<detail::ApplicationTimerQueue> timers_;
  std::shared_ptr<detail::ApplicationRuntimeState> state_;

  friend class UiWindow;
  friend class detail::ApplicationService;
  friend struct detail::InternalAccess;
  friend detail::PlatformChannelEndpoint detail::MakePlatformChannelEndpoint(Runtime& runtime);
};

/// Provides application activation, lifecycle, clipboard, directories, notifications, tray, and termination access.
///
/// `StartupActivation()` is immutable. Install the later activation handler through
/// `ApplicationContext::OnActivation()`.
/// `LifecycleState()` exposes the coalesced current value, while `OnLifecycleChanged()` preserves distinct mounted
/// transitions:
/// @code
/// const ApplicationHandle application = UseApplication();
/// const auto& startup = application.StartupActivation();
/// UpdateForLifecycle(application.LifecycleState());
/// application.OnLifecycleChanged([](ApplicationLifecycleState state) {
///   PersistForLifecycle(state);
/// });
/// @endcode
/// Route startup input once during application installation, or derive initial UI from startup without side effects.
class ApplicationHandle final {
public:
  /// Reads the immutable activation that originally started this application.
  /// @return A service-owned optional value valid while this handle retains the service; empty for background-only
  /// startup.
  /// Later activations are delivered through ApplicationContext::OnActivation and do not replace this value.
  [[nodiscard]] const std::optional<ApplicationActivation>& StartupActivation() const noexcept;
  /// Returns the current platform-owned lifecycle state.
  ///
  /// Reading the value during composition subscribes the current scope to later distinct state changes.
  [[nodiscard]] ApplicationLifecycleState LifecycleState() const;
  /// @brief Returns the shared plain-text clipboard for this application's Runtime.
  /// @return The same non-null instance on every call, including on hosts without clipboard support. Query
  /// Clipboard::IsAvailable() before presenting clipboard actions; individual reads and writes may still fail.
  ///
  /// This accessor does not require an active composition and may be called on a captured ApplicationHandle.
  /// Clipboard operations run synchronously on the owning UI thread. After Runtime destruction, the returned
  /// instance remains safe to retain and call but reports unavailable results.
  /// @code{.cpp}
  /// auto clipboard = UseApplication().Clipboard();
  /// return Button("Copy").OnClick([clipboard] { clipboard->WriteText("HuxerUI"); });
  /// @endcode
  [[nodiscard]] std::shared_ptr<huxerui::Clipboard> Clipboard() const noexcept;
  /// @brief Returns the platform-selected application directories captured at initialization without performing I/O.
  /// @return A reference valid while this ApplicationHandle or its Runtime retains the application service.
  /// Individual File values and the complete AppDirectories value may be copied and retained independently.
  /// @throws std::logic_error If the host does not provide application directories.
  ///
  /// A captured ApplicationHandle can query these paths outside composition and after Runtime destruction.
  /// Paths remain values, not access grants; later I/O can fail if permissions or on-disk contents change.
  /// @code{.cpp}
  /// auto application = UseApplication();
  /// File settings = application.Directories().data_directory.Child("settings.json");
  /// @endcode
  [[nodiscard]] const AppDirectories& Directories() const;
  /// @brief Queries the process current working directory at the time of the call.
  /// @return An absolute File path, which need not be an application data or executable directory.
  /// @throws std::runtime_error If the platform cannot determine the current working directory.
  ///
  /// This query does not require an active composition or a connected Runtime. The working directory is shared
  /// by all Runtime instances in the process and is not cached in AppDirectories.
  /// @code{.cpp}
  /// File working_directory = application.CurrentDirectory();
  /// @endcode
  [[nodiscard]] File CurrentDirectory() const;
  /// Acquires the application's shared tray handle without requiring active composition.
  /// @return A handle retaining the original application's tray service, independently of UI retirement.
  /// Register primary activation once through SystemTrayHandle::OnActivate, normally in an ApplicationHook.
  [[nodiscard]] SystemTrayHandle SystemTray() const;
  /// Acquires the application's notification service without capturing the current Environment.
  /// @return A handle whose Show/Schedule calls resolve text using the calling context's current resource
  /// configuration.
  [[nodiscard]] LocalNotificationHandle LocalNotifications() const;
  /// Queries current platform authorization without presenting system UI.
  /// @param permission Permission capability to query; validation occurs before returning the lazy Task.
  /// @return A Task resolving to current authorization or Unavailable when unsupported/disconnected.
  /// Call and await on the original application's thread; no UiWindow is required.
  [[nodiscard]] Task<PermissionStatus> CheckPermissionAsync(Permission permission) const;
  /// Requests authorization using the native presentation endpoint captured when this method is called.
  /// @param permission Permission capability to request.
  /// @return A lazy Task resolving to the platform's authorization result, or Unavailable if the original endpoint
  /// expires.
  /// Requires the application thread. Without a UiWindow, a platform-provided native endpoint may present the request;
  /// unsupported hosts return Unavailable. Queued requests never borrow a replacement window or permission launcher.
  /// Launch from an ApplicationHook or an ordinary application-thread callback, outside composition:
  /// @code{.cpp}
  /// auto application = UseApplication();
  /// auto tasks = UseApplicationTaskScope();
  /// tasks.Launch([application]() -> Task<void> {
  ///   const auto status = co_await application.RequestPermissionAsync(Permission::Camera);
  ///   HandleCameraPermission(status);
  /// });
  /// @endcode
  [[nodiscard]] Task<PermissionStatus> RequestPermissionAsync(Permission permission) const;
  /// Requests the native settings surface for a permission using the original presentation endpoint.
  /// @param permission Permission whose settings should be opened.
  /// @return A lazy Task resolving true if the platform accepted the opening request, not if permission was granted.
  /// False represents unsupported or expired presentation. Shares RequestPermissionAsync's thread and endpoint rules.
  [[nodiscard]] Task<bool> OpenPermissionSettingsAsync(Permission permission) const;
  /// Requests orderly whole-application termination through Runtime.
  void Quit() const;

  /// Observes distinct application transitions while the declaring composition Lifecycle is mounted.
  /// @tparam Dependencies Values compared by Lifecycle to decide when to reconnect the handler.
  /// @param handler Nonempty callback receiving ordered transitions on the application thread; current state is not
  /// replayed.
  /// @param dependencies Captured ordinary values whose changes require handler replacement.
  /// Requires active composition; multiple observers may coexist. Read LifecycleState for the current value.
  /// @throws std::invalid_argument If handler is empty.
  template <class... Dependencies>
  void OnLifecycleChanged(
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

private:
  explicit ApplicationHandle(std::shared_ptr<detail::ApplicationService> service) : service_(std::move(service)) {}
  /// Connects a service observer for the public Lifecycle wrapper.
  /// @param handler Nonempty transition callback bound to this handle's original application service.
  /// @return A disconnection callback owned by the declaring composition Lifecycle.
  [[nodiscard]] std::function<void()>
  ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler) const;

  std::shared_ptr<detail::ApplicationService> service_;

  friend ApplicationHandle UseApplication();
};

/// Acquires the original application handle from the active context or the published hosted Runtime.
/// @return A retained application handle; no composition or state slot is required.
/// @throws std::logic_error If no live application exists, the source window is disconnected, or the thread is wrong.
/// May be used during application_hooks, ordinary application-thread functions, events, and running Tasks.
ApplicationHandle UseApplication();

namespace detail {

const Application& CurrentApplication();
int RunPlatformApplication(const Application& application);

struct MountedNode;
struct InternalAccess;
class SceneTransitionService;
struct ViewSpec;
class RecomposeScope;
class VirtualMeasureSession;

} // namespace detail

/// Owns one mounted UI tree and the shared state used by its native window or embedded surface.
/// The platform derives directly from UiWindow and associates it with an already initialized Runtime. Composition,
/// layout, input, State observation, and frame production stay on that application's thread. Native operations are
/// overridden at this boundary; application components use WindowHandle, hooks, and services.
///
/// Prepare native members, call InitializeWindow once, then admit input and BuildFrame calls. Call Retire before native
/// members are released. A UiWindow may represent an embedded view rather than an application-owned top-level window;
/// retiring it does not itself terminate the application or its application-owned Tasks.
class UiWindow : public TextMeasurer {
public:
  virtual ~UiWindow();
  /// Borrows the initialized window's application owner.
  /// @return The associated Runtime, kept alive by the native host for this window's lifetime.
  /// @throws std::logic_error If no application has been attached.
  Runtime& ApplicationRuntime() const;
  /// Uses the associated Runtime's native posting function.
  /// @param task Nonempty owned callback queued to the application's UI thread; never invoked inline.
  /// Keep this window and its Runtime alive for the call. Posting does not capture this window's Environment or cancel
  /// on retirement; use a composition-owned TaskScope::Post for callbacks canceled with their UI scope.
  void DispatchToUiThread(std::function<void()> task) const;
  /// Requests native frame delivery at or after an absolute monotonic time.
  /// @param deadline Time in seconds in the same clock domain as Now; a past deadline requests the next available
  /// frame.
  /// Called on the application thread. The host coalesces wakeups, later calls BuildFrame, and presents its
  /// FrameCommit.
  virtual void RequestFrameAt(double deadline) = 0;
  /// Reads the monotonic clock used for this window's frame and animation timestamps.
  /// @return Seconds in the same time domain accepted by RequestFrameAt; unrelated to wall-clock calendar time.
  virtual double Now() const noexcept;
  /// Obtains platform gesture thresholds for this window.
  /// @return Device-independent gesture settings used by the shared input state machines.
  virtual GestureSettings GestureDefaults() const noexcept;
  /// Obtains native-feeling defaults for shared scroll motion.
  /// @return Scroll physics used where the UI tree supplies no explicit override.
  virtual ScrollPhysics ScrollDefaults() const noexcept;
  /// Requests the cursor represented by the PointerCursorKind argument on this native window.
  /// Called on the application thread; the default implementation ignores the request on hosts without pointer cursors.
  virtual void SetPointerCursor(PointerCursorKind) {}
  /// Creates native text measurement/layout state for attributed content on the application thread.
  /// @param text Attributed content to retain or copy into the layout.
  /// @param style Resolved default typography used where spans do not override it.
  /// @param max_width Available wrapping width in logical units.
  /// @param options Resolved text layout behavior, including line constraints and truncation.
  /// @return An owned layout used by shared text measurement and rendering; it must not borrow temporary arguments.
  virtual std::unique_ptr<detail::TextLayout> CreateTextLayout(const AttributedText& text, const TextStyle& style,
                                                               float max_width, const TextLayoutOptions& options = {});
  /// Creates a text layout for a plain UTF-8 string through the attributed-text overload.
  /// @param text Borrowed UTF-8 input, needed only during this call.
  /// @param style Resolved typography.
  /// @param max_width Available wrapping width in logical units.
  /// @param options Resolved line and truncation behavior.
  /// @return An owned native text layout for this window.
  std::unique_ptr<detail::TextLayout> CreateTextLayout(std::string_view text, const TextStyle& style, float max_width,
                                                       const TextLayoutOptions& options = {});
  /// Borrows this window's native text-input/IME endpoint.
  /// @return A platform-owned endpoint valid until shared retirement, or null when text input is unsupported.
  virtual PlatformTextInput* TextInput() noexcept {
    return nullptr;
  }
  /// Asks the native host to perform the operation carried by the WindowCommand argument.
  /// Called on the application thread. Requests may be ignored when the embedded surface does not own a top-level
  /// window;
  /// native minimize/close callbacks still pass through HandleWindowRequest before applying their default operation.
  virtual void RequestWindowCommand(WindowCommand) {}
  /// Updates native system-bar foreground contrast on the application thread.
  /// The first SystemBarContentBrightness argument describes the status bar; the second describes the navigation bar.
  /// The UI paints backgrounds separately. Hosts without these system bars may ignore the request.
  virtual void SetSystemBarsContentBrightness(SystemBarContentBrightness, SystemBarContentBrightness) {}

  UiWindow(const UiWindow&) = delete;
  UiWindow& operator=(const UiWindow&) = delete;
  UiWindow(UiWindow&&) = delete;
  UiWindow& operator=(UiWindow&&) = delete;

  /// Publishes current native geometry and invalidates affected layout/composition.
  /// @param metrics Complete viewport, safe-area insets, and optional title-bar geometry in logical window-local units.
  /// Call on the application thread after successful initialization; native geometry is authoritative.
  void SetWindowMetrics(WindowMetrics metrics);
  /// Queries the committed tree for a native title-bar drag hit.
  /// @param position Point in logical window-local coordinates.
  /// @return True when a declared drag region claims the point and interactive descendants do not take precedence.
  /// Native non-client hit testing uses this result without rebuilding the UI tree.
  [[nodiscard]] bool IsWindowDragRegion(Point position) const;
  /// Publishes resource configuration for this window without replacing application-wide defaults.
  /// @param configuration Current window density and locale; display_scale is finite and positive.
  /// Call on the application thread; affected UI observations are invalidated for later composition.
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  /// Builds and commits pending composition, layout, semantics, animation, and rendering work.
  /// @return A reference owned by this UiWindow, valid until its next frame build or retirement.
  /// Call after successful shared initialization on the application thread, then consume the commit in the native
  /// renderer.
  const FrameCommit& BuildFrame();
  /// Delivers normalized pointer input to the shared interaction tree.
  /// @param event Pointer identity, phase, and position in logical window-local coordinates.
  /// The native host preserves ordered input and cancellation on the application thread.
  void HandlePointerEvent(const PointerEvent& event);
  /// Starts native file-drag hover over this window.
  /// @param session Nonzero session identity increasing within this UiWindow.
  /// @param offer Provisional file formats and operation capabilities; no file payload is read yet.
  /// @param position Hover location in window-local logical units.
  /// @return Provisional Copy acceptance, not confirmation of completed asynchronous file delivery.
  [[nodiscard]] bool HandleFileDragEntered(std::uint64_t session, FileDropOffer offer, Point position);
  /// Updates hover for the active native file-drag session.
  /// @param session Identity of the current native drag; stale sessions are ignored.
  /// @param offer Updated provisional file formats and capabilities.
  /// @param position Current logical window-local hover point.
  /// @return Whether the current target provisionally accepts Copy.
  [[nodiscard]] bool HandleFileDragMoved(std::uint64_t session, FileDropOffer offer, Point position);
  /// Ends matching native file-drag hover without canceling already accepted asynchronous deliveries.
  /// @param session Session to leave; stale or unknown identities have no effect.
  void HandleFileDragExited(std::uint64_t session);
  /// Commits a native file drop and starts platform preparation for its selected target.
  /// @param session Current nonzero native drag identity.
  /// @param offer Formats and capabilities accepted by the target.
  /// @param position Final logical window-local drop point.
  /// @param prepare Platform-owned preparation callback; asynchronous delivery retains only the accepted source
  /// lifetime.
  /// @return Whether the drop was accepted for preparation, not whether every file was subsequently received.
  [[nodiscard]] bool HandleFileDrop(std::uint64_t session, FileDropOffer offer, Point position,
                                    detail::FileDropPreparation prepare);
  /// Checks whether HuxerUI claims a native context-menu location.
  /// @param position Logical window-local point.
  /// @return True when a mounted handler claims it; otherwise the host may keep its ordinary native menu.
  [[nodiscard]] bool HasContextMenuHandler(Point position) const;
  /// Dispatches wheel/trackpad input through the shared scroll and gesture state machines.
  /// @param event Normalized native scroll input in the contract's logical units and phases.
  /// @return The actual delta consumed by HuxerUI, for native propagation of any remaining input.
  [[nodiscard]] Point HandleScrollInput(const ScrollInputEvent& event);
  /// Dispatches one normalized keyboard event through the focused UI route.
  /// @param event Key identity, phase, and modifiers normalized by the native host.
  /// @return Whether HuxerUI consumed the event.
  bool HandleKeyEvent(const KeyEvent& event);
  /// Publishes native foreground state for this window or scene independently of application lifecycle.
  /// @param lifecycle_state Current state of this attachment.
  /// Call on the application thread; repeated values are ignored and distinct transitions use the application queue.
  void UpdateWindowLifecycleState(WindowLifecycleState lifecycle_state);
  /// Consults application handlers before a native minimize or close operation.
  /// @param command Minimize or Close request emitted by the native host.
  /// @return True when an installed handler consumes the request; false allows the native default operation.
  [[nodiscard]] bool HandleWindowRequest(WindowCommand command);
  /// Dispatches an immediate platform Back action on the application thread.
  /// @return Whether the active shared Back route consumed the request.
  bool HandleBack();
  /// Dispatches one phase of immediate or predictive platform Back navigation.
  /// @param event Normalized phase and progress supplied by the platform.
  /// @return Whether the shared Back route consumed this event.
  bool HandleBack(const BackEvent& event);
  /// Dispatches a native IME action only when its session and configured action remain current.
  /// @param session_id Original nonzero text-input session identity.
  /// @param action Native action such as submission, already normalized to HuxerUI semantics.
  /// @return Whether the live text client accepted the action.
  bool PerformTextInputAction(TextInputSessionId session_id, TextInputAction action);
  /// Checks an editing action against the focused text editor or selection client.
  /// @param action Clipboard, selection, or editing action to query.
  /// @return Whether the focused client currently allows it; this query does not mutate text.
  [[nodiscard]] bool CanPerformTextEditingAction(TextEditingAction action) const;
  /// Executes a supported action on the focused editing or selection client.
  /// @param action Normalized editing action requested by a native menu or shortcut.
  /// @return Whether the action was handled; controlled text changes still use the shared editing protocol.
  bool PerformTextEditingAction(TextEditingAction action);
  /// Applies an ordered native text-input command batch to its original active session.
  /// @param batch Session/revision-qualified commands in the text-input contract's UTF-16 coordinates.
  /// @return Accepted/rejected status and synchronization data needed by the native IME endpoint.
  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch);
  /// Queries UTF-16 context from a specific text-input session.
  /// @param session_id Original active text session identity.
  /// @param start First UTF-16 code-unit offset requested by the native endpoint.
  /// @param length Number of UTF-16 code units requested.
  /// @return Context or an explicit mismatch/rejection result. Invalid client-provided results are invariant failures.
  [[nodiscard]] TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const;
  /// Queries caret/range geometry from the active text-input client.
  /// @param session_id Original active text session identity.
  /// @param range Range expressed in UTF-16 code units.
  /// @return Geometry in logical coordinates relative to this host view, or a session/rejection result.
  [[nodiscard]] TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const;
  /// Maps a native host point to the nearest text position in an active session.
  /// @param session_id Original active text session identity.
  /// @param point Logical host-view position.
  /// @return A UTF-16 position result or explicit session/rejection status.
  [[nodiscard]] TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const;
  /// Dispatches a native accessibility action to the committed semantic tree.
  /// @param node_id Identity of the target semantic node in this window.
  /// @param action Typed accessibility action and its payload.
  /// @return Whether the current tree accepted the action; stale nodes do not redirect to a replacement node.
  bool PerformSemanticAction(SemanticNodeId node_id, const SemanticAction& action);

protected:
  UiWindow();
  /// Tests whether this window completed shared initialization and has not begun retirement.
  /// @return False during WindowHook installation, after initialization failure, or after retirement begins.
  /// Native callbacks may enter shared UI operations only while this is true.
  bool IsInitialized() const noexcept;
  /// Installs this native attachment once after its derived facilities are ready.
  /// @param application Already initialized Runtime on whose application thread this window will operate.
  /// @param configuration Optional per-window resource overrides; absence uses application defaults.
  /// @param initial_lifecycle Initial native foreground state, visible to WindowHook before first composition.
  /// Runs window_hooks, installs shared UI services, then admits native input. Failure retires partial UI state.
  /// @throws std::logic_error If initialized twice, the application is not running, or the calling thread differs.
  void InitializeWindow(Runtime& application, std::optional<ResourceConfiguration> configuration = std::nullopt,
                        WindowLifecycleState initial_lifecycle = WindowLifecycleState::Background);
  /// Idempotently tears down the mounted UI before the native attachment releases its facilities.
  /// Runs on the application thread, clears the window identity, retires component work and window services, and leaves
  /// application-owned services and Tasks alive. Native callbacks must stop entering this window as retirement begins.
  void Retire() noexcept;
  /// Creates the file-picker transport anchored to this native attachment during window installation.
  /// @return A transport retaining its native operations safely, or null when this host does not support picking.
  virtual std::shared_ptr<detail::FilePickerTransport> CreateFilePickerTransport();
  /// Borrows the associated application's frozen platform factory catalog.
  /// @return The same catalog used by every attachment of that Runtime; this window does not own a separate registry.
  detail::PlatformRegistry& PlatformRegistry();

private:
  struct State;

  void RequestFrame();
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
  void SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible = std::nullopt,
                      bool reverse = false);
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

  friend class Runtime;
  friend class LayerController;
  friend class detail::ApplicationService;
  friend class detail::FileDropReceiver;
  friend class detail::PointerInteraction;
  friend class detail::RecomposeScope;
  friend class detail::SceneTransitionService;
  friend class detail::SemanticTree;
  friend class detail::TextInteraction;
  friend class detail::VirtualMeasureSession;
  friend struct detail::InternalAccess;
  Runtime* application_runtime_ = nullptr;
  std::shared_ptr<detail::ExternalTextureFrameRequester> external_texture_frame_requester_;
  friend detail::PlatformChannelEndpoint detail::MakePlatformChannelEndpoint(UiWindow& ui_window);
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
