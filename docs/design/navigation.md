# Navigation Design

Navigation retains pages in the application tree. Factory operations own their entry history through a scoped controller, while typed navigation takes an application-owned path. Both share page identity, rendering, transition execution, and Back dispatch.

## Page stack design summary

Navigation has four distinct responsibilities:

```text
NavigationStack
    owns logical history and retained page entries
        -> NavigationController
    mutates the nearest stack
        -> NavigationStackLayout
    measures and overlays the active transition pair
        -> Navigation transition extensions
    update presentation transforms and opacity
```

Pages remain ordinary View factories.
NavigationStack is part of the application tree rather than the LayerStack because pages participate in application layout, inherit ordinary Environment values, retain RecomposeScopes, and may contain nested navigation.

LayerStack continues to own content outside the application tree, including Dialog, BottomSheet, Popup, Menu, Toast, and diagnostic presentation.
Navigation must not become another Layer level or another root service.

## Top app bars, destination selection, and drawers

TopAppBar is an explicit application-tree layout with a required StringVariant title, an optional leading View, and trailing action Views.
The title is bar-owned so the component can apply its typography, single-line geometry, and Heading semantics consistently.
Leading and action slots remain ordinary Views because they independently own events, enabled state, focus, semantics, menu anchors, and custom visuals.
TopAppBar does not inspect action component types, synthesize callbacks, or couple a leading action to NavigationController or DrawerLayout.

TopAppBar supports Start and Center title alignment within one component.
Start alignment uses the themed title inset when leading content is absent.
Center alignment first centers the title within the complete bar and then clamps it between the occupied leading and action regions.
Leading and actions retain their constrained interaction geometry before the title receives the remaining width.
Excess action content is clipped with its hit-test region rather than drawing across leading content or outside the bar.

Automatic overflow remains application policy.
An arbitrary View cannot be converted reliably into a MenuItem because it may represent a composite control rather than a command with an icon, label, enabled state, and callback.
Applications keep their direct action list intentionally small and use the existing Menu service for secondary actions.

The [Window Insets and System Bars Design](window-insets.md) gives UiWindow one full-window geometry contract.
TopAppBar consumes its top and horizontal insets when they remain available in edge-to-edge content and contributes its themed background to the status region without duplicating platform inset handling.

NavigationBar and NavigationPane share NavigationItem and NavigationEvents::Changed.
They are controlled selection views: the selected index enters through construction and a requested index leaves through the typed event.
They do not create pages, retain destination history, or assume that selecting an item always replaces visible content.
An application may use the selection to control [IndexedPages](indexed-pages.md), select a page-owned NavigationStack, or update a typed route path.

NavigationBar lays destinations along the horizontal axis.
NavigationPane lays them vertically, supports compact icon-only and expanded icon-and-label presentation, and scrolls when its destinations exceed the viewport.
NavigationBar and compact NavigationPane items require icons, while an expanded NavigationPane may use label-only items.
Keyboard traversal lives in one retained selection behavior shared by the two controls and skips disabled destinations.
Geometry, colors, indication, and selection motion remain separate NavigationBarStyle and NavigationPaneStyle Theme values because the two surfaces follow different visual specifications.

Tabs publishes a TabList collection with one real Tab semantic node per retained item.
NavigationBar and NavigationPane publish a Navigation collection whose real retained item surfaces are Button semantic nodes.
Each item carries its resolved accessible label, zero-based collection index, selected state, and inherited enabled state; its existing Click binding remains the only Activate route.
Disabled items remain discoverable without actions, and compact or expanded Pane presentation does not change semantic structure or identity.
Decorative item descendants are excluded after the item surface supplies its complete label.

The retained selection behaviors continue to own keyboard movement and reveal requests only.
They do not construct virtual semantic children, copy item geometry, or add a second action path.
Virtual semantic children remain reserved for meaningful entities, such as self-drawn Canvas data points, that do not already have mounted Views.

DrawerLayout accepts main content plus optional strongly typed StartDrawer and EndDrawer children.
The strong child types make ownership and logical edge explicit without a slot enum, runtime child inspection, or a parallel builder protocol.
Start and End are semantic edges rather than physical Left and Right; layout direction can resolve them later without changing the API.

Drawers remain ordinary application-tree content because they inherit application Environment values, may contain stateful controls or a NavigationPane, and are structurally owned by the surrounding page shell.
They do not enter LayerStack and do not acquire a LayerId.
Dialog, BottomSheet, Menu, Popup, Toast, and debug presentation remain window-level layers.

Drawer open state is controlled and applies when the drawer resolves to modal placement.
Buttons update the owner state directly, while modal scrim input, modal edge dragging, and Back emit DrawerEvents::OpenChanged.
The retained drawer extension owns only transient modal drag and animation progress.
It applies the modal panel transform and scrim opacity as presentation changes, so those animations do not require per-frame recomposition, measurement, layout, or PaintSequence recording.

A modal drawer marks its overlay subtree as a focus trap from opening until its exit animation finishes, while a persistent inline drawer remains visible and participates in ordinary focus traversal regardless of the controlled modal state.
UiWindow resolves the highest painted enabled trap generically, so Layer presentation and application drawers share focus confinement and restoration rather than maintaining separate mechanisms.
Back resolution likewise walks application nodes once in reverse paint order, checking a node's explicit Back event before its retained extensions.
This lets an open drawer consume Back before underlying page content while a page-local Back handler still precedes its enclosing NavigationStack.

DrawerLayout resolves presentation from ViewportClass while preserving the same StartDrawer and EndDrawer subtrees and controlled modal states.
Compact presents both edges modally, Medium keeps Start persistently inline and End modal, and Expanded keeps both persistently inline.
Inline drawers consume application width automatically and never write the owner's Open state.
They omit modal scrim, shape, shadow, focus trapping, edge gestures, and Back handling.
If the local constraints cannot preserve DrawerStyle::minimum_content_width plus every requested inline drawer minimum, End falls back to modal placement first and Start follows only when required.
After fallback, the controlled Open state determines whether that modal drawer is visible.
This constraint fallback makes the local layout authoritative even when the window-level ViewportClass is wider than the bounds assigned to DrawerLayout.
If both controlled states are open while the available structure becomes modal, the drawers stack in child paint order with End above Start.
Back closes End before Start, preserving state continuity across viewport changes without a layout-time state mutation or exception.

## Page stack public API

The factory stack and controller APIs are declared in [navigation.h](../../include/huxerui/navigation.h).

`Push` appends a new logical entry.
`Pop` returns false when the logical stack contains only its root or when the controller is no longer connected.
`Replace` replaces the logical top entry; replacing a depth-one stack creates a new unpoppable root.

An empty page factory is invalid and throws `std::invalid_argument`.
`Push` or `Replace` through a disconnected controller throws `std::logic_error`, matching operations that require a live owner.
Read-only queries return zero or false after disconnection, and `Pop` returns false.
Controller operations run on the application thread owning UiWindow and do not add cross-thread synchronization to navigation state.

`UseNavigation()` resolves the nearest NavigationStack from the current Environment.
Calling it outside a NavigationStack throws `std::logic_error` with an English HuxerUI diagnostic.
`UseRootNavigation()` resolves the outermost compatible factory stack in the same inherited navigation chain.

## Basic use

```cpp
View App() {
  return NavigationStack([] {
    return HomePage();
  });
}

View HomePage() {
  auto navigation = UseNavigation();

  return Button("Open details").OnClick([navigation] {
    navigation.Push(DetailPage, 42);
  });
}

View DetailPage(int document_id) {
  auto navigation = UseNavigation();

  return Column {
    Button("Back").OnClick([navigation] {
      navigation.Pop();
    }),
    Text::Format("Document {}", document_id),
  };
}
```

The public API does not return a page identifier because operations address the logical top of one scoped stack.
LayerId remains necessary for independently addressable window presentation, while navigation does not expose arbitrary removal of middle entries.

## Why pages are factories

`View` is a transient declaration value and is not retained application state.
A page factory allows UiWindow to create and recompose a page under the NavigationStack's Environment and independent RecomposeScope at the correct time.

The following form is intentionally unsupported:

```cpp
navigation.Push(DetailPage());
```

Constructing a View before it enters the navigation entry would compose it under the caller's current declaration context, obscure scope ownership, and make delayed creation and future path resolution harder.

Factories also bind typed page parameters directly:

```cpp
navigation.Push(DocumentPage, document_id);
navigation.Replace(DocumentPage, replacement_id);
```

The bound values are retained by value and passed to every factory invocation, so they must remain safe to copy across recomposition.
A lambda remains available for derived arguments or more complex capture behavior.

The initial factory stack is intentionally not serializable.
A factory may capture arbitrary application state, native handles, services, or callbacks and therefore cannot truthfully represent a URL or restorable route.

## Scope ownership

An ordinary HuxerUI component is a function returning View.
It does not automatically own an independent RecomposeScope merely because it is a component function.

The application root already owns a scope.
`[[huxerui::composable]]` marks reusable functions that directly call composition-bound `UseXxx()` facilities or need an independent local recomposition lifetime.

NavigationStack is explicit even when every page function is scoped because page scopes and navigation ownership solve different problems:

```text
NavigationStack scope
    owns history, controller, active transaction, and invalidation
    |-- root page scope
    |   owns root page local state
    |-- covered page scope
    |   owns covered page local state
    `-- active page scope
        owns active page local state
```

A page scope cannot determine which sibling page is on top, preserve a stack of sibling factories, coordinate a two-page transition, route system Back, or provide the nearest controller.
NavigationStack therefore remains an explicit stateful container rather than syntax generated around every component function.

## Controller and Environment ownership

Each NavigationStack owns one private `NavigationCoordinator`.
The stack scope holds the strong reference, while public NavigationController copies hold weak references so a page factory that captures its controller cannot form a cycle through the coordinator.
Controller mutations invalidate only that NavigationStack scope through its retained invalidation binding; they do not retain UiWindow directly or recompose the application root.

The controller is provided through a private typed Environment value while each page factory composes.
Nested NavigationStacks override that value for their descendants, so `UseNavigation()` always returns the nearest active stack.

Destinations inherit the Environment at the NavigationStack boundary, not an arbitrary narrower Environment surrounding the Button that happens to call Push.
This gives every page in one stack stable Theme, Locale, resources, root services, and third-party values.
An application that wants a page-specific Theme or other value provides it explicitly inside that page factory.

When the NavigationStack's inherited Environment changes, entry declarations reconcile under the new parent Environment without replacing their stable page scopes.

## Entry identity and retained state

The private logical model starts with a root entry and assigns a monotonic identifier to each Push or Replace:

```cpp
struct NavigationEntry {
  std::uint64_t id = 0;
  std::function<View()> factory;
};
```

Each entry uses a keyed container around its independent page Scope:

```cpp
Stack {
  Scope(entry.factory),
}.Key(entry.id)
```

Pushing the same factory twice produces two distinct entries and two independent local-state identities.
Unkeyed page position never defines navigation identity.

Covered entries remain mounted so their page-local state, scroll state, retained modifier state, and PaintSequences survive until the entry is popped or replaced.
An exiting entry remains mounted until its exit animation completes.
A cancelled predictive Pop never unmounts either participant.

The logical history and render transition are related but not identical:

```text
logical stack
    application-visible target history

render entries
    logical entries plus any entry retained temporarily for exit

active transition
    source entry, destination entry, operation, and progress
```

`Depth()` and `CanPop()` report the logical target history rather than counting an exiting render-only entry.

## Operation serialization

Only one visual transition executes at a time.
Commands accepted during an active transition update the logical target and enter a private operation queue.

A predictive Pop reserves its logical history change at Begin.
When its destination is not yet available because an earlier operation is running, an uncommitted queue entry preserves the Pop's position without starting another visual transition.
Commit makes that entry executable, while Cancel removes it and restores the reserved logical depth.
Commands issued during the gesture therefore remain ordered after the predictive Pop and cannot decrement the same history entry twice.

Serializing topology changes avoids separate page pairs trying to own one presentation frame and avoids visibly snapping the current animation to completion before another command starts.

The queue is private implementation state rather than a public operation or transaction type.
Reduced motion settles queued operations without exposing intermediate animation frames.

An implementation may coalesce operations that have not started only when doing so preserves the same logical history, entry lifetime, callbacks, and observable mount and unmount behavior.

## NavigationStackLayout

NavigationStackLayout is private and overlays the pages participating in the current visible state.

Its measurement rules are:

- Fill each bounded parent dimension.
- In an unbounded dimension, use the maximum measured extent of the participating entries.
- Measure only the active page in a stable state.
- Measure the source and destination pages during a transition.
- Place participating pages at the NavigationStack content origin.
- Clip page presentation to the NavigationStack bounds.

Covered entries that are not participating in the current transition retain their last committed geometry but do not require measurement or placement.
When an entry becomes a transition destination, the layout measures it with current constraints before publishing the first transition frame.

The layout policy never retains child references across reconciliation.
Stable entry identifiers and shared transition state, rather than MountedNode pointers, coordinate page roles.

## Visibility and interaction

Mounted retention does not make every entry interactive.

In a stable state:

- The logical top is visible, enabled, focusable, and hit-testable.
- Covered entries have zero presentation visibility and do not participate in pointer or focus routing.
- Covered entries retain state and drawing records without contributing platform damage.

During a programmatic Push, Pop, or Replace:

- Both participating pages remain renderable but disable content interaction until the operation completes.
- Existing page pointer sessions are cancelled, and disabled page focus and text-input sessions are cleared.
- Completion enables the final destination; container-level Back remains available during motion.

During a predictive Pop:

- Both pages disable content interaction while progress is tentative and while Commit or Cancel settles.
- Cancel restores the source page's stable presentation and enables its content after settling, without automatically restoring focus or reopening the IME.
- Commit completes the Pop and enables the revealed page after settling.
- The container continues receiving predictive Back updates and cancellation.

UiWindow uses shared disabled-subtree input cleanup rather than a navigation-specific `DeactivateNavigationInput()` path.
The shared cleanup cancels pointer capture and observers, clears hover, clears focus that belongs to the disabled subtree, and ends an owned text-input session with `TextInputEndReason::FocusLost`.

## Focus and text input

Leaving a page clears focus when the focused node belongs to the outgoing subtree and stops its platform text-input session.
Returning to a retained page restores its component and scroll state but does not automatically focus its previous TextField or reopen the system keyboard.

Automatic focus restoration is not supported because it could unexpectedly reopen mobile IMEs after Pop.

Dialogs and other trap-focus Layers continue to use their existing focus restoration semantics.
Because layers route Back before page navigation, dismissing a dialog restores application focus without changing the page stack.

## Page transition customization

Page motion uses the shared `TransitionSpec` and retained progress engine defined in [Animation](animation.md#extensible-transitions). `NavigationStyle::motion` supplies the default policy for Push, Pop, and Replace. Flat uses a full-width slide and Material uses a parallax slide. Each configures a reversed Pop, and `NavigationStyle::Default()` supplies the generic slide policy.

### Page configuration

Apply `PageTransition` to the ordinary page root. The last declaration on that root replaces the complete themed policy. Lookup crosses transparent Scope and Environment boundaries to the root and rejects a declaration on an arbitrary descendant. `PageTransition{}` explicitly selects immediate presentation for every operation. An absent theme policy also means immediate presentation.

| Operation | Selected effect |
| --- | --- |
| Push | Incoming page's `push` |
| Pop or predictive Back | Departing page's `pop` |
| Replace or changed-path replacement | Incoming page's `replace` |

The operation retains the selected configuration until completion. Recomposition or Theme changes supply defaults for later operations. Initial stack construction and initial deep paths display the final destination immediately.

### Transition execution

One retained controller drives source and destination page presentation and shared elements. It advances before the page extensions read progress. Ordinary transitions animate through the selected timing. Predictive Back seeks normalized progress directly during the gesture. Settling begins at the current sample without the configured start delay. Commit settles to one and Cancel restores the original page by settling to zero.

Active operations disable page content interaction and preserve container Back handling. Fragments are render data with no additional focus, hit-test, text-input, or semantic targets. Effect decorations can record independently of clean page content. A viewport change preserves page progress while evaluating effects against the new bounds. Captured shared pairs in the former coordinate space are released. Reduced motion reaches the same logical result immediately. Retirement releases retained presentation and controller work.

An explicit Scene transition may include a mutation of the navigation path. Scene and page transitions have independent clocks. Selecting immediate page motion leaves the enclosing Scene effect as the only visual transition.

## Shared-element transitions

`SharedElement` and `SharedBounds` use the matcher, retention rules, and fallback defined in [Shared transitions](animation.md#shared-transitions). A navigation operation supplies its own progress to the matching session. It limits capture to its outgoing and incoming pages and excludes nested NavigationStacks. Immediate, initial, and reduced-motion navigation skip shared capture.

Push and Replace select the destination configuration. Pop selects the departing configuration and samples the canonical curve in reverse. Successful pairs suppress their original drawing before page fragment capture. Shared visuals draw above the pages and below synchronized decoration and window layers, inside the navigation clip. A missing or unsupported pair uses ordinary page drawing. A later change that invalidates a pair ends only that pair while page motion continues.

## Incremental rendering

Navigation transitions are presentation-only updates:

```text
transition progress
    -> page-local transform and opacity
    -> resolved presentation tree
    -> conservative old and new damage
    -> platform RenderScene traversal
```

An animation frame does not recompose the application root or either page scope, remeasure the page pair, rerun layout, or record unchanged PaintSequences.

Only the source and destination entries carry transition presentation changes.
Covered stable entries remain invisible and produce no platform damage.
NavigationStack clipping limits transformed damage and drawing to the stack bounds.

A viewport or constraint change remeasures the visible pair.
Offsets derive from the current NavigationStack size, so an in-flight transition continues at the same normalized progress rather than restarting with stale pixels.

## Back event model

`BackEvent` in [event.h](../../include/huxerui/event.h) carries Begin, Update, Cancel, or Commit and normalized progress. The parameterless `UiWindow::HandleBack()` requests an ordinary Commit. The event overload drives a predictive transaction.

UiWindow routes Back in this order:

```text
framework-owned TextSelectionOverlay
    -> topmost public Layer that does not pass through Cancel
    -> deepest enabled application Back consumer in reverse paint order
         -> explicit View BackRequested event before extensions on the same node
    -> platform fallback
```

UiWindow captures the selected consumer at Begin.
Update, Cancel, and Commit target the same consumer rather than rerunning routing against a tree that may have changed during the gesture.
If the captured mounted target disappears, UiWindow ends the transaction safely without dereferencing stale state or unexpectedly falling through to a system window close.

Retained behaviors participate through `NodeExtension::OnBack()`.

Traversal visits enabled descendants before their parents and follows reverse paint order, allowing an active nested NavigationStack to Pop before its enclosing stack.
An inner stack at its root returns false so routing continues to the enclosing stack.

## Application Back interception

ViewEvents gains a commit-only `BackRequested` event:

```cpp
struct ViewEvents {
  struct BackRequested : Event<void()> {};
};
```

An application attaches the handler to the page root when leaving the page requires a decision:

```cpp
View EditorPage() {
  auto navigation = UseNavigation();
  auto dialog = UseDialog();

  return Editor()
      .On<ViewEvents::BackRequested>([dialog, navigation] {
        dialog.Show(
            "Discard changes?",
            "Your unsaved changes will be lost.",
            "Discard",
            "Cancel",
            [navigation] {
              navigation.Pop();
            }
        );
      });
}
```

The presence of a BackRequested handler consumes that Back transaction.
UiWindow selects the handler at Begin but invokes it only at Commit.
Cancel does not invoke application code.

A guarded page does not display a predictive page preview because the destination is not authoritative until the handler decides whether to Pop.
An explicit `navigation.Pop()` is an already-authorized application operation and bypasses BackRequested, allowing the positive dialog action to complete without recursion.

This reuses typed View events and avoids separate PopGuard, NavigationOptions callback, and BackHandler abstractions.
If future application requirements need progress-aware custom Back visuals, the general NodeExtension capability remains available without changing NavigationController.

## Platform Back mapping

Desktop and Web platforms produce Commit-only Back requests:

- Windows, macOS, and Linux map Escape through ordinary key dispatch.
- Web maps Escape through ordinary key dispatch and does not call browser history automatically.

Android uses the richest available platform callback while retaining API 23 as the minimum:

- API 34 and later map `OnBackAnimationCallback` start, progress, cancel, and invoke callbacks to Begin, Update, Cancel, and Commit.
- API 33 maps `OnBackInvokedCallback` to Commit.
- API 23 through 32 map the Activity Back callback to Commit.
- A completely unhandled Commit invokes the Activity's platform fallback.

The full-screen HuxerUIActivity owns callback registration and Activity fallback behavior.
An embedded HuxerUIView exposes the UiWindow Back operations but does not finish its containing Activity or assume ownership of another navigation system.

iOS does not install an unconditional edge recognizer.
A full-screen integration may later map an owned edge-pan gesture to the same BackEvent phases, while an embedded View must not steal a UIViewController navigation controller's interactive-pop gesture.

## Responsive layout

Navigation history and responsive structure remain separate concerns.
NavigationStack fills its assigned bounds but does not automatically turn a compact page stack into an expanded master-detail layout.
DrawerLayout is the responsive application-shell exception: it changes whether its declared StartDrawer and EndDrawer children are controlled modal surfaces or persistent inline content.
It does not synthesize destinations, convert page content into a NavigationPane, or change the owner's controlled modal states.

Applications continue to use ViewportClass to choose structure:

```cpp
View AppContent() {
  if (UseViewportClass() == ViewportClass::Compact) {
    return NavigationStack(MobileRoot);
  }

  return Row {
    MasterPane(),
    DetailPane(),
  };
}
```

Applications that need history to survive a structural replacement hoist the authoritative domain selection or controlled NavigationPath above that branch.
There is no `NavigationSplitView`; split layouts are separate adaptive application containers rather than a behavior switch inside NavigationStack.

Tabs also remain independent.
Each IndexedPages child may own a nested NavigationStack, but Tabs and IndexedPages do not acquire page-history ownership.
IndexedPages retains peer page subtrees according to the separate [Indexed Pages Design](indexed-pages.md).

## Component lifecycle

Navigation visibility is not equivalent to component mount state.
A covered page remains mounted, an exiting page remains mounted until animation completion, and a predictive Pop may be cancelled.

Navigation therefore does not add `OnPageEnter`, `OnPageLeave`, `OnAppear`, or `OnDisappear` callbacks.
Composition-scoped `Lifecycle` effects define setup and cleanup around actual mounted lifetime.
Applications that need to pause work while a retained page is covered keep that visibility policy in controlled application state rather than creating a second navigation lifecycle.

## Typed routed navigation

Factory navigation is convenient but cannot represent browser URLs, deep links, restoration, or externally controlled history.
Typed routed navigation adds a data source above the existing resolved-entry engine instead of adding another navigator:

```text
factory command                 controlled NavigationPath<Route>
       |                                      |
       `------------------ navigation source -'
                              |
                    destination resolver
                              |
                     resolved entry identity
                              |
        shared mounting, transition, interaction, and Back engine
```

The terms have distinct meanings:

- A Route is an application-defined value that identifies one destination and its minimum parameters.
- A NavigationPath is the authoritative ordered route value for one routed stack.
- A destination resolver is an application function that converts a Route into an ordinary View declaration.
- A resolved navigation entry is a private mounted instance with an internal identity, retained scope, and page factory.

HuxerUI does not add a public Scene, Screen, Page, Route, Destination, or NavigationEntry base class.
Scene already denotes render and whole-scene transition data, while Screen incorrectly implies that every navigation container fills a platform window.
Applications may use Screen or Page in their own component function names without adding either concept to the framework type system.

### Route values

A routed application defines lightweight value types and normally closes its route set with `std::variant`:

```cpp
struct ArticleRoute {
  ArticleId article_id;

  bool operator==(const ArticleRoute&) const = default;
};

struct SettingsRoute {
  bool operator==(const SettingsRoute&) const = default;
};

using AppRoute = std::variant<
    ArticleRoute,
    SettingsRoute
>;
```

The shared navigation contract requires Route to be copy-constructible and equality-comparable.
It does not require a framework base class, runtime type name, string identifier, hash, reflection, or built-in serialization protocol.

Applications that use URLs or restoration keep route values serializable by policy.
Routes should contain stable identifiers, enums, and small configuration values rather than domain object graphs, callbacks, Views, FileReference values, or platform handles.
An application resolves a stable identifier through its own model or document session when the destination composes.

### NavigationPath

`NavigationPath<Route>` is a typed value containing the ordered destination data. The public API borrows these values through `Routes()`.

The fixed root page is not an element of NavigationPath.
An empty path displays only that root, and each route element adds one destination above it.
The path stores no View factory, internal entry identifier, transition state, mounted state, or platform value.

NavigationPath is a concrete typed value rather than a type-erased heterogeneous container.
An application that needs heterogeneous destinations uses one explicit `std::variant`, preserving compile-time knowledge of its complete route set.

### Routed NavigationStack

A routed stack receives a root factory, application-owned State, and one total destination resolver:

```cpp
auto path = UseState(NavigationPath<AppRoute>{});

return NavigationStack(
    AppShell,
    path,
    ResolveAppRoute
);
```

The resolver remains ordinary application code:

```cpp
View ResolveAppRoute(const AppRoute& route) {
  return std::visit(
      [](const auto& value) -> View {
        using Route = std::decay_t<decltype(value)>;

        if constexpr (std::same_as<Route, ArticleRoute>) {
          return ArticlePage(value.article_id);
        } else {
          return SettingsPage();
        }
      },
      route
  );
}
```

The application resolver is the single route-to-View mapping.
HuxerUI does not duplicate it in a process-global route registry, string route table, per-page registration modifier, or Runtime callback map.
Large applications may delegate branches of the resolver to feature libraries while assembling the closed AppRoute type and top-level resolver in the application target.

The resolver and each route value are retained by value and invoked inside the destination entry's independent page scope.
When the routed NavigationStack recomposes with an equal path and an updated compatible resolver, existing entry identifiers and page scopes remain stable while their retained factories use the latest resolver value.
The resolver therefore follows the same copy-safety requirements as other delayed HuxerUI View factories and does not capture references whose lifetime ends before a later recomposition.

Destination resolution is total for every accepted route value.
URL parsing, authorization redirects, and malformed parameter handling occur before a route enters the path.
Missing domain content is ordinary destination state and may render an application-owned unavailable or not-found page.

### One authoritative path

The `State<NavigationPath<Route>>` supplied by the application is the only logical history for a routed stack.
A routed controller mutates that State rather than a private logical stack.
The private `NavigationCoordinator` retains non-authoritative requested and realized descriptor snapshots, resolved entries, internal identities, queued visual operations, and transition state needed to reconcile the controlled value without discarding exiting pages early.
Controller `Depth()` is therefore the path size plus the fixed root entry, while `CanPop()` is true exactly when the path is non-empty.

`UseNavigation<Route>()` returns the typed controller. Its Push, Pop, Replace, and SetPath operations edit the authoritative path. The non-template `UseNavigation()` serves factory stacks.

Because the root is fixed and absent from the path, `Replace()` requires a non-empty path and throws `std::logic_error` when only the root is visible.

A NavigationStack is either factory-driven or route-driven for its complete mounted lifetime.
A routed stack rejects factory Push and Replace operations, and a factory stack does not claim a NavigationPath.
Mixing the two entry sources in one stack would make the public path unable to describe the actual history.

### Path reconciliation and identity

The stack compares the previous and next route sequences by their longest equal prefix:

- Appending one value resolves as Push.
- Removing the last value resolves as Pop.
- Replacing only the final value resolves as Replace.
- Changing an earlier value or several values resolves as a path replacement.
- Initial construction resolves the complete path before the first committed page and does not animate through intermediate destinations.

Resolved entries in the equal prefix retain their private identifiers, page scopes, mounted state, scroll state, and PaintSequences.
The changed suffix receives new monotonic entry identifiers.
Route equality expresses route-data equality and never becomes a MountedNode key directly.

Duplicate equal routes are valid.
Their sequence positions resolve to distinct private entries, so pushing the same ArticleRoute twice creates two independent page scopes even though both route values compare equal.

A single Push, Pop, or Replace uses the existing themed transition and operation queue.
An arbitrary path replacement commits the final topology as one replacement operation rather than visually replaying every intermediate route.
Browser Back or Forward that removes or appends one suffix element retains ordinary Pop or Push motion.

### Nested and root navigation

UseNavigation() continues to resolve the nearest enclosing factory stack.
UseNavigation<Route>() resolves the nearest enclosing routed stack with the requested Route type.
Nested stacks remain independent owners and do not share history merely because their routes use the same C++ type.

Cross-feature navigation sometimes intentionally targets the outer application stack.
The explicit operations are:

```cpp
UseRootNavigation();
UseRootNavigation<AppRoute>();
```

Root means the outermost compatible stack in the current Runtime's inherited navigation chain.
It is not a process global, does not cross windows, and does not search a string-keyed registry.
The private navigation Environment therefore becomes a parent-linked chain of weak stack states rather than a single overwritten state.

Code that needs a particular sibling or locally owned stack receives its controller explicitly.
The routed extension does not add NavigationId, named stacks, UseNavigationByName, or a general service locator.

NavigationBar, NavigationPane, Tabs, and responsive multi-pane structures remain selection and layout mechanisms rather than implicit stack operations.
Each top-level destination may retain an independent nested NavigationPath.
A deep link may update the selected destination and its associated path atomically, while an application-wide route may use the root controller for a cross-feature full-page transition.

## Application activation integration

Open With, document URL contexts, share intents, and equivalent requests are application activations rather than navigation commands or FilePicker results.
The boundary defined by [Application Activation and Lifecycle Design](application.md) selects a platform target before delivering an activation to its Runtime.

```text
platform application activation
    -> application activation policy
    -> create or select a platform target
    -> Runtime activation queue
    -> application document or domain service
    -> stable route parameters
    -> root NavigationPath update
```

An initial activation must be available before the first root composition so the application can construct its initial route path without briefly committing the default page.
An activation received by an existing session is delivered on that Runtime's UI thread.
The application decides whether it pushes into the existing history, replaces the path, opens another window, imports content, or rejects the request.

A passive file activation carries FileReference capability values only to the application activation policy or its document service.
The application establishes a document session and places a stable DocumentId in the route rather than embedding FileReference in a URL-backed or restorable path.

Navigation does not add FileEvents, inspect platform intent types, select windows, or retain an activation queue.
Those responsibilities belong to application activation and platform window policy, while NavigationPath remains the destination state consumed after policy has run.

## Web URL integration

Browser navigation is an adapter above the controlled path rather than behavior in NavigationStack or Runtime:

```text
browser URL and history state
    <-> application Route codec
    <-> controlled NavigationPath
    <-> destination resolver
    <-> shared NavigationStack engine
```

The application owns conversion between URLs and typed route values because only the application knows parameter validation, canonical URLs, authorization redirects, and missing-content policy.
The codec is an application value or pair of callables rather than a virtual framework registry:

```cpp
struct AppRouteCodec {
  std::optional<NavigationPath<AppRoute>> Decode(std::string_view location) const;
  std::string Encode(const NavigationPath<AppRoute>& path) const;
};
```

Decode validates path segments, query values, and fragments before constructing typed routes.
Encode produces the canonical browser location for one accepted path.
An empty Decode result invokes application fallback policy such as a not-found destination, redirect, or default path; NavigationStack never receives a malformed route value.

The Web integration owns browser mechanics:

- Application Push maps to `history.pushState()`.
- Application Replace maps to `history.replaceState()`.
- An arbitrary `SetPath()` maps to `history.replaceState()` unless application policy explicitly starts a new browser history entry.
- `popstate` and same-document hash changes decode the browser location and update the controlled path without echoing another history mutation.
- Initial mount decodes the current URL before the first destination is committed.
- Reload reconstructs only serializable route entries and application state.

The explicit Web surface lives in `<huxerui/web/navigation.h>` and keeps the shared stack signature as its prefix:

```cpp
return web::BrowserNavigationStack(
    AppShell,
    path,
    ResolveAppRoute,
    AppRouteCodec{}
);
```

The adapter initializes the controlled path from the current location before building its routed stack.
If `Decode` rejects the initial or a later browser location, the adapter restores the last accepted canonical location; on initial mount the application-provided path is the fallback.
`Encode` returns a same-document path, query, fragment, or same-origin URL suitable for the Browser History API.
Direct writes to `State<NavigationPath<Route>>` use `replaceState()`, while only `RouteNavigationController::Push()` starts a new browser history entry.
Controller Pop uses `history.back()` when the current entry was created by the same adapter from the requested parent path and otherwise replaces the current entry, so popping an initial deep link does not leave the application accidentally.

One `BrowserNavigationStack` may own a browser document's History at a time.
Mounting another session whose root also declares `BrowserNavigationStack` is rejected; additional sessions in that document use ordinary `NavigationStack` values.

Browser History remains outside the generic Runtime and UiWindow host operations because it is application navigation policy rather than a renderer, text, clipboard, or frame-scheduling capability.
A focused Web navigation bridge supplies the routed controller's typed history-commit policy without introducing JavaScript types into shared headers.
The policy commits the accepted canonical browser location and the corresponding controlled State update as one operation, while direct State writes remain replace-only synchronization.

Nested URL routes may resolve to an application-owned aggregate containing a selected top-level destination and one or more nested path values.
URL segment nesting does not force every visual component function to become a NavigationStack, and responsive layout changes do not alter the route solely because the same destination moves between panes.

## Failure and lifetime behavior

Page factories execute during scoped composition and follow the same exception handling as other Scope factories.
A failed factory keeps its scope invalidated for a later frame and does not partially commit a new navigation topology.

The mounted NavigationStack owns its private state while page and container extensions participate in a frame.
External controller copies retain that state weakly, so copies that outlive their NavigationStack cannot dereference Runtime or mounted nodes.
Unmounting the stack releases retained factories and abandons active or queued operations without invoking application page callbacks.

Back sessions retain stable framework handles rather than raw MountedNode pointers.
Tree replacement, page removal, and Runtime destruction invalidate those handles safely.

## Implementation boundaries

The public files are:

- `include/huxerui/event.h` for BackEvent and ViewEvents::BackRequested.
- `include/huxerui/modifier.h` for the general NodeExtension Back capability.
- `include/huxerui/navigation.h` for top app bars, page stacks, the typed path and routed controller, destination selectors, drawers, and their Theme styles.
- `include/huxerui/web/navigation.h` for the typed route-codec and Browser History adapter.
- `include/huxerui/huxerui.h` for the public umbrella include.

`src/components/navigation.cpp` owns the navigation coordinator, navigation-source reconciliation, resolved entries, NavigationStack layout, retained page modifiers, and page motion.
`src/components/navigation_ui.cpp` owns TopAppBar layout, NavigationItem resolution, NavigationBar, NavigationPane, DrawerLayout, and retained drawer presentation.

Runtime changes remain limited to generic Back routing, Back-session capture, and disabled-subtree input cleanup.
Runtime must not include navigation entry types or branch on NavigationStack, NavigationController, MaterialTheme, FlatTheme, Web, or Android.

## Validation

The [navigation tests](../../tests/runtime/navigation.cpp) cover factory and routed stack ownership, keyed retention, lifecycle, Back, and transition policy. The [navigation UI tests](../../tests/runtime/navigation_ui.cpp) exercise destination selectors, drawers, Theme, and focus. The Web history adapter additionally needs browser validation for deep links, edits to the path, Back and Forward, and history feedback suppression. See [Application Activation](application.md) for activation delivery and file capability ownership.

Platform Back validation covers Android's API 34 predictive phases, the Commit-only callback on earlier Android versions, and embedded Android host ownership. Windows, macOS, Linux, and Web route Escape through the shared Commit path.

### Transition validation

Navigation-specific coverage includes root override selection, queued operations, controlled path replacement, predictive seeking, cancellation, reduced motion, viewport changes, and retirement. Shared-element scenarios verify page-scoped matching, ordinary painting for unsupported pairs, clipping and decoration order, and the absence of a second input or accessibility owner. See [Animation](animation.md#implementation-and-acceptance) for shared effect validation.

The [Shared Transitions example](../../examples/shared_transition/main.cpp) exercises page and local matching, and [Transition Studio](../../examples/transition/main.cpp) covers whole-page and Scene effects.
