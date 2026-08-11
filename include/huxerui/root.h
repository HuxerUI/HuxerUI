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

class PlatformModules;
class Runtime;

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

  PlatformModules& Modules() noexcept {
    return *modules_;
  }

private:
  RootContext(
      LayerController& layers,
      PlatformModules& modules,
      Environment& environment,
      std::unordered_set<std::type_index>& service_types,
      std::vector<std::shared_ptr<void>>& services
  )
      : layers_(&layers), modules_(&modules), environment_(&environment), service_types_(&service_types),
        services_(&services) {}

  LayerController* layers_;
  PlatformModules* modules_;
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
