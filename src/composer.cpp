#include "internal.h"

#include <algorithm>
#include <stdexcept>

#include "task_internal.h"

namespace huxerui::detail {

namespace {

void HashCombine(std::size_t& seed, std::size_t value) {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

std::size_t CompositionSlotKeyHash::operator()(const CompositionSlotKey& key) const noexcept {
  std::size_t seed = std::hash<std::string>{}(key.file);
  HashCombine(seed, std::hash<std::string>{}(key.function));
  HashCombine(seed, std::hash<std::uint32_t>{}(key.line));
  HashCombine(seed, std::hash<std::uint32_t>{}(key.column));
  HashCombine(seed, std::hash<std::uint32_t>{}(key.occurrence));
  return seed;
}

RecomposeScope::RecomposeScope(Runtime& runtime, std::uint64_t id, StateSlotStorage state_slots)
    : runtime_(&runtime), id_(id), state_slots_(std::move(state_slots)) {}

RecomposeScope::~RecomposeScope() {
  for (auto& [cell_address, weak_cell] : dependencies_) {
    static_cast<void>(cell_address);
    if (auto cell = weak_cell.lock()) {
      cell->subscribers.erase(id_);
    }
  }
  runtime_->RetireLifecycles(*this);
  runtime_->RetireTaskScope(std::move(task_scope_));
}

void RecomposeScope::BeginComposition() {
  if (composing_) {
    throw std::logic_error("HuxerUI recompose scope is already composing");
  }

  StateSlotStorage pending = state_slots_;
  pending_state_slots_ = std::move(pending);
  pending_dependencies_.clear();
  observed_versions_.clear();
  touched_state_slots_.clear();
  state_slot_occurrences_.clear();
  pending_lifecycle_declarations_.clear();
  pending_lifecycle_indices_.clear();
  lifecycle_occurrences_.clear();
  invalidated_during_composition_ = false;
  composing_ = true;
}

void RecomposeScope::EndComposition() {
  if (!composing_) {
    throw std::logic_error("HuxerUI recompose scope is not composing");
  }

  std::erase_if(pending_state_slots_.slots, [this](const auto& entry) {
    return !touched_state_slots_.contains(entry.first);
  });

  const auto self = shared_from_this();
  for (auto& [cell_address, weak_cell] : pending_dependencies_) {
    static_cast<void>(cell_address);
    if (auto cell = weak_cell.lock()) {
      cell->subscribers[id_] = self;
    }
  }
  for (auto& [cell_address, weak_cell] : dependencies_) {
    if (pending_dependencies_.contains(cell_address)) {
      continue;
    }
    if (auto cell = weak_cell.lock()) {
      cell->subscribers.erase(id_);
    }
  }

  bool observed_value_changed = false;
  for (const auto& [cell_address, version] : observed_versions_) {
    const auto found = pending_dependencies_.find(cell_address);
    if (found == pending_dependencies_.end()) {
      continue;
    }
    if (auto cell = found->second.lock(); cell && cell->version != version) {
      observed_value_changed = true;
      break;
    }
  }

  state_slots_ = std::move(pending_state_slots_);
  dependencies_ = std::move(pending_dependencies_);
  observed_versions_.clear();
  touched_state_slots_.clear();
  state_slot_occurrences_.clear();
  composing_ = false;
  dirty_ = invalidated_during_composition_ || observed_value_changed;
  invalidated_during_composition_ = false;
  if ((!pending_lifecycle_declarations_.empty() || !lifecycle_slots_.empty()) && !lifecycle_commit_pending_) {
    runtime_->QueueLifecycleCommit(shared_from_this());
    lifecycle_commit_pending_ = true;
  }
  if (dirty_) {
    runtime_->InvalidateScope(id_);
  }
}

void RecomposeScope::AbortComposition() noexcept {
  pending_state_slots_.slots.clear();
  pending_dependencies_.clear();
  observed_versions_.clear();
  touched_state_slots_.clear();
  state_slot_occurrences_.clear();
  pending_lifecycle_declarations_.clear();
  pending_lifecycle_indices_.clear();
  lifecycle_occurrences_.clear();
  composing_ = false;
  invalidated_during_composition_ = false;
  dirty_ = true;
}

void RecomposeScope::Observe(const std::shared_ptr<StateCellBase>& cell) {
  if (!composing_) {
    throw std::logic_error("HuxerUI state observation requires an active composition");
  }
  pending_dependencies_[cell.get()] = cell;
  observed_versions_.try_emplace(cell.get(), cell->version);
}

void RecomposeScope::RegisterLifecycle(LifecycleSetup setup, std::vector<LifecycleDependency> dependencies) {
  if (!composing_) {
    throw std::logic_error("HuxerUI Lifecycle registration requires an active composition");
  }
  const std::source_location& location = setup.Location();
  CompositionSlotKey key{
      location.file_name(),
      location.function_name(),
      location.line(),
      location.column(),
      0,
  };
  key.occurrence = lifecycle_occurrences_[key]++;
  pending_lifecycle_indices_.emplace(key, pending_lifecycle_declarations_.size());
  pending_lifecycle_declarations_.push_back({std::move(key), std::move(setup), std::move(dependencies)});
}

TaskScope RecomposeScope::Tasks() {
  if (!task_scope_) {
    task_scope_ = runtime_->CreateTaskScope();
  }
  return TaskScope(task_scope_);
}

void RecomposeScope::PrepareLifecycleCommit() {
  for (const CompositionSlotKey& key : lifecycle_order_) {
    auto active = lifecycle_slots_.find(key);
    if (active == lifecycle_slots_.end()) {
      continue;
    }
    active->second.retained_for_commit = false;
    const auto pending_index = pending_lifecycle_indices_.find(key);
    if (pending_index != pending_lifecycle_indices_.end()) {
      active->second.retained_for_commit =
          active->second.dependencies == pending_lifecycle_declarations_[pending_index->second].dependencies;
    }
  }
}

void RecomposeScope::CommitLifecycleCleanups() noexcept {
  for (auto key = lifecycle_order_.rbegin(); key != lifecycle_order_.rend(); ++key) {
    auto active = lifecycle_slots_.find(*key);
    if (active == lifecycle_slots_.end() || active->second.retained_for_commit) {
      continue;
    }
    active->second.cleanup.Run();
    lifecycle_slots_.erase(active);
  }
}

void RecomposeScope::CommitLifecycleSetups() {
  const auto rebuild_order = [this] {
    lifecycle_order_.clear();
    lifecycle_order_.reserve(lifecycle_slots_.size());
    for (const LifecycleDeclaration& declaration : pending_lifecycle_declarations_) {
      if (lifecycle_slots_.contains(declaration.key)) {
        lifecycle_order_.push_back(declaration.key);
      }
    }
  };

  try {
    for (LifecycleDeclaration& declaration : pending_lifecycle_declarations_) {
      if (lifecycle_slots_.contains(declaration.key)) {
        continue;
      }
      LifecycleCleanup cleanup = declaration.setup.Run();
      try {
        auto [slot, inserted] = lifecycle_slots_.try_emplace(declaration.key);
        static_cast<void>(inserted);
        slot->second.dependencies = std::move(declaration.dependencies);
        slot->second.cleanup = std::move(cleanup);
      } catch (...) {
        cleanup.Run();
        throw;
      }
    }
    rebuild_order();
  } catch (...) {
    rebuild_order();
    DiscardLifecycleCommit();
    throw;
  }
  DiscardLifecycleCommit();
}

void RecomposeScope::DiscardLifecycleCommit() noexcept {
  pending_lifecycle_declarations_.clear();
  pending_lifecycle_indices_.clear();
  lifecycle_occurrences_.clear();
  lifecycle_commit_pending_ = false;
}

void RecomposeScope::Invalidate() {
  if (composing_) {
    if (!invalidated_during_composition_) {
      invalidated_during_composition_ = true;
      runtime_->InvalidateScope(id_);
    }
    return;
  }
  if (dirty_) {
    return;
  }
  dirty_ = true;
  runtime_->InvalidateScope(id_);
}

void RecomposeScope::SetEventBindings(EventBindings bindings) {
  event_hub_->SetBindings(std::move(bindings));
}

std::shared_ptr<StateCellBase> RecomposeScope::UseState(
    std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial
) {
  CompositionSlotKey key{
      location.file_name(),
      location.function_name(),
      location.line(),
      location.column(),
      0,
  };
  key.occurrence = state_slot_occurrences_[key]++;
  touched_state_slots_.insert(key);

  auto found = pending_state_slots_.slots.find(key);
  if (found != pending_state_slots_.slots.end()) {
    if (found->second->Type() != type) {
      throw std::logic_error("HuxerUI state kind or value type changed at the same call site");
    }
    return found->second;
  }

  pending_state_slots_.slots.emplace(std::move(key), initial);
  return initial;
}

thread_local Composer* Composer::current_ = nullptr;

Composer::Composer(std::shared_ptr<RecomposeScope> scope, std::shared_ptr<const Environment> environment)
    : scope_(std::move(scope)), environment_(std::move(environment)) {}

Composer* Composer::Current() noexcept {
  return current_;
}

Composer& Composer::RequireCurrent() {
  if (current_ == nullptr) {
    throw std::logic_error("UseState() must be called while HuxerUI is composing a view");
  }
  return *current_;
}

void Composer::Observe(const std::shared_ptr<StateCellBase>& cell) {
  scope_->Observe(cell);
}

std::shared_ptr<StateCellBase>
Composer::UseState(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial) {
  return scope_->UseState(type, location, std::move(initial));
}

void Composer::RegisterLifecycle(LifecycleSetup setup, std::vector<LifecycleDependency> dependencies) {
  scope_->RegisterLifecycle(std::move(setup), std::move(dependencies));
}

TaskScope Composer::Tasks() {
  return scope_->Tasks();
}

std::shared_ptr<EventHub> Composer::Events() const noexcept {
  return scope_->Events();
}

Composer::EnvironmentGuard::EnvironmentGuard(std::shared_ptr<const Environment> environment)
    : composer_(&Composer::RequireCurrent()), previous_(composer_->environment_) {
  composer_->environment_ = std::move(environment);
}

Composer::EnvironmentGuard::~EnvironmentGuard() {
  composer_->environment_ = std::move(previous_);
}

Composer::Guard::Guard(Composer& composer) : previous_(current_) {
  current_ = &composer;
}

Composer::Guard::~Guard() {
  current_ = previous_;
}

void ObserveState(const std::shared_ptr<StateCellBase>& cell) {
  if (auto* composer = Composer::Current()) {
    composer->Observe(cell);
  }
}

void NotifyState(const std::shared_ptr<StateCellBase>& cell) {
  std::vector<std::shared_ptr<RecomposeScope>> scopes;
  scopes.reserve(cell->subscribers.size());

  std::erase_if(cell->subscribers, [&scopes](const auto& entry) {
    if (auto scope = entry.second.lock()) {
      scopes.push_back(std::move(scope));
      return false;
    }
    return true;
  });

  for (const auto& scope : scopes) {
    scope->Invalidate();
  }
}

std::shared_ptr<StateCellBase>
UseStateCell(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial) {
  return Composer::RequireCurrent().UseState(type, location, std::move(initial));
}

void RegisterLifecycle(LifecycleSetup setup, std::vector<LifecycleDependency> dependencies) {
  Composer* composer = Composer::Current();
  if (composer == nullptr) {
    throw std::logic_error("Lifecycle() must be called while HuxerUI is composing a view");
  }
  composer->RegisterLifecycle(std::move(setup), std::move(dependencies));
}

std::shared_ptr<EventHub> UseEventHub() {
  return Composer::RequireCurrent().Events();
}

} // namespace huxerui::detail
