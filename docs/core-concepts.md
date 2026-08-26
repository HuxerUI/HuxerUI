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
[[huxerui::composable]]
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

`StateList<T>` is the corresponding shared handle for an observable mutable sequence. `UseStateList<T>()` creates an empty list, while `UseStateList(range)` and the initializer-list overload copy or move initial values from an input range:

```cpp
auto tasks = UseStateList<std::string>({
    "Review",
    "Publish",
});

return Column {
  ForEach(tasks, [](const std::string& task) {
    return Text(task).Key(task);
  }),
  Button("Add").OnClick([tasks] {
    tasks.PushBack("Follow up");
  }),
};
```

Reading the size, an element, or an iterator subscribes the current scope. `PushBack()`, `Insert()`, `Set()`, `Erase()`, `Move()`, `PopBack()`, and `Clear()` mutate the retained sequence directly and coalesce scope invalidation without copying and comparing a complete `std::vector` through `State<std::vector<T>>`.

`StateList` does not bypass declarative composition or assign identity to its elements. A scope that enumerates the list recomposes after a mutation, and dynamic stateful children still require stable semantic keys so reconciliation preserves the correct mounted state.

The application root has an implicit scope. Mark a reusable function with `[[huxerui::composable]]` when it directly calls a composition-bound `UseXxx()` facility; an Environment-independent View helper remains unmarked.

## Lifecycle

`Lifecycle()` attaches an external resource lifetime to the current composition scope rather than to a returned View:

```cpp
[[huxerui::composable]]
View AccountStatus(std::string account_id) {
  auto service = UseService<AccountService>();

  Lifecycle([service, account_id] {
    auto subscription = service->Subscribe(account_id);
    return [subscription = std::move(subscription)] {
      subscription.Cancel();
    };
  }, account_id);

  return Text("Connected");
}
```

The setup callable runs after a successful frame commit and may return either a `void` cleanup callable or `void`.
The cleanup runs before setup restarts, when a successful composition omits the declaration, when the owning scope unmounts, or when Runtime shuts down.
Setup exceptions propagate from `Runtime::BuildFrame()`; a cleanup exception terminates the process at Runtime's non-throwing teardown boundary.

Dependencies follow the setup callable and may be `State<T>`, `StateList<T>`, or ordinary copyable equality-comparable values.
State handles compare cell identity and version, while ordinary values compare their captured values.
Changing any dependency performs cleanup followed by setup; an unchanged recomposition retains the active resource.
Multiple dependency writes before a frame coalesce into one restart using the latest committed declaration.

`Lifecycle()` has the same current-scope and call-site occurrence identity model as `UseState()`.
Changing the returned root View does not restart it, keyed movement preserves it, and failed composition does not run setup or cleanup.
A helper without its own scope contributes lifecycle declarations to its caller; mark a reusable component with `[[huxerui::composable]]` when it requires an independent lifetime.
State reads performed later inside setup do not create composition subscriptions, so every value that should restart the resource must be listed explicitly.

Lifecycle is not a modifier.
Modifiers and `NodeExtension` own behavior attached to a particular mounted node, while `Lifecycle()` owns component-level external setup and cleanup.

## Application lifecycle and activation

Runtime provides one application-level handle during composition:

```cpp
auto application = UseApplication();
```

`LifecycleState()` returns the current platform-owned `Active`, `Inactive`, or `Background` state. Reading it during composition subscribes the current scope, and repeated equal platform updates do not cause recomposition:

```cpp
auto application = UseApplication();
const ApplicationLifecycleState lifecycle_state = application.LifecycleState();

application.OnLifecycleChange([](ApplicationLifecycleState state) {
  PersistOrPauseFor(state);
});
```

`LifecycleState()` is a coalescing current value for declarative UI. `OnLifecycleChange()` preserves each distinct transition while the declaring component Lifecycle is mounted, even when several transitions occur before the next visible frame. This lets an application observe `Background` after it returns to the foreground rather than relying on UI that could not be visible while backgrounded.

`StartupActivation()` is the immutable activation that created this Runtime and is available while constructing the initial application state. `OnActivation()` declares the single handler for later application activations and follows the current scope's Lifecycle:

```cpp
auto application = UseApplication();
auto path = UseState(InitialPath(application.StartupActivation()));

application.OnActivation([path](ApplicationActivation activation) mutable {
  path = PathForActivation(std::move(activation));
});
```

Lifecycle exposes both a coalescing current value and a mounted ordered transition stream, while activation is an ordered external-input stream. `LaunchActivation`, `UrlActivation`, and `FileActivation` distinguish ordinary launch, opaque URL input, and platform-granted external files. Runtime preserves subsequent activations in FIFO order but leaves document and route policy to application code. See [Application Activation and Lifecycle Design](design/application.md) for the complete contract and staged activation mappings.

## Tasks

`Task<T>` is a lazy move-only C++20 coroutine result, while `TaskScope` starts and owns `Task<void>` children for one composition scope:

```cpp
[[huxerui::composable]]
View UserName(UserId user_id, std::shared_ptr<UserService> service) {
  auto tasks = UseTaskScope();
  auto name = UseState(std::string{"Loading..."});

  Lifecycle([=] {
    TaskHandle request = tasks.Launch([=]() -> Task<void> {
      name = co_await service->LoadName(user_id);
    });

    return [request] {
      request.Cancel();
    };
  }, user_id);

  return Column {
    Text(name),
    Button("Reload").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        name = co_await service->LoadName(user_id);
      });
    }),
  };
}
```

Launch queues the first resume on the owning UI thread and retains the task even when its returned TaskHandle is ignored.
TaskHandle destruction does not cancel; `Cancel()` stops that task, while successful scope unmount and Runtime teardown cancel every remaining child in the TaskScope.
Compatible recomposition and keyed movement retain the same scope and tasks.

`Delay()` is a lazy Task that suspends for at least a standard chrono duration and resumes on the owning UI thread:

```cpp
tasks.Launch([=]() -> Task<void> {
  loading = true;
  co_await Delay(300ms);
  loading = false;
});
```

The standard duration literals are available through the huxerui namespace.
Delay is aligned with the UI frame scheduler, remains asynchronous for `0ms`, and is canceled with its owning Task without an exception or later State write.
It is intended for UI timing and retry delays rather than high-precision media or sampling work.

HuxerUI awaitables may finish work on another thread but resume their coroutine through the owning UI dispatcher.
Task code may therefore update State directly before suspension and after a HuxerUI awaitable resumes it.
State itself does not dispatch between threads, and arbitrary third-party awaitables must provide their own UI-thread handoff.

Lifecycle and EventBindings do not recognize Task types.
Lifecycle setup explicitly launches work, dependency cleanup explicitly cancels the relevant TaskHandle, and ordinary `void` event handlers may call `Launch()` for scope-owned fire-and-forget work.
See [Task and Structured Concurrency Design](design/tasks.md) for cancellation, exception, and platform contracts.

## HTTP requests

Runtime provides one typed HttpClient Root Service.
HttpClient returns Task values, so a component uses its existing TaskScope for ownership and cancellation:

```cpp
auto http = UseService<HttpClient>();
auto tasks = UseTaskScope();

Button("Load").OnClick([=] {
  tasks.Launch([=]() -> Task<void> {
    HttpResult result = co_await http->Send({
        .url = "https://api.example.com/value",
        .headers = {{"Accept", "application/json"}},
    });
    if (result.HasResponse()) {
      value = std::move(result).Response().body;
    } else {
      value = result.Error().message;
    }
  });
});
```

HTTP 4xx and 5xx statuses remain HttpResponse values.
Transport failures are explicit HttpError values, while Task cancellation stops the native request without resuming application code.
Invalid portable request configuration still throws `std::invalid_argument` synchronously from `Send()`.
The response body is a binary-safe in-memory byte string; JSON parsing, streaming, caching, and application retry policy remain separate concerns.
The shared API and independent Windows, macOS, iOS, Linux, Android, and Web transports are implemented.
See [HTTP Client Design](design/http.md) for the complete ownership and platform contract.

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

[[huxerui::composable]]
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

  bool operator==(const GreetingLocale&) const = default;
};

const GreetingLocale& locale = UseEnvironment<GreetingLocale>();
return ProvideEnvironment(GreetingLocale{"fr"}, Content());
```

Use a semantic wrapper when two values have the same underlying representation but different meanings.

Theme is a transparent Environment boundary for visual tokens and component styles. See [Theme, Animation, and Presentation](theme-animation-and-presentation.md).

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
