#include <huxerui/system.h>

#include <algorithm>
#include <coroutine>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "application_internal.h"
#include "platform_registry_internal.h"
#include "resources/resource_internal.h"
#include "runtime/task_internal.h"

namespace huxerui::detail {

namespace {

void InvokeCancellation(std::function<void()> cancellation) noexcept {
  if (!cancellation) {
    return;
  }
  try {
    cancellation();
  } catch (...) {
  }
}

void ValidateIdentifier(std::string_view identifier) {
  if (identifier.empty() || identifier.find('\0') != std::string_view::npos || !IsValidUtf8(identifier)) {
    throw std::invalid_argument(
        "HuxerUI local notification identifier must contain non-empty valid UTF-8 without null characters");
  }
}

void ValidateTemplateIdentifier(std::string_view identifier) {
  if (identifier.empty() || identifier.find('\0') != std::string_view::npos || !IsValidUtf8(identifier)) {
    throw std::invalid_argument(
        "HuxerUI local notification template identifier must contain non-empty valid UTF-8 without null characters");
  }
}

void ValidateResolvedText(std::string_view text, std::string_view field) {
  if (text.find('\0') != std::string_view::npos || !IsValidUtf8(text)) {
    throw std::invalid_argument("HuxerUI local notification " + std::string(field) +
                                " must resolve to valid UTF-8 without null characters");
  }
}

} // namespace

struct LocalNotificationService::State final : public std::enable_shared_from_this<State> {
  class Operation {
  public:
    virtual ~Operation() = default;

    virtual void Start(const std::shared_ptr<LocalNotificationTransport>& transport,
                       const std::weak_ptr<State>& controller) = 0;
    virtual void Fail() noexcept = 0;
    virtual std::function<void()> Detach() noexcept = 0;
  };

  State(std::shared_ptr<LocalNotificationTransport> transport, UIThreadDispatcher dispatch_to_ui_thread)
      : transport(std::move(transport)), dispatch_to_ui_thread(std::move(dispatch_to_ui_thread)) {
    if (this->transport && !this->dispatch_to_ui_thread) {
      throw std::logic_error("HuxerUI local notification transport requires a UIThreadDispatcher");
    }
  }

  ~State() {
    Disconnect();
  }

  LocalNotificationCapabilities Capabilities() const noexcept {
    return connected && transport ? transport->Capabilities() : LocalNotificationCapabilities{};
  }

  void Submit(const std::shared_ptr<Operation>& operation, bool ordered) {
    if (!connected || !transport) {
      operation->Fail();
      return;
    }
    // Queries do not block behind prompts; mutations retain their order within this Runtime.
    if (ordered) {
      queued.push_back(operation);
      StartNext();
      return;
    }

    independent.push_back(operation);
    try {
      operation->Start(transport, weak_from_this());
    } catch (...) {
      operation->Fail();
      Finish(operation);
    }
  }

  void Cancel(const std::shared_ptr<Operation>& operation) noexcept {
    std::function<void()> cancellation = operation->Detach();
    if (active == operation) {
      if (!cancellation) {
        // Detaching a Task cannot dismiss a native prompt. Its completion must still release the queue slot.
        return;
      }
      active.reset();
      InvokeCancellation(std::move(cancellation));
      StartNext();
      return;
    }

    const auto queued_operation = std::find(queued.begin(), queued.end(), operation);
    if (queued_operation != queued.end()) {
      queued.erase(queued_operation);
      return;
    }

    const auto independent_operation = std::find(independent.begin(), independent.end(), operation);
    if (independent_operation != independent.end()) {
      independent.erase(independent_operation);
    }
    InvokeCancellation(std::move(cancellation));
  }

  void Finish(const std::shared_ptr<Operation>& operation) {
    if (active == operation) {
      active.reset();
      StartNext();
      return;
    }
    const auto found = std::find(independent.begin(), independent.end(), operation);
    if (found != independent.end()) {
      independent.erase(found);
    }
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
    // Detach application continuations only; accepted notifications and alarms belong to the operating system.
    if (active) {
      InvokeCancellation(active->Detach());
      active.reset();
    }
    for (const std::shared_ptr<Operation>& operation : queued) {
      operation->Detach();
    }
    queued.clear();
    for (const std::shared_ptr<Operation>& operation : independent) {
      InvokeCancellation(operation->Detach());
    }
    independent.clear();
    transport.reset();
  }

  void StartNext() {
    while (connected && !active && !queued.empty()) {
      active = std::move(queued.front());
      queued.pop_front();
      const std::shared_ptr<Operation> operation = active;
      try {
        operation->Start(transport, weak_from_this());
      } catch (...) {
        operation->Fail();
        if (active == operation) {
          active.reset();
        }
      }
    }
  }

  std::shared_ptr<LocalNotificationTransport> transport;
  UIThreadDispatcher dispatch_to_ui_thread;
  std::deque<std::shared_ptr<Operation>> queued;
  std::vector<std::shared_ptr<Operation>> independent;
  std::shared_ptr<Operation> active;
  bool connected = true;
};

namespace {

template <class Result>
class LocalNotificationOperation final : public LocalNotificationService::State::Operation,
                                         public std::enable_shared_from_this<LocalNotificationOperation<Result>> {
public:
  using Completion = std::function<void(Result)>;
  using Starter = std::function<std::function<void()>(LocalNotificationTransport&, Completion)>;

  LocalNotificationOperation(Starter starter, Result failure)
      : starter_(std::move(starter)), failure_(std::move(failure)) {}

  void Suspend(std::weak_ptr<TaskExecution> execution, std::coroutine_handle<> continuation) {
    std::scoped_lock lock(mutex_);
    execution_ = std::move(execution);
    continuation_ = continuation;
  }

  void Start(const std::shared_ptr<LocalNotificationTransport>& transport,
             const std::weak_ptr<LocalNotificationService::State>& controller) override {
    if (!transport) {
      Fail();
      return;
    }
    std::weak_ptr<LocalNotificationOperation> weak = this->shared_from_this();
    // Native completions may be synchronous or arrive on a worker; queue advancement stays on the UI thread.
    std::function<void()> cancellation = starter_(*transport, [weak, controller](Result result) mutable {
      if (const std::shared_ptr<LocalNotificationService::State> owner = controller.lock()) {
        owner->Post([weak, controller, result = std::move(result)]() mutable {
          const std::shared_ptr<LocalNotificationOperation> operation = weak.lock();
          const std::shared_ptr<LocalNotificationService::State> active_controller = controller.lock();
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
      if (const std::shared_ptr<LocalNotificationService::State> owner = controller.lock()) {
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

  Result TakeResult() {
    std::scoped_lock lock(mutex_);
    if (!result_.has_value()) {
      throw std::logic_error("HuxerUI local notification operation resumed without a result");
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
  std::weak_ptr<TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  std::function<void()> cancellation_;
  std::optional<Result> result_;
  bool canceled_ = false;
};

template <class Result> class LocalNotificationAwaiter final {
public:
  using Operation = LocalNotificationOperation<Result>;

  LocalNotificationAwaiter(std::shared_ptr<LocalNotificationService::State> controller,
                           typename Operation::Starter starter, Result failure, bool ordered)
      : controller_(std::move(controller)), operation_(std::make_shared<Operation>(std::move(starter), failure)),
        ordered_(ordered) {}

  ~LocalNotificationAwaiter() {
    if (operation_) {
      // The awaiter owns the queue before Start runs, so cancellation also removes operations still waiting in it.
      controller_->Cancel(operation_);
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<typename Task<Result>::promise_type> continuation) {
    operation_->Suspend(TaskExecutionFor(continuation), continuation);
    controller_->Submit(operation_, ordered_);
  }

  Result await_resume() {
    Result result = operation_->TakeResult();
    operation_.reset();
    return result;
  }

private:
  std::shared_ptr<LocalNotificationService::State> controller_;
  std::shared_ptr<Operation> operation_;
  bool ordered_ = false;
};

template <class Result>
Task<Result> RunLocalNotificationOperation(std::shared_ptr<LocalNotificationService::State> controller,
                                           typename LocalNotificationOperation<Result>::Starter starter, Result failure,
                                           bool ordered) {
  co_return co_await LocalNotificationAwaiter<Result>(std::move(controller), std::move(starter), std::move(failure),
                                                      ordered);
}

Task<PermissionStatus> CheckAuthorization(std::shared_ptr<LocalNotificationService::State> state) {
  return RunLocalNotificationOperation<PermissionStatus>(
      std::move(state),
      [](LocalNotificationTransport& transport, PermissionStatusCompletion completion) {
        return transport.CheckAuthorization(std::move(completion));
      },
      PermissionStatus::Unavailable, false);
}

Task<PermissionStatus> RequestAuthorization(std::shared_ptr<LocalNotificationService::State> state) {
  return RunLocalNotificationOperation<PermissionStatus>(
      std::move(state),
      [](LocalNotificationTransport& transport, PermissionStatusCompletion completion) {
        return transport.RequestAuthorization(std::move(completion));
      },
      PermissionStatus::Unavailable, true);
}

Task<LocalNotificationOperationStatus> ShowNotification(std::shared_ptr<LocalNotificationService::State> state,
                                                        ResolvedLocalNotification notification) {
  return RunLocalNotificationOperation<LocalNotificationOperationStatus>(
      std::move(state),
      [notification = std::move(notification)](LocalNotificationTransport& transport,
                                               LocalNotificationOperationCompletion completion) mutable {
        return transport.Show(std::move(notification), std::move(completion));
      },
      LocalNotificationOperationStatus::Unavailable, true);
}

Task<LocalNotificationOperationStatus> ScheduleNotification(std::shared_ptr<LocalNotificationService::State> state,
                                                            ResolvedLocalNotification notification,
                                                            std::chrono::system_clock::time_point delivery_time) {
  return RunLocalNotificationOperation<LocalNotificationOperationStatus>(
      std::move(state),
      [notification = std::move(notification), delivery_time](LocalNotificationTransport& transport,
                                                              LocalNotificationOperationCompletion completion) mutable {
        return transport.Schedule(std::move(notification), delivery_time, std::move(completion));
      },
      LocalNotificationOperationStatus::Unavailable, true);
}

Task<LocalNotificationOperationStatus> CancelNotification(std::shared_ptr<LocalNotificationService::State> state,
                                                          std::string identifier) {
  return RunLocalNotificationOperation<LocalNotificationOperationStatus>(
      std::move(state),
      [identifier = std::move(identifier)](LocalNotificationTransport& transport,
                                           LocalNotificationOperationCompletion completion) mutable {
        return transport.Cancel(std::move(identifier), std::move(completion));
      },
      LocalNotificationOperationStatus::Unavailable, true);
}

} // namespace

std::shared_ptr<LocalNotificationService>
LocalNotificationService::Create(std::shared_ptr<LocalNotificationTransport> transport,
                                 UIThreadDispatcher dispatch_to_ui_thread, std::shared_ptr<AppResources> resources) {
  return std::shared_ptr<LocalNotificationService>(
      new LocalNotificationService(std::move(transport), std::move(dispatch_to_ui_thread), std::move(resources)));
}

LocalNotificationService::LocalNotificationService(std::shared_ptr<LocalNotificationTransport> transport,
                                                   UIThreadDispatcher dispatch_to_ui_thread,
                                                   std::shared_ptr<AppResources> resources)
    : state_(std::make_shared<State>(std::move(transport), std::move(dispatch_to_ui_thread))),
      resources_(std::move(resources)) {
  if (!resources_) {
    throw std::invalid_argument("HuxerUI local notification service requires application resources");
  }
}

LocalNotificationService::~LocalNotificationService() {
  Disconnect();
}

LocalNotificationCapabilities LocalNotificationService::Capabilities() const {
  return state_->Capabilities();
}

Task<PermissionStatus> LocalNotificationService::CheckAuthorization() const {
  return detail::CheckAuthorization(state_);
}

Task<PermissionStatus> LocalNotificationService::RequestAuthorization() const {
  return detail::RequestAuthorization(state_);
}

Task<LocalNotificationOperationStatus>
LocalNotificationService::Show(LocalNotification notification, std::shared_ptr<const Environment> environment) const {
  // Resolve and encode before creating the lazy Task, so later locale or caller changes cannot alter its content.
  return ShowNotification(state_, Resolve(std::move(notification), std::move(environment)));
}

Task<LocalNotificationOperationStatus>
LocalNotificationService::Schedule(LocalNotification notification, std::chrono::system_clock::time_point delivery_time,
                                   std::shared_ptr<const Environment> environment) const {
  if (delivery_time <= std::chrono::system_clock::now()) {
    throw std::invalid_argument("HuxerUI local notification delivery time must be in the future");
  }
  return ScheduleNotification(state_, Resolve(std::move(notification), std::move(environment)), delivery_time);
}

Task<LocalNotificationOperationStatus> LocalNotificationService::Cancel(std::string_view identifier) const {
  ValidateIdentifier(identifier);
  return CancelNotification(state_, std::string(identifier));
}

void LocalNotificationService::Disconnect() noexcept {
  state_->Disconnect();
}

Bytes EncodeLocalNotificationData(const PlatformPayload& data) {
  PlatformPayload::Envelope envelope = data.Encode();
  if (!envelope.external_textures.empty() || !envelope.file_references.empty() || !envelope.buffer_references.empty()) {
    throw std::invalid_argument("HuxerUI local notification data cannot contain retained capabilities");
  }
  if (envelope.bytes.size() > max_local_notification_data_bytes) {
    throw std::invalid_argument("HuxerUI local notification data exceeds the 64 KiB encoded size limit");
  }
  return std::move(envelope.bytes);
}

PlatformPayload DecodeLocalNotificationData(std::span<const std::byte> bytes) {
  if (bytes.size() > max_local_notification_data_bytes) {
    throw std::invalid_argument("HuxerUI local notification data exceeds the 64 KiB encoded size limit");
  }
  // Empty capability tables make nested resource references invalid during decoding as well.
  return PlatformPayload::Decode({
      .bytes = Bytes(bytes.begin(), bytes.end()),
      .external_textures = {}, .file_references = {}, .buffer_references = {},
  });
}

ResolvedLocalNotification LocalNotificationService::Resolve(LocalNotification notification,
                                                            std::shared_ptr<const Environment> environment) const {
  ValidateIdentifier(notification.identifier);
  if (const auto* template_presentation = std::get_if<TemplateNotificationPresentation>(&notification.presentation)) {
    ValidateTemplateIdentifier(template_presentation->identifier);
  }
  const Locale locale = ResolveResourceLocale(std::move(environment), *resources_);
  ResolvedLocalNotification resolved{
      .identifier = std::move(notification.identifier),
      .title = ResolveString(std::move(notification.title), *resources_, locale),
      .body = ResolveString(std::move(notification.body), *resources_, locale),
      .presentation = std::move(notification.presentation),
      .data = EncodeLocalNotificationData(notification.data),
  };
  ValidateResolvedText(resolved.title, "title");
  ValidateResolvedText(resolved.body, "body");
  if (resolved.title.empty() && resolved.body.empty()) {
    throw std::invalid_argument("HuxerUI local notification title and body must not both be empty");
  }
  return resolved;
}

} // namespace huxerui::detail

namespace huxerui {

LocalNotificationCapabilities LocalNotificationHandle::Capabilities() const {
  return service_->Capabilities();
}

Task<PermissionStatus> LocalNotificationHandle::CheckAuthorizationAsync() const {
  return service_->CheckAuthorization();
}

Task<PermissionStatus> LocalNotificationHandle::RequestAuthorizationAsync() const {
  return service_->RequestAuthorization();
}

Task<LocalNotificationOperationStatus> LocalNotificationHandle::ShowAsync(LocalNotification notification) const {
  return service_->Show(std::move(notification), environment_);
}

Task<LocalNotificationOperationStatus>
LocalNotificationHandle::ScheduleAsync(LocalNotification notification,
                                      std::chrono::system_clock::time_point delivery_time) const {
  return service_->Schedule(std::move(notification), delivery_time, environment_);
}

Task<LocalNotificationOperationStatus> LocalNotificationHandle::CancelAsync(std::string_view identifier) const {
  return service_->Cancel(identifier);
}

} // namespace huxerui
