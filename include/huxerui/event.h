#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <huxerui/geometry.h>

namespace huxerui {

enum class PointerEventType {
  Down,
  Up,
  Move,
  Cancel,
};

struct PointerEvent {
  PointerEventType type = PointerEventType::Move;
  std::int64_t pointer_id = 0;
  Point position;
};

struct ScrollEvent {
  Point position;
  float delta_x = 0.0F;
  float delta_y = 0.0F;
};

enum class Key {
  Unknown,
  Tab,
  Enter,
  Space,
  Escape,
  Backspace,
  Delete,
  ArrowLeft,
  ArrowRight,
  ArrowUp,
  ArrowDown,
  Home,
  End,
  PageUp,
  PageDown,
};

enum class KeyEventType {
  Down,
  Up,
};

struct KeyModifiers {
  bool shift = false;
  bool control = false;
  bool alt = false;
  bool meta = false;
};

struct KeyEvent {
  KeyEventType type = KeyEventType::Down;
  Key key = Key::Unknown;
  std::string text;
  KeyModifiers modifiers;
  bool repeat = false;
};

template <class Owner, class EventSignature> struct Event {
  using OwnerType = Owner;
  using Signature = EventSignature;
};

struct ViewEvents {
  struct Click : Event<ViewEvents, void()> {};
  struct PointerDown : Event<ViewEvents, void(const PointerEvent &)> {};
  struct PointerMove : Event<ViewEvents, void(const PointerEvent &)> {};
  struct PointerUp : Event<ViewEvents, void(const PointerEvent &)> {};
  struct PointerCancel : Event<ViewEvents, void(const PointerEvent &)> {};
  struct FocusChanged : Event<ViewEvents, void(bool)> {};
  struct KeyDown : Event<ViewEvents, void(const KeyEvent &)> {};
  struct KeyUp : Event<ViewEvents, void(const KeyEvent &)> {};
};

namespace detail {

template <class Key>
concept EventKey = requires {
  typename Key::OwnerType;
  typename Key::Signature;
};

class EventHandlerBase {
public:
  virtual ~EventHandlerBase() = default;
};

template <class Signature> class EventHandler;

template <class... Arguments>
class EventHandler<void(Arguments...)> final : public EventHandlerBase {
public:
  explicit EventHandler(std::function<void(Arguments...)> function)
      : function_(std::move(function)) {}

  template <class... Values> void Invoke(Values&&... values) const {
    function_(std::forward<Values>(values)...);
  }

private:
  std::function<void(Arguments...)> function_;
};

using EventBindings = std::unordered_map<std::type_index, std::shared_ptr<EventHandlerBase>>;

template <class Key>
bool HasEventBinding(const EventBindings &bindings) {
  return bindings.contains(typeid(Key));
}

template <class Key, class... Arguments>
bool EmitEvent(const EventBindings &bindings, Arguments&&... arguments) {
  const auto found = bindings.find(typeid(Key));
  if (found == bindings.end()) {
    return false;
  }
  const auto handler =
      std::dynamic_pointer_cast<EventHandler<typename Key::Signature>>(
          found->second);
  if (!handler) {
    return false;
  }
  handler->Invoke(std::forward<Arguments>(arguments)...);
  return true;
}

class EventHub {
public:
  void SetBindings(EventBindings bindings) { bindings_ = std::move(bindings); }

  template <class Key, class... Arguments> void Emit(Arguments&&... arguments) const {
    EmitEvent<Key>(bindings_, std::forward<Arguments>(arguments)...);
  }

private:
  EventBindings bindings_;
};

std::shared_ptr<EventHub> UseEventHub();

}  // namespace detail

template <class Owner> class EventEmitter {
public:
  EventEmitter() = default;

  template <class Key, class... Arguments>
    requires detail::EventKey<Key> && std::same_as<typename Key::OwnerType, Owner> &&
             std::invocable<std::function<typename Key::Signature>&, Arguments...>
  void Emit(Arguments&&... arguments) const {
    if (auto hub = hub_.lock()) {
      hub->template Emit<Key>(std::forward<Arguments>(arguments)...);
    }
  }

  [[nodiscard]] bool IsConnected() const noexcept { return !hub_.expired(); }

private:
  explicit EventEmitter(std::shared_ptr<detail::EventHub> hub) : hub_(std::move(hub)) {}

  std::weak_ptr<detail::EventHub> hub_;

  template <class EventOwner> friend EventEmitter<EventOwner> UseEvents();
};

template <class Owner> EventEmitter<Owner> UseEvents() {
  return EventEmitter<Owner>{detail::UseEventHub()};
}

}  // namespace huxerui
