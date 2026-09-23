#include <huxerui/app.h>

#include <stdexcept>
#include <utility>

#include "resources/resource_internal.h"
#include "system_tray_internal.h"
#include "application_internal.h"

namespace huxerui::detail {

std::shared_ptr<SystemTrayService>
SystemTrayService::Create(std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources) {
  return std::shared_ptr<SystemTrayService>(new SystemTrayService(std::move(transport), std::move(resources)));
}

SystemTrayService::SystemTrayService(
    std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources
)
    : transport_(std::move(transport)), resources_(std::move(resources)),
      available_(std::make_shared<StateCell<bool>>(transport_ != nullptr && transport_->IsAvailable())),
      execution_(CurrentExecutionContext()) {
  if (!resources_) {
    throw std::invalid_argument("HuxerUI system tray resource service must not be empty");
  }
}

SystemTrayService::~SystemTrayService() {
  Disconnect();
}

void SystemTrayService::EnsureInitialized() {
  if (initialized_ || !connected_) {
    return;
  }
  initialized_ = true;
  std::weak_ptr<SystemTrayService> service = weak_from_this();
  try {
    if (!transport_) {
      return;
    }
    transport_->SetEventHandler([service](SystemTrayEvent event) {
      if (const auto active = service.lock()) {
        const auto application = active->execution_->application.lock();
        if (application) application->Post([service, event] {
          if (const auto current = service.lock()) {
            ExecutionGuard guard(current->execution_);
            current->HandleEvent(event);
          }
        });
      }
    });
  } catch (...) {
    initialized_ = false;
    throw;
  }
}

bool SystemTrayService::IsAvailable() {
  ValidateApplicationCall(execution_);
  EnsureInitialized();
  ObserveState(available_);
  return available_->value;
}

void SystemTrayService::Show(ImageVariant icon, SystemTrayOptions options) {
  const auto application = execution_->application.lock();
  if (!application) throw std::logic_error("HuxerUI system tray application is disconnected");
  ValidateApplicationCall(execution_);
  ExecutionGuard guard(execution_);
  if (!connected_) {
    return;
  }
  ValidateImageVariant(icon);
  DesiredPresentation desired{
      .icon = std::move(icon),
      .options = std::move(options),
  };
  std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
  std::uint64_t next_command = next_command_;
  ResolvedSystemTrayPresentation presentation = ResolvePresentation(desired, generation_ + 1, next_command, callbacks);
  EnsureInitialized();
  if (transport_ && available_->value) {
    transport_->Show(presentation);
  }
  desired_ = std::move(desired);
  callbacks_ = std::move(callbacks);
  generation_ = presentation.generation;
  next_command_ = next_command;
}

void SystemTrayService::Hide() {
  ValidateApplicationCall(execution_);
  if (!desired_.has_value()) {
    return;
  }
  desired_.reset();
  callbacks_.clear();
  ++generation_;
  if (transport_) {
    transport_->Hide();
  }
}

void SystemTrayService::OnActivate(std::function<void()> handler) {
  ValidateApplicationCall(execution_);
  if (!connected_ || execution_->application.expired()) {
    throw std::logic_error("HuxerUI system tray application is disconnected");
  }
  if (!handler) {
    throw std::invalid_argument("HuxerUI system tray activation handler must not be empty");
  }
  if (activation_handler_) {
    throw std::logic_error("HuxerUI system tray activation handler is already connected");
  }
  EnsureInitialized();
  activation_handler_ = std::move(handler);
}

void SystemTrayService::Disconnect() noexcept {
  if (!connected_) {
    return;
  }
  const bool had_desired_presentation = desired_.has_value();
  const bool was_initialized = initialized_;
  connected_ = false;
  desired_.reset();
  callbacks_.clear();
  activation_handler_ = {};
  initialized_ = false;
  if (available_->value) {
    available_->value = false;
    ++available_->version;
  }
  if (transport_) {
    if (was_initialized) {
      try {
        transport_->SetEventHandler({});
      } catch (...) {
      }
    }
    if (had_desired_presentation) {
      transport_->Hide();
    }
    transport_.reset();
  }
}

void SystemTrayService::HandleEvent(const SystemTrayEvent& event) {
  const auto application = execution_->application.lock();
  if (!connected_ || !application || !application->accepts_tasks) {
    return;
  }
  switch (event.type) {
  case SystemTrayEventType::AvailabilityChanged:
    if (available_->value != event.available) {
      available_->value = event.available;
      ++available_->version;
      NotifyState(available_);
    }
    if (event.available) {
      RefreshPresentation();
    }
    break;
  case SystemTrayEventType::Activate:
    if (available_->value && desired_.has_value() && activation_handler_) {
      auto handler = activation_handler_;
      handler();
    }
    break;
  case SystemTrayEventType::Command:
    if (event.generation == generation_) {
      if (const auto found = callbacks_.find(event.command); found != callbacks_.end()) {
        auto handler = found->second;
        handler();
      }
    }
    break;
  }
}

void SystemTrayService::RefreshPresentation() {
  if (!connected_ || !desired_.has_value()) {
    return;
  }
  std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
  std::uint64_t next_command = next_command_;
  ResolvedSystemTrayPresentation presentation =
      ResolvePresentation(*desired_, generation_ + 1, next_command, callbacks);
  if (transport_ && available_->value) {
    transport_->Show(presentation);
  }
  callbacks_ = std::move(callbacks);
  generation_ = presentation.generation;
  next_command_ = next_command;
}

ResolvedSystemTrayPresentation SystemTrayService::ResolvePresentation(
    const DesiredPresentation& desired,
    std::uint64_t generation,
    std::uint64_t& next_command,
    std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
) {
  const auto configuration = resources_->Configuration(false);
  const auto& locale = configuration.locale;
  ResolvedImageAsset icon = ResolveImage(desired.icon, *resources_, locale, configuration.display_scale);
  if (!std::holds_alternative<ImageAsset>(icon)) {
    throw std::invalid_argument("HuxerUI system tray icon must resolve to an ImageAsset");
  }
  ResolvedSystemTrayPresentation presentation;
  presentation.icon = std::get<ImageAsset>(std::move(icon));
  presentation.tooltip = ResolveString(desired.options.tooltip, *resources_, locale);
  presentation.generation = generation;
  if (!desired.options.menu.empty()) {
    presentation.menu = ResolveMenu(desired.options.menu, configuration, next_command, callbacks);
  }
  return presentation;
}

std::vector<ResolvedSystemTrayMenuEntry> SystemTrayService::ResolveMenu(
    const std::vector<MenuEntry>& entries,
    const ResourceConfiguration& configuration,
    std::uint64_t& next_command,
    std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
) {
  const auto& locale = configuration.locale;
  if (entries.empty()) {
    throw std::invalid_argument("HuxerUI menu must contain at least one item");
  }
  std::vector<ResolvedSystemTrayMenuEntry> result;
  result.reserve(entries.size());
  bool previous_was_section = true;
  for (const MenuEntry& entry : entries) {
    if (std::holds_alternative<MenuSection>(entry.value_)) {
      if (previous_was_section) {
        throw std::invalid_argument("HuxerUI menu section must separate two items");
      }
      ResolvedSystemTrayMenuEntry section;
      section.section = true;
      result.push_back(std::move(section));
      previous_was_section = true;
      continue;
    }

    const MenuItem& item = std::get<MenuItem>(entry.value_);
    ResolvedSystemTrayMenuEntry resolved;
    resolved.label = ResolveString(item.label_, *resources_, locale);
    resolved.enabled = item.enabled_;
    resolved.checked = item.checked_;
    resolved.icon_tint = item.icon_tint_;
    if (resolved.label.empty()) {
      throw std::invalid_argument("HuxerUI menu item label must not be empty");
    }
    if (item.icon_.has_value()) {
      ResolvedImageAsset icon = ResolveImage(*item.icon_, *resources_, locale, configuration.display_scale);
      if (!std::holds_alternative<ImageAsset>(icon)) {
        throw std::invalid_argument("HuxerUI system tray menu icon must resolve to an ImageAsset");
      }
      resolved.icon = std::get<ImageAsset>(std::move(icon));
    }
    if (const auto* action = std::get_if<std::function<void()>>(&item.destination_)) {
      if (!*action) {
        throw std::invalid_argument("HuxerUI menu action item must provide an action");
      }
      resolved.command = next_command++;
      callbacks.emplace(resolved.command, *action);
    } else {
      resolved.children =
          ResolveMenu(std::get<std::vector<MenuEntry>>(item.destination_), configuration, next_command, callbacks);
    }
    result.emplace_back(std::move(resolved));
    previous_was_section = false;
  }
  if (previous_was_section) {
    throw std::invalid_argument("HuxerUI menu section must separate two items");
  }
  return result;
}

} // namespace huxerui::detail

namespace huxerui {

bool SystemTrayHandle::IsAvailable() const {
  return service_->IsAvailable();
}

void SystemTrayHandle::Show(ImageVariant icon, SystemTrayOptions options) const {
  service_->Show(std::move(icon), std::move(options));
}

void SystemTrayHandle::Hide() const {
  service_->Hide();
}

void SystemTrayHandle::OnActivate(std::function<void()> handler) const {
  service_->OnActivate(std::move(handler));
}

} // namespace huxerui
