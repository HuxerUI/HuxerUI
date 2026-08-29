#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <huxerui/geometry.h>

namespace huxerui {

struct TextEditingValue;

struct InteractionState {
  bool enabled = true;
  bool hovered = false;
  bool focused = false;
  bool focus_visible = false;
  // Aggregate state for every active pointer or keyboard Press owned by this node.
  bool pressed = false;

  bool operator==(const InteractionState&) const = default;
};

struct InteractionEvent {
  enum class Type {
    Press,
    Release,
    Cancel,
  };

  enum class Source {
    Pointer,
    Keyboard,
  };

  Type type = Type::Press;
  Source source = Source::Pointer;
  // Release and Cancel retain the identifier allocated for their matching Press.
  std::uint64_t press_id = 0;
  // Pointer positions are node-local; keyboard interactions have no spatial origin.
  std::optional<Point> position;

  bool operator==(const InteractionEvent&) const = default;
};

enum class PointerEventType {
  Down,
  Up,
  Move,
  Cancel,
};

enum class PointerDeviceKind {
  Mouse,
  Touch,
  Pen,
};

struct PointerEvent {
  PointerEventType type = PointerEventType::Move;
  std::int64_t pointer_id = 0;
  Point position;
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  std::uint32_t click_count = 1;

  bool operator==(const PointerEvent&) const = default;
};

struct ScrollEvent {
  Point position;
  float delta_x = 0.0F;
  float delta_y = 0.0F;

  bool operator==(const ScrollEvent&) const = default;
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
  A,
  C,
  V,
  X,
  Y,
  Z,
  Shift,
  Control,
  Alt,
  Meta,
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

  bool operator==(const KeyModifiers&) const = default;
};

struct KeyEvent {
  KeyEventType type = KeyEventType::Down;
  Key key = Key::Unknown;
  std::string text;
  KeyModifiers modifiers;
  bool repeat = false;

  bool operator==(const KeyEvent&) const = default;
};

enum class BackPhase {
  Begin,
  Update,
  Cancel,
  Commit,
};

struct BackEvent {
  BackPhase phase = BackPhase::Commit;
  float progress = 1.0F;

  bool operator==(const BackEvent&) const = default;
};

template <class... Arguments> struct Event {
  using Signature = void(Arguments...);
};

struct ViewEvents {
  struct Click : Event<> {};
  struct PointerDown : Event<const PointerEvent&> {};
  struct PointerMove : Event<const PointerEvent&> {};
  struct PointerUp : Event<const PointerEvent&> {};
  struct PointerCancel : Event<const PointerEvent&> {};
  struct FocusChanged : Event<bool> {};
  struct KeyDown : Event<const KeyEvent&> {};
  struct KeyUp : Event<const KeyEvent&> {};
  struct BackRequested : Event<> {};
};

struct ToggleEvents {
  struct Changed : Event<bool> {};
};

struct SliderEvents {
  struct Changed : Event<float> {};
};

struct SegmentedButtonEvents {
  struct Changed : Event<std::size_t> {};
};

struct TabsEvents {
  struct Changed : Event<std::size_t> {};
};

struct SelectEvents {
  struct Changed : Event<std::size_t> {};
};

struct NavigationEvents {
  struct Changed : Event<std::size_t> {};
};

struct DrawerEvents {
  struct OpenChanged : Event<bool> {};
};

struct TextFieldEvents {
  struct Changed : Event<const TextEditingValue&> {};
  struct Submitted : Event<> {};
};

namespace detail {

template <class Key>
concept EventKey = requires { typename Key::Signature; };

class EventHandlerBase {
public:
  virtual ~EventHandlerBase() = default;
};

template <class Signature> class EventHandler;

template <class... Arguments> class EventHandler<void(Arguments...)> final : public EventHandlerBase {
public:
  explicit EventHandler(std::function<void(Arguments...)> function) : function_(std::move(function)) {}

  template <class... Values> void Invoke(Values&&... values) const {
    function_(std::forward<Values>(values)...);
  }

private:
  std::function<void(Arguments...)> function_;
};

using EventBindings = std::unordered_map<std::type_index, std::shared_ptr<EventHandlerBase>>;

template <class Key> bool HasEventBinding(const EventBindings& bindings) {
  return bindings.contains(typeid(Key));
}

template <class Key, class... Arguments> bool EmitEvent(const EventBindings& bindings, Arguments&&... arguments) {
  const auto found = bindings.find(typeid(Key));
  if (found == bindings.end()) {
    return false;
  }
  const auto handler = std::dynamic_pointer_cast<EventHandler<typename Key::Signature>>(found->second);
  if (!handler) {
    return false;
  }
  handler->Invoke(std::forward<Arguments>(arguments)...);
  return true;
}

class EventHub {
public:
  void SetBindings(EventBindings bindings) {
    bindings_ = std::move(bindings);
  }

  template <class Key, class... Arguments> void Emit(Arguments&&... arguments) const {
    EmitEvent<Key>(bindings_, std::forward<Arguments>(arguments)...);
  }

private:
  EventBindings bindings_;
};

std::shared_ptr<EventHub> UseEventHub();

} // namespace detail

class EventEmitter {
public:
  EventEmitter() = default;

  template <class Key, class... Arguments>
    requires detail::EventKey<Key> && std::invocable<std::function<typename Key::Signature>&, Arguments...>
  void Emit(Arguments&&... arguments) const {
    if (auto hub = hub_.lock()) {
      hub->template Emit<Key>(std::forward<Arguments>(arguments)...);
    }
  }

  [[nodiscard]] bool IsConnected() const noexcept {
    return !hub_.expired();
  }

private:
  explicit EventEmitter(std::shared_ptr<detail::EventHub> hub) : hub_(std::move(hub)) {}

  std::weak_ptr<detail::EventHub> hub_;

  friend EventEmitter UseEvents();
};

inline EventEmitter UseEvents() {
  return EventEmitter{detail::UseEventHub()};
}

} // namespace huxerui
