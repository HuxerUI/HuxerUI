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

namespace huxerui {

class Runtime;

template <class Service> struct ServiceEnvironmentKey {
  using Value = std::shared_ptr<Service>;

  static Value Default() {
    return {};
  }
};

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
    environment_->Set<ServiceEnvironmentKey<Service>>(std::move(service));
  }

  LayerController& Layers() noexcept {
    return *layers_;
  }

private:
  RootContext(
      LayerController& layers,
      EnvironmentValues& environment,
      std::unordered_set<std::type_index>& service_types,
      std::vector<std::shared_ptr<void>>& services
  )
      : layers_(&layers), environment_(&environment), service_types_(&service_types), services_(&services) {}

  LayerController* layers_;
  EnvironmentValues* environment_;
  std::unordered_set<std::type_index>* service_types_;
  std::vector<std::shared_ptr<void>>* services_;

  friend class Runtime;
};

using RootHook = std::function<void(RootContext&)>;

template <class Service> std::shared_ptr<Service> UseService() {
  auto service = UseEnvironment<ServiceEnvironmentKey<Service>>();
  if (!service) {
    throw std::logic_error("HuxerUI requested root service is not installed");
  }
  return service;
}

} // namespace huxerui
