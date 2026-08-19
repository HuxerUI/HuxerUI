#pragma once

#include <algorithm>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

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

template <class T> class StateListCell final : public StateCellBase {
public:
  explicit StateListCell(std::vector<T> initial) : values(std::move(initial)) {}

  [[nodiscard]] std::type_index Type() const noexcept override {
    return typeid(StateListCell<T>);
  }

  std::vector<T> values;
};

template <std::ranges::input_range Range>
  requires(
      (std::is_lvalue_reference_v<Range&&> &&
       std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>>) ||
      (!std::is_lvalue_reference_v<Range&&> &&
       std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_rvalue_reference_t<Range>>)
  )
std::vector<std::ranges::range_value_t<Range>> CollectStateListValues(Range&& range) {
  using Value = std::ranges::range_value_t<Range>;
  std::vector<Value> values;
  if constexpr (std::ranges::sized_range<Range>) {
    values.reserve(static_cast<std::size_t>(std::ranges::size(range)));
  }
  if constexpr (std::is_lvalue_reference_v<Range&&>) {
    for (auto&& value : range) {
      values.emplace_back(value);
    }
  } else {
    auto iterator = std::ranges::begin(range);
    const auto end = std::ranges::end(range);
    while (iterator != end) {
      values.emplace_back(std::ranges::iter_move(iterator));
      ++iterator;
    }
  }
  return values;
}

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

template <class T> class StateList {
public:
  using ValueType = T;
  using ConstIterator = typename std::vector<T>::const_iterator;

  StateList() = default;

  explicit StateList(std::shared_ptr<detail::StateListCell<T>> cell) : cell_(std::move(cell)) {}

  StateList(const StateList&) = default;
  StateList(StateList&&) noexcept = default;
  StateList& operator=(const StateList&) = default;
  StateList& operator=(StateList&&) noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept {
    return static_cast<bool>(cell_);
  }

  [[nodiscard]] std::size_t Size() const {
    Observe();
    return cell_->values.size();
  }

  [[nodiscard]] bool Empty() const {
    Observe();
    return cell_->values.empty();
  }

  [[nodiscard]] const T& At(std::size_t index) const {
    Observe();
    EnsureIndex(index);
    return cell_->values[index];
  }

  [[nodiscard]] const T& operator[](std::size_t index) const {
    return At(index);
  }

  [[nodiscard]] ConstIterator begin() const {
    Observe();
    return cell_->values.cbegin();
  }

  [[nodiscard]] ConstIterator end() const {
    Observe();
    return cell_->values.cend();
  }

  void PushBack(T value) const {
    EnsureValid();
    cell_->values.push_back(std::move(value));
    NotifyChanged();
  }

  void Insert(std::size_t index, T value) const {
    EnsureValid();
    if (index > cell_->values.size()) {
      throw std::out_of_range("HuxerUI StateList insertion index is out of range");
    }
    cell_->values.insert(cell_->values.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
    NotifyChanged();
  }

  void Set(std::size_t index, T value) const {
    EnsureValid();
    EnsureIndex(index);
    if constexpr (std::equality_comparable<T>) {
      if (cell_->values[index] == value) {
        return;
      }
    }
    cell_->values[index] = std::move(value);
    NotifyChanged();
  }

  void Erase(std::size_t index) const {
    EnsureValid();
    EnsureIndex(index);
    cell_->values.erase(cell_->values.begin() + static_cast<std::ptrdiff_t>(index));
    NotifyChanged();
  }

  void Move(std::size_t from, std::size_t to) const {
    EnsureValid();
    EnsureIndex(from);
    EnsureIndex(to);
    if (from == to) {
      return;
    }
    const auto begin = cell_->values.begin();
    const auto source = begin + static_cast<std::ptrdiff_t>(from);
    const auto destination = begin + static_cast<std::ptrdiff_t>(to);
    if (from < to) {
      std::rotate(source, source + 1, destination + 1);
    } else {
      std::rotate(destination, source, source + 1);
    }
    NotifyChanged();
  }

  void PopBack() const {
    EnsureValid();
    if (cell_->values.empty()) {
      throw std::out_of_range("HuxerUI StateList cannot pop an empty list");
    }
    cell_->values.pop_back();
    NotifyChanged();
  }

  void Clear() const {
    EnsureValid();
    if (cell_->values.empty()) {
      return;
    }
    cell_->values.clear();
    NotifyChanged();
  }

private:
  void Observe() const {
    EnsureValid();
    detail::ObserveState(cell_);
  }

  void NotifyChanged() const {
    ++cell_->version;
    detail::NotifyState(cell_);
  }

  void EnsureValid() const {
    if (!cell_) {
      throw std::logic_error("HuxerUI StateList is invalid");
    }
  }

  void EnsureIndex(std::size_t index) const {
    if (index >= cell_->values.size()) {
      throw std::out_of_range("HuxerUI StateList index is out of range");
    }
  }

  std::shared_ptr<detail::StateListCell<T>> cell_;
};

namespace detail {

template <class T> StateList<T> UseStateListValues(std::vector<T> initial, const std::source_location& location) {
  auto candidate = std::make_shared<StateListCell<T>>(std::move(initial));
  auto cell = UseStateCell(typeid(StateListCell<T>), location, std::move(candidate));
  return StateList<T>{std::static_pointer_cast<StateListCell<T>>(std::move(cell))};
}

} // namespace detail

template <class T>
State<std::decay_t<T>> UseState(T&& initial, const std::source_location& location = std::source_location::current()) {
  using Value = std::decay_t<T>;
  auto candidate = std::make_shared<detail::StateCell<Value>>(std::forward<T>(initial));
  auto cell = detail::UseStateCell(typeid(Value), location, std::move(candidate));
  return State<Value>{std::static_pointer_cast<detail::StateCell<Value>>(std::move(cell))};
}

template <class T> StateList<T> UseStateList(const std::source_location& location = std::source_location::current()) {
  return detail::UseStateListValues<T>({}, location);
}

template <std::ranges::input_range Range>
  requires requires(Range&& range) { detail::CollectStateListValues(std::forward<Range>(range)); }
StateList<std::ranges::range_value_t<Range>>
UseStateList(Range&& initial, const std::source_location& location = std::source_location::current()) {
  using Value = std::ranges::range_value_t<Range>;
  return detail::UseStateListValues<Value>(detail::CollectStateListValues(std::forward<Range>(initial)), location);
}

template <class T>
StateList<T>
UseStateList(std::initializer_list<T> initial, const std::source_location& location = std::source_location::current()) {
  return detail::UseStateListValues<T>(std::vector<T>(initial), location);
}

} // namespace huxerui
