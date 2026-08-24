#pragma once

#include <any>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <huxerui/view.h>

namespace huxerui {

class Environment;
class Runtime;

enum class ViewportClass {
  Compact,
  Medium,
  Expanded,
};

struct ViewportBreakpoints {
  float medium_width = 600.0F;
  float expanded_width = 840.0F;

  bool operator==(const ViewportBreakpoints&) const = default;
};

namespace detail {
class CompositionDependency;
class EnvironmentTransaction;

using EnvironmentEquals = bool (*)(const std::any&, const std::any&);

template <class Value> constexpr EnvironmentEquals EnvironmentEqualsFor() noexcept {
  return [](const std::any& left, const std::any& right) {
    return std::any_cast<const Value&>(left) == std::any_cast<const Value&>(right);
  };
}

struct ViewportEnvironment {
  ViewportClass value = ViewportClass::Compact;

  static ViewportEnvironment Default() {
    return {};
  }

  bool operator==(const ViewportEnvironment&) const = default;
};

void SetEnvironmentValue(
    Environment& environment,
    std::type_index key,
    std::any value,
    EnvironmentEquals equals = nullptr
);
void MergeEnvironment(Environment& target, const Environment& source);
const std::any* FindLocalEnvironmentValue(const Environment& environment, std::type_index key);
const std::shared_ptr<const Environment>& EnvironmentParent(const Environment& environment) noexcept;
} // namespace detail

template <class Value>
concept EnvironmentValue = std::copy_constructible<Value> && std::equality_comparable<Value> && requires {
  { Value::Default() } -> std::convertible_to<Value>;
};

class Environment {
public:
  Environment() = default;
  Environment(const Environment& other);
  Environment(Environment&&) noexcept = default;
  Environment& operator=(const Environment& other);
  Environment& operator=(Environment&&) noexcept = default;

  template <EnvironmentValue Value> Environment& Set(Value value) {
    detail::SetEnvironmentValue(
        *this,
        typeid(Value),
        std::move(value),
        detail::EnvironmentEqualsFor<Value>()
    );
    return *this;
  }

private:
  struct Entry {
    std::any value;
    detail::EnvironmentEquals equals = nullptr;
    std::shared_ptr<detail::CompositionDependency> dependency;
  };

  std::shared_ptr<const Environment> parent_;
  mutable std::unordered_map<std::type_index, Entry> entries_;

  template <EnvironmentValue Value> bool Update(Value value) {
    return Update(typeid(Value), std::move(value), detail::EnvironmentEqualsFor<Value>());
  }

  bool Update(std::type_index key, std::any value, detail::EnvironmentEquals equals);

  friend class detail::EnvironmentTransaction;
  friend void detail::SetEnvironmentValue(
      Environment& environment,
      std::type_index key,
      std::any value,
      detail::EnvironmentEquals equals
  );
  friend void detail::MergeEnvironment(Environment& target, const Environment& source);
  friend const std::any* detail::FindLocalEnvironmentValue(const Environment& environment, std::type_index key);
  friend const std::shared_ptr<const Environment>& detail::EnvironmentParent(const Environment& environment) noexcept;
  friend class Runtime;
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

inline ViewportClass UseViewportClass() {
  return UseEnvironment<detail::ViewportEnvironment>().value;
}

View ProvideEnvironment(Environment environment, View content);

template <EnvironmentValue Value> View ProvideEnvironment(Value value, View content) {
  Environment environment;
  environment.Set(std::move(value));
  return ProvideEnvironment(std::move(environment), std::move(content));
}

} // namespace huxerui
