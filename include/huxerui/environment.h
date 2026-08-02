#pragma once

#include <any>
#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <huxerui/view.h>

namespace huxerui {

class Environment;

namespace detail {
void SetEnvironmentValue(Environment& environment, std::type_index key, std::any value);
void MergeEnvironment(Environment& target, const Environment& source);
const std::any* FindLocalEnvironmentValue(const Environment& environment, std::type_index key) noexcept;
const std::shared_ptr<const Environment>& EnvironmentParent(const Environment& environment) noexcept;
} // namespace detail

template <class Value>
concept EnvironmentValue = std::copy_constructible<Value> && requires {
  { Value::Default() } -> std::convertible_to<Value>;
};

class Environment {
public:
  template <EnvironmentValue Value> Environment& Set(Value value) {
    local_values_.insert_or_assign(typeid(Value), std::move(value));
    return *this;
  }

private:
  std::shared_ptr<const Environment> parent_;
  std::unordered_map<std::type_index, std::any> local_values_;

  friend void detail::SetEnvironmentValue(Environment& environment, std::type_index key, std::any value);
  friend void detail::MergeEnvironment(Environment& target, const Environment& source);
  friend const std::any*
  detail::FindLocalEnvironmentValue(const Environment& environment, std::type_index key) noexcept;
  friend const std::shared_ptr<const Environment>& detail::EnvironmentParent(const Environment& environment) noexcept;
  friend View ProvideEnvironment(Environment environment, std::function<View()> content);
};

namespace detail {

std::shared_ptr<const Environment> CurrentEnvironment();
const std::any* FindEnvironmentValue(std::shared_ptr<const Environment> environment, std::type_index key);
const std::any* FindEnvironmentValue(std::type_index key);

} // namespace detail

template <EnvironmentValue Value> const Value& UseEnvironment() {
  if (const std::any* value = detail::FindEnvironmentValue(typeid(Value))) {
    if (const auto* typed = std::any_cast<Value>(value)) {
      return *typed;
    }
    throw std::logic_error("HuxerUI environment value has an invalid stored type");
  }
  static const Value fallback = Value::Default();
  return fallback;
}

View ProvideEnvironment(Environment environment, std::function<View()> content);

template <EnvironmentValue Value, class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View ProvideEnvironment(Value value, Factory&& content) {
  Environment environment;
  environment.Set(std::move(value));
  return ProvideEnvironment(std::move(environment), std::forward<Factory>(content));
}

} // namespace huxerui
