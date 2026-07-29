#include <huxerui/app.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "internal.h"

namespace huxerui {

namespace {

std::optional<AppDefinition>& AppRegistration() {
  static std::optional<AppDefinition> definition;
  return definition;
}

} // namespace

namespace detail {

void RegisterAppDefinition(AppDefinition definition) {
  if (definition.root_factory == nullptr) {
    throw std::invalid_argument("HuxerUI application registration requires a root factory");
  }

  auto& registration = AppRegistration();
  if (registration.has_value()) {
    throw std::logic_error("HuxerUI application has already been registered");
  }
  registration.emplace(std::move(definition));
}

const AppDefinition& RegisteredAppDefinition() {
  const auto& registration = AppRegistration();
  if (!registration.has_value()) {
    throw std::logic_error("HuxerUI application has not been registered");
  }
  return *registration;
}

} // namespace detail

int RunApp(AppDefinition definition) {
#if defined(__ANDROID__)
  static_cast<void>(definition);
  throw std::runtime_error("RunApp() is not available on Android; use Runtime with HuxerUIView");
#else
  return detail::RunPlatformApp(std::move(definition));
#endif
}

} // namespace huxerui
