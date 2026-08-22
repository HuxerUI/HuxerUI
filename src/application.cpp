#include <huxerui/app.h>

#include <stdexcept>
#include <type_traits>
#include <utility>

#include "application_internal.h"

namespace huxerui::detail {

namespace {

void ValidateApplicationActivation(const ApplicationActivation& activation) {
  std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, LaunchActivation>) {
          return;
        } else if constexpr (std::is_same_v<Value, UrlActivation>) {
          if (!value.url.empty()) {
            return;
          }
        } else {
          if (!value.files.empty()) {
            return;
          }
        }
        throw std::invalid_argument("HuxerUI application activation must contain a URL or at least one file");
      },
      activation
  );
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

} // namespace

ApplicationService::ApplicationService(Runtime& runtime, ApplicationActivation startup_activation)
    : runtime_(&runtime), startup_activation_(std::move(startup_activation)),
      lifecycle_state_(std::make_shared<StateCell<ApplicationLifecycleState>>(ApplicationLifecycleState::Active)) {
  ValidateApplicationActivation(startup_activation_);
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
