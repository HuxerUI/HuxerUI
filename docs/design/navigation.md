# Navigation Design

This document defines explicit top app bars, page stacks, destination selection, application drawers, scoped navigation controllers, page transitions, Back routing, typed route paths, Web URL history, and application activation integration.

Factory navigation is deliberately factory-driven and imperative at the navigation boundary.
Typed-route navigation preserves the same private entry, mounting, transition, interaction, and Back engine without introducing route registries, URL concepts, or platform types into the shared Runtime.

Custom page transitions are implemented as described in [Page transition customization](#page-transition-customization).
[Shared-element transitions](#planned-shared-element-transitions) remain approved but unimplemented.

## Goals

- Present a retained stack of ordinary HuxerUI Views with Push, Pop, and Replace operations.
- Preserve page-local state while another page covers it.
- Keep navigation ownership local so independent and nested stacks do not share history accidentally.
- Route Back through framework overlays, public layers, application handlers, and nested page stacks before invoking a platform fallback.
- Drive page transitions through retained presentation state without per-frame recomposition, measurement, layout, or rerecording clean page content; optional decoration records as its sample changes.
- Support reduced motion and deterministic animation tests through the existing AnimationSpec model.
- Cancel input, focus, and text-input work when a page stops accepting interaction.
- Define a platform-neutral predictive Back transaction that Android can drive immediately and other platforms can adopt when their host integration owns an equivalent gesture.
- Add a typed controlled route path for browser URLs, deep links, restoration, and externally selected destinations without replacing the factory API.
- Provide an explicit theme-owned TopAppBar without coupling its actions to page history or menu presentation.
- Provide theme-owned NavigationBar and NavigationPane selectors without coupling selection to page history.
- Provide controlled StartDrawer and EndDrawer content inside ordinary application layout.

## Non-goals

The current factory navigation implementation does not provide:

- Named routes, string route tables, URI matching, or a destination registry.
- A public `Page`, `Route`, or `NavigationEntry` base class.
- A serializable or type-erased path inferred from retained factories.
- Automatic synthesis of application bars, titles, Back buttons, overflow menus, or master-detail page composition.
- Navigation-specific `OnAppear`, `OnDisappear`, or other component lifecycle callbacks.
- Shared-element, hero, container-transform, or cross-page layout animation.
- Automatic suspension or serialization of covered page state.
- Browser History integration in the shared Runtime.
- An iOS edge gesture owned unconditionally by an embedded HuxerUI View.

Saveable state, route serialization, and navigation-aware lifecycle effects build on this contract after their independent ownership rules are defined.
The approved shared-element ownership rules are recorded in the explicitly planned section.

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
A future automatic overflow feature requires a reusable structured command model shared by toolbars, menus, and shortcuts rather than a TopAppBar-only item type.

The [Window Insets and System Bars Design](window-insets.md) gives Runtime one full-window geometry contract.
TopAppBar consumes its top and horizontal insets when they remain available in edge-to-edge content and contributes its themed background to the status region without duplicating platform inset handling.
Medium and Large two-row bars, scrolled-under colors, and pinned or collapsing behavior remain deferred until shared nested-scroll coordination exists.

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
Runtime resolves the highest painted enabled trap generically, so Layer presentation and application drawers share focus confinement and restoration rather than maintaining separate mechanisms.
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

The page-stack public API lives in `<huxerui/navigation.h>`:

```cpp
namespace huxerui {

namespace detail {
class NavigationState;
}

class NavigationController {
public:
  NavigationController() = default;

  void Push(std::function<View()> page) const;

  template <class Factory, class... Arguments>
  void Push(Factory&& page, Arguments&&... arguments) const;

  bool Pop() const;
  void Replace(std::function<View()> page) const;

  template <class Factory, class... Arguments>
  void Replace(Factory&& page, Arguments&&... arguments) const;

  [[nodiscard]] bool CanPop() const;
  [[nodiscard]] std::size_t Depth() const;

private:
  explicit NavigationController(std::weak_ptr<detail::NavigationState> state);

  std::weak_ptr<detail::NavigationState> state_;

  friend View NavigationStack(std::function<View()> root);
  friend NavigationController UseNavigation();
  friend NavigationController UseRootNavigation();
};

View NavigationStack(std::function<View()> root);

template <class Factory, class... Arguments>
View NavigationStack(Factory&& root, Arguments&&... arguments);

NavigationController UseNavigation();
NavigationController UseRootNavigation();

} // namespace huxerui
```

The API deliberately uses `Push`, `Pop`, and `Replace` rather than `PushPage` and `PopPage` because the controller type already establishes the navigation domain.

`Push` appends a new logical entry.
`Pop` returns false when the logical stack contains only its root or when the controller is no longer connected.
`Replace` replaces the logical top entry; replacing a depth-one stack creates a new unpoppable root.

An empty page factory is invalid and throws `std::invalid_argument`.
`Push` or `Replace` through a disconnected controller throws `std::logic_error`, matching operations that require a live owner.
Read-only queries return zero or false after disconnection, and `Pop` returns false.
Controller operations run on the Runtime UI thread and do not add cross-thread synchronization to navigation state.

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

Pop-to, reset, and result-returning navigation remain deferred until concrete application use demonstrates the smallest coherent API.

## Why pages are factories

`View` is a transient declaration value and is not retained application state.
A page factory allows Runtime to create and recompose a page under the NavigationStack's Environment and independent RecomposeScope at the correct time.

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

Each NavigationStack owns one shared private navigation state.
The stack scope holds the strong reference, while public NavigationController copies hold weak references so a page factory that captures its controller cannot form a cycle through the controller state.
Controller mutations invalidate only that NavigationStack scope through its retained invalidation binding; they do not retain Runtime directly or recompose the application root.

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

Runtime uses shared disabled-subtree input cleanup rather than a navigation-specific `DeactivateNavigationInput()` path.
The shared cleanup cancels pointer capture and observers, clears hover, clears focus that belongs to the disabled subtree, and ends an owned text-input session with `TextInputEndReason::FocusLost`.

## Focus and text input

Leaving a page clears focus when the focused node belongs to the outgoing subtree and stops its platform text-input session.
Returning to a retained page restores its component and scroll state but does not automatically focus its previous TextField or reopen the system keyboard.

Automatic focus restoration is not supported because it could unexpectedly reopen mobile IMEs after Pop.

Dialogs and other trap-focus Layers continue to use their existing focus restoration semantics.
Because layers route Back before page navigation, dismissing a dialog restores application focus without changing the page stack.

## Theme-owned page motion

Navigation motion is a Theme value because Material, Flat, future iOS, and third-party themes may choose different page-transition geometry.
The logical stack and controller never inspect a concrete Theme type.

NavigationStyle::motion is an optional PageTransition, selecting independent Push, Pop, and Replace descriptions.
An explicit page-root declaration overrides the complete theme policy; see [Page configuration](#page-configuration).

The built-in themes use opaque SlideTransition effects by default.
Flat moves the entering and covered pages by one full viewport in opposite directions.
Material moves the entering page by one viewport and the covered page by a 0.2-viewport parallax offset.
Both themes use Reversed() for Pop, with their own timing, and use the forward effect for Replace.
NavigationStyle::Default() supplies a full-width slide with 0.3-second forward and 0.25-second reverse motion; a directly constructed empty NavigationStyle has no motion.

PresentationMotion continues to own window-level surfaces.
PageTransition describes a pair of application pages and shares TransitionSpec with scene transitions without changing layer lifecycles.

An absent motion disables animation.
Reduced-motion resolution uses the existing Theme and animation path and settles to the target without adding a Navigation-specific accessibility switch.

## Transition execution

One retained extension owns the authoritative transition progress:

```text
NavigationTransitionExtension
    advances or seeks one MotionController
        -> source NavigationPageExtension
        applies source transform and opacity
        -> destination NavigationPageExtension
        applies destination transform and opacity
```

The two pages must not own independent animation clocks.
One shared progress guarantees synchronized page geometry, one completion condition, deterministic cancellation, and direct predictive Back seeking.

Programmatic transitions advance progress through AnimationSpec.
Predictive Update clamps platform progress to `[0, 1]` and sets progress directly.
Cancel retargets the current value to zero, while Commit retargets it to one.
Tween completion time scales with remaining distance where necessary, and SpringSpec preserves the existing retargeting behavior and velocity rules.

The parent transition extension advances before page extensions read progress in the same frame.
Completion updates render retention and begins the next queued operation without asking either page to own frame scheduling.

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

The platform-neutral event is:

```cpp
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
```

Runtime retains `bool HandleBack()` as the ordinary Commit convenience and adds `bool HandleBack(const BackEvent& event)` for predictive integration.

Runtime routes Back in this order:

```text
framework-owned TextSelectionOverlay
    -> topmost public Layer that does not pass through Cancel
    -> deepest enabled application Back consumer in reverse paint order
         -> explicit View BackRequested event before extensions on the same node
    -> platform fallback
```

Runtime captures the selected consumer at Begin.
Update, Cancel, and Commit target the same consumer rather than rerunning routing against a tree that may have changed during the gesture.
If the captured mounted target disappears, Runtime ends the transaction safely without dereferencing stale state or unexpectedly falling through to a system window close.

NodeExtension gains a general Back capability instead of Runtime checking for a Navigation component type:

```cpp
class NodeExtension {
public:
  [[nodiscard]] virtual bool OnBack(
      ViewNode& node,
      const BackEvent& event
  );
};
```

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
Runtime selects the handler at Begin but invokes it only at Commit.
Cancel does not invoke application code.

A guarded page does not display a predictive page preview because the destination is not authoritative until the handler decides whether to Pop.
An explicit `navigation.Pop()` is an already-authorized application operation and bypasses BackRequested, allowing the positive dialog action to complete without recursion.

This reuses typed View events and avoids separate PopGuard, NavigationOptions callback, and BackHandler abstractions.
If future application requirements need progress-aware custom Back visuals, the general NodeExtension capability remains available without changing NavigationController.

## Platform Back mapping

Desktop and Web platforms initially produce Commit-only Back requests:

- Windows maps Escape through ordinary key dispatch.
- macOS maps Escape through ordinary key dispatch.
- Web maps Escape through ordinary key dispatch and does not call browser history automatically.

Android uses the richest available platform callback while retaining API 23 as the minimum:

- API 34 and later map `OnBackAnimationCallback` start, progress, cancel, and invoke callbacks to Begin, Update, Cancel, and Commit.
- API 33 maps `OnBackInvokedCallback` to Commit.
- API 23 through 32 map the Activity Back callback to Commit.
- A completely unhandled Commit invokes the Activity's platform fallback.

The full-screen HuxerUIActivity owns callback registration and Activity fallback behavior.
An embedded HuxerUIView exposes the Runtime Back operations but does not finish its containing Activity or assume ownership of another navigation system.

iOS does not install an unconditional edge recognizer.
A full-screen integration may later map an owned edge-pan gesture to the same BackEvent phases, while an embedded View must not steal a UIViewController navigation controller's interactive-pop gesture.

OHOS and future platforms map their platform navigation gestures or commands to the same shared transaction rather than adding platform-specific navigation state.

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

The public value is:

```cpp
template <class Route>
class NavigationPath {
public:
  NavigationPath() = default;
  NavigationPath(std::initializer_list<Route> routes);
  explicit NavigationPath(std::vector<Route> routes);

  [[nodiscard]] bool Empty() const noexcept;
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] std::span<const Route> Routes() const noexcept;

  bool operator==(const NavigationPath&) const = default;
};
```

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
The private NavigationState retains non-authoritative requested and realized descriptor snapshots, resolved entries, internal identities, queued visual operations, and transition state needed to reconcile the controlled value without discarding exiting pages early.
Controller `Depth()` is therefore the path size plus the fixed root entry, while `CanPop()` is true exactly when the path is non-empty.

The routed controller surface is:

```cpp
template <class Route>
class RouteNavigationController {
public:
  void Push(Route route) const;
  bool Pop() const;
  void Replace(Route route) const;
  void SetPath(NavigationPath<Route> path) const;

  [[nodiscard]] bool CanPop() const;
  [[nodiscard]] std::size_t Depth() const;
};

template <class Route>
RouteNavigationController<Route> UseNavigation();
```

The concrete controller type is normally inferred with `auto`, while the existing non-template `NavigationController` and `UseNavigation()` continue to serve factory stacks.
This avoids turning the existing controller into a `NavigationController<void>` specialization or accepting arbitrary route types through runtime type checks.
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

Browser History must not be exposed through PlatformAdapter because it is application navigation policy rather than a renderer, text, clipboard, or frame-scheduling capability.
A focused Web navigation bridge supplies the routed controller's typed history-commit policy without introducing JavaScript types into shared headers.
The policy commits the accepted canonical browser location and the corresponding controlled State update as one operation, while direct State writes remain replace-only synchronization.

Nested URL routes may resolve to an application-owned aggregate containing a selected top-level destination and one or more nested path values.
URL segment nesting does not force every visual component function to become a NavigationStack, and responsive layout changes do not alter the route solely because the same destination moves between panes.

## Future work

- Define saveable navigation state without implying that factory-only entries are serializable.
- Add typed navigation results without introducing a second controller or route registry.
- Extend adaptive navigation and platform transitions while preserving one authoritative path and application-owned URL policy.

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

`src/components/navigation.cpp` owns controller state, navigation-source reconciliation, resolved entries, NavigationStack layout, retained page modifiers, and page motion.
`src/components/navigation_ui.cpp` owns TopAppBar layout, NavigationItem resolution, NavigationBar, NavigationPane, DrawerLayout, and retained drawer presentation.

Runtime changes remain limited to generic Back routing, Back-session capture, and disabled-subtree input cleanup.
Runtime must not include navigation entry types or branch on NavigationStack, NavigationController, MaterialTheme, FlatTheme, Web, or Android.

A `navigation_internal.h` is justified only if multiple implementation files share a private contract that cannot remain behind NodeExtension or View declarations.
Controller state alone does not justify another private header.

## Validation

Shared navigation work requires focused Runtime tests for:

- A root-only stack and rejected root Pop.
- Push, Pop, and Replace logical history.
- Distinct identity when the same factory is pushed more than once.
- Page-local State, ScrollController, and retained modifier preservation while covered.
- Removal only after exit completion.
- Predictive Cancel preserving both logical history and mounted state.
- Nested stacks resolving the nearest controller.
- Inner-root Back falling through to an enclosing stack.
- Disconnected controller queries and mutations.
- Deterministic operation serialization.
- Empty factories and exception categories.

TopAppBar tests verify Start and Center title geometry, excess-action constraints, action behavior, Heading semantics, required configuration, and built-in Theme styles.
Destination selection tests verify collection hierarchy, accessible labels, selected and disabled states, semantic activation, identity stability, icon requirements, disabled keyboard traversal, and dynamic compact or expanded Pane composition.
Drawer tests verify responsive inline fallback, controlled modal state, gesture and Back ordering, exit-time focus confinement, and built-in Theme styles.

Transition and incremental-rendering tests verify:

- One shared progress value controls both pages.
- Stable frames do not recompose, remeasure, relayout, or rerecord clean pages.
- Transform and opacity changes update conservative damage for old and new bounds.
- NavigationStack clipping contains transformed drawing and hit testing.
- Viewport changes preserve normalized progress and remeasure the active pair.
- Reduced motion completes without continuing frame requests.
- Tween, Spring, Cancel, Commit, and retarget behavior are deterministic under supplied frame times.

Interaction tests verify:

- Outgoing pointer capture and observers receive Cancel.
- Hover and keyboard activation are cleared from an inactive page.
- Focus leaves an inactive page.
- An owned text-input session ends with FocusLost.
- Covered pages cannot receive pointer, key, focus, or text-input events.
- Predictive Cancel does not cause focus or IME churn.

Back tests verify the complete order among TextSelectionOverlay, Layer entries, BackRequested, nested NavigationStacks, and platform fallback.

Typed routed-navigation tests additionally verify:

- Empty, single-route, duplicate-route, and heterogeneous variant paths.
- `Push()`, `Pop()`, `Replace()`, and arbitrary `SetPath()` reconciliation.
- Longest-equal-prefix retention and changed-suffix replacement.
- Stable page state within an equal prefix and independent identity for duplicate equal routes.
- One application-owned path as the logical source of truth while private render entries outlive an exit transition.
- Initial deep paths committing their final destination without replaying intermediate transitions.
- A routed stack rejecting factory operations and a factory stack exposing no route path.
- Nearest and root controller lookup through nested stacks, including nested stacks with the same Route type.
- Controller disconnection without retaining Runtime, mounted nodes, or a resolver cycle.
- Total destination resolution and propagation of resolver composition failures through the existing frame exception boundary.

Platform validation includes:

- Android API 34 Begin, Update, Cancel, and Commit JNI and Java mapping.
- Android API 33 Commit mapping.
- Android API 23 through 32 Activity fallback.
- Embedded Android View behavior without Activity ownership.
- Windows, macOS, and Web Escape Commit behavior.
- Available platform builds after shared Back API changes.

Web URL integration requires browser tests for initial deep links, `Push()`, `Replace()`, `SetPath()`, Back, Forward, reload, invalid URLs, canonical encoding, duplicate routes, nested route projections, and suppression of history-update feedback loops.
Application activation integration requires platform tests for cold-start delivery before first composition, warm delivery on the selected Runtime UI thread, session selection, Open With file capability lifetime, and rejection or routing policy without a committed Root View callback.

## Invariants

- NavigationStack is an application-tree container, not a Layer or root service.
- TopAppBar actions remain ordinary Views and never imply navigation, menu, or automatic overflow policy.
- A page is an ordinary View factory with an independent keyed RecomposeScope.
- NavigationController resolves the nearest scoped stack and does not own Runtime or mounted nodes.
- The stack scope is the only strong owner of controller state.
- A routed stack has exactly one application-owned NavigationPath as its logical history.
- Factory and routed entries never coexist in one NavigationStack.
- Route values contain destination data, while private entry identifiers define mounted page identity.
- A destination resolver is application-owned and total; Runtime owns no route registry or route-type branch.
- Nearest and root lookup remain scoped to one inherited navigation chain and never become process-global lookup.
- Entry identifiers, not factory identity or unkeyed position, define mounted page identity.
- Covered entries retain state but do not receive input or contribute visible damage.
- Only the active page or active transition pair participates in navigation layout.
- One progress value drives both pages of a transition.
- Animation frames change presentation state without page recomposition or layout.
- Runtime routes Back through generic events and NodeExtension capabilities without concrete Navigation branches.
- Layers consume Back before page navigation.
- Explicit Pop is an authorized operation and does not invoke BackRequested recursively.
- Navigation does not automatically restore TextField focus or reopen IME.
- Factory entries make no serialization, URL, or restoration claim.
- Route paths resolve into the same private page-entry engine rather than creating another navigator.
- Application activation selects a target Runtime before it requests route state and never targets an arbitrary committed View.
- Browser URL policy remains outside shared Runtime and PlatformAdapter.

## Page transition customization

Custom page motion uses reusable effects inside the existing navigation engine.
The common `TransitionSpec`, effect evaluation, timing, reversal, and scene contract are defined in [Animation and Scene Transitions](animation.md#extensible-transitions).
Navigation reuses that contract instead of introducing another effect interface or animation scheduler.

### Page configuration

`PageTransition` is both the complete page-transition configuration and a View modifier:

```cpp
struct PageTransition {
  TransitionSpec push;
  TransitionSpec pop;
  TransitionSpec replace;

  static const detail::ModifierDescriptor& Descriptor();
  bool operator==(const PageTransition&) const = default;
};
```

An application attaches the value directly to the root of an ordinary page:

```cpp
auto enter = TransitionSpec(LiftTransition{}, TweenSpec{0.28, Easing::EaseOut});
auto product_transition = PageTransition{
    .push = enter,
    .pop = enter.Reversed(),
    .replace = TransitionSpec(FadeTransition{}, TweenSpec{0.18, Easing::EaseInOut}),
};

return Column {
  Text("Product"),
  Text("Product details"),
}.With(product_transition);
```

`LiftTransition` is the application-defined effect illustrated in the animation design.
Page-transition policy uses View::With() without layout metadata, a public page wrapper, or a new generic View method.
An internal retained check validates root placement; it owns no progress or effect execution.

The declaration belongs to the page root.
Compiler-generated Scope and transparent Environment boundaries may surround that root, but lookup does not search arbitrary layout descendants.
A declaration on an unsupported descendant is diagnosed instead of silently controlling its containing page.
Multiple declarations on the same root follow ordinary property-modifier ordering: the last complete value wins.

`NavigationStyle::motion` is `std::optional<PageTransition>`.
An explicit page declaration overrides the themed value as a whole; there is no per-field merge.
`PageTransition{}` explicitly selects immediate Push, Pop, and Replace because a default `TransitionSpec` completes immediately.
Without a page declaration, the resolved theme value applies; an absent themed value also means immediate presentation.
Flat and Material themes explicitly supply their intended motion instead of depending on accidental aggregate defaults.

| Operation | Selected configuration |
| --- | --- |
| Push | Incoming page's `push` |
| Pop or predictive Back | Departing page's `pop` |
| Replace or arbitrary changed-path replacement | Incoming page's `replace` |

The selected configuration is captured when the operation starts and remains stable until it finishes.
Recomposition or theme changes configure later operations without restarting an active transition.
Initial stack construction and initial deep paths present their final active destination immediately.

### Navigation execution and interruption

The existing entry engine, operation queue, retained pages, and factory-versus-controlled-path ownership remain authoritative.
Transition customization changes presentation, not when a controlled path becomes authoritative or how factory operations reserve and commit history.
One normalized progress value drives the outgoing and incoming pages; planned shared elements will consume the same value.
Effects evaluate against the current stack bounds and update retained presentation without per-frame state writes, recomposition, measurement, layout, or rerecording clean page content.
Optional foreground decoration records into its own PaintSequence as the sampled progress changes.

Predictive Back seeks this same progress directly and ignores the configured start delay during gesture control.
Commit settles toward one and Cancel settles toward zero using the existing controller.
Velocity is handed through `Seek` only when a real velocity sample is available; the engine must not invent one from an absent platform value.
At Cancel, source presentation and the original interaction ownership are restored.
The departing page's Pop declaration supplies the effect for both interactive and ordinary Back.

A viewport change preserves normalized page progress and reevaluates page effects in the new bounds after required layout.
The planned shared-element contract ends pairs captured in the former coordinate space while page motion continues.
Reduced motion takes the same logical navigation outcome without retaining animation frame work.
Unmounting the owner releases all visual retention and controller work through the existing lifecycle.

Custom effects do not bypass navigation's input, focus, IME, clipping, or page-activation rules.
Both pages disable content interaction during an active navigation operation and restore the final page after completion or cancellation; the container still handles Back and predictive cancellation.
Fragments only replicate render data, so there is no per-fragment hit-test, focus, or accessibility representation.
Their transforms, source clips, and optional synchronized foreground decoration use the shared rendering primitives described in [Animation](animation.md#fragment-composition-and-decoration).
No new completion event, transition-operation handle, result type, or public cancellation protocol is introduced.

An explicit Scene transition whose mutation also changes navigation composes with page presentation.
There is no automatic suppression based on which system began first.
An application that wants only the Scene effect selects immediate page motion for that operation's participating page.
Planned shared elements follow navigation progress, not the independent Scene clock.

## Planned shared-element transitions

Status: approved design, not implemented.
All shared-element declarations and examples below describe planned APIs rather than available SDK behavior.

### Shared-element declarations and matching

Two property modifiers describe the intended relationship:

```cpp
Image(product.image).With(SharedElement(product.id));

Column {
  Text(product.name),
  Text(product.description),
}.With(SharedBounds(product.id));
```

`SharedElement(key)` denotes the same visual content moving between page locations.
`SharedBounds(key)` denotes different content whose regions transition together, including source-to-target crossfade.
They use the same integer and string key forms as `View::Key`, but shared matching has a separate identity domain and does not affect reconciliation keys.

The nearest `NavigationStack` owns the matching scope.
Only the active outgoing and incoming pages participate in a Push, Pop, or Replace operation.
Keys never match across nested stacks, independent stacks, windows, or unrelated Scene transitions.
Static pages and initial stack construction do not start shared-element motion.

A key may occur once in each participating page.
Duplicate keys within a participant are caller errors; using different marker kinds for the same matched key is also an error.
Matching an ancestor and its descendant would create overlapping retained representations, so such pairs are rejected in the first implementation.
Applications choose one enclosing `SharedBounds` or independently marked, nonoverlapping children.

The engine resolves pairs once, using the first committed layout that provides valid participating page geometry.
An unmatched key, unrealized virtual item, unavailable asset, or target without valid laid-out bounds skips only that element's visual transition.
There is no implicit network wait, deferred navigation, or late joining after a pair set has been captured.
Applications that require a particular pair prepare its content and scroll position before requesting navigation.

### Shared bounds transforms

The default shared geometry interpolates source and target bounds using navigation progress.
An application can provide a copied, equality-comparable value through the marker's component-specific fluent method:

```cpp
Image(product.image).With(
    SharedElement(product.id).BoundsTransform(ArcBounds{36.0F})
);
```

The application-defined bounds transform has this evaluation contract:

```cpp
Rect Evaluate(Rect from, Rect to, float progress) const;
```

`ArcBounds` is an example application value, not another framework type.
The callable is immutable and pure, preserves the supplied endpoint rectangles at zero and one, and returns finite valid bounds.
The engine privately erases its value type and compares configuration without a public bounds-transform interface or registry.
It follows the page transition's progress, including reversal and predictive cancellation, and has no independent `AnimationSpec` or clock.
The shared-element geometry does not expand `TransitionContext` with source and destination node data.

### Shared-element retention and fallback

Original nodes remain mounted and retain their normal layout, state, and lifecycle.
The engine suppresses their duplicate painting while a retained shared representation supplies the transition visual.
That representation appears above the stack's pages, below window layers, and inside the stack's clipping boundary.
It creates no additional pointer target, focus target, accessibility node, or text-input owner.
Input and semantics continue to follow the existing active-page policy.

The first implementation remaps retained rendering into interpolated bounds rather than laying out the subtree on every frame.
`SharedElement` requires visually corresponding content; it does not infer semantic equivalence from a key.
`SharedBounds` can retain both sides and crossfade their content as the region changes.
Text scales or crossfades with that rendering; glyph morphing and intermediate line-wrap reflow are outside the contract.

| Condition | Behavior |
| --- | --- |
| Missing counterpart, unavailable content, or invalid geometry | Skip the affected pair and keep ordinary page presentation |
| Source or target removed or replaced during motion | End that pair and restore painting for every surviving original |
| Viewport or coordinate-space change | End affected pairs and continue page motion with current bounds |
| Predictive Back cancellation | Reverse the retained pair with page progress, then restore source presentation |
| Stack destruction | Release all retained representations and references |
| Reduced motion or immediate page transition | Present the final logical state without retaining shared animation work |

Supported content initially includes ordinary paint, images, and geometry representable by valid two-dimensional bounds.
Native `PlatformView` content, continuously updated external textures, and transforms whose rotation or skew cannot be represented by this bounds contract use an explicit pair-level fallback to ordinary page presentation.
The implementation must not claim frozen external-texture pixels merely because it retains a resource reference.
Fallback leaves the remaining pairs and logical navigation intact and uses existing diagnostics or tracing to identify the reason.

Retained visual ownership is bounded by the operation and the existing rendering resources it captures.
Implementation must preserve the [native-content and scene-retention contract](animation.md#native-content-and-scene-retention), including native-view eligibility and release of captured resources at the end of their lifetime.
No fixed structural quota is defined for shared-element retention; capability failures preserve navigation and restore surviving originals.
There is no public resource-budget object in this design.

### Planned implementation boundaries

Public timing and effect declarations belong in `animation.h`; page policy and shared markers belong in `navigation.h`.
The navigation state and existing page/container extensions own matching, progress, suppression, restoration, and cleanup.
Private shared declarations should move into a focused internal header only when multiple implementations actually need the contract.
This design introduces no global coordinator, plugin interface, context store, root service, or second retained-page engine.
Any rendering support added to Runtime remains generic and must not inspect concrete Navigation components or shared-marker types.

Shared-element retention and matching will build on the implemented common effects and page/Scene owners.
This planned extension introduces neither compatibility aliases nor a parallel page-configuration path.
Ordinary View appearance/disappearance, automatic layout animation, three-dimensional transforms, arbitrary shaders, and effect filters remain separate work.

## Transition validation

Common effect, timing, and Scene acceptance criteria remain in the animation design.
Page-transition tests cover the implemented policy and execution paths; shared-element items below remain acceptance criteria for the planned extension:

- Complete page override, theme fallback, explicit immediate defaults, modifier ordering, transparent page-root boundaries, and misplaced declarations.
- Incoming Push and Replace selection, departing Pop selection, immutable active configuration, queued operations, and controlled path replacement.
- Initial and deep-path presentation, independent nested stacks, retained page state, and absence of per-frame recomposition, layout, or rerecording clean page content.
- Deterministic predictive seeking, velocity availability, commit, cancellation, reduced motion, interruption, viewport changes, and owner destruction.
- Separate reconciliation and shared identities, duplicate and mismatched marker errors, and rejection of overlapping ancestor/descendant pairs.
- One-time pairing, missing counterparts, unavailable images, unrealized virtual targets, late content, and independent fallback without stopping other pairs.
- Custom bounds endpoints, finite geometry, overshoot handling, reversal, draw order, clipping, paint suppression, and restoration after replacement or removal.
- Source and target state retention without duplicate input, focus, semantics, or IME ownership, including predictive cancellation.
- Ordinary image and painted-content transitions, text scale/crossfade boundaries, native-content fallback, interruption, and resource release.
- Explicit Scene-plus-navigation composition with independent clocks and intentional immediate page configuration.

A Gallery demonstration should cover a card-to-detail image transition, a differing-content `SharedBounds` transition, a custom page effect, and a custom Scene effect without becoming a new showcase framework.
Predictive Back, reduced motion, interrupted transitions, and unsupported-content fallback need executable coverage beyond visual inspection.
Changes to shared presentation or drawing require a renderer audit and every affected platform build available locally; unavailable platforms must be reported explicitly.
