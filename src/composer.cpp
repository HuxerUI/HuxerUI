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
  for (auto& [dependency_address, weak_dependency] : dependencies_) {
    static_cast<void>(dependency_address);
    if (auto dependency = weak_dependency.lock()) {
      dependency->subscribers.erase(id_);
    }
  }
  for (auto& [owner_identity, dependencies] : retained_dependencies_) {
    static_cast<void>(owner_identity);
    for (auto& [dependency_address, weak_dependency] : dependencies) {
      static_cast<void>(dependency_address);
      if (auto dependency = weak_dependency.lock()) {
        dependency->subscribers.erase(id_);
      }
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
  for (auto& [dependency_address, weak_dependency] : pending_dependencies_) {
    static_cast<void>(dependency_address);
    if (auto dependency = weak_dependency.lock()) {
      dependency->subscribers[id_] = self;
    }
  }
  for (auto& [dependency_address, weak_dependency] : dependencies_) {
    if (pending_dependencies_.contains(dependency_address)) {
      continue;
    }
    if (auto dependency = weak_dependency.lock()) {
      if (!HasRetainedDependency(dependency_address)) {
        dependency->subscribers.erase(id_);
      }
    }
  }

  bool observed_value_changed = false;
  for (const auto& [cell_address, version] : observed_versions_) {
    const auto found = pending_dependencies_.find(cell_address);
    if (found == pending_dependencies_.end()) {
      continue;
    }
    if (auto dependency = found->second.lock();
        dependency && static_cast<StateCellBase&>(*dependency).version != version) {
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
  Observe(std::static_pointer_cast<CompositionDependency>(cell));
  observed_versions_.try_emplace(cell.get(), cell->version);
}

void RecomposeScope::Observe(const std::shared_ptr<CompositionDependency>& dependency) {
  if (!composing_) {
    throw std::logic_error("HuxerUI dependency observation requires an active composition");
  }
  pending_dependencies_[dependency.get()] = dependency;
  if (dependency->notification_pending) {
    dependency->pending_readers.insert(id_);
  }
}

void RecomposeScope::ObserveRetained(
    std::uint64_t owner_identity,
    const std::shared_ptr<CompositionDependency>& dependency
) {
  retained_dependencies_[owner_identity][dependency.get()] = dependency;
  dependency->subscribers[id_] = shared_from_this();
  if (dependency->notification_pending) {
    dependency->pending_readers.insert(id_);
  }
}

void RecomposeScope::ClearRetained(std::uint64_t owner_identity) {
  const auto found = retained_dependencies_.find(owner_identity);
  if (found == retained_dependencies_.end()) {
    return;
  }
  for (const auto& [dependency_address, weak_dependency] : found->second) {
    if (dependencies_.contains(dependency_address) || pending_dependencies_.contains(dependency_address) ||
        HasRetainedDependency(dependency_address, owner_identity)) {
      continue;
    }
    if (auto dependency = weak_dependency.lock()) {
      dependency->subscribers.erase(id_);
    }
  }
  retained_dependencies_.erase(found);
}

bool RecomposeScope::HasRetainedDependency(
    CompositionDependency* dependency,
    std::optional<std::uint64_t> excluding_owner
) const {
  return std::ranges::any_of(retained_dependencies_, [dependency, excluding_owner](const auto& entry) {
    return (!excluding_owner.has_value() || entry.first != *excluding_owner) && entry.second.contains(dependency);
  });
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
thread_local VirtualItemDependencyCapture* VirtualItemDependencyCapture::current_ = nullptr;

VirtualItemDependencyCapture::VirtualItemDependencyCapture(
    std::shared_ptr<RecomposeScope> scope,
    std::uint64_t owner_identity
) : scope_(std::move(scope)), owner_identity_(owner_identity) {}

VirtualItemDependencyCapture::~VirtualItemDependencyCapture() {
  Clear();
}

void VirtualItemDependencyCapture::Clear() {
  if (auto scope = scope_.lock()) {
    scope->ClearRetained(owner_identity_);
  }
}

void VirtualItemDependencyCapture::Observe(const std::shared_ptr<CompositionDependency>& dependency) {
  if (auto scope = scope_.lock()) {
    scope->ObserveRetained(owner_identity_, dependency);
  }
}

VirtualItemDependencyCapture::Guard::Guard(VirtualItemDependencyCapture& capture) : previous_(current_) {
  current_ = &capture;
}

VirtualItemDependencyCapture::Guard::~Guard() {
  current_ = previous_;
}

VirtualItemDependencyCapture* VirtualItemDependencyCapture::Current() noexcept {
  return current_;
}

std::shared_ptr<RecomposeScope> VirtualItemDependencyCapture::Scope() const noexcept {
  return scope_.lock();
}

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

void Composer::Observe(const std::shared_ptr<CompositionDependency>& dependency) {
  scope_->Observe(dependency);
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

void ObserveDependency(const std::shared_ptr<CompositionDependency>& dependency) {
  if (auto* composer = Composer::Current()) {
    composer->Observe(dependency);
  } else if (auto* capture = VirtualItemDependencyCapture::Current()) {
    capture->Observe(dependency);
  }
}

namespace {

void NotifyDependency(const std::shared_ptr<CompositionDependency>& dependency) {
  std::vector<std::shared_ptr<RecomposeScope>> scopes;
  scopes.reserve(dependency->subscribers.size());

  std::erase_if(dependency->subscribers, [&scopes, &dependency](const auto& entry) {
    if (auto scope = entry.second.lock()) {
      if (!dependency->pending_readers.contains(entry.first)) {
        scopes.push_back(std::move(scope));
      }
      return false;
    }
    return true;
  });

  for (const auto& scope : scopes) {
    scope->Invalidate();
  }
}

} // namespace

void NotifyState(const std::shared_ptr<StateCellBase>& cell) {
  NotifyDependency(cell);
}

void BeginDependencyChange(const std::shared_ptr<CompositionDependency>& dependency) {
  if (dependency->notification_pending) {
    throw std::logic_error("HuxerUI composition dependency already has a pending change");
  }
  dependency->notification_pending = true;
  dependency->pending_readers.clear();
  if (const Composer* composer = Composer::Current()) {
    dependency->pending_readers.insert(composer->ScopeId());
  }
}

void CommitDependencyChange(const std::shared_ptr<CompositionDependency>& dependency) {
  if (!dependency->notification_pending) {
    throw std::logic_error("HuxerUI composition dependency has no pending change");
  }
  NotifyDependency(dependency);
  dependency->pending_readers.clear();
  dependency->notification_pending = false;
}

void CancelDependencyChange(const std::shared_ptr<CompositionDependency>& dependency) noexcept {
  dependency->pending_readers.clear();
  dependency->notification_pending = false;
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
