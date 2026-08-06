# Navigation Design

Status: implemented

This document defines the implemented contract for page stacks, destination selection, application drawers, scoped navigation controllers, page transitions, and Back routing, together with the extension boundary for future URL-backed navigation.

The first implementation is intentionally factory-driven and imperative at the navigation boundary.
It preserves a separate extension point for a future controlled, serializable navigation path without introducing route registries, URL concepts, or platform types into the shared Runtime.

## Goals

- Present a retained stack of ordinary HuxerUI Views with Push, Pop, and Replace operations.
- Preserve page-local state while another page covers it.
- Keep navigation ownership local so independent and nested stacks do not share history accidentally.
- Route Back through framework overlays, public layers, application handlers, and nested page stacks before invoking a platform fallback.
- Drive page transitions through retained presentation state without per-frame recomposition, measurement, layout, or PaintSequence recording.
- Support reduced motion and deterministic animation tests through the existing AnimationSpec model.
- Cancel input, focus, and text-input work when a page stops accepting interaction.
- Define a platform-neutral predictive Back transaction that Android can drive immediately and other platforms can adopt when their host integration owns an equivalent gesture.
- Preserve a direct path to browser URLs, deep links, and saveable navigation state through a future controlled route path.
- Provide theme-owned NavigationBar and NavigationPane selectors without coupling selection to page history.
- Provide controlled StartDrawer and EndDrawer content inside ordinary application layout.

## Non-goals

The initial implementation does not provide:

- Named routes, string route tables, URI matching, or a destination registry.
- A public `Page`, `Route`, or `NavigationEntry` base class.
- A serializable or type-erased NavigationPath.
- Automatic application bars, titles, Back buttons, or master-detail page composition.
- Navigation-specific `OnAppear`, `OnDisappear`, or other component lifecycle callbacks.
- Shared-element, hero, container-transform, or cross-page layout animation.
- Automatic suspension or serialization of covered page state.
- Browser History integration in the shared Runtime.
- An iOS edge gesture owned unconditionally by an embedded HuxerUI View.

Saveable state, route serialization, shared-element transitions, and navigation-aware lifecycle effects build on this contract after their independent ownership rules are defined.

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

## Destination selection and drawers

NavigationBar and NavigationPane share NavigationItem and NavigationEvents::Changed.
They are controlled selection views: the selected index enters through construction and a requested index leaves through the typed event.
They do not create pages, retain destination history, or assume that selecting an item always replaces visible content.
An application may use the selection to switch sibling content, select a tab-owned NavigationStack, or update a future URL-backed route path.

NavigationBar lays destinations along the horizontal axis.
NavigationPane lays them vertically, supports compact icon-only and expanded icon-and-label presentation, and scrolls when its destinations exceed the viewport.
NavigationBar and compact NavigationPane items require icons, while an expanded NavigationPane may use label-only items.
Keyboard traversal lives in one retained selection behavior shared by the two controls and skips disabled destinations.
Geometry, colors, indication, and selection motion remain separate NavigationBarStyle and NavigationPaneStyle Theme values because the two surfaces follow different visual specifications.

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

The initial public API lives in `<huxerui/navigation.h>`:

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
};

View NavigationStack(std::function<View()> root);

template <class Factory, class... Arguments>
View NavigationStack(Factory&& root, Arguments&&... arguments);

NavigationController UseNavigation();

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

The initial API does not return a page identifier because public operations address the logical top of one scoped stack.
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
`[[huxerui::scope]]` is used only when a reusable component needs independent local state, a local event hub, or a local recomposition boundary.

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

- The destination becomes the active interaction subtree.
- The source remains renderable for its exit but stops receiving new input.
- Existing source pointer sessions are cancelled.

During a predictive Pop:

- The current page remains the active interaction subtree until Commit.
- The revealed page remains non-interactive while progress is tentative.
- Cancel restores the current stable state without focus or IME churn.
- Commit deactivates the outgoing page and completes the Pop.

Runtime should generalize disabled-subtree input cleanup rather than add `DeactivateNavigationInput()` beside the existing Layer-specific path.
The shared cleanup cancels pointer capture and observers, clears hover, clears focus that belongs to the disabled subtree, and ends an owned text-input session with `TextInputEndReason::FocusLost`.

## Focus and text input

Leaving a page clears focus when the focused node belongs to the outgoing subtree and stops its native text-input session.
Returning to a retained page restores its component and scroll state but does not automatically focus its previous TextField or reopen the native keyboard.

Automatic focus restoration would require a generic nested FocusScope contract and could unexpectedly reopen mobile IMEs after Pop.
It remains separate future work rather than a navigation-specific identity map in Runtime.

Dialogs and other trap-focus Layers continue to use their existing focus restoration semantics.
Because layers route Back before page navigation, dismissing a dialog restores application focus without changing the page stack.

## Theme-owned page motion

Navigation motion is a Theme value because Material, Flat, future iOS, and third-party themes may choose different page-transition geometry.
The logical stack and controller never inspect a concrete Theme type.

The public values are:

```cpp
struct NavigationMotion {
  Point entering_offset_fraction;
  Point covered_offset_fraction;
  float entering_scale = 1.0F;
  float covered_scale = 1.0F;
  float entering_opacity = 1.0F;
  float covered_opacity = 1.0F;
  AnimationSpec push = TweenSpec{.duration = 0.3};
  AnimationSpec pop = TweenSpec{.duration = 0.25};

  bool operator==(const NavigationMotion&) const = default;
};

struct NavigationStyle {
  std::optional<NavigationMotion> motion;

  static NavigationStyle Default();

  bool operator==(const NavigationStyle&) const = default;
};
```

Offset fractions multiply the current NavigationStack width and height.
An entering offset of `{1.0F, 0.0F}` expresses a full-width horizontal slide, while a small fraction expresses a shared-axis-like shift and `{}` expresses no translation.

The built-in themes use opaque horizontal transitions by default.
Flat moves the entering and covered pages by one full viewport in opposite directions, while Material moves the entering page by one viewport and gives the covered page a shorter parallax offset.
Custom themes may still combine offsets with scale and opacity.

Push interpolates the covered page from its stable values toward the covered values and the entering page from its entering values toward its stable values.
Pop reverses the same pair.

Point offsets, scale, opacity, and independent push and pop AnimationSpecs can express:

- A Material shared-axis-like slide and fade.
- A Flat fade or short displacement.
- A future iOS-style full-width slide with a partially displaced covered page.
- A scale-and-fade transition used by another Theme.

NavigationMotion is distinct from PresentationMotion.
PresentationMotion describes one window-level surface entering or leaving a Layer placement, while NavigationMotion synchronizes two application pages and supports direct progress seeking.
Combining them would make the meaning of initial scale, slide distance, and exit timing depend on the consumer.

An absent motion disables animation.
Reduced-motion resolution uses the existing Theme and animation path and settles to the target without adding a Navigation-specific accessibility switch.

## Transition execution

One retained extension owns the authoritative transition progress:

```text
NavigationTransitionExtension
    advances or seeks one AnimatedValue<float>
        -> source NavigationPageExtension
        applies source transform and opacity
        -> destination NavigationPageExtension
        applies destination transform and opacity
```

The two pages must not own independent animation clocks.
One shared progress guarantees synchronized page geometry, one completion condition, deterministic cancellation, and direct predictive Back seeking.

Programmatic transitions advance progress through AnimationSpec.
Predictive Update clamps native progress to `[0, 1]` and sets progress directly.
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
If the captured mounted target disappears, Runtime ends the transaction safely without dereferencing stale state or unexpectedly falling through to a native window close.

NodeExtension gains a general Back capability instead of Runtime checking for a Navigation component type:

```cpp
class NodeExtension {
public:
  [[nodiscard]] virtual bool OnBack(
      MountedNode& node,
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
  struct BackRequested : Event<> {};
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

Android uses the richest available native callback while retaining API 23 as the minimum:

- API 34 and later map `OnBackAnimationCallback` start, progress, cancel, and invoke callbacks to Begin, Update, Cancel, and Commit.
- API 33 maps `OnBackInvokedCallback` to Commit.
- API 23 through 32 map the Activity Back callback to Commit.
- A completely unhandled Commit invokes the Activity's native fallback.

The full-screen HuxerUIActivity owns callback registration and Activity fallback behavior.
An embedded HuxerUIView exposes the Runtime Back operations but does not finish its containing Activity or assume ownership of another navigation system.

iOS does not install an unconditional edge recognizer in the initial implementation.
A full-screen integration may later map an owned edge-pan gesture to the same BackEvent phases, while an embedded View must not steal a UIViewController navigation controller's interactive-pop gesture.

OHOS and future platforms map their native navigation gestures or commands to the same shared transaction rather than adding platform-specific navigation state.

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

Applications that need history to survive a structural replacement hoist the authoritative domain selection or future controlled NavigationPath above that branch.
A future NavigationSplitView is a separate adaptive container rather than a behavior switch inside NavigationStack.

Tabs also remain independent.
Each tab may own a nested NavigationStack when the tab-content container retains inactive tab subtrees, but Tabs does not acquire page-history ownership.

## Component lifecycle

Navigation visibility is not equivalent to component mount state.
A covered page remains mounted, an exiting page remains mounted until animation completion, and a predictive Pop may be cancelled.

Navigation therefore does not add `OnPageEnter`, `OnPageLeave`, `OnAppear`, or `OnDisappear` callbacks.
Composition-scoped effects define setup and cleanup around actual mounted lifetime when that roadmap item is implemented.

If applications later need to pause work while a retained page is covered, a read-only navigation activity value may be exposed through Environment and observed by an effect.
It must not turn navigation callbacks into a second lifecycle system.

## Future controlled path

Factory navigation is convenient but cannot represent browser URLs, deep links, restoration, or externally controlled history.
The internal architecture must therefore separate logical route data from resolved render entries from its first implementation:

```text
navigation source
    factory command or future route path
        -> page resolver
        -> resolved entry identity and page factory
        -> shared mounting, transition, interaction, and Back engine
```

The initial factory API is one navigation source.
A future typed path supplies lightweight, equality-comparable, and serializable route values plus an application destination resolver.

A possible future shape is:

```cpp
using AppRoute = std::variant<
    HomeRoute,
    ArticleRoute,
    SettingsRoute
>;

return NavigationStack<AppRoute>(
    path,
    HomePage,
    [](const AppRoute& route) -> View {
      return std::visit(
          Overloaded{
              [](const HomeRoute&) -> View {
                return HomePage();
              },
              [](const ArticleRoute& value) -> View {
                return ArticlePage(value.article_id);
              },
              [](const SettingsRoute&) -> View {
                return SettingsPage();
              },
          },
          route
      );
    }
);
```

This snippet documents the ownership boundary rather than committing the exact future template API.
The route value remains application-defined instead of requiring a HuxerUI Route inheritance hierarchy.

The future controlled path must define reconciliation identity, duplicate routes, path replacement, invalid route values, destination resolution failures, saveable encoding, and operation results before becoming public.
It can coexist with factory entries, but a nonserializable factory entry cannot claim URL or restoration identity.

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

The Web integration owns browser mechanics:

- Application Push maps to `history.pushState()`.
- Application Replace maps to `history.replaceState()`.
- `popstate` decodes the browser location and updates the controlled path without echoing another history mutation.
- Initial mount decodes the current URL before the first destination is committed.
- Reload reconstructs only serializable route entries and application state.

Browser History must not be exposed through PlatformAdapter because it is application navigation policy rather than a renderer, text, clipboard, or frame-scheduling capability.
A focused Web navigation bridge may use the future controlled-path contract without introducing JavaScript types into shared headers.

Nested URL routes may resolve to nested path values or a branch of route values, but URL segment nesting does not force every visual component function to become a NavigationStack.

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
- `include/huxerui/navigation.h` for page stacks, destination selectors, drawers, and their Theme styles.
- `include/huxerui/huxerui.h` for the public umbrella include.

`src/navigation.cpp` owns controller state, entry resolution, NavigationStack layout, retained page modifiers, and page motion.
`src/navigation_ui.cpp` owns NavigationItem resolution, NavigationBar, NavigationPane, DrawerLayout, and retained drawer presentation.

Runtime changes remain limited to generic Back routing, Back-session capture, and disabled-subtree input cleanup.
Runtime must not include navigation entry types or branch on NavigationStack, NavigationController, MaterialTheme, FlatTheme, Android, or Web.

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

Destination selection and drawer tests additionally verify icon requirements, disabled keyboard traversal, dynamic compact and expanded Pane composition, responsive inline fallback, controlled modal state, gesture and Back ordering, exit-time focus confinement, and built-in Theme styles.

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

Platform validation includes:

- Android API 34 Begin, Update, Cancel, and Commit JNI and Java mapping.
- Android API 33 Commit mapping.
- Android API 23 through 32 Activity fallback.
- Embedded Android View behavior without Activity ownership.
- Windows, macOS, and Web Escape Commit behavior.
- Available platform builds after shared Back API changes.

Future URL integration requires browser tests for initial deep links, Push, Replace, Back, Forward, reload, invalid URLs, duplicate routes, nested routes, and suppression of history-update feedback loops.

## Delivery sequence

The initial factory-driven stack delivers NavigationStack, NavigationController, retained entries, serialized programmatic transitions, Theme motion, generic Back routing, predictive Back on Android API 34, Commit-only fallback paths, and a dedicated navigation example.

The controlled path, saveable state, Web URL bridge, iOS interactive gesture, split navigation, and shared-element transitions remain later milestones built on the same resolved-entry engine.

## Invariants

- NavigationStack is an application-tree container, not a Layer or root service.
- A page is an ordinary View factory with an independent keyed RecomposeScope.
- NavigationController resolves the nearest scoped stack and does not own Runtime or mounted nodes.
- The stack scope is the only strong owner of controller state.
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
- Future route paths resolve into the same private page-entry engine rather than creating another navigator.
- Browser URL policy remains outside shared Runtime and PlatformAdapter.
