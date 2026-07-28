#include "internal.h"

#include <algorithm>
#include <stdexcept>

namespace huxerui::detail {

namespace {

void HashCombine(std::size_t& seed, std::size_t value) {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

}  // namespace

std::size_t StateSlotKeyHash::operator()(const StateSlotKey& key) const noexcept {
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
}

void RecomposeScope::BeginComposition() {
  dirty_ = false;
  for (auto& [cell_address, weak_cell] : dependencies_) {
    static_cast<void>(cell_address);
    if (auto cell = weak_cell.lock()) {
      cell->subscribers.erase(id_);
    }
  }
  dependencies_.clear();
  touched_state_slots_.clear();
  state_slot_occurrences_.clear();
}

void RecomposeScope::EndComposition() {
  std::erase_if(state_slots_.slots,
                [this](const auto& entry) { return !touched_state_slots_.contains(entry.first); });
}

void RecomposeScope::Observe(const std::shared_ptr<StateCellBase>& cell) {
  dependencies_[cell.get()] = cell;
  cell->subscribers[id_] = shared_from_this();
}

void RecomposeScope::Invalidate() {
  if (dirty_) {
    return;
  }
  dirty_ = true;
  runtime_->InvalidateScope(id_);
}

void RecomposeScope::SetEventBindings(EventBindings bindings) {
  event_hub_->SetBindings(std::move(bindings));
}

std::shared_ptr<StateCellBase> RecomposeScope::UseState(std::type_index type,
                                                        const std::source_location& location,
                                                        std::shared_ptr<StateCellBase> initial) {
  StateSlotKey key{
      location.file_name(), location.function_name(), location.line(), location.column(), 0,
  };
  key.occurrence = state_slot_occurrences_[key]++;
  touched_state_slots_.insert(key);

  auto found = state_slots_.slots.find(key);
  if (found != state_slots_.slots.end()) {
    if (found->second->Type() != type) {
      throw std::logic_error("UseState() value type changed at the same call site");
    }
    return found->second;
  }

  state_slots_.slots.emplace(std::move(key), initial);
  return initial;
}

thread_local Composer* Composer::current_ = nullptr;

Composer::Composer(
    std::shared_ptr<RecomposeScope> scope,
    std::shared_ptr<const EnvironmentFrame> environment)
    : scope_(std::move(scope)), environment_(std::move(environment)) {}

Composer* Composer::Current() noexcept { return current_; }

Composer& Composer::RequireCurrent() {
  if (current_ == nullptr) {
    throw std::logic_error("UseState() must be called while HuxerUI is composing a view");
  }
  return *current_;
}

void Composer::Observe(const std::shared_ptr<StateCellBase>& cell) { scope_->Observe(cell); }

std::shared_ptr<StateCellBase> Composer::UseState(std::type_index type,
                                                  const std::source_location& location,
                                                  std::shared_ptr<StateCellBase> initial) {
  return scope_->UseState(type, location, std::move(initial));
}

std::shared_ptr<EventHub> Composer::Events() const noexcept { return scope_->Events(); }

Composer::EnvironmentGuard::EnvironmentGuard(
    std::shared_ptr<const EnvironmentFrame> environment)
    : composer_(&Composer::RequireCurrent()),
      previous_(composer_->environment_) {
  composer_->environment_ = std::move(environment);
}

Composer::EnvironmentGuard::~EnvironmentGuard() {
  composer_->environment_ = std::move(previous_);
}

Composer::Guard::Guard(Composer& composer) : previous_(current_) { current_ = &composer; }

Composer::Guard::~Guard() { current_ = previous_; }

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

std::shared_ptr<StateCellBase> UseStateCell(std::type_index type,
                                            const std::source_location& location,
                                            std::shared_ptr<StateCellBase> initial) {
  return Composer::RequireCurrent().UseState(type, location, std::move(initial));
}

std::shared_ptr<EventHub> UseEventHub() { return Composer::RequireCurrent().Events(); }

}  // namespace huxerui::detail
