#include <huxerui/environment.h>

#include <stdexcept>

#include "internal.h"

namespace huxerui {

namespace detail {

void SetEnvironmentValue(EnvironmentValues& values, std::type_index key, std::any value) {
  values.values_.insert_or_assign(key, std::move(value));
}

void MergeEnvironmentValues(EnvironmentValues& target, const EnvironmentValues& source) {
  for (const auto& [key, value] : source.values_) {
    target.values_.insert_or_assign(key, value);
  }
}

const std::any* FindLocalEnvironmentValue(const EnvironmentValues& values, std::type_index key) noexcept {
  const auto found = values.values_.find(key);
  return found == values.values_.end() ? nullptr : &found->second;
}

std::shared_ptr<const EnvironmentFrame> CurrentEnvironmentFrame() {
  Composer* composer = Composer::Current();
  return composer ? composer->Environment() : nullptr;
}

const std::any* FindEnvironmentValue(std::shared_ptr<const EnvironmentFrame> environment, std::type_index key) {
  for (auto frame = std::move(environment); frame; frame = frame->parent) {
    if (const std::any* value = FindLocalEnvironmentValue(frame->overrides, key)) {
      return value;
    }
  }
  return nullptr;
}

const std::any* FindEnvironmentValue(std::type_index key) {
  return FindEnvironmentValue(Composer::RequireCurrent().Environment(), key);
}

} // namespace detail

View ProvideEnvironment(EnvironmentValues values, std::function<View()> content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI environment content factory must not be empty");
  }
  auto frame = std::make_shared<detail::EnvironmentFrame>(detail::EnvironmentFrame{
      detail::CurrentEnvironmentFrame(),
      std::move(values),
  });
  return Scope([frame = std::move(frame), content = std::move(content)]() mutable {
    detail::Composer::EnvironmentGuard guard{frame};
    return content();
  });
}

} // namespace huxerui
