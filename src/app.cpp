#include <huxerui/app.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "text_layout_internal.h"

namespace huxerui {

namespace {

std::optional<AppDefinition>& AppRegistration() {
  static std::optional<AppDefinition> definition;
  return definition;
}

} // namespace

PlatformAdapter::PlatformAdapter(UIThreadDispatcher dispatch_to_ui_thread)
    : platform_modules_(new PlatformModules(std::move(dispatch_to_ui_thread))) {}

PlatformAdapter::~PlatformAdapter() = default;

std::unique_ptr<detail::TextLayout> PlatformAdapter::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  static_cast<void>(text);
  static_cast<void>(style);
  static_cast<void>(max_width);
  static_cast<void>(options);
  return {};
}

namespace detail {

int RunPlatformApp(AppDefinition definition);
#if defined(__EMSCRIPTEN__)
void EnsureWebPlatformLinked();
#endif

void RegisterAppDefinition(AppDefinition definition) {
#if defined(__EMSCRIPTEN__)
  EnsureWebPlatformLinked();
#endif
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
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
  static_cast<void>(definition);
  throw std::runtime_error("RunApp() is not available on Android or Web");
#else
  return detail::RunPlatformApp(std::move(definition));
#endif
}

} // namespace huxerui
