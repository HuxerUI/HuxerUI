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

namespace detail {
struct EnvironmentFrame;
}

class EnvironmentValues {
public:
  template <class Key>
    requires requires {
      typename Key::Value;
    }
  EnvironmentValues &Set(typename Key::Value value) {
    values_.insert_or_assign(typeid(Key), std::move(value));
    return *this;
  }

  void Merge(const EnvironmentValues &values);

  [[nodiscard]] bool Empty() const noexcept {
    return values_.empty();
  }

  [[nodiscard]] const std::any *
  Find(std::type_index key) const noexcept {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
  }

private:
  std::unordered_map<std::type_index, std::any> values_;

  friend struct detail::EnvironmentFrame;
};

namespace detail {

std::shared_ptr<const EnvironmentFrame> CurrentEnvironmentFrame();
const std::any *FindEnvironmentValue(
    std::shared_ptr<const EnvironmentFrame> environment,
    std::type_index key);
const std::any *FindEnvironmentValue(std::type_index key);

} // namespace detail

template <class Key>
concept EnvironmentKey = requires {
  typename Key::Value;
  {
    Key::Default()
  } -> std::convertible_to<typename Key::Value>;
};

template <EnvironmentKey Key>
const typename Key::Value &UseEnvironment() {
  if (const std::any *value =
          detail::FindEnvironmentValue(typeid(Key))) {
    if (const auto *typed =
            std::any_cast<typename Key::Value>(value)) {
      return *typed;
    }
    throw std::logic_error(
        "HuxerUI environment value type does not match its key");
  }
  static const typename Key::Value fallback = Key::Default();
  return fallback;
}

View ProvideEnvironment(
    EnvironmentValues values, std::function<View()> content);

template <EnvironmentKey Key, class Factory>
  requires std::invocable<Factory &> &&
           std::convertible_to<std::invoke_result_t<Factory &>, View>
View ProvideEnvironment(
    typename Key::Value value, Factory &&content) {
  EnvironmentValues values;
  values.Set<Key>(std::move(value));
  return ProvideEnvironment(
      std::move(values), std::forward<Factory>(content));
}

} // namespace huxerui
