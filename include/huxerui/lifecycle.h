#pragma once

#include <any>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <huxerui/state.h>

namespace huxerui {

namespace detail {

class LifecycleCleanup {
public:
  LifecycleCleanup() = default;

  template <class Function>
    requires(!std::same_as<std::remove_cvref_t<Function>, LifecycleCleanup> &&
             std::move_constructible<std::decay_t<Function>> &&
             std::same_as<std::invoke_result_t<std::decay_t<Function>&>, void>)
  explicit LifecycleCleanup(Function&& function)
      : function_(std::make_unique<Model<std::decay_t<Function>>>(std::forward<Function>(function))) {}

  LifecycleCleanup(const LifecycleCleanup&) = delete;
  LifecycleCleanup& operator=(const LifecycleCleanup&) = delete;
  LifecycleCleanup(LifecycleCleanup&&) noexcept = default;
  LifecycleCleanup& operator=(LifecycleCleanup&&) noexcept = default;

  void Run() noexcept {
    if (function_) {
      function_->Run();
      function_.reset();
    }
  }

private:
  class Concept {
  public:
    virtual ~Concept() = default;
    virtual void Run() noexcept = 0;
  };

  template <class Function> class Model final : public Concept {
  public:
    explicit Model(Function function) : function_(std::move(function)) {}

    void Run() noexcept override {
      std::invoke(function_);
    }

  private:
    Function function_;
  };

  std::unique_ptr<Concept> function_;
};

template <class Function>
concept LifecycleSetupFunction = std::move_constructible<Function> && std::invocable<Function&> &&
                                 (std::is_void_v<std::invoke_result_t<Function&>> ||
                                  (std::move_constructible<std::invoke_result_t<Function&>> &&
                                   std::same_as<std::invoke_result_t<std::invoke_result_t<Function&>&>, void>));

class LifecycleSetup {
public:
  template <class Function>
    requires(!std::same_as<std::remove_cvref_t<Function>, LifecycleSetup> &&
             LifecycleSetupFunction<std::decay_t<Function>>)
  LifecycleSetup(Function&& function, const std::source_location& location = std::source_location::current())
      : function_(std::make_unique<Model<std::decay_t<Function>>>(std::forward<Function>(function))),
        location_(location) {}

  LifecycleSetup(const LifecycleSetup&) = delete;
  LifecycleSetup& operator=(const LifecycleSetup&) = delete;
  LifecycleSetup(LifecycleSetup&&) noexcept = default;
  LifecycleSetup& operator=(LifecycleSetup&&) noexcept = default;

  [[nodiscard]] const std::source_location& Location() const noexcept {
    return location_;
  }

  LifecycleCleanup Run() {
    return function_->Run();
  }

private:
  class Concept {
  public:
    virtual ~Concept() = default;
    virtual LifecycleCleanup Run() = 0;
  };

  template <class Function> class Model final : public Concept {
  public:
    explicit Model(Function function) : function_(std::move(function)) {}

    LifecycleCleanup Run() override {
      if constexpr (std::is_void_v<std::invoke_result_t<Function&>>) {
        std::invoke(function_);
        return {};
      } else {
        return LifecycleCleanup(std::invoke(function_));
      }
    }

  private:
    Function function_;
  };

  std::unique_ptr<Concept> function_;
  std::source_location location_;
};

class LifecycleDependency {
public:
  template <class T>
    requires std::copy_constructible<std::decay_t<T>> && std::equality_comparable<std::decay_t<T>>
  static LifecycleDependency Value(T&& value) {
    using Value = std::decay_t<T>;
    return LifecycleDependency(
        std::any(Value(std::forward<T>(value))),
        [](const std::any& left, const std::any& right) {
          return std::any_cast<const Value&>(left) == std::any_cast<const Value&>(right);
        }
    );
  }

  [[nodiscard]] bool operator==(const LifecycleDependency& other) const {
    return value_.type() == other.value_.type() && equals_(value_, other.value_);
  }

private:
  LifecycleDependency(std::any value, bool (*equals)(const std::any&, const std::any&))
      : value_(std::move(value)), equals_(equals) {}

  std::any value_;
  bool (*equals_)(const std::any&, const std::any&);
};

struct LifecycleStateDependency {
  std::shared_ptr<StateCellBase> cell;
  std::uint64_t version = 0;

  bool operator==(const LifecycleStateDependency& other) const noexcept {
    return cell == other.cell && version == other.version;
  }
};

struct LifecycleDependencyAccess {
  template <class T> static std::shared_ptr<StateCellBase> Cell(const State<T>& state) {
    if (!state.cell_) {
      throw std::logic_error("HuxerUI Lifecycle dependency State is empty");
    }
    return state.cell_;
  }

  template <class T> static std::shared_ptr<StateCellBase> Cell(const StateList<T>& state) {
    if (!state.cell_) {
      throw std::logic_error("HuxerUI Lifecycle dependency StateList is invalid");
    }
    return state.cell_;
  }
};

template <class T> struct IsLifecycleState : std::false_type {};
template <class T> struct IsLifecycleState<State<T>> : std::true_type {};
template <class T> struct IsLifecycleState<StateList<T>> : std::true_type {};

template <class T> LifecycleDependency CaptureLifecycleDependency(const State<T>& state) {
  auto cell = LifecycleDependencyAccess::Cell(state);
  ObserveState(cell);
  return LifecycleDependency::Value(LifecycleStateDependency{cell, cell->version});
}

template <class T> LifecycleDependency CaptureLifecycleDependency(const StateList<T>& state) {
  auto cell = LifecycleDependencyAccess::Cell(state);
  ObserveState(cell);
  return LifecycleDependency::Value(LifecycleStateDependency{cell, cell->version});
}

template <class T>
  requires(
      !IsLifecycleState<std::remove_cvref_t<T>>::value && std::copy_constructible<std::decay_t<T>> &&
      std::equality_comparable<std::decay_t<T>>
  )
LifecycleDependency CaptureLifecycleDependency(T&& value) {
  return LifecycleDependency::Value(std::forward<T>(value));
}

void RegisterLifecycle(LifecycleSetup setup, std::vector<LifecycleDependency> dependencies);

} // namespace detail

template <class... Dependencies>
  requires requires(Dependencies&&... dependencies) {
    (detail::CaptureLifecycleDependency(std::forward<Dependencies>(dependencies)), ...);
  }
void Lifecycle(detail::LifecycleSetup setup, Dependencies&&... dependencies) {
  std::vector<detail::LifecycleDependency> captured;
  captured.reserve(sizeof...(Dependencies));
  (captured.push_back(detail::CaptureLifecycleDependency(std::forward<Dependencies>(dependencies))), ...);
  detail::RegisterLifecycle(std::move(setup), std::move(captured));
}

} // namespace huxerui
