#include <huxerui/app.h>

#include <algorithm>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "application_internal.h"
#include "runtime_internal.h"
#include "system_tray_internal.h"
#include "task_internal.h"

namespace huxerui::detail {

namespace {

void ValidateApplicationActivation(const ApplicationActivation& activation) {
  const auto* files = std::get_if<FileActivation>(&activation);
  if (files != nullptr && files->files.empty()) {
    throw std::invalid_argument("HuxerUI file activation must contain at least one file");
  }
}

void ValidateApplicationLifecycleState(ApplicationLifecycleState lifecycle_state) {
  switch (lifecycle_state) {
  case ApplicationLifecycleState::Active:
  case ApplicationLifecycleState::Inactive:
  case ApplicationLifecycleState::Background:
    return;
  }
  throw std::invalid_argument("HuxerUI application lifecycle state is invalid");
}

void ValidatePermission(Permission permission) {
  switch (permission) {
  case Permission::Camera:
  case Permission::Microphone:
    return;
  }
  throw std::invalid_argument("HuxerUI permission is invalid");
}

void InvokeCancellation(std::function<void()> cancellation) noexcept {
  if (!cancellation) {
    return;
  }
  try {
    cancellation();
  } catch (...) {
  }
}

} // namespace

struct PermissionController::State final : public std::enable_shared_from_this<State> {
  class Operation {
  public:
    virtual ~Operation() = default;

    virtual void Start(const std::shared_ptr<PermissionTransport>& transport, const std::weak_ptr<State>& controller) = 0;
    virtual void Fail() noexcept = 0;
    virtual std::function<void()> Detach() noexcept = 0;
  };

  State(std::shared_ptr<PermissionTransport> transport, UIThreadDispatcher dispatch_to_ui_thread)
      : transport(std::move(transport)), dispatch_to_ui_thread(std::move(dispatch_to_ui_thread)) {
    if (this->transport && !this->dispatch_to_ui_thread) {
      throw std::logic_error("HuxerUI permission transport requires a UIThreadDispatcher");
    }
  }

  ~State() {
    Disconnect();
  }

  void Submit(const std::shared_ptr<Operation>& operation, bool interactive) {
    if (!connected || !transport) {
      operation->Fail();
      return;
    }
    if (!interactive) {
      try {
        operation->Start(transport, weak_from_this());
      } catch (...) {
        operation->Fail();
      }
      return;
    }
    queued.push_back(operation);
    StartNext();
  }

  void Cancel(const std::shared_ptr<Operation>& operation) noexcept {
    std::function<void()> cancellation = operation->Detach();
    if (active == operation) {
      if (!cancellation) {
        return;
      }
      active.reset();
      InvokeCancellation(std::move(cancellation));
      StartNext();
      return;
    }
    const auto found = std::find(queued.begin(), queued.end(), operation);
    if (found != queued.end()) {
      queued.erase(found);
      return;
    }
    InvokeCancellation(std::move(cancellation));
  }

  void Finish(const std::shared_ptr<Operation>& operation) {
    if (active != operation) {
      return;
    }
    active.reset();
    StartNext();
  }

  void Post(std::function<void()> operation) noexcept {
    try {
      dispatch_to_ui_thread(std::move(operation));
    } catch (...) {
      std::terminate();
    }
  }

  void Disconnect() noexcept {
    if (!connected) {
      return;
    }
    connected = false;
    if (active) {
      InvokeCancellation(active->Detach());
      active.reset();
    }
    for (const std::shared_ptr<Operation>& operation : queued) {
      operation->Detach();
    }
    queued.clear();
    transport.reset();
  }

  void StartNext() {
    while (connected && !active && !queued.empty()) {
      active = std::move(queued.front());
      queued.pop_front();
      try {
        active->Start(transport, weak_from_this());
      } catch (...) {
        active->Fail();
        active.reset();
      }
    }
  }

  std::shared_ptr<PermissionTransport> transport;
  UIThreadDispatcher dispatch_to_ui_thread;
  std::deque<std::shared_ptr<Operation>> queued;
  std::shared_ptr<Operation> active;
  bool connected = true;
};

namespace {

template <class Result>
class PermissionOperation final : public PermissionController::State::Operation,
                                  public std::enable_shared_from_this<PermissionOperation<Result>> {
public:
  using Completion = std::function<void(Result)>;
  using Starter = std::function<std::function<void()>(PermissionTransport&, Completion)>;

  PermissionOperation(Starter starter, Result failure) : starter_(std::move(starter)), failure_(std::move(failure)) {}

  void Suspend(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<> continuation) {
    std::scoped_lock lock(mutex_);
    execution_ = std::move(execution);
    continuation_ = continuation;
  }

  void Start(const std::shared_ptr<PermissionTransport>& transport,
      const std::weak_ptr<PermissionController::State>& controller) override {
    if (!transport) {
      Fail();
      return;
    }
    controller_ = controller;
    std::weak_ptr<PermissionOperation> weak = this->shared_from_this();
    std::function<void()> cancellation = starter_(*transport, [weak, controller](Result result) mutable {
      if (const std::shared_ptr<PermissionController::State> owner = controller.lock()) {
        owner->Post([weak, controller, result = std::move(result)]() mutable {
          const std::shared_ptr<PermissionOperation> operation = weak.lock();
          const std::shared_ptr<PermissionController::State> active_controller = controller.lock();
          if (!operation || !active_controller) {
            return;
          }
          operation->Complete(std::move(result));
          active_controller->Finish(operation);
        });
      }
    });

    bool cancel_started_operation = false;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_) {
        cancel_started_operation = static_cast<bool>(cancellation);
      } else if (!result_.has_value()) {
        cancellation_ = std::move(cancellation);
      }
    }
    if (cancel_started_operation) {
      InvokeCancellation(std::move(cancellation));
      if (const std::shared_ptr<PermissionController::State> owner = controller.lock()) {
        owner->Finish(this->shared_from_this());
      }
    }
  }

  void Fail() noexcept override {
    Complete(std::move(failure_));
  }

  std::function<void()> Detach() noexcept override {
    std::scoped_lock lock(mutex_);
    canceled_ = true;
    execution_.reset();
    continuation_ = {};
    return std::move(cancellation_);
  }

  void Cancel() noexcept {
    if (const std::shared_ptr<PermissionController::State> controller = controller_.lock()) {
      controller->Cancel(this->shared_from_this());
    } else {
      Detach();
    }
  }

  Result TakeResult() {
    std::scoped_lock lock(mutex_);
    if (!result_.has_value()) {
      throw std::logic_error("HuxerUI permission operation resumed without a result");
    }
    return std::move(*result_);
  }

private:
  void Complete(Result result) noexcept {
    std::weak_ptr<TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex_);
      if (canceled_ || result_.has_value()) {
        return;
      }
      result_.emplace(std::move(result));
      cancellation_ = {};
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    ResumeTask(execution, continuation);
  }

  std::mutex mutex_;
  Starter starter_;
  Result failure_;
  std::weak_ptr<PermissionController::State> controller_;
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::function<void()> cancellation_;
  std::optional<Result> result_;
  bool canceled_ = false;
};

template <class Result>
class PermissionAwaiter final {
public:
  using Operation = PermissionOperation<Result>;

  PermissionAwaiter(std::shared_ptr<PermissionController::State> controller, typename Operation::Starter starter,
    Result failure, bool interactive)
      : controller_(std::move(controller)), operation_(std::make_shared<Operation>(std::move(starter), failure)),
        interactive_(interactive) {}

  ~PermissionAwaiter() {
    if (operation_) {
      operation_->Cancel();
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<typename Task<Result>::promise_type> continuation) {
    operation_->Suspend(TaskExecutionFor(continuation), continuation);
    controller_->Submit(operation_, interactive_);
  }

  Result await_resume() {
    Result result = operation_->TakeResult();
    operation_.reset();
    return result;
  }

private:
  std::shared_ptr<PermissionController::State> controller_;
  std::shared_ptr<Operation> operation_;
  bool interactive_ = false;
};

template <class Result>
Task<Result> RunPermissionOperation(std::shared_ptr<PermissionController::State> controller,
    typename PermissionOperation<Result>::Starter starter, Result failure, bool interactive) {
  co_return co_await PermissionAwaiter<Result>(std::move(controller), std::move(starter), std::move(failure), interactive);
}

Task<PermissionStatus> CheckPermission(std::shared_ptr<PermissionController::State> state, Permission permission) {
  return RunPermissionOperation<PermissionStatus>(
      std::move(state),
      [permission](PermissionTransport& transport, PermissionStatusCompletion completion) {
        return transport.Check(permission, std::move(completion));
      },
      PermissionStatus::Unavailable,
      false);
}

Task<PermissionStatus> RequestPermission(std::shared_ptr<PermissionController::State> state, Permission permission) {
  return RunPermissionOperation<PermissionStatus>(
      std::move(state),
      [permission](PermissionTransport& transport, PermissionStatusCompletion completion) {
        return transport.Request(permission, std::move(completion));
      },
      PermissionStatus::Unavailable,
      true);
}

Task<bool> OpenPermissionSettings(std::shared_ptr<PermissionController::State> state, Permission permission) {
  return RunPermissionOperation<bool>(
      std::move(state),
      [permission](PermissionTransport& transport, PermissionSettingsCompletion completion) {
        return transport.OpenSettings(permission, std::move(completion));
      },
      false,
      true);
}

} // namespace

PermissionController::PermissionController(std::shared_ptr<PermissionTransport> transport,
    UIThreadDispatcher dispatch_to_ui_thread)
    : state_(std::make_shared<State>(std::move(transport), std::move(dispatch_to_ui_thread))) {}

PermissionController::~PermissionController() {
  Disconnect();
}

Task<PermissionStatus> PermissionController::Check(Permission permission) const {
  ValidatePermission(permission);
  return CheckPermission(state_, permission);
}

Task<PermissionStatus> PermissionController::Request(Permission permission) const {
  ValidatePermission(permission);
  return RequestPermission(state_, permission);
}

Task<bool> PermissionController::OpenSettings(Permission permission) const {
  ValidatePermission(permission);
  return OpenPermissionSettings(state_, permission);
}

void PermissionController::Disconnect() noexcept {
  state_->Disconnect();
}

ApplicationService::ApplicationService(
    Runtime& runtime,
    ApplicationActivation startup_activation,
    std::shared_ptr<PermissionController> permissions,
    std::shared_ptr<SystemTrayService> system_tray)
    : runtime_(&runtime), startup_activation_(std::move(startup_activation)),
      lifecycle_state_(std::make_shared<StateCell<ApplicationLifecycleState>>(ApplicationLifecycleState::Active)),
      permissions_(std::move(permissions)), system_tray_(std::move(system_tray)) {
  ValidateApplicationActivation(startup_activation_);
  if (!permissions_) {
    throw std::invalid_argument("HuxerUI application permission controller must not be empty");
  }
  if (!system_tray_) {
    throw std::invalid_argument("HuxerUI application system tray service must not be empty");
  }
}

const ApplicationActivation& ApplicationService::StartupActivation() const noexcept {
  return startup_activation_;
}

ApplicationLifecycleState ApplicationService::LifecycleState() const {
  ObserveState(lifecycle_state_);
  return lifecycle_state_->value;
}

std::function<void()> ApplicationService::ConnectActivation(std::function<void(ApplicationActivation)> handler) {
  if (!handler) {
    throw std::invalid_argument("HuxerUI application activation handler must not be empty");
  }
  if (activation_handler_) {
    throw std::logic_error("HuxerUI application activation handler is already connected");
  }
  if (runtime_ == nullptr) {
    throw std::logic_error("HuxerUI application activation handle is disconnected");
  }

  activation_connection_ = next_connection_++;
  activation_handler_ = std::move(handler);
  if (!pending_activations_.empty()) {
    runtime_->RequestFrame();
  }

  const std::uint64_t connection = activation_connection_;
  std::weak_ptr<ApplicationService> service = weak_from_this();
  return [service = std::move(service), connection] {
    if (const std::shared_ptr<ApplicationService> active = service.lock()) {
      active->DisconnectActivationHandler(connection);
    }
  };
}

std::function<void()>
ApplicationService::ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler) {
  if (!handler) {
    throw std::invalid_argument("HuxerUI application lifecycle handler must not be empty");
  }
  if (lifecycle_handler_) {
    throw std::logic_error("HuxerUI application lifecycle handler is already connected");
  }
  if (runtime_ == nullptr) {
    throw std::logic_error("HuxerUI application lifecycle handle is disconnected");
  }

  lifecycle_connection_ = next_connection_++;
  lifecycle_handler_ = std::move(handler);

  const std::uint64_t connection = lifecycle_connection_;
  std::weak_ptr<ApplicationService> service = weak_from_this();
  return [service = std::move(service), connection] {
    if (const std::shared_ptr<ApplicationService> active = service.lock()) {
      active->DisconnectLifecycleHandler(connection);
    }
  };
}

const std::shared_ptr<SystemTrayService>& ApplicationService::SystemTray() const noexcept {
  return system_tray_;
}

Task<PermissionStatus> ApplicationService::CheckPermission(Permission permission) const {
  return permissions_->Check(permission);
}

Task<PermissionStatus> ApplicationService::RequestPermission(Permission permission) const {
  return permissions_->Request(permission);
}

Task<bool> ApplicationService::OpenPermissionSettings(Permission permission) const {
  return permissions_->OpenSettings(permission);
}

void ApplicationService::Quit() const {
  if (runtime_ != nullptr) {
    runtime_->RequestApplicationQuit();
  }
}

void ApplicationService::Enqueue(ApplicationActivation activation) {
  ValidateApplicationActivation(activation);
  if (runtime_ == nullptr) {
    return;
  }
  pending_activations_.push_back(std::move(activation));
  runtime_->RequestFrame();
}

void ApplicationService::UpdateLifecycleState(ApplicationLifecycleState lifecycle_state) {
  ValidateApplicationLifecycleState(lifecycle_state);
  if (runtime_ == nullptr || lifecycle_state_->value == lifecycle_state) {
    return;
  }
  lifecycle_state_->value = lifecycle_state;
  ++lifecycle_state_->version;
  NotifyState(lifecycle_state_);
  if (lifecycle_handler_) {
    pending_lifecycle_states_.push_back(lifecycle_state);
    runtime_->RequestFrame();
  }
}

void ApplicationService::DispatchPending() {
  // Snapshot both channels before invoking application code so reentrant submissions wait for the next frame.
  const std::size_t activation_count = pending_activations_.size();
  const std::size_t lifecycle_count = pending_lifecycle_states_.size();
  if (activation_handler_) {
    for (std::size_t index = 0; index < activation_count; ++index) {
      ApplicationActivation activation = std::move(pending_activations_.front());
      pending_activations_.pop_front();
      activation_handler_(std::move(activation));
    }
  }
  if (lifecycle_handler_) {
    for (std::size_t index = 0; index < lifecycle_count; ++index) {
      const ApplicationLifecycleState lifecycle_state = pending_lifecycle_states_.front();
      pending_lifecycle_states_.pop_front();
      lifecycle_handler_(lifecycle_state);
    }
  }
}

void ApplicationService::Disconnect() noexcept {
  permissions_->Disconnect();
  system_tray_->Disconnect();
  runtime_ = nullptr;
  pending_activations_.clear();
  pending_lifecycle_states_.clear();
  activation_handler_ = {};
  lifecycle_handler_ = {};
  activation_connection_ = 0;
  lifecycle_connection_ = 0;
}

void ApplicationService::DisconnectActivationHandler(std::uint64_t connection) noexcept {
  if (activation_connection_ != connection) {
    return;
  }
  activation_handler_ = {};
  activation_connection_ = 0;
}

void ApplicationService::DisconnectLifecycleHandler(std::uint64_t connection) noexcept {
  if (lifecycle_connection_ != connection) {
    return;
  }
  // Ordered transitions belong to the mounted handler; LifecycleState remains authoritative after it disconnects.
  pending_lifecycle_states_.clear();
  lifecycle_handler_ = {};
  lifecycle_connection_ = 0;
}

} // namespace huxerui::detail

namespace huxerui {

const ApplicationActivation& ApplicationHandle::StartupActivation() const noexcept {
  return service_->StartupActivation();
}

ApplicationLifecycleState ApplicationHandle::LifecycleState() const {
  return service_->LifecycleState();
}

SystemTrayHandle ApplicationHandle::SystemTray() const {
  return SystemTrayHandle{
      service_->SystemTray(),
      detail::CurrentEnvironment(),
      detail::Composer::RequireCurrent().ScopeId(),
  };
}

Task<PermissionStatus> ApplicationHandle::CheckPermissionAsync(Permission permission) const {
  return service_->CheckPermission(permission);
}

Task<PermissionStatus> ApplicationHandle::RequestPermissionAsync(Permission permission) const {
  return service_->RequestPermission(permission);
}

Task<bool> ApplicationHandle::OpenPermissionSettingsAsync(Permission permission) const {
  return service_->OpenPermissionSettings(permission);
}

void ApplicationHandle::Quit() const {
  service_->Quit();
}

std::function<void()>
ApplicationHandle::ConnectActivation(std::function<void(ApplicationActivation)> handler) const {
  return service_->ConnectActivation(std::move(handler));
}

std::function<void()>
ApplicationHandle::ConnectLifecycle(std::function<void(ApplicationLifecycleState)> handler) const {
  return service_->ConnectLifecycle(std::move(handler));
}

ApplicationHandle UseApplication() {
  return ApplicationHandle{UseService<detail::ApplicationService>()};
}

} // namespace huxerui
