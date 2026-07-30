# Core Concepts

## Views and components

A component is a C++ function that returns a transient `View`. Calling a component describes the desired UI; it does not directly create a native widget. The runtime reconciles the resulting `ViewSpec` values with persistent `MountedNode` objects.

Layout containers use braces to make parent-child structure visible, while leaf controls use constructors:

```cpp
return Column {
  Text("Account", TextRole::Title),
  Row {
    Button("Cancel"),
    Button("Save"),
  }.With(Spacing(8.0F)),
};
```

`View` uses copy-on-write storage. Applying a modifier or event to one copied view does not change another copy.

## State and scopes

`State<T>` is a lightweight handle to a shared state cell. Reading it while a scope is composed subscribes that scope to future changes:

```cpp
[[huxerui::scope]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Text::Format("Count {}", count),
    Button("+1").OnClick([count] {
      count += 1;
    }),
  };
}
```

Each mounted scope owns its own `UseState()` table. State identity combines the source call site with the occurrence of that call during the current composition. A state change invalidates subscribed scopes and multiple writes before the next frame are coalesced.

The application root has an implicit scope. Mark a reusable stateful component with `[[huxerui::scope]]`; stateless functions do not need a scope.

## Node identity and keys

Unkeyed siblings use position and node type as identity. This is appropriate for stable UI structures. Use a stable key when siblings can be inserted, removed, or reordered:

```cpp
Column {
  ForEach(users, [](const User& user) {
    return UserRow(user).Key(user.id);
  }),
};
```

Keys can be signed integers, unsigned integers, strings, or enums and must be unique only among siblings. Duplicate sibling keys are rejected.

`ForEach` returns a `Views` collection that containers flatten directly. There is no fragment layout node between the parent and the generated views.

## Modifiers and component methods

Generic visual, layout, and interaction properties use `With()`:

```cpp
Button("Save").With(
    Enabled(can_save),
    Padding(12.0F),
    Background{Color::Rgb(40, 100, 220)},
    CornerRadius(8.0F)
);
```

Component-specific configuration remains on the component:

```cpp
TextField(value)
    .Placeholder("Email")
    .MaxLength(200)
    .OnChanged([value](const TextEditingValue& next) {
      value = next;
    });
```

Controllers and events are methods because they bind behavior or an external handle rather than describe a reusable generic property.

## Typed events

Built-in interactions and custom component events share one typed event system:

```cpp
struct SearchSubmitted : Event<std::string> {};

[[huxerui::scope]]
View SearchBox() {
  auto events = UseEvents();

  return Button("Search").OnClick([events] {
    events.Emit<SearchSubmitted>("query");
  });
}
```

Consumers subscribe without adding callback parameters to the component:

```cpp
SearchBox().On<SearchSubmitted>([](std::string query) {
  SubmitSearch(std::move(query));
});
```

`OnClick()` is a convenience wrapper for `On<ViewEvents::Click>()`. Each event key has at most one handler and a later subscription replaces an earlier one. Events do not currently bubble.

## Environment and Theme

Environment values propagate through a subtree. Their value type is also the lookup identity and owns its fallback:

```cpp
struct Locale {
  std::string language;

  static Locale Default() {
    return {"en"};
  }
};

const Locale& locale = UseEnvironment<Locale>();
return ProvideEnvironment(Locale{"fr"}, Content);
```

Use a semantic wrapper when two values have the same underlying representation but different meanings.

Theme is a deferred Environment provider for visual tokens and component styles. See [Theme, Animation, and Presentation](theme-animation-and-presentation.md).

## Runtime flow

```text
component functions
  -> ViewSpec
  -> reconciliation
  -> MountedNode
  -> measure and layout
  -> hit testing and interaction
  -> DisplayList
  -> native renderer
```

The shared C++ runtime does not own Android Views, AppKit objects, or Win32 windows. Platform adapters translate native lifecycle, input, text, and drawing operations at the edge.

For implementation details, see the [architecture design](design/architecture.md).
