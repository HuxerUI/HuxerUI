#include <huxerui/environment.h>

#include <stdexcept>

#include "internal.h"

namespace huxerui {

void EnvironmentValues::Merge(const EnvironmentValues &values) {
  for (const auto &[key, value] : values.values_) {
    values_.insert_or_assign(key, value);
  }
}

namespace detail {

std::shared_ptr<const EnvironmentFrame> CurrentEnvironmentFrame() {
  Composer *composer = Composer::Current();
  return composer ? composer->Environment() : nullptr;
}

const std::any *FindEnvironmentValue(
    std::shared_ptr<const EnvironmentFrame> environment,
    std::type_index key) {
  for (auto frame = std::move(environment);
       frame; frame = frame->parent) {
    if (const std::any *value = frame->overrides.Find(key)) {
      return value;
    }
  }
  return nullptr;
}

const std::any *FindEnvironmentValue(std::type_index key) {
  return FindEnvironmentValue(
      Composer::RequireCurrent().Environment(), key);
}

} // namespace detail

View ProvideEnvironment(
    EnvironmentValues values, std::function<View()> content) {
  if (!content) {
    throw std::invalid_argument(
        "HuxerUI environment content factory must not be empty");
  }
  auto frame = std::make_shared<detail::EnvironmentFrame>(
      detail::EnvironmentFrame{
          detail::CurrentEnvironmentFrame(),
          std::move(values),
      });
  return Scope(
      [frame = std::move(frame),
       content = std::move(content)]() mutable {
        detail::Composer::EnvironmentGuard guard{frame};
        return content();
      });
}

} // namespace huxerui
