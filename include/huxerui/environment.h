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

class EnvironmentValues;

namespace detail {
struct EnvironmentFrame;

void SetEnvironmentValue(EnvironmentValues& values, std::type_index key, std::any value);
void MergeEnvironmentValues(EnvironmentValues& target, const EnvironmentValues& source);
const std::any* FindLocalEnvironmentValue(const EnvironmentValues& values, std::type_index key) noexcept;
}

template <class Value>
concept EnvironmentValue = std::copy_constructible<Value> && requires {
  { Value::Default() } -> std::convertible_to<Value>;
};

class EnvironmentValues {
public:
  template <EnvironmentValue Value> EnvironmentValues& Set(Value value) {
    values_.insert_or_assign(typeid(Value), std::move(value));
    return *this;
  }

private:
  std::unordered_map<std::type_index, std::any> values_;

  friend struct detail::EnvironmentFrame;
  friend void detail::SetEnvironmentValue(EnvironmentValues& values, std::type_index key, std::any value);
  friend void detail::MergeEnvironmentValues(EnvironmentValues& target, const EnvironmentValues& source);
  friend const std::any* detail::FindLocalEnvironmentValue(
      const EnvironmentValues& values,
      std::type_index key
  ) noexcept;
};

namespace detail {

std::shared_ptr<const EnvironmentFrame> CurrentEnvironmentFrame();
const std::any* FindEnvironmentValue(std::shared_ptr<const EnvironmentFrame> environment, std::type_index key);
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

View ProvideEnvironment(EnvironmentValues values, std::function<View()> content);

template <EnvironmentValue Value, class Factory>
  requires std::invocable<Factory&> && std::convertible_to<std::invoke_result_t<Factory&>, View>
View ProvideEnvironment(Value value, Factory&& content) {
  EnvironmentValues values;
  values.Set(std::move(value));
  return ProvideEnvironment(std::move(values), std::forward<Factory>(content));
}

} // namespace huxerui
