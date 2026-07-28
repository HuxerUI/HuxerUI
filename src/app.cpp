#include <huxerui/app.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "internal.h"

namespace huxerui {

namespace {

std::optional<AppDefinition> &AppRegistration() {
  static std::optional<AppDefinition> definition;
  return definition;
}

} // namespace

struct AppRuntime::Impl {
  class HostAdapter final : public detail::PlatformHost, public detail::TextService {
  public:
    explicit HostAdapter(AppHost &host) : host_(&host) {}

    int Run(detail::Runtime &, const AppOptions &) override {
      throw std::runtime_error("Externally driven HuxerUI applications do not own the platform event loop");
    }

    void RequestFrame(double delay_seconds) override {
      host_->RequestFrame(delay_seconds);
    }

    double Now() const noexcept override {
      return host_->Now();
    }

    detail::TextService &Text() override {
      return *this;
    }

    Size MeasureText(std::string_view text, float font_size, float max_width) override {
      return host_->MeasureText(text, font_size, max_width);
    }

  private:
    AppHost *host_;
  };

  Impl(AppDefinition definition, AppHost &host)
      : host_adapter(host), runtime(definition.root_factory, host_adapter, std::move(definition.options)) {}

  HostAdapter host_adapter;
  detail::Runtime runtime;
};

AppRuntime::AppRuntime(AppDefinition definition, AppHost &host) {
  if (definition.root_factory == nullptr) {
    throw std::invalid_argument("HuxerUI AppRuntime requires a root factory");
  }
  impl_ = std::make_unique<Impl>(std::move(definition), host);
}

AppRuntime::~AppRuntime() = default;

void AppRuntime::SetViewport(Size viewport) {
  impl_->runtime.SetViewport(viewport);
}

const DisplayList &AppRuntime::BuildFrame() {
  return impl_->runtime.BuildFrame();
}

void AppRuntime::HandlePointerEvent(const PointerEvent &event) {
  impl_->runtime.HandlePointerEvent(event);
}

void AppRuntime::HandleScrollEvent(const ScrollEvent &event) {
  impl_->runtime.HandleScrollEvent(event);
}

void AppRuntime::HandleKeyEvent(const KeyEvent &event) {
  impl_->runtime.HandleKeyEvent(event);
}

namespace detail {

void RegisterAppDefinition(AppDefinition definition) {
  if (definition.root_factory == nullptr) {
    throw std::invalid_argument("HuxerUI application registration requires a root factory");
  }

  auto &registration = AppRegistration();
  if (registration.has_value()) {
    throw std::logic_error("HuxerUI application has already been registered");
  }
  registration.emplace(std::move(definition));
}

const AppDefinition &RegisteredAppDefinition() {
  const auto &registration = AppRegistration();
  if (!registration.has_value()) {
    throw std::logic_error("HuxerUI application has not been registered");
  }
  return *registration;
}

} // namespace detail

int RunApp(RootFactory root_factory, AppOptions options) {
  auto platform = detail::CreateDefaultPlatformHost();
  if (!platform) {
    throw std::runtime_error("HuxerUI could not create a platform host");
  }

  detail::Runtime runtime{root_factory, *platform, options};
  return platform->Run(runtime, options);
}

} // namespace huxerui
