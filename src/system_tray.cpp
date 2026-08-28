#include <huxerui/app.h>

#include <stdexcept>
#include <utility>

#include "resource_internal.h"
#include "system_tray_internal.h"

namespace huxerui::detail {

std::shared_ptr<SystemTrayService>
SystemTrayService::Create(std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources) {
  return std::shared_ptr<SystemTrayService>(new SystemTrayService(std::move(transport), std::move(resources)));
}

SystemTrayService::SystemTrayService(
    std::shared_ptr<SystemTrayTransport> transport, std::shared_ptr<AppResources> resources
)
    : transport_(std::move(transport)), resources_(std::move(resources)),
      available_(std::make_shared<StateCell<bool>>(transport_ != nullptr && transport_->IsAvailable())) {
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
        active->HandleEvent(event);
      }
    });
  } catch (...) {
    initialized_ = false;
    throw;
  }
}

bool SystemTrayService::IsAvailable() {
  EnsureInitialized();
  ObserveState(available_);
  return available_->value;
}

void SystemTrayService::Show(
    std::uint64_t owner, ImageVariant icon, SystemTrayOptions options, std::shared_ptr<const Environment> environment
) {
  if (!connected_) {
    return;
  }
  if (owner == 0) {
    throw std::logic_error("HuxerUI system tray owner is invalid");
  }
  if (desired_.has_value() && desired_->owner != owner) {
    throw std::logic_error("HuxerUI system tray presentation already has an active owner");
  }
  ValidateImageVariant(icon);
  DesiredPresentation desired{
      .owner = owner,
      .icon = std::move(icon),
      .options = std::move(options),
      .environment = std::move(environment),
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

void SystemTrayService::Hide(std::uint64_t owner) noexcept {
  if (!desired_.has_value() || desired_->owner != owner) {
    return;
  }
  desired_.reset();
  callbacks_.clear();
  ++generation_;
  if (transport_) {
    transport_->Hide();
  }
}

std::function<void()> SystemTrayService::ConnectActivate(std::function<void()> handler) {
  if (!connected_) {
    return {};
  }
  if (!handler) {
    throw std::invalid_argument("HuxerUI system tray activation handler must not be empty");
  }
  if (activation_handler_) {
    throw std::logic_error("HuxerUI system tray activation handler is already connected");
  }
  EnsureInitialized();
  activation_connection_ = next_connection_++;
  activation_handler_ = std::move(handler);
  const std::uint64_t connection = activation_connection_;
  std::weak_ptr<SystemTrayService> service = weak_from_this();
  return [service, connection] {
    if (const auto active = service.lock()) {
      active->DisconnectActivate(connection);
    }
  };
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
  activation_connection_ = 0;
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
  if (!connected_) {
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
      activation_handler_();
    }
    break;
  case SystemTrayEventType::Command:
    if (event.generation == generation_) {
      if (const auto found = callbacks_.find(event.command); found != callbacks_.end()) {
        found->second();
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
  const Locale locale = ResolveResourceLocale(desired.environment, *resources_);
  ResolvedImageAsset icon = ResolveImage(desired.icon, *resources_, locale);
  if (!std::holds_alternative<ImageAsset>(icon)) {
    throw std::invalid_argument("HuxerUI system tray icon must resolve to an ImageAsset");
  }
  ResolvedSystemTrayPresentation presentation;
  presentation.icon = std::get<ImageAsset>(std::move(icon));
  presentation.tooltip = ResolveString(desired.options.tooltip, *resources_, locale);
  presentation.generation = generation;
  if (!desired.options.menu.empty()) {
    presentation.menu = ResolveMenu(desired.options.menu, locale, next_command, callbacks);
  }
  return presentation;
}

std::vector<ResolvedSystemTrayMenuEntry> SystemTrayService::ResolveMenu(
    const std::vector<MenuEntry>& entries,
    const Locale& locale,
    std::uint64_t& next_command,
    std::unordered_map<std::uint64_t, std::function<void()>>& callbacks
) {
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
      ResolvedImageAsset icon = ResolveImage(*item.icon_, *resources_, locale);
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
          ResolveMenu(std::get<std::vector<MenuEntry>>(item.destination_), locale, next_command, callbacks);
    }
    result.emplace_back(std::move(resolved));
    previous_was_section = false;
  }
  if (previous_was_section) {
    throw std::invalid_argument("HuxerUI menu section must separate two items");
  }
  return result;
}

void SystemTrayService::DisconnectActivate(std::uint64_t connection) noexcept {
  if (activation_connection_ != connection) {
    return;
  }
  activation_handler_ = {};
  activation_connection_ = 0;
}

} // namespace huxerui::detail

namespace huxerui {

bool SystemTrayHandle::IsAvailable() const {
  return service_->IsAvailable();
}

void SystemTrayHandle::Show(ImageVariant icon, SystemTrayOptions options) const {
  service_->Show(owner_, std::move(icon), std::move(options), environment_);
}

void SystemTrayHandle::Hide() const {
  service_->Hide(owner_);
}

std::function<void()> SystemTrayHandle::ConnectActivate(std::function<void()> handler) const {
  return service_->ConnectActivate(std::move(handler));
}

} // namespace huxerui
