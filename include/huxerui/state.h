#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace huxerui {

namespace detail {

class RecomposeScope;

class StateCellBase {
public:
  virtual ~StateCellBase() = default;

  [[nodiscard]] virtual std::type_index Type() const noexcept = 0;

  std::uint64_t version = 0;
  std::unordered_map<std::uint64_t, std::weak_ptr<RecomposeScope>> subscribers;
};

template <class T> class StateCell final : public StateCellBase {
public:
  template <class U> explicit StateCell(U&& initial) : value(std::forward<U>(initial)) {}

  [[nodiscard]] std::type_index Type() const noexcept override {
    return typeid(T);
  }

  T value;
};

void ObserveState(const std::shared_ptr<StateCellBase>& cell);
void NotifyState(const std::shared_ptr<StateCellBase>& cell);

std::shared_ptr<StateCellBase>
UseStateCell(std::type_index type, const std::source_location& location, std::shared_ptr<StateCellBase> initial);

} // namespace detail

template <class T> class State {
public:
  using ValueType = T;

  State() = default;

  explicit State(std::shared_ptr<detail::StateCell<T>> cell) : cell_(std::move(cell)) {}

  State(const State&) = default;
  State(State&&) noexcept = default;
  State& operator=(const State&) = default;
  State& operator=(State&&) noexcept = default;

  [[nodiscard]] const T& Get() const {
    EnsureValid();
    detail::ObserveState(cell_);
    return cell_->value;
  }

  const State& operator=(T value) const {
    Write(std::move(value));
    return *this;
  }

  template <class Function> void Update(Function&& function) const {
    EnsureValid();
    if constexpr (std::copy_constructible<T> && std::assignable_from<T&, T>) {
      T next = cell_->value;
      std::forward<Function>(function)(next);
      Write(std::move(next));
    } else {
      std::forward<Function>(function)(cell_->value);
      CommitMutation();
    }
  }

  [[nodiscard]] bool IsValid() const noexcept {
    return static_cast<bool>(cell_);
  }

  [[nodiscard]] operator const T&() const {
    return Get();
  }

  [[nodiscard]] const T* operator->() const {
    return &Get();
  }

  template <class U>
  const State& operator+=(U&& value) const
    requires requires(const T& current, U&& delta) { T(current + std::forward<U>(delta)); }
  {
    Write(T(Get() + std::forward<U>(value)));
    return *this;
  }

  template <class U>
  const State& operator-=(U&& value) const
    requires requires(T& current, U&& delta) { current -= std::forward<U>(delta); }
  {
    Update([&](T& current) { current -= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator*=(U&& value) const
    requires requires(T& current, U&& factor) { current *= std::forward<U>(factor); }
  {
    Update([&](T& current) { current *= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator/=(U&& value) const
    requires requires(T& current, U&& divisor) { current /= std::forward<U>(divisor); }
  {
    Update([&](T& current) { current /= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator%=(U&& value) const
    requires requires(T& current, U&& divisor) { current %= std::forward<U>(divisor); }
  {
    Update([&](T& current) { current %= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator&=(U&& value) const
    requires requires(T& current, U&& mask) { current &= std::forward<U>(mask); }
  {
    Update([&](T& current) { current &= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator|=(U&& value) const
    requires requires(T& current, U&& mask) { current |= std::forward<U>(mask); }
  {
    Update([&](T& current) { current |= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator^=(U&& value) const
    requires requires(T& current, U&& mask) { current ^= std::forward<U>(mask); }
  {
    Update([&](T& current) { current ^= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator<<=(U&& value) const
    requires requires(T& current, U&& count) { current <<= std::forward<U>(count); }
  {
    Update([&](T& current) { current <<= std::forward<U>(value); });
    return *this;
  }

  template <class U>
  const State& operator>>=(U&& value) const
    requires requires(T& current, U&& count) { current >>= std::forward<U>(count); }
  {
    Update([&](T& current) { current >>= std::forward<U>(value); });
    return *this;
  }

  const State& operator++() const
    requires requires(T& value) { ++value; }
  {
    Update([](T& value) { ++value; });
    return *this;
  }

  T operator++(int) const
    requires std::copy_constructible<T> && requires(T& value) { ++value; }
  {
    return UpdateAndReturnPrevious([](T& value) { ++value; });
  }

  const State& operator--() const
    requires requires(T& value) { --value; }
  {
    Update([](T& value) { --value; });
    return *this;
  }

  T operator--(int) const
    requires std::copy_constructible<T> && requires(T& value) { --value; }
  {
    return UpdateAndReturnPrevious([](T& value) { --value; });
  }

private:
  template <class Function> T UpdateAndReturnPrevious(Function&& function) const {
    EnsureValid();
    T previous = cell_->value;
    std::forward<Function>(function)(cell_->value);
    if constexpr (std::equality_comparable<T>) {
      if (previous == cell_->value) {
        return previous;
      }
    }
    CommitMutation();
    return previous;
  }

  void CommitMutation() const {
    ++cell_->version;
    detail::NotifyState(cell_);
  }

  void Write(T value) const {
    EnsureValid();
    if constexpr (std::equality_comparable<T>) {
      if (cell_->value == value) {
        return;
      }
    }
    cell_->value = std::move(value);
    CommitMutation();
  }

  void EnsureValid() const {
    if (!cell_) {
      throw std::logic_error("HuxerUI state is empty");
    }
  }

  std::shared_ptr<detail::StateCell<T>> cell_;
};

template <class T>
State<std::decay_t<T>> UseState(T&& initial, const std::source_location& location = std::source_location::current()) {
  using Value = std::decay_t<T>;
  auto candidate = std::make_shared<detail::StateCell<Value>>(std::forward<T>(initial));
  auto cell = detail::UseStateCell(typeid(Value), location, std::move(candidate));
  auto typed_cell = std::dynamic_pointer_cast<detail::StateCell<Value>>(cell);
  if (!typed_cell) {
    throw std::logic_error("UseState() value type changed at the same call site");
  }
  return State<Value>{std::move(typed_cell)};
}

} // namespace huxerui
