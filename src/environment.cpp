#include <huxerui/environment.h>

#include <stdexcept>

#include "internal.h"

namespace huxerui {

namespace detail {

void SetEnvironmentValue(Environment& environment, std::type_index key, std::any value) {
  environment.local_values_.insert_or_assign(key, std::move(value));
}

void MergeEnvironment(Environment& target, const Environment& source) {
  for (const auto& [key, value] : source.local_values_) {
    target.local_values_.insert_or_assign(key, value);
  }
}

const std::any* FindLocalEnvironmentValue(const Environment& environment, std::type_index key) noexcept {
  const auto found = environment.local_values_.find(key);
  return found == environment.local_values_.end() ? nullptr : &found->second;
}

const std::shared_ptr<const Environment>& EnvironmentParent(const Environment& environment) noexcept {
  return environment.parent_;
}

std::shared_ptr<const Environment> CurrentEnvironment() {
  Composer* composer = Composer::Current();
  return composer ? composer->CurrentEnvironment() : nullptr;
}

const std::any* FindEnvironmentValue(std::shared_ptr<const Environment> environment, std::type_index key) {
  for (auto current = std::move(environment); current; current = EnvironmentParent(*current)) {
    if (const std::any* value = FindLocalEnvironmentValue(*current, key)) {
      return value;
    }
  }
  return nullptr;
}

const std::any* FindEnvironmentValue(std::type_index key) {
  return FindEnvironmentValue(Composer::RequireCurrent().CurrentEnvironment(), key);
}

} // namespace detail

View ProvideEnvironment(Environment environment, std::function<View()> content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI environment content factory must not be empty");
  }
  environment.parent_ = detail::CurrentEnvironment();
  auto shared_environment = std::make_shared<Environment>(std::move(environment));
  return Scope([environment = std::move(shared_environment), content = std::move(content)]() mutable {
    detail::Composer::EnvironmentGuard guard{environment};
    return content();
  });
}

} // namespace huxerui
