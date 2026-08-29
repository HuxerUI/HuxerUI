#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_set>
#include <utility>
#include <vector>

#include <huxerui/environment.h>
#include <huxerui/layer.h>
#include <huxerui/platform_registry.h>

namespace huxerui {

class Runtime;

/// Configures root-owned services, presentation layers, and platform registrations before first composition.
///
/// A RootContext is supplied to each selected RootHook. Registration names share one case-sensitive namespace for
/// PlatformModule and PlatformView entries and become immutable after root installation completes.
class RootContext {
public:
  template <class Service> void Provide(std::shared_ptr<Service> service) {
    if (!service) {
      throw std::invalid_argument("HuxerUI root service must not be empty");
    }
    if (!service_types_->insert(typeid(Service)).second) {
      throw std::logic_error("HuxerUI root service type was provided more than once");
    }
    services_->push_back(service);
    detail::SetEnvironmentValue(*environment_, typeid(Service), std::move(service));
  }

  LayerController& Layers() noexcept {
    return *layers_;
  }

  /// Registers a PlatformModule factory without construction options.
  ///
  /// Factory receives the owning PlatformAdapter& and returns exactly Module. A library normally hides the stable name
  /// and registration call inside its Install function.
  template <class Module, class Factory> void RegisterPlatformModule(std::string name, Factory factory) {
    registry_->template RegisterModule<Module>(std::move(name), std::move(factory));
  }

  /// Registers a PlatformModule factory with strongly typed construction Options.
  ///
  /// Factory receives PlatformAdapter& followed by const Options& and returns exactly Module.
  template <class Module, class Options, class Factory> void RegisterPlatformModule(std::string name, Factory factory) {
    registry_->template RegisterModule<Module, Options>(std::move(name), std::move(factory));
  }

  /// Registers a platform-specific PlatformView factory with controlled Properties and no Controller.
  ///
  /// Properties must be move-constructible and equality-comparable. Factory must be the platform factory type from the
  /// host-specific platform_registry.h header.
  template <class Properties, class Factory> void RegisterPlatformView(std::string name, Factory factory) {
    registry_->template RegisterView<Properties>(std::move(name), std::move(factory));
  }

  /// Registers a platform-specific PlatformView factory with controlled Properties and a typed Controller.
  ///
  /// Properties and Controller must be move-constructible and equality-comparable.
  template <class Properties, class Controller, class Factory>
  void RegisterPlatformView(std::string name, Factory factory) {
    registry_->template RegisterView<Properties, Controller>(std::move(name), std::move(factory));
  }

  /// Opens a registered root-owned PlatformModule without construction options.
  template <class Module> [[nodiscard]] Module OpenPlatformModule(std::string name) {
    return registry_->template OpenModule<Module>(std::move(name));
  }

  /// Opens a registered root-owned PlatformModule with strongly typed construction Options.
  template <class Module, class Options> [[nodiscard]] Module OpenPlatformModule(std::string name, Options options) {
    return registry_->template OpenModule<Module, Options>(std::move(name), std::move(options));
  }

private:
  RootContext(LayerController& layers, detail::PlatformRegistry& registry, Environment& environment,
              std::unordered_set<std::type_index>& service_types, std::vector<std::shared_ptr<void>>& services)
      : layers_(&layers), registry_(&registry), environment_(&environment), service_types_(&service_types),
        services_(&services) {}

  LayerController* layers_;
  detail::PlatformRegistry* registry_;
  Environment* environment_;
  std::unordered_set<std::type_index>* service_types_;
  std::vector<std::shared_ptr<void>>* services_;

  friend class Runtime;
};

using RootHook = std::function<void(RootContext&)>;

template <class Service> std::shared_ptr<Service> UseService() {
  const std::any* value = detail::FindEnvironmentValue(typeid(Service));
  if (!value) {
    throw std::logic_error("HuxerUI requested root service is not installed");
  }
  const auto* service = std::any_cast<std::shared_ptr<Service>>(value);
  if (!service || !*service) {
    throw std::logic_error("HuxerUI root service environment value has an invalid stored type");
  }
  return *service;
}

} // namespace huxerui
