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

} // namespace

ApplicationService::ApplicationService(Runtime& runtime, ApplicationActivation startup_activation)
    : runtime_(&runtime), startup_activation_(std::move(startup_activation)) {
  ValidateApplicationActivation(startup_activation_);
}

const ApplicationActivation& ApplicationService::StartupActivation() const noexcept {
  return startup_activation_;
}

std::function<void()> ApplicationService::Connect(std::function<void(ApplicationActivation)> handler) {
  if (!handler) {
    throw std::invalid_argument("HuxerUI application activation handler must not be empty");
  }
  if (handler_) {
    throw std::logic_error("HuxerUI application activation handler is already connected");
  }
  if (runtime_ == nullptr) {
    throw std::logic_error("HuxerUI application activation handle is disconnected");
  }

  connection_ = next_connection_++;
  handler_ = std::move(handler);
  if (!pending_activations_.empty()) {
    runtime_->RequestFrame();
  }

  const std::uint64_t connection = connection_;
  std::weak_ptr<ApplicationService> service = weak_from_this();
  return [service = std::move(service), connection] {
    if (const std::shared_ptr<ApplicationService> active = service.lock()) {
      active->DisconnectHandler(connection);
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

void ApplicationService::DispatchPending() {
  if (!handler_) {
    return;
  }

  const std::size_t pending_count = pending_activations_.size();
  for (std::size_t index = 0; index < pending_count; ++index) {
    ApplicationActivation activation = std::move(pending_activations_.front());
    pending_activations_.pop_front();
    handler_(std::move(activation));
  }
}

void ApplicationService::Disconnect() noexcept {
  runtime_ = nullptr;
  pending_activations_.clear();
  handler_ = {};
  connection_ = 0;
}

void ApplicationService::DisconnectHandler(std::uint64_t connection) noexcept {
  if (connection_ != connection) {
    return;
  }
  handler_ = {};
  connection_ = 0;
}

} // namespace huxerui::detail

namespace huxerui {

const ApplicationActivation& ApplicationHandle::StartupActivation() const noexcept {
  return service_->StartupActivation();
}

std::function<void()> ApplicationHandle::Connect(std::function<void(ApplicationActivation)> handler) const {
  return service_->Connect(std::move(handler));
}

ApplicationHandle UseApplication() {
  return ApplicationHandle{UseService<detail::ApplicationService>()};
}

} // namespace huxerui
