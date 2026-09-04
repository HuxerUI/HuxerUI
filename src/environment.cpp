#include <huxerui/environment.h>

#include <stdexcept>

#include "runtime_internal.h"

namespace huxerui {

Environment::Environment(const Environment& other) {
  for (const auto& [key, entry] : other.entries_) {
    if (entry.value.has_value()) {
      entries_.emplace(key, Entry{entry.value, entry.equals, {}});
    }
  }
}

Environment& Environment::operator=(const Environment& other) {
  if (this == &other) {
    return *this;
  }
  Environment copy(other);
  parent_.reset();
  entries_.swap(copy.entries_);
  return *this;
}

bool Environment::Update(std::type_index key, std::any value, detail::EnvironmentEquals equals) {
  Entry& entry = entries_[key];
  if (entry.value.has_value() && equals != nullptr && equals(entry.value, value)) {
    return false;
  }
  if (!entry.dependency) {
    entry.dependency = std::make_shared<detail::CompositionDependency>();
  }
  detail::BeginDependencyChange(entry.dependency);
  entry.value = std::move(value);
  entry.equals = equals;
  detail::CommitDependencyChange(entry.dependency);
  return true;
}

namespace detail {

EnvironmentTransaction::EnvironmentTransaction(
    Environment& mounted,
    const Environment& declaration,
    std::shared_ptr<const Environment> parent
) : mounted_(&mounted) {

  const auto begin_change = [this](Environment::Entry& entry) {
    if (!entry.dependency) {
      entry.dependency = std::make_shared<CompositionDependency>();
    }
    if (std::ranges::find(dependencies_, entry.dependency) != dependencies_.end()) {
      return;
    }
    BeginDependencyChange(entry.dependency);
    dependencies_.push_back(entry.dependency);
  };
  const auto stage_value = [this, &mounted, &begin_change](
                               std::type_index key,
                               std::any value,
                               EnvironmentEquals equals
                           ) {
    Environment::Entry& entry = mounted.entries_[key];
    changes_.push_back({key, entry.value, entry.equals});
    begin_change(entry);
    entry.value = std::move(value);
    entry.equals = equals;
  };

  try {
    if (mounted.parent_ != parent) {
      previous_parent_ = mounted.parent_;
      parent_changed_ = true;
      mounted.parent_ = std::move(parent);
      for (auto& [key, entry] : mounted.entries_) {
        static_cast<void>(key);
        if (!entry.value.has_value()) {
          begin_change(entry);
        }
      }
    }

    for (const auto& [key, mounted_entry] : mounted.entries_) {
      if (!mounted_entry.value.has_value()) {
        continue;
      }
      const auto incoming = declaration.entries_.find(key);
      if (incoming == declaration.entries_.end() || !incoming->second.value.has_value()) {
        stage_value(key, {}, nullptr);
      }
    }

    for (const auto& [key, declaration_entry] : declaration.entries_) {
      if (!declaration_entry.value.has_value()) {
        continue;
      }
      const auto current = mounted.entries_.find(key);
      if (current != mounted.entries_.end() && current->second.value.has_value() &&
          declaration_entry.equals != nullptr &&
          declaration_entry.equals(current->second.value, declaration_entry.value)) {
        continue;
      }
      stage_value(key, declaration_entry.value, declaration_entry.equals);
    }
  } catch (...) {
    Rollback();
    throw;
  }
}

EnvironmentTransaction::~EnvironmentTransaction() {
  if (!committed_) {
    Rollback();
  }
}

void EnvironmentTransaction::Rollback() noexcept {
  for (auto change = changes_.rbegin(); change != changes_.rend(); ++change) {
    Environment::Entry& entry = mounted_->entries_[change->key];
    entry.value = std::move(change->previous_value);
    entry.equals = change->previous_equals;
  }
  if (parent_changed_) {
    mounted_->parent_ = std::move(previous_parent_);
  }
  for (const auto& dependency : dependencies_) {
    CancelDependencyChange(dependency);
  }
}

void EnvironmentTransaction::Commit() {
  for (const auto& dependency : dependencies_) {
    CommitDependencyChange(dependency);
  }
  committed_ = true;
}

void SetEnvironmentValue(
    Environment& environment,
    std::type_index key,
    std::any value,
    EnvironmentEquals equals
) {
  Environment::Entry& entry = environment.entries_[key];
  entry.value = std::move(value);
  entry.equals = equals;
}

void MergeEnvironment(Environment& target, const Environment& source) {
  for (const auto& [key, source_entry] : source.entries_) {
    if (!source_entry.value.has_value()) {
      continue;
    }
    Environment::Entry& target_entry = target.entries_[key];
    target_entry.value = source_entry.value;
    target_entry.equals = source_entry.equals;
  }
}

const std::any* FindLocalEnvironmentValue(const Environment& environment, std::type_index key) {
  Environment::Entry& entry = environment.entries_[key];
  if (!entry.dependency) {
    entry.dependency = std::make_shared<CompositionDependency>();
  }
  ObserveDependency(entry.dependency);
  return entry.value.has_value() ? &entry.value : nullptr;
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

View ProvideEnvironment(Environment environment, View content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI environment content must not be empty");
  }
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Environment);
  spec->local_environment = std::move(environment);
  spec->children.push_back(std::move(content));
  return View(std::move(spec));
}

} // namespace huxerui
