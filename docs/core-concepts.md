# Core Concepts

## Views and components

A component is a C++ function that returns a transient `View`. Calling a component describes the desired UI; it does not directly create a platform widget. The runtime reconciles the resulting `ViewSpec` values with persistent `MountedNode` objects.

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

`State<T>` supports value assignment, `Update()`, arithmetic compound assignments (`+=`, `-=`, `*=`, `/=`, and `%=`), bitwise and shift compound assignments (`&=`, `|=`, `^=`, `<<=`, and `>>=`), and prefix or postfix increment and decrement when the corresponding operations are available for `T`. Copyable, assignable values update transactionally and skip invalidation when they are equality-comparable and the result is unchanged. Move-only or non-assignable values update in place and conservatively invalidate subscribed scopes after a successful mutation. Postfix increment and decrement require a copyable value and return a `T` snapshot containing the value before the mutation.

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
    Shadow{
        .color = Color::Rgb(0, 0, 0, 0.2F),
        .offset = {0.0F, 4.0F},
        .blur_radius = 10.0F,
        .spread = -1.0F,
    },
    CornerRadius(8.0F)
);
```

`Shadow` paints a Gaussian-blurred copy of the node's rectangular or rounded-rectangular shape behind its background.
Blurred shadows exclude the caster interior so offsets produce a soft exterior elevation instead of a second solid shape.
It follows presentation transforms and group opacity without changing measurement, layout, clipping, or hit testing.
The blur radius is the outer falloff extent in logical units, while positive and negative spread expand and contract the shadow caster.
The complete shadow overflow participates in visibility and damage calculation.

`ClipChildren{}` explicitly clips descendant drawing and pointer hit testing to the View bounds, using its `CornerRadius` when present. Clipping is opt-in, so transformed or overflowing children remain visible and interactive by default. A ScrollView additionally retains its content-viewport clip when `ClipChildren{}` contributes a separate rounded container clip.

`CornerRadius` accepts either one radius or `CornerRadii` for independent corners. For example, `CornerRadius{CornerRadii::Top(28.0F)}` rounds only the top edge of a bottom sheet. Uniform corners keep the dedicated rounded-rectangle command, while asymmetric corners use the shared Path command path without changing layout semantics.

Component-specific configuration remains on the component:

```cpp
Text("Diagnostic")
    .Style({
        Font::Monospace(14.0F).WithWeight(FontWeight::SemiBold),
        Color::Rgb(207, 34, 46),
        TextDecoration::Underline,
    });

TextField(value)
    .Placeholder("Email")
    .MaxLength(200)
    .OnChanged([value](const TextEditingValue& next) {
      value = next;
    });
```

`Text::Style` replaces the complete theme-resolved text style, while later `Foreground` and `FontSize` modifiers update only their corresponding members.

Controllers and events are methods because they bind behavior or an external handle rather than describe a reusable generic property.

## Canvas and Path drawing

`Canvas` is a leaf View that records custom drawing through the same `PaintContext` used by built-in components and NodeExtensions.
Its painter receives a content-local Size and draws from `(0, 0)` without depending on a platform Canvas:

```cpp
Canvas([](PaintContext& paint, Size size) {
  Path triangle;
  triangle.MoveTo({size.width * 0.5F, 0.0F})
      .LineTo({size.width, size.height})
      .LineTo({0.0F, size.height})
      .Close();

  paint.DrawPathShadow(triangle, Color::Rgb(0, 0, 0, 0.24F), {0.0F, 6.0F}, 16.0F);
  paint.FillPath(triangle, Color::Rgb(103, 80, 164));
  paint.StrokePath(triangle, Color::White(), 2.0F, StrokeCap::Round, StrokeJoin::Round);
}).With(Frame{.height = 180.0F});
```

Canvas has no intrinsic size and is not clipped automatically.
Use `Frame`, `Grow`, or parent constraints for layout and explicit rectangle or Path clips when drawing must stay inside a shape.
Clean Canvas PaintSequences are retained, while a changed painter or Canvas size rerecords only that node.
See [Canvas and Path Design](design/canvas.md) for command semantics and platform renderer ownership.

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
struct GreetingLocale {
  std::string language;

  static GreetingLocale Default() {
    return {"en"};
  }
};

const GreetingLocale& locale = UseEnvironment<GreetingLocale>();
return ProvideEnvironment(GreetingLocale{"fr"}, Content);
```

Use a semantic wrapper when two values have the same underlying representation but different meanings.

Theme is a deferred Environment provider for visual tokens and component styles. See [Theme, Animation, and Presentation](theme-animation-and-presentation.md).

`UseViewportClass()` is the framework-managed responsive Environment read. It exposes Compact, Medium, or Expanded rather than raw dimensions, so an ordinary resize does not continuously recompose the application. The Runtime updates this value only when width crosses the configured `ViewportBreakpoints`; exact responsive geometry remains the responsibility of layout constraints.

## Runtime flow

```text
component functions
  -> ViewSpec
  -> reconciliation
  -> MountedNode
  -> measure and layout
  -> hit testing and interaction
  -> RenderScene
  -> platform renderer
```

The shared C++ runtime does not own Win32 windows, AppKit objects, or Android Views. Platform adapters translate system lifecycle, input, text, and drawing operations at the edge.

For implementation details, see the [architecture design](design/architecture.md).
