#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include <huxerui/geometry.h>

namespace huxerui {

/// Stores the complete controlled text value emitted by TextFieldEvents::Changed.
struct TextEditingValue;

/// Describes the effective interaction state of one mounted View.
///
/// The snapshot combines inherited enabled state with hover, focus, and active press state. Component-owned states such
/// as selected, checked, loading, and validation are intentionally separate.
struct InteractionState {
  /// Whether the View and all of its interaction ancestors are enabled.
  bool enabled = true;
  /// Whether a hover-capable pointer currently targets the View.
  bool hovered = false;
  /// Whether the View owns keyboard focus.
  bool focused = false;
  /// Whether focus indication should currently be visible.
  bool focus_visible = false;
  /// Whether at least one active pointer or keyboard press is owned by the View.
  bool pressed = false;

  bool operator==(const InteractionState&) const = default;
};

/// Describes an ordered press transition accompanying an InteractionState snapshot.
struct InteractionEvent {
  /// Identifies the transition within a press lifecycle.
  enum class Type {
    /// Begins a press.
    Press,
    /// Completes a press successfully.
    Release,
    /// Aborts a press without activation.
    Cancel,
  };

  /// Identifies the input source that created the press.
  enum class Source {
    /// A mouse, touch, or pen pointer.
    Pointer,
    /// A keyboard activation key.
    Keyboard,
  };

  /// Current transition type.
  Type type = Type::Press;
  /// Input source for the complete press lifecycle.
  Source source = Source::Pointer;
  /// Runtime-unique identifier retained by the matching Release or Cancel transition.
  std::uint64_t press_id = 0;
  /// Node-local pointer position, or an empty value for keyboard interaction.
  std::optional<Point> position;

  bool operator==(const InteractionEvent&) const = default;
};

/// Identifies the lifecycle phase of a PointerEvent.
enum class PointerEventType {
  /// Presses one pointer button and begins a sequence when no button is active.
  Down,
  /// Releases one pointer button and completes a sequence when no button remains active.
  Up,
  /// Reports movement or hover without ending the sequence.
  Move,
  /// Aborts a pointer sequence.
  Cancel,
};

/// Identifies the physical class of pointer device.
enum class PointerDeviceKind {
  /// A mouse or mouse-compatible pointing device.
  Mouse,
  /// A direct touch contact.
  Touch,
  /// A pen or stylus.
  Pen,
};

/// Identifies one phase of hover-capable pointer presence over a View.
enum class HoverEventType {
  /// The pointer entered the View's presented bounds.
  Enter,
  /// The pointer moved while remaining within the View's presented bounds.
  Move,
  /// The pointer left the View's presented bounds or hover tracking was canceled.
  Leave,
};

/// Identifies a portable pointer cursor requested by a View.
///
/// Platforms map each value to the closest native cursor they provide. A PlatformView keeps ownership of the cursor
/// shown over its native content.
enum class PointerCursorKind {
  /// Uses the platform's ordinary cursor for the current surface.
  Default,
  /// Indicates selectable or editable text.
  Text,
  /// Indicates a link or another directly invokable target.
  Hand,
  /// Indicates precise point selection.
  Crosshair,
  /// Indicates content that can move in any direction.
  Move,
  /// Indicates content that can be grabbed.
  Grab,
  /// Indicates content currently being grabbed.
  Grabbing,
  /// Indicates horizontal resizing.
  ResizeHorizontal,
  /// Indicates vertical resizing.
  ResizeVertical,
  /// Indicates resizing along the northeast-to-southwest diagonal.
  ResizeNorthEastSouthWest,
  /// Indicates resizing along the northwest-to-southeast diagonal.
  ResizeNorthWestSouthEast,
  /// Indicates that the requested operation is unavailable.
  NotAllowed,
  /// Indicates that the application is busy.
  Wait,
};

/// Identifies portable pointer buttons and pressed-button combinations.
///
/// Primary and Secondary follow the operating system's semantic button roles rather than fixed physical positions.
enum class PointerButton : std::uint32_t {
  /// No button.
  None = 0,
  /// The primary selection and activation button.
  Primary = 1U << 0U,
  /// The secondary context-menu button.
  Secondary = 1U << 1U,
  /// The middle or wheel button.
  Middle = 1U << 2U,
  /// The backward navigation button.
  Back = 1U << 3U,
  /// The forward navigation button.
  Forward = 1U << 4U,
};

/// Combines pointer-button flags.
[[nodiscard]] constexpr PointerButton operator|(PointerButton left, PointerButton right) noexcept {
  return static_cast<PointerButton>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

/// Intersects pointer-button flags.
[[nodiscard]] constexpr PointerButton operator&(PointerButton left, PointerButton right) noexcept {
  return static_cast<PointerButton>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

/// Adds pointer-button flags to an existing mask.
constexpr PointerButton& operator|=(PointerButton& left, PointerButton right) noexcept {
  left = left | right;
  return left;
}

/// Retains pointer-button flags present in both masks.
constexpr PointerButton& operator&=(PointerButton& left, PointerButton right) noexcept {
  left = left & right;
  return left;
}

/// Carries normalized pointer input in logical coordinates.
///
/// The receiving API defines the coordinate space. Runtime input and ViewEvents use the window coordinate space, while
/// retained gesture APIs may provide a node-local position.
struct PointerEvent {
  /// Lifecycle phase represented by this event.
  PointerEventType type = PointerEventType::Move;
  /// Identifier shared by every event in one active pointer sequence.
  std::int64_t pointer_id = 0;
  /// Position in the logical coordinate space defined by the receiving API.
  Point position;
  /// Physical class of the source device.
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  /// Platform-reported consecutive click count, starting at one when unavailable.
  std::uint32_t click_count = 1;
  /// Button added by Down or removed by Up, or None for Move and Cancel.
  PointerButton changed_button = PointerButton::None;
  /// Complete pressed-button state after this event.
  PointerButton pressed_buttons = PointerButton::None;

  /// Returns whether every button in a nonempty flag mask is currently pressed.
  /// @code
  /// if (event.IsButtonPressed(PointerButton::Primary | PointerButton::Secondary)) {
  ///   BeginChord();
  /// }
  /// @endcode
  [[nodiscard]] constexpr bool IsButtonPressed(PointerButton button) const noexcept {
    return button != PointerButton::None && (pressed_buttons & button) == button;
  }

  bool operator==(const PointerEvent&) const = default;
};

/// Carries one mouse or pen hover update for a View.
///
/// `position` is local to the receiving View, while `window_position` remains stable when the same physical update is
/// delivered to nested Views. Touch input does not produce this event. Final presentation geometry can produce Enter
/// or Leave under a stationary pointer, while an unchanged host position does not produce another Move.
struct HoverEvent {
  /// Lifecycle phase represented by this event.
  HoverEventType type = HoverEventType::Move;
  /// Platform pointer identifier associated with the hover-capable device.
  std::int64_t pointer_id = 0;
  /// Mouse or pen device that produced the update.
  PointerDeviceKind device_kind = PointerDeviceKind::Mouse;
  /// Current position in the receiving View's local logical coordinate space.
  Point position;
  /// Current position in host-window logical coordinates.
  Point window_position;

  bool operator==(const HoverEvent&) const = default;
};

/// Carries a platform-recognized wheel or trackpad scroll update.
struct ScrollEvent {
  /// Pointer position in window logical coordinates.
  Point position;
  /// Horizontal scroll delta in logical units.
  float delta_x = 0.0F;
  /// Vertical scroll delta in logical units.
  float delta_y = 0.0F;

  bool operator==(const ScrollEvent&) const = default;
};

/// Identifies portable keyboard keys independently of layout-resolved text.
///
/// Use `KeyEvent::text` when the produced character matters. Main-row and numeric-keypad keys, left and right
/// modifiers, punctuation, international keys, and named function keys retain distinct identities.
enum class Key {
  /// A key without a portable mapping.
  Unknown,

  Backspace,
  Tab,
  Enter,
  Escape,
  Space,
  Insert,
  Delete,
  Home,
  End,
  PageUp,
  PageDown,
  ArrowLeft,
  ArrowRight,
  ArrowUp,
  ArrowDown,

  A,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,

  Digit0,
  Digit1,
  Digit2,
  Digit3,
  Digit4,
  Digit5,
  Digit6,
  Digit7,
  Digit8,
  Digit9,

  Backquote,
  Minus,
  Equal,
  BracketLeft,
  BracketRight,
  Backslash,
  Semicolon,
  Quote,
  Comma,
  Period,
  Slash,

  /// The layout-specific international backslash key.
  IntlBackslash,
  /// The Japanese Ro key.
  IntlRo,
  /// The Japanese Yen key.
  IntlYen,

  ShiftLeft,
  ShiftRight,
  ControlLeft,
  ControlRight,
  AltLeft,
  AltRight,
  MetaLeft,
  MetaRight,

  CapsLock,
  NumLock,
  ScrollLock,

  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  F13,
  F14,
  F15,
  F16,
  F17,
  F18,
  F19,
  F20,
  F21,
  F22,
  F23,
  F24,

  PrintScreen,
  Pause,
  ContextMenu,
  Help,

  Numpad0,
  Numpad1,
  Numpad2,
  Numpad3,
  Numpad4,
  Numpad5,
  Numpad6,
  Numpad7,
  Numpad8,
  Numpad9,

  NumpadDecimal,
  NumpadDivide,
  NumpadMultiply,
  NumpadSubtract,
  NumpadAdd,
  NumpadEnter,
  NumpadEqual,
  NumpadComma,
  /// The numeric keypad Clear key, distinct from ordinary deletion.
  NumpadClear,
};

/// Identifies whether a key is being pressed or released.
enum class KeyEventType {
  /// The key transitioned to the pressed state.
  Down,
  /// The key transitioned to the released state.
  Up,
};

/// Describes the modifier keys active for a KeyEvent.
struct KeyModifiers {
  /// Whether Shift is active.
  bool shift = false;
  /// Whether Control is active.
  bool control = false;
  /// Whether Alt or Option is active.
  bool alt = false;
  /// Whether the platform Meta, Command, or Windows modifier is active.
  bool meta = false;

  bool operator==(const KeyModifiers&) const = default;
};

/// Carries normalized keyboard input.
struct KeyEvent {
  /// Whether the key is being pressed or released.
  KeyEventType type = KeyEventType::Down;
  /// Portable key identity, or Key::Unknown when no mapping exists.
  Key key = Key::Unknown;
  /// UTF-8 text reported for a Down event, which may be empty and does not replace text-input services.
  /// Up events always carry an empty string.
  std::string text;
  /// Modifier state accompanying this event.
  KeyModifiers modifiers;
  /// Whether this Down event was generated by platform key repeat.
  bool repeat = false;

  bool operator==(const KeyEvent&) const = default;
};

/// Identifies one phase of a platform Back transaction.
enum class BackPhase {
  /// Begins a predictive Back transaction and selects its consumer.
  Begin,
  /// Updates the progress of the selected predictive Back consumer.
  Update,
  /// Cancels the selected Back transaction.
  Cancel,
  /// Commits Back immediately or completes a predictive transaction.
  Commit,
};

/// Carries a normalized platform Back request.
struct BackEvent {
  /// Current transaction phase.
  BackPhase phase = BackPhase::Commit;
  /// Normalized predictive progress in the inclusive range from zero to one.
  float progress = 1.0F;

  bool operator==(const BackEvent&) const = default;
};

/// Defines a typed event key by its complete handler signature.
///
/// Derive a distinct key type for each semantic event. Use `void` for notifications and a result type for a synchronous
/// decision:
/// @code
/// struct Submitted : Event<void(std::string)> {};
/// struct NavigationRequested : Event<bool(const std::string&)> {};
/// @endcode
template <class Function> struct Event {
  static_assert(std::is_function_v<Function>, "HuxerUI Event requires a complete function signature");

  /// Complete `Result(Arguments...)` handler signature carried by the event key.
  using Signature = Function;
};

/// Built-in event keys shared by ordinary Views.
struct ViewEvents {
  /// Reports semantic activation from a successful tap, keyboard action, or accessibility Invoke action.
  struct Click : Event<void()> {};
  /// Reports the start of the raw pointer stream targeting the deepest eligible View.
  struct PointerDown : Event<void(const PointerEvent&)> {};
  /// Reports movement in a raw pointer stream or an unowned hover update.
  struct PointerMove : Event<void(const PointerEvent&)> {};
  /// Reports successful completion of the raw pointer stream.
  struct PointerUp : Event<void(const PointerEvent&)> {};
  /// Reports that another recognizer or the platform canceled the raw pointer stream.
  struct PointerCancel : Event<void(const PointerEvent&)> {};
  /// Reports mouse or pen entry, movement, and departure without joining pointer-sequence ownership.
  ///
  /// Disabled Views participate. Nested bound Views receive independent direct lifecycles rather than a bubbled event,
  /// and a PlatformView owns hover over its native content.
  /// @code
  /// content.On<ViewEvents::Hover>([](const HoverEvent& event) {
  ///   SetHighlighted(event.type != HoverEventType::Leave);
  /// });
  /// @endcode
  struct Hover : Event<void(const HoverEvent&)> {};
  /// Requests a context menu at a window-local logical position.
  /// @code
  /// content.On<ViewEvents::ContextMenuRequested>([menu](Point position) {
  ///   menu.ShowAt(position, {MenuItem("Refresh", Refresh)});
  /// });
  /// @endcode
  struct ContextMenuRequested : Event<void(Point)> {};
  /// Observes a pointer sequence and returns true when this View takes exclusive ownership.
  /// False keeps the recognition pending. After acceptance, later return values are ignored and the handler continues
  /// receiving Move, Up, or Cancel until the sequence ends.
  struct PointerIntercept : Event<bool(const PointerEvent&)> {};
  /// Reports whether the View gained or lost keyboard focus.
  struct FocusChanged : Event<void(bool)> {};
  /// Offers a key event from the active focus scope before focused component and Runtime defaults.
  ///
  /// Runtime visits handlers from the active focus-scope root through the focused View, stopping when one returns true.
  /// Use this event for shortcuts or parent policy that must override a focused component. It is not a bubbling event.
  /// @code
  /// page.On<ViewEvents::KeyIntercept>([](const KeyEvent& event) {
  ///   if (event.type == KeyEventType::Down && event.modifiers.control && event.key == Key::S) {
  ///     Save();
  ///     return true;
  ///   }
  ///   return false;
  /// });
  /// @endcode
  struct KeyIntercept : Event<bool(const KeyEvent&)> {};
  /// Offers an otherwise unhandled key press to the focused View.
  ///
  /// Component-owned NodeExtension handling runs first. Return true to prevent Runtime and platform defaults, or use
  /// KeyIntercept when parent policy must run before the component.
  struct KeyDown : Event<bool(const KeyEvent&)> {};
  /// Offers an otherwise unhandled key release to the focused View.
  ///
  /// Component-owned NodeExtension handling runs first. Return true to prevent Runtime and platform defaults, or use
  /// KeyIntercept when parent policy must run before the component.
  struct KeyUp : Event<bool(const KeyEvent&)> {};
  /// Consumes a committed platform Back request and delegates the resulting action to the handler.
  struct BackRequested : Event<void()> {};
};

/// Event keys shared by controlled boolean selection components.
struct ToggleEvents {
  /// Requests a new checked or selected value.
  struct Changed : Event<void(bool)> {};
};

/// Event keys emitted by Slider.
struct SliderEvents {
  /// Requests a new constrained Slider value.
  struct Changed : Event<void(float)> {};
};

/// Event keys emitted by SegmentedButton.
struct SegmentedButtonEvents {
  /// Requests selection of the item at the supplied zero-based index.
  struct Changed : Event<void(std::size_t)> {};
};

/// Event keys emitted by Tabs.
struct TabsEvents {
  /// Requests selection of the tab at the supplied zero-based index.
  struct Changed : Event<void(std::size_t)> {};
};

/// Event keys emitted by Select.
struct SelectEvents {
  /// Requests selection of the option at the supplied zero-based index.
  struct Changed : Event<void(std::size_t)> {};
};

/// Event keys shared by controlled navigation-selection components.
struct NavigationEvents {
  /// Requests selection of the destination at the supplied zero-based index.
  struct Changed : Event<void(std::size_t)> {};
};

/// Event keys emitted by controlled drawers.
struct DrawerEvents {
  /// Requests a new open state after modal scrim, drag, or Back interaction.
  struct OpenChanged : Event<void(bool)> {};
};

/// Event keys emitted by TextField.
struct TextFieldEvents {
  /// Requests a complete controlled editing value after a user edit.
  struct Changed : Event<void(const TextEditingValue&)> {};
  /// Reports that the configured text submission action was performed.
  struct Submitted : Event<void()> {};
};

namespace detail {

template <class Signature> struct EventSignature;

template <class Result, class... Arguments> struct EventSignature<Result(Arguments...)> {
  using ResultType = Result;
};

template <class Key>
concept EventKey = requires { typename Key::Signature; } && std::is_function_v<typename Key::Signature> &&
                   requires { typename EventSignature<typename Key::Signature>::ResultType; } &&
                   (std::is_void_v<typename EventSignature<typename Key::Signature>::ResultType> ||
                    (std::is_object_v<typename EventSignature<typename Key::Signature>::ResultType> &&
                     std::move_constructible<typename EventSignature<typename Key::Signature>::ResultType>));

template <EventKey Key> using EventResult = typename EventSignature<typename Key::Signature>::ResultType;

class EventHandlerBase {
public:
  virtual ~EventHandlerBase() = default;
};

template <class Signature> class EventHandler;

template <class Result, class... Arguments> class EventHandler<Result(Arguments...)> final : public EventHandlerBase {
public:
  explicit EventHandler(std::function<Result(Arguments...)> function) : function_(std::move(function)) {}

  template <class... Values> Result Invoke(Values&&... values) const {
    return function_(std::forward<Values>(values)...);
  }

private:
  std::function<Result(Arguments...)> function_;
};

using EventBindings = std::unordered_map<std::type_index, std::shared_ptr<EventHandlerBase>>;

template <class Key> bool HasEventBinding(const EventBindings& bindings) {
  return bindings.contains(typeid(Key));
}

template <EventKey Key, class... Arguments> auto EmitEvent(const EventBindings& bindings, Arguments&&... arguments) {
  using Result = EventResult<Key>;
  const auto found = bindings.find(typeid(Key));
  if (found == bindings.end()) {
    if constexpr (std::is_void_v<Result>) {
      return false;
    } else {
      return std::optional<Result>{};
    }
  }
  const auto handler = std::dynamic_pointer_cast<EventHandler<typename Key::Signature>>(found->second);
  if (!handler) {
    if constexpr (std::is_void_v<Result>) {
      return false;
    } else {
      return std::optional<Result>{};
    }
  }
  if constexpr (std::is_void_v<Result>) {
    handler->Invoke(std::forward<Arguments>(arguments)...);
    return true;
  } else {
    return std::optional<Result>{handler->Invoke(std::forward<Arguments>(arguments)...)};
  }
}

class EventHub {
public:
  void SetBindings(EventBindings bindings) {
    bindings_ = std::move(bindings);
  }

  template <EventKey Key, class... Arguments> auto Emit(Arguments&&... arguments) const {
    return EmitEvent<Key>(bindings_, std::forward<Arguments>(arguments)...);
  }

private:
  EventBindings bindings_;
};

std::shared_ptr<EventHub> UseEventHub();

} // namespace detail

/// Emits typed events toward the current handler bindings of one composition scope.
///
/// The emitter does not own the scope. It disconnects when that scope is unmounted and observes replacement handlers
/// after recomposition while the scope remains alive.
class EventEmitter {
public:
  /// Creates a disconnected emitter.
  EventEmitter() = default;

  /// Emits one event through the scope's current handler binding.
  ///
  /// A `void` signature returns `void`. A value signature returns `std::optional<Result>`, where an empty value means
  /// that the scope has ended or no handler is currently bound. Handler exceptions propagate to the caller.
  /// @code
  /// events.Emit<Submitted>("query");
  /// const bool allow = events.Emit<NavigationRequested>("https://example.com").value_or(true);
  /// @endcode
  template <class Key, class... Arguments>
    requires detail::EventKey<Key> && std::invocable<std::function<typename Key::Signature>&, Arguments...>
  auto Emit(Arguments&&... arguments) const {
    using Result = detail::EventResult<Key>;
    if constexpr (std::is_void_v<Result>) {
      if (auto hub = hub_.lock()) {
        static_cast<void>(hub->template Emit<Key>(std::forward<Arguments>(arguments)...));
      }
    } else {
      if (auto hub = hub_.lock()) {
        return hub->template Emit<Key>(std::forward<Arguments>(arguments)...);
      }
      return std::optional<Result>{};
    }
  }

  /// Returns whether the originating composition scope is still alive.
  ///
  /// A connected emitter may still have no handler for a particular event key.
  [[nodiscard]] bool IsConnected() const noexcept {
    return !hub_.expired();
  }

private:
  explicit EventEmitter(std::shared_ptr<detail::EventHub> hub) : hub_(std::move(hub)) {}

  std::weak_ptr<detail::EventHub> hub_;

  friend EventEmitter UseEvents();
};

/// Returns an emitter bound to the current composition scope's event handlers.
///
/// Call this only while composing a View. A reusable component that calls UseEvents() directly must be marked
/// composable so that it owns an independent recomposition lifetime.
/// @code
/// struct Submitted : Event<void(std::string)> {};
///
/// View App() {
///   const EventEmitter events = UseEvents();
///   return Button("Search").OnClick([events] {
///     events.Emit<Submitted>("query");
///   });
/// }
/// @endcode
inline EventEmitter UseEvents() {
  return EventEmitter{detail::UseEventHub()};
}

} // namespace huxerui
