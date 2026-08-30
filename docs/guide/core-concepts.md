# Core Concepts

## Views and components

A component is an ordinary C++ function returning `View`.
Calling it declares the desired interface; persistent state belongs to the mounted runtime tree rather than the transient `View` value.
`View` is a lightweight copy-on-write value, so application code normally passes and returns it by value without explicit moves.

```cpp
View AccountActions() {
  return Row {
    Button("Cancel"),
    Button("Save"),
  }.With(Spacing(8.0F));
}
```

Containers use braces for child declarations.
Generic behavior is applied with `.With(...)`, typed events with `.On<EventKey>(...)`, parent-child metadata with `.LayoutValue<Key>(...)`, and stable identity with `.Key(...)`.
These fluent APIs are rvalue-qualified, but a newly constructed component is already a temporary.
Use `std::move` only when consuming a named View to call such an API; do not move temporary expressions, every container child, or a local return value.

## Composition and state

The application root owns an implicit composition scope.
A reusable function that directly calls a composition-bound `UseXxx()` function must be marked `[[huxerui::composable]]`.

```cpp
[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count: {}", count),
    Button("Increment").OnClick([count] {
      count += 1;
    }),
  };
}
```

`UseState` returns a copyable `State<T>` handle.
Reads subscribe the current scope; writes update the authoritative value and invalidate subscribed scopes.

State identity is determined by the current scope, source location, and occurrence at that location.
Dynamic stateful content therefore needs stable scopes or keys when it can be inserted, removed, or reordered.

## Identity and keys

Unkeyed siblings reconcile by position.
Use a stable semantic key for dynamic siblings:

```cpp
return Column {
  ForEach(records, [](const Record& record) {
    return RecordRow(record).Key(record.id);
  }),
};
```

Keys are unique among one parent's children.
Do not use a current index as the key for reorderable stateful data.

## Controlled values

Application-owned values such as text, selection, checked state, progress, and declarative visibility are controlled.
The component emits a requested change and the owner supplies the next value.

```cpp
[[huxerui::composable]]
View ToggleSetting() {
  auto enabled = UseState(false);

  return Switch(enabled).OnChanged([enabled](bool value) {
    enabled = value;
  });
}
```

Transient hover, pressed, animation, momentum, caret, and IME state are retained by mounted framework behavior.

## Events

Components expose typed events and convenience methods such as `OnClick`, `OnChanged`, and `OnSubmitted`.

Custom event identity is its event key type:

```cpp
struct Submitted : Event<std::string> {};

[[huxerui::composable]]
View SearchButton() {
  auto events = UseEvents();
  return Button("Search").OnClick([events] {
    events.Emit<Submitted>("query");
  });
}
```

Parents attach handlers with `.On<Submitted>(...)`.

## Modifiers and component configuration

Property modifiers apply from left to right without creating wrapper nodes:

```cpp
return Text("Profile").With(
    Padding(12.0F),
    Background(Color::White()),
    CornerRadius(8.0F)
);
```

Required component data belongs in its constructor.
Component-only semantics use typed fluent methods, while reusable node behavior uses modifiers.

## Environment and Theme

Environment propagates typed values through the mounted tree.
`UseEnvironment<Value>()` reads the closest value and subscribes the current scope when used during composition.

```cpp
struct ContentScale {
  float value = 1.0F;

  static ContentScale Default() { return {}; }
  bool operator==(const ContentScale&) const = default;
};

View Content() {
  return ProvideEnvironment(ContentScale{2.0F}, ScaledContent());
}
```

Read the closest value with `UseEnvironment<ContentScale>()`.
Theme is the visual specialization of Environment.
Nested themes override only their subtree, and theme changes invalidate the scopes and retained presentation that consume changed values.

## Lifecycle and tasks

`Lifecycle(...)` defines setup and cleanup tied to a mounted composition scope.
Dependency changes run cleanup before the effect is installed again.

```cpp
[[huxerui::composable]]
View UserSession(UserId user_id) {
  Lifecycle([user_id] {
    StartSubscription(user_id);
    return [user_id] {
      StopSubscription(user_id);
    };
  }, user_id);

  return UserContent();
}
```

`Task<T>` represents asynchronous work.
`UseTaskScope()` owns launched work for the current composition scope.
Its tasks are canceled when that scope is replaced or unmounted, and continuations resume on the Runtime thread.
`RunWorker()` owns a synchronous callable and its arguments, executes them on HuxerUI's bounded native worker pool, and resumes the awaiting Task on that Runtime thread.
Worker code must not access State, composition, Views, or UI-affine platform objects.
`TaskScope::Post()` is the lifecycle-bound handoff for an external thread or callback to enqueue a `void` update on the same Runtime thread; pending updates are discarded when the scope closes.
Neither API requests or guarantees platform background execution.

## Application state

`Application` declares the process-level root and options.
The root can observe application lifecycle state and receive cold-start or later activation containing URLs or external files.
Navigation and file handling remain application policy; the platform shell only normalizes and delivers activation data.

`UrlActivation::url` is an immutable validated `Uri` from `<huxerui/data.h>`.
Inspect its typed components directly rather than parsing the serialized string again.
`FileActivation` carries `FileReference` capabilities instead of assuming that every platform-opened document has a local path.

`UseApplication()` returns the application-level handle.
On desktop, its `SystemTray()` sub-handle presents one tray item and `Quit()` requests orderly application termination.
Tray declarations reuse `MenuItem`, `MenuEntry`, and `MenuSection`; their `ImageVariant` icons must resolve to raster `ImageAsset` values.
`UseWindow()` supplies independent visibility commands and lifecycle-bound minimize and close request handlers, allowing an application to compose minimize-to-tray behavior while preserving the normal window action when no tray host is available.

## Runtime model

The shared Runtime owns composition, reconciliation, layout, interaction, scrolling, animation, semantics, and retained scene generation.
Platform adapters own platform lifecycle, frame scheduling, event conversion, text services, accessibility bridges, and scene rendering.

```text
component declarations
  -> composition and ViewSpec compilation
  -> reconciliation and MountedNode state
  -> layout, interaction, animation, and semantics
  -> immutable frame commit
  -> platform renderer and accessibility bridge
```

See [Architecture Design](../design/architecture.md) for internal ownership and invariants.
