#include <huxerui/app.h>
#include "platform_registry_internal.h"
#include <unordered_map>
#include <thread>
#include <stdexcept>
#include <optional>
#include <mutex>
#include <memory>
#include <map>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <chrono>

#include <cmath>
#include <exception>
#include <utility>

#include <huxerui/http.h>

#include "application_internal.h"
#include "io/http_internal.h"
#include "resources/resource_internal.h"
#include "runtime/task_internal.h"
#include "system_tray_internal.h"
#include "internal_access.h"

namespace huxerui::detail {

namespace {

std::shared_ptr<ApplicationServiceTable> hosted_services;
std::mutex hosted_services_mutex;
thread_local std::shared_ptr<ExecutionContext> execution_context;
std::atomic<std::uint64_t> next_composition_identity{1};

/// Acquires the currently published hosted service table under the process publication lock.
/// @return A retained table or null; callers separately check readiness and original application context.
std::shared_ptr<ApplicationServiceTable> HostedServices() {
  std::lock_guard lock(hosted_services_mutex);
  return hosted_services;
}

}

std::uint64_t NextCompositionIdentity() {
  return next_composition_identity.fetch_add(1, std::memory_order_relaxed);
}

void BindStateLifetime(StateCellBase& cell) {
  const auto application = CurrentApplicationRuntime();
  if (cell.application_bound) {
    if (cell.application.lock() != application) {
      throw std::logic_error("HuxerUI State belongs to another application lifetime");
    }
    return;
  }
  cell.application = application;
  cell.application_bound = true;
}

void ValidateStateLifetime(const StateCellBase& cell) {
  const auto application = cell.application.lock();
  if (!application) {
    throw std::logic_error("HuxerUI State application lifetime has ended");
  }
  application->RequireThread();
  if (application->phase == ApplicationRuntimeState::Phase::Stopped) {
    throw std::logic_error("HuxerUI State application lifetime has ended");
  }
  if (execution_context && execution_context->application.lock() != application) {
    throw std::logic_error("HuxerUI State belongs to another application lifetime");
  }
}

ExecutionGuard::ExecutionGuard(std::shared_ptr<ExecutionContext> source) : previous_(execution_context) {
  if (source) {
    execution_context = std::move(source);
  }
}

ExecutionGuard::~ExecutionGuard() {
  execution_context = std::move(previous_);
}

std::shared_ptr<ExecutionContext> CurrentExecutionContext() {
  return execution_context;
}

void ValidateApplicationCall(const std::shared_ptr<ExecutionContext>& application_source) {
  const auto application = application_source->application.lock();
  if (application) application->RequireThread();
  if (execution_context && execution_context->application.lock() != application) {
    throw std::logic_error("HuxerUI capability belongs to another application lifetime");
  }
}

std::shared_ptr<ExecutionContext> CapturePresentationContext(
    const std::shared_ptr<ExecutionContext>& application_source) {
  ValidateApplicationCall(application_source);
  auto source = std::make_shared<ExecutionContext>(execution_context ? *execution_context : *application_source);
  source->environment.reset();
  return source;
}

bool IsPresentationSourceAvailable(const std::shared_ptr<ExecutionContext>& source) {
  const auto application = source->application.lock();
  if (!application || application->phase == ApplicationRuntimeState::Phase::Stopped) return false;
  application->RequireThread();
  if (!source->requires_ui) return true;
  const auto ui = source->ui.lock();
  return ui && ui->ui_window;
}

std::shared_ptr<ApplicationRuntimeState> CurrentApplicationRuntime() {
  std::shared_ptr<ApplicationRuntimeState> state;
  if (execution_context) {
    state = execution_context->application.lock();
    if (execution_context->requires_ui) {
      const auto ui = execution_context->ui.lock();
      if (!ui || !ui->ui_window) {
        throw std::logic_error("HuxerUI originating UI is disconnected");
      }
    }
  } else if (const auto table = HostedServices(); table && table->ready.load()) {
    state = table->runtime.lock();
  }
  if (!state) {
    throw std::logic_error("HuxerUI operation requires an active application Runtime");
  }
  state->RequireThread();
  if (state->phase == ApplicationRuntimeState::Phase::Stopped) {
    throw std::logic_error("HuxerUI application Runtime is stopped");
  }
  return state;
}

UiWindow* CurrentUiWindow() {
  const auto state = CurrentApplicationRuntime();
  static_cast<void>(state);
  if (!execution_context || !execution_context->requires_ui) {
    return nullptr;
  }
  const auto ui = execution_context->ui.lock();
  return ui ? ui->ui_window : nullptr;
}

void ApplicationServiceTable::Provide(std::type_index type, std::shared_ptr<void> service) {
  if (ready.load()) {
    throw std::logic_error("HuxerUI application services are already frozen");
  }
  if (entries.contains(type)) {
    throw std::logic_error("HuxerUI application service type was provided more than once");
  }
  order.push_back(type);
  try {
    entries.emplace(type, std::move(service));
  } catch (...) {
    order.pop_back();
    throw;
  }
}

std::shared_ptr<void> ApplicationServiceTable::Find(std::type_index type) const {
  std::lock_guard lock(values_mutex);
  const auto found = entries.find(type);
  return found == entries.end() ? nullptr : found->second;
}

void ApplicationServiceTable::Clear() noexcept {
  for (auto slot = order.rbegin(); slot != order.rend(); ++slot) {
    std::shared_ptr<void> released;
    {
      std::lock_guard lock(values_mutex);
      released = std::move(entries.at(*slot));
    }
  }
}

std::shared_ptr<void> FindApplicationService(std::type_index type) {
  if (execution_context) {
    return CurrentApplicationRuntime()->services->Find(type);
  }
  const auto table = HostedServices();
  return table && table->ready.load() ? table->Find(type) : nullptr;
}

void ValidateRootServiceType(std::type_index type) {
  if (FindApplicationService(type)) {
    throw std::logic_error("HuxerUI root service type conflicts with an application service");
  }
}

ApplicationRuntimeState::ApplicationRuntimeState(Runtime& runtime, const Application& application,
                                                 UiThreadDispatcher dispatcher)
    : owner(&runtime), declaration(&application), thread(std::this_thread::get_id()),
      native_dispatcher(std::move(dispatcher)), registry(runtime) {}

void ApplicationRuntimeState::RequireThread() const {
  if (std::this_thread::get_id() != thread) {
    throw std::logic_error("HuxerUI operation must run on the application thread");
  }
}

UiThreadDispatcher ApplicationRuntimeState::Dispatcher() {
  const std::weak_ptr<ApplicationRuntimeState> weak = weak_from_this();
  return [weak](std::function<void()> callback) {
    if (const auto state = weak.lock()) {
      state->Post(std::move(callback));
    }
  };
}

void ApplicationRuntimeState::Post(std::function<void()> callback) {
  if (!callback) {
    throw std::invalid_argument("HuxerUI application callback must not be empty");
  }
  std::unique_lock lock(queue_mutex_);
  if (dispatch_closed_) {
    return;
  }
  const auto callback_index = queue_.size();
  queue_.push_back(std::move(callback));
  if (!dispatch_enabled_ || wake_posted_) {
    return;
  }
  wake_posted_ = true;
  const std::weak_ptr<ApplicationRuntimeState> weak = weak_from_this();
  lock.unlock();
  try {
    native_dispatcher([weak] {
      if (const auto state = weak.lock()) {
        state->Drain();
      }
    });
  } catch (...) {
    lock.lock();
    wake_posted_ = false;
    if (!dispatch_closed_) {
      // A rejected native wake cannot drain the queue; concurrent posts only append.
      callback = std::move(queue_[callback_index]);
      queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(callback_index));
    }
    lock.unlock();
    throw;
  }
}

void ApplicationRuntimeState::StartDispatch() {
  {
    std::lock_guard lock(queue_mutex_);
    dispatch_enabled_ = true;
    if (queue_.empty()) return;
  }
  Post([] {});
}

void ApplicationRuntimeState::CloseDispatch() noexcept {
  std::deque<std::function<void()>> discarded;
  {
    std::lock_guard lock(queue_mutex_);
    dispatch_closed_ = true;
    discarded.swap(queue_);
  }
}

void ApplicationRuntimeState::Drain() {
  RequireThread();
  std::deque<std::function<void()>> pending;
  {
    std::lock_guard lock(queue_mutex_);
    if (dispatch_closed_ || !dispatch_enabled_) {
      return;
    }
    pending.swap(queue_);
  }
  ExecutionGuard guard(execution);
  std::exception_ptr failure;
  for (auto& callback : pending) {
    if (phase == Phase::Stopped) {
      break;
    }
    try {
      callback();
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  bool rearm = false;
  {
    std::lock_guard lock(queue_mutex_);
    wake_posted_ = false;
    rearm = !dispatch_closed_ && !queue_.empty();
  }
  if (rearm) {
    Post([] {});
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}

} // namespace huxerui::detail

namespace huxerui {
using namespace detail;

void Runtime::InitializeApplication(const Application& application,
    std::optional<ApplicationActivation> startup_activation, ApplicationLifecycleState initial_lifecycle) {
  InitializeApplication(application, std::move(startup_activation), initial_lifecycle, true);
}

void Runtime::InitializeApplication(const Application& application,
    std::optional<ApplicationActivation> startup_activation, ApplicationLifecycleState initial_lifecycle, bool hosted) {
  if (state_) throw std::logic_error("HuxerUI application Runtime has already been initialized");
  Runtime& platform = *this;
  state_ = std::make_shared<ApplicationRuntimeState>(*this, application, application_dispatcher_);
  state_->hosted = hosted;
  state_->services->runtime = state_;
  state_->execution = std::make_shared<ExecutionContext>();
  state_->execution->application = state_;
  if (hosted) {
    std::lock_guard lock(hosted_services_mutex);
    if (hosted_services) {
      throw std::logic_error("HuxerUI already has a hosted application Runtime");
    }
    hosted_services = state_->services;
  }
  ExecutionGuard guard(state_->execution);
  try {
    const auto dispatch = state_->Dispatcher();
    state_->tasks = MakeTaskScopeState(dispatch, state_->execution);
    state_->resources = std::make_shared<AppResources>(platform.Resources());
    state_->services->Provide(typeid(AppResources), state_->resources);
    state_->services->Provide(typeid(huxerui::Resources),
        std::shared_ptr<huxerui::Resources>(new huxerui::Resources(state_->resources, state_)));
    state_->http_transport = platform.CreateHttpTransport();
    state_->application_service = std::make_shared<ApplicationService>(
        *this, std::move(startup_activation), initial_lifecycle,
        std::make_shared<PermissionController>(platform.CreatePermissionTransport(), dispatch),
        LocalNotificationService::Create(platform.CreateLocalNotificationTransport(), dispatch, state_->resources),
        SystemTrayService::Create(platform.CreateSystemTrayTransport(), state_->resources), platform.Clipboard(),
        platform.CreateAppDirectories());
    state_->services->Provide(typeid(ApplicationService), state_->application_service);
    state_->services->Provide(typeid(HttpClient), std::shared_ptr<HttpClient>(new HttpClient(state_->http_transport)));
    ApplicationContext context(*state_);
    for (const ApplicationHook& hook : application.options.application_hooks) {
      if (!hook) {
        throw std::invalid_argument("HuxerUI application hook must not be empty");
      }
      hook(context);
    }
    state_->registry.Freeze();
    state_->services->ready.store(true);
    state_->phase = ApplicationRuntimeState::Phase::Running;
    state_->StartDispatch();
  } catch (...) {
    Retire();
    throw;
  }
}

Runtime::~Runtime() { Retire(); CloseTimers(); }

bool Runtime::IsInitialized() const noexcept {
  return state_ && state_->phase == ApplicationRuntimeState::Phase::Running;
}

void Runtime::RequestShutdown() {
  state_->RequireThread();
  if (state_->phase == ApplicationRuntimeState::Phase::Stopping ||
      state_->phase == ApplicationRuntimeState::Phase::Stopped) {
    return;
  }
  if (state_->phase == ApplicationRuntimeState::Phase::Installing) {
    throw std::logic_error("HuxerUI cannot request shutdown during application installation");
  }
  state_->phase = ApplicationRuntimeState::Phase::Stopping;
  state_->accepts_tasks = false;
  const std::weak_ptr<ApplicationRuntimeState> weak = state_;
  state_->Post([weak] {
    if (const auto state = weak.lock(); state && state->owner) {
      state->owner->FinishShutdown();
    }
  });
}

void Runtime::Retire() noexcept {
  if (!state_ || state_->phase == ApplicationRuntimeState::Phase::Stopped) {
    return;
  }
  if (std::this_thread::get_id() != state_->thread) {
    std::terminate();
  }
  ExecutionGuard guard(state_->execution);
  state_->phase = ApplicationRuntimeState::Phase::Stopping;
  state_->accepts_tasks = false;
  for (const auto& weak : state_->uis) {
    if (const auto ui = weak.lock(); ui && ui->ui_window) {
      ui->ui_window->Retire();
    }
  }
  CloseTaskScope(state_->tasks);
  DisconnectHttpOperations(*state_);
  for (auto cleanup = state_->handler_cleanups.rbegin(); cleanup != state_->handler_cleanups.rend(); ++cleanup) {
    (*cleanup)();
  }
  state_->handler_cleanups.clear();
  if (state_->application_service) {
    state_->application_service->Disconnect();
  }
  if (state_->resources) {
    state_->resources->Disconnect();
  }
  state_->owner->CloseTimers();
  state_->CloseDispatch();
  if (state_->hosted) {
    std::lock_guard lock(hosted_services_mutex);
    if (hosted_services == state_->services) hosted_services.reset();
  }
  state_->services->Clear();
  state_->http_transport.reset();
  state_->application_service.reset();
  state_->resources.reset();
  state_->owner = nullptr;
  state_->phase = ApplicationRuntimeState::Phase::Stopped;
}

void Runtime::FinishShutdown() {
  Runtime* platform = state_->owner;
  Retire();
  platform->OnRuntimeStopped();
}

void Runtime::RequestEventDispatch() {
  if (state_->event_dispatch_pending || state_->phase == ApplicationRuntimeState::Phase::Stopped) {
    return;
  }
  state_->event_dispatch_pending = true;
  const std::weak_ptr<ApplicationRuntimeState> weak = state_;
  state_->Post([weak] {
    if (const auto state = weak.lock(); state && state->application_service) {
      state->event_dispatch_pending = false;
      state->application_service->DispatchPending();
    }
  });
}

void Runtime::HandleApplicationActivation(ApplicationActivation activation) {
  state_->RequireThread();
  if (state_->phase == ApplicationRuntimeState::Phase::Running) {
    state_->application_service->Enqueue(std::move(activation));
  }
}

void Runtime::UpdateApplicationLifecycleState(ApplicationLifecycleState lifecycle) {
  state_->RequireThread();
  if (state_->phase == ApplicationRuntimeState::Phase::Running) {
    state_->application_service->UpdateLifecycleState(lifecycle);
  }
}

void Runtime::UpdateResourceConfiguration(ResourceConfiguration configuration) {
  state_->RequireThread();
  ExecutionGuard guard(state_->execution);
  if (state_->resources) {
    state_->resources->UpdateConfiguration(std::move(configuration));
    state_->application_service->SystemTray()->RefreshPresentation();
  }
}

ApplicationContext::ApplicationContext(detail::ApplicationRuntimeState& state)
    : state_(&state), registry_(&state.registry) {}

void ApplicationContext::ProvideService(std::type_index type, std::shared_ptr<void> service) {
  state_->RequireThread();
  state_->services->Provide(type, std::move(service));
}

void ApplicationContext::OnActivation(std::function<void(ApplicationActivation)> handler) {
  state_->handler_cleanups.push_back(state_->application_service->ConnectActivation(std::move(handler)));
}

void ApplicationContext::OnLifecycleChanged(std::function<void(ApplicationLifecycleState)> handler) {
  state_->handler_cleanups.push_back(state_->application_service->ConnectLifecycle(std::move(handler)));
}

TaskScope UseApplicationTaskScope() {
  const auto state = CurrentApplicationRuntime();
  if (state->phase == ApplicationRuntimeState::Phase::Stopping) {
    throw std::logic_error("HuxerUI application is stopping");
  }
  return TaskScope(state->tasks);
}

void detail::InternalAccess::InitializeRuntime(Runtime& runtime, const Application& application,
    std::optional<ApplicationActivation> startup_activation, bool hosted, ApplicationLifecycleState lifecycle) {
  runtime.InitializeApplication(application, std::move(startup_activation), lifecycle, hosted);
}

void detail::InternalAccess::RetireRuntime(Runtime& runtime) noexcept { runtime.Retire(); }

} // namespace huxerui

namespace huxerui::detail {

/// Ordinary application timers with one lazy worker for deadlines and application-thread callback delivery.
/// The worker never touches UI or State. Queued native wakes retain only a weak queue identity; closing/canceling
/// removes
/// the callback before delivery, and capture destruction occurs outside the queue lock.
class ApplicationTimerQueue final : public std::enable_shared_from_this<ApplicationTimerQueue> {
public:
  explicit ApplicationTimerQueue(UiThreadDispatcher dispatcher)
      : dispatcher_(std::move(dispatcher)), owner_thread_(std::this_thread::get_id()) {}

  ~ApplicationTimerQueue() {
    Close();
  }

  /// Registers a one-shot timer on the queue's owning application thread.
  /// @param deadline Absolute steady-clock deadline.
  /// @param callback Nonempty callback delivered on the application thread after the deadline, never on the timer
  /// worker.
  /// @return An idempotent cancellation function requiring the same owning thread.
  /// @throws std::invalid_argument If callback is empty; std::logic_error for a closed queue, wrong thread, or Web
  /// default.
  std::function<void()> Schedule(std::chrono::steady_clock::time_point deadline, std::function<void()> callback) {
    if (!callback) {
      throw std::invalid_argument("HuxerUI application timer callback must not be empty");
    }
    if (std::this_thread::get_id() != owner_thread_) {
      throw std::logic_error("HuxerUI application timers must be scheduled on their owning thread");
    }
#if defined(__EMSCRIPTEN__)
    static_cast<void>(deadline);
    throw std::logic_error("HuxerUI Web Runtime must provide its native timer implementation");
#else
    const std::weak_ptr<ApplicationTimerQueue> weak = weak_from_this();
    std::unique_lock lock(mutex_);
    if (closed_) {
      throw std::logic_error("HuxerUI application timers are closed");
    }
    if (!worker_.joinable()) {
      worker_ = std::thread([this] { Run(); });
    }
    const std::uint64_t identity = next_identity_++;
    const auto position = deadlines_.emplace(deadline, identity);
    try {
      callbacks_.emplace(identity, Entry{position, std::move(callback)});
    } catch (...) {
      deadlines_.erase(position);
      throw;
    }
    lock.unlock();
    changed_.notify_one();
    return [weak, identity] {
      if (const auto queue = weak.lock()) {
        queue->Cancel(identity);
      }
    };
#endif
  }

  /// Closes timer admission, drops pending callbacks, and joins the deadline worker once.
  /// Must run on the owning application thread; any already posted weak delivery sees a closed queue.
  void Close() noexcept {
    std::unordered_map<std::uint64_t, Entry> discarded;
    {
      std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
      }
      closed_ = true;
      deadlines_.clear();
      discarded.swap(callbacks_);
    }
    changed_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  using Deadlines = std::multimap<std::chrono::steady_clock::time_point, std::uint64_t>;
  /// One pending callback and its optional position in the deadline index.
  /// An empty position means its deadline fired and native delivery is queued, but cancellation can still remove it.
  struct Entry {
    std::optional<Deadlines::iterator> position;
    std::function<void()> callback;
  };

  /// Suppresses a pending timer callback on the owning application thread.
  /// @param identity Timer identity returned internally by Schedule; missing or already delivered identities are
  /// ignored.
  void Cancel(std::uint64_t identity) noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
      std::terminate();
    }
    std::function<void()> discarded;
    {
      std::lock_guard lock(mutex_);
      const auto found = callbacks_.find(identity);
      if (found == callbacks_.end()) {
        return;
      }
      if (found->second.position) {
        deadlines_.erase(*found->second.position);
      }
      discarded = std::move(found->second.callback);
      callbacks_.erase(found);
    }
    changed_.notify_one();
  }

  /// Invokes a still-pending callback after its native application-thread wake arrives.
  /// @param identity Scheduled timer identity; closed, canceled, or already delivered entries are ignored.
  /// Removes the entry before invoking user code and holds no queue lock while the callback runs.
  void Deliver(std::uint64_t identity) {
    std::function<void()> callback;
    {
      std::lock_guard lock(mutex_);
      const auto found = callbacks_.find(identity);
      if (closed_ || found == callbacks_.end()) {
        return;
      }
      callback = std::move(found->second.callback);
      callbacks_.erase(found);
    }
    callback();
  }

  /// Runs the deadline worker until closure, posting expired identities to the native application dispatcher.
  /// Does not invoke application callbacks, retain native window pointers, or promise recovery from a posting failure.
  void Run() noexcept {
    std::unique_lock lock(mutex_);
    while (!closed_) {
      if (deadlines_.empty()) {
        changed_.wait(lock, [this] { return closed_ || !deadlines_.empty(); });
        continue;
      }
      const auto deadline = deadlines_.begin()->first;
      if (std::chrono::steady_clock::now() < deadline) {
        changed_.wait_until(lock, deadline);
        continue;
      }
      const std::uint64_t identity = deadlines_.begin()->second;
      deadlines_.erase(deadlines_.begin());
      callbacks_.at(identity).position.reset();
      const std::weak_ptr<ApplicationTimerQueue> weak = weak_from_this();
      lock.unlock();
      dispatcher_([weak, identity] {
        if (const auto queue = weak.lock()) {
          queue->Deliver(identity);
        }
      });
      lock.lock();
    }
  }

  UiThreadDispatcher dispatcher_;
  std::thread::id owner_thread_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  Deadlines deadlines_;
  std::unordered_map<std::uint64_t, Entry> callbacks_;
  std::uint64_t next_identity_ = 1;
  bool closed_ = false;
};

PlatformChannelEndpoint MakePlatformChannelEndpoint(Runtime& runtime) {
  if (!runtime.state_) throw std::logic_error("HuxerUI PlatformChannel requires an initialized Runtime");
  runtime.state_->RequireThread();
  return MakePlatformChannelEndpoint(runtime.application_dispatcher_, runtime.state_->execution);
}

} // namespace huxerui::detail

namespace huxerui {

Runtime::Runtime(UiThreadDispatcher dispatcher) : application_dispatcher_(std::move(dispatcher)) {
  if (!application_dispatcher_) {
    throw std::invalid_argument("HuxerUI Runtime requires a queued dispatcher");
  }
  timers_ = std::make_shared<detail::ApplicationTimerQueue>(application_dispatcher_);
}

void Runtime::CloseTimers() noexcept {
  if (timers_) {
    timers_->Close();
  }
}

void Runtime::DispatchToApplicationThread(std::function<void()> task) const {
  if (!task) {
    throw std::invalid_argument("HuxerUI application-thread task must not be empty");
  }
  application_dispatcher_(std::move(task));
}

std::function<void()> Runtime::ScheduleTimerAt(std::chrono::steady_clock::time_point deadline,
                                                       std::function<void()> callback) {
  return timers_->Schedule(deadline, std::move(callback));
}

std::optional<AppDirectories> Runtime::CreateAppDirectories() { return {}; }
std::shared_ptr<detail::HttpTransport> Runtime::CreateHttpTransport() { return {}; }
std::shared_ptr<detail::LocalNotificationTransport> Runtime::CreateLocalNotificationTransport() { return {}; }
std::shared_ptr<detail::PermissionTransport> Runtime::CreatePermissionTransport() { return {}; }
std::shared_ptr<detail::SystemTrayTransport> Runtime::CreateSystemTrayTransport() { return {}; }

} // namespace huxerui
