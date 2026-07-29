# HuxerUI Architecture Design

Status: proposed

This document describes the target architecture for the next stage of HuxerUI.
It combines the modifier, animation, interaction, theme, presentation, and root
extension designs into one model. The APIs in this document are not all
implemented yet.

Current implementation status:

- Generic View modifiers, mounted modifier reconciliation, frame callbacks,
  pointer observation, foreground painting, and third-party descriptors are
  implemented.
- ScrollBar animation, hit testing, dragging, and painting are implemented as
  a mounted modifier without Runtime feature branches.
- Typed Environment, direct Theme providers, nested Theme propagation, and
  reduced-motion animation resolution are implemented.
- The synthetic RuntimeRoot, fixed LayerHost ordering, RootHook services,
  Toast, command and declarative Dialog presentation are implemented.
- Tween and spring animated Offset and Opacity values, state-overlay
  indication, and multi-pointer ripple indication are implemented.
- Retained exit transitions, keyframes, decay animation, focus restoration,
  platform Back handling, and advanced Toast queue policy remain follow-up
  work.

The design has four goals:

- Keep the common View API small and declarative.
- Give built-in and third-party features the same extension mechanisms.
- Preserve mounted state across recomposition without adding feature-specific
  branches to `Runtime`.
- Reuse the existing Scope, Composer, reconciliation, layout, event, and
  virtual layout systems.

## Architecture overview

Each window owns one internal runtime root:

```text
RuntimeRoot
├── Root Environment
├── ContentHost
│   └── application MountedNode tree
└── LayerHost
    ├── popup entries
    ├── modal entries
    ├── toast entries
    └── system entries
```

The application still starts with the existing shape:

```cpp
HUXERUI_APP(App, {})
```

`RuntimeRoot` is synthesized by the Runtime. It is not a public layout
component and does not require applications to wrap their root View.

The main data flow is:

```text
State / Environment changes
    ↓
dirty RecomposeScope
    ↓
ViewSpec and ModifierSpec
    ↓
reconciliation
    ↓
MountedNode and MountedModifier
    ↓
frame, measure, layout, hit testing, and paint
    ↓
DisplayList
```

## Public View surface

The target `View` API has four primary extension points:

```cpp
class View {
public:
  template <ViewModifier... Modifiers>
  View With(Modifiers&&... modifiers) &&;

  template <EventKey Key, class Handler>
  View On(Handler&& handler) &&;

  template <LayoutValueKey Key>
  View LayoutValue(typename Key::Value value) &&;

  View Key(ViewKey key) &&;
};
```

`OnClick()` remains a high-frequency convenience wrapper for the typed event
API:

```cpp
template <class Handler>
View OnClick(Handler&& handler) &&
{
  return std::move(*this).On<ViewEvents::Click>(
      std::forward<Handler>(handler));
}
```

Visual effects, interaction behavior, animation, presentation, and parent
layout data are modifier values passed to `With()`:

```cpp
return Button("Save")
    .With(
        Padding{12.0F},
        Frame{
            .width = 120.0F,
            .height = 44.0F,
        },
        Background{Colors::Blue},
        CornerRadius{8.0F})
    .OnClick([=] {
      Save();
    });
```

The variadic form keeps common declarations compact:

```cpp
return Text("Hello").With(
    FontSize{18.0F},
    Foreground{Colors::White},
    Padding{12.0F},
    Background{Colors::Blue});
```

It also gives third-party modifiers the same syntax as built-in modifiers:

```cpp
return Card().With(
    ThirdParty::Glow{
        .color = Colors::Cyan,
        .radius = 16.0F,
    });
```

The target API does not require a dedicated `View` member function for every
new modifier type.

### Modifier order

Modifier order is observable. Modifiers are applied from left to right, with a
later modifier wrapping the effects before it. This preserves the expected
difference between declarations such as:

```cpp
view.With(
    Padding{12.0F},
    Background{Colors::Blue});
```

and:

```cpp
view.With(
    Background{Colors::Blue},
    Padding{12.0F});
```

The first form paints the background around the padded result. The second form
adds padding outside the background.

## Modifier descriptions and mounted state

A modifier has two representations:

- The immutable modifier value stored in `ViewSpec`.
- The persistent mounted implementation stored in `MountedNode`.

Conceptually:

```text
Padding / Ripple / ScrollBar / Glow
    ↓ type erasure
ModifierSpec
    ↓ reconciliation
MountedModifier
```

Each modifier type has a stable descriptor identity. Reconciliation compares
modifier type and position:

- A compatible modifier updates its existing mounted implementation.
- An incompatible modifier destroys the previous mounted implementation and
  mounts a new one.
- Reusing a `MountedNode` also preserves compatible modifier animation,
  gesture, and presentation state.
- Reordering modifiers is a semantic change and may recreate affected mounted
  implementations.

A third-party modifier can expose its mounted implementation without changing
`View`:

```cpp
struct Glow {
  using Mounted = MountedGlow;

  Color color;
  float radius = 12.0F;
};
```

The framework-provided adapter performs type erasure and dispatches typed
updates:

```cpp
class MountedGlow final : public MountedModifier {
public:
  void Update(MountedNode& node, const Glow& spec);

  void Paint(
      MountedNode& node,
      DisplayList& display_list,
      PaintNext next) override;
};
```

## MountedModifier lifecycle

`MountedModifier` operates directly on a controlled public `MountedNode`.
There is no separate `ModifierHost` and no context object for every phase.

The complete interface can grow by capability, while the common lifecycle
remains:

```cpp
class MountedModifier {
public:
  virtual ~MountedModifier() = default;

  virtual void OnFrame(
      MountedNode& node,
      const FrameInfo& frame);

  virtual void OnPointer(
      MountedNode& node,
      const PointerEvent& event);

  virtual Size Measure(
      MountedNode& node,
      Constraints constraints,
      MeasureNext next);

  virtual void Layout(
      MountedNode& node,
      Rect frame,
      LayoutNext next);

  virtual bool HitTest(
      MountedNode& node,
      Point position,
      HitTestNext next);

  virtual void Paint(
      MountedNode& node,
      DisplayList& display_list,
      PaintNext next);
};
```

The `Next` values are lightweight continuations, not stateful context classes.
They allow a modifier to wrap the next modifier or the underlying View:

```cpp
void MountedClip::Paint(
    MountedNode& node,
    DisplayList& display_list,
    PaintNext next)
{
  display_list.PushClip(node.Frame());
  next(display_list);
  display_list.PopClip();
}
```

An internal capability mask prevents the Runtime from invoking irrelevant
hooks:

```text
Frame
Pointer
Measure
Layout
HitTest
Paint
```

The existing `LayoutContext` and `VirtualLayoutContext` remain because they
represent real child measurement sessions. They are not replaced by modifier
contexts.

## MountedNode capabilities

The public `MountedNode` surface exposes controlled operations needed by
layouts and modifiers:

```cpp
class MountedNode {
public:
  template <class Key>
  const typename Key::Value& Environment() const;

  template <class Key, class... Arguments>
  void Emit(Arguments&&... arguments);

  void Invalidate(Invalidation invalidation);

  void InvalidateAfter(
      Invalidation invalidation,
      double delay_seconds);

  Rect Frame() const;
  Size MeasuredSize() const;

  std::size_t ChildCount() const;
  MountedNode& ChildAt(std::size_t index);
  const MountedNode& ChildAt(std::size_t index) const;
};
```

It does not expose Runtime ownership, reconciliation internals, or direct child
insertion and removal.

Invalidation is explicit:

```cpp
enum class Invalidation : std::uint32_t {
  None = 0,
  Frame = 1 << 0,
  Measure = 1 << 1,
  Layout = 1 << 2,
  Paint = 1 << 3,
};
```

Requesting an invalidation also schedules the required frame. A modifier does
not need direct access to `Runtime::RequestFrame()`.

## Frame lifecycle

The target frame sequence is:

```text
apply State and Environment invalidations
recompose dirty scopes
reconcile ViewSpec and MountedNode
update mounted modifier targets
advance active mounted modifiers
measure
layout
paint
schedule the next frame or delayed wake-up
```

Each node tracks the work required by itself and its descendants:

```text
NeedsCompose
NeedsMeasure
NeedsLayout
NeedsPaint
NeedsFrame
```

The frame traversal prunes subtrees without active frame work. A modifier that
is waiting for a delayed transition schedules one wake-up rather than running
empty frames.

Runtime calls fixed node and modifier lifecycle functions. It does not contain
branches for concrete features such as ScrollBar, Ripple, Dialog, or a
particular animation.

## Animation model

Animation is separated into motion parameters, animated modifier values, and
visibility transitions.

### AnimationSpec

`AnimationSpec` describes how a value moves:

```cpp
using AnimationSpec = std::variant<
    SnapSpec,
    TweenSpec,
    SpringSpec,
    KeyframesSpec,
    DecaySpec>;
```

Examples:

```cpp
TweenSpec{
    .duration = 0.2,
    .easing = Easing::EaseOut,
};
```

```cpp
SpringSpec{
    .stiffness = 320.0F,
    .damping_ratio = 0.82F,
};
```

`AnimationSpec` is a value. It is not a modifier and does not own runtime
state.

### Animated modifier values

`AnimateTo()` combines a target with an animation description:

```cpp
template <class T>
struct Animated {
  T target;
  AnimationSpec animation;
};
```

Modifiers can accept either immediate or animated values:

```cpp
return Panel().With(
    Offset{
        AnimateTo(
            target_offset,
            SpringSpec{
                .stiffness = 320.0F,
                .damping_ratio = 0.82F,
            }),
    },
    Opacity{
        AnimateTo(
            visible ? 1.0F : 0.0F,
            TweenSpec{
                .duration = 0.2,
            }),
    });
```

The current value, velocity, start time, and target are stored in the
compatible `MountedModifier`. Retargeting starts from the current presentation
value. Advancing an animation does not recompose the component.

### TransitionSpec

`TransitionSpec` describes insertion and removal:

```cpp
TransitionSpec{
    .enter = {
        FadeTransition{
            TweenSpec{.duration = 0.18},
        },
        ScaleTransition{
            .from = 0.96F,
        },
    },
    .exit = {
        FadeTransition{
            TweenSpec{.duration = 0.14},
        },
    },
};
```

When a node with an exit transition disappears from the incoming tree, it
enters a retained exit state:

```text
remove from the logical composition
    ↓
stop normal input delivery
    ↓
retain mounted presentation state
    ↓
run the exit transition
    ↓
unmount after completion
```

Layer entries use the same transition model.

### Reduced motion

Accessibility and platform preferences enter through Environment. Theme motion
resolution can replace animations with `SnapSpec` or shorter motion without
changing each component.

## Interaction and indication

Pointer input follows one shared pipeline:

```text
PointerEvent
    ↓
hit testing and gesture arbitration
    ↓
clickable or gesture modifier
    ↓
InteractionState
    ↓
IndicationSpec
    ↓
mounted animation and paint
```

Interaction state is tracked per pointer ID:

```text
Press
Release
Cancel
```

A Press records the pointer ID and local press position. Release and Cancel
refer to the corresponding Press. This supports multiple simultaneous
pointers and multiple active ripple instances.

`OnClick()` and `.On<ViewEvents::Click>()` register the same typed event.
Adding a Click handler makes the View participate in click interaction.
Flat themes use a state-overlay indication, while Material themes select a
ripple with a hover state layer. Both resolve their colors from
`InteractionScheme` and their transition durations from `MotionScheme`.
Reduced-motion themes snap those transitions.

`Enabled` is a semantic modifier. Effective enabled state is resolved from
the root toward its descendants, so a child cannot re-enable itself beneath a
disabled parent. Disabled controls remain hit-test barriers without receiving
pointer, scroll, focus, or Click interaction. The renderer applies disabled
opacity once at the boundary instead of repeatedly dimming every descendant.

`Focusable` lets a custom View participate in the window focus order. Button
is focusable by default. Runtime owns one focused mounted-node identity,
dispatches `FocusChanged`, `KeyDown`, and `KeyUp`, and moves focus for Tab or
Shift+Tab. Enter activates a focused Button on key down; Space shows pressed
indication and activates on key up. Focus ring color, width, disabled opacity,
and key indication timing resolve from Theme.

The topmost modal Layer is the active focus traversal root. Opening a nested
modal captures the current focus, and dismissing it restores the previously
focused mounted node when that node still exists and remains enabled.

When a pointer drag crosses the scroll threshold, the selected scroll
container wins gesture arbitration. The original click target receives
PointerCancel, Click is suppressed, and its indication runs the cancellation
animation.

### IndicationSpec

Indications describe interaction visuals:

```cpp
using IndicationSpec = std::variant<
    NoIndication,
    StateOverlayIndication,
    RippleIndication>;
```

The default indication comes from Theme. A View can override it:

```cpp
return Button("Save").With(
    Indication{
        RippleIndication{
            .color = Color::White(),
        },
    });
```

A ripple is one mounted instance per Press. It continues expanding and fading
after Release or Cancel until its configured transition finishes. Its
DisplayList clip uses the resolved component corner radius.

## ScrollBar as a modifier

ScrollBar is a View modifier:

```cpp
return VirtualList(items, ItemView).With(
    ScrollBar{});
```

Explicit values override Theme defaults:

```cpp
return VirtualList(items, ItemView).With(
    ScrollBar{
        .thickness = 8.0F,
        .minimum_thumb_extent = 32.0F,
    });
```

`MountedScrollBar` owns:

- Opacity animation state.
- Delayed hide scheduling.
- Hover and drag state.
- Thumb geometry and pointer handling.
- Foreground painting.

Scroll activity, hover, and drag update this modifier. Runtime does not retain
ScrollBar-specific animation or pointer functions.

## Environment

Environment is a typed, hierarchical value system:

```cpp
template <class Key>
const typename Key::Value& UseEnvironment();
```

Each Environment frame stores only local overrides and points to its parent:

```cpp
struct EnvironmentFrame {
  std::shared_ptr<const EnvironmentFrame> parent;
  EnvironmentValues overrides;
};
```

Composer records the concrete Environment keys read by a `RecomposeScope`.
Changing one key invalidates only scopes that observed that key.

Environment carries:

- Theme values.
- Platform and accessibility values.
- Per-window services.
- Other typed third-party values.

Theme and services reuse Environment rather than introducing parallel tree
propagation systems.

## Theme

Theme is a direct, deferred subtree provider built on Environment:

```cpp
template <class Factory>
View Theme(
    ThemeDefinition definition,
    Factory&& content);
```

The content factory is stored and invoked only after the Theme Environment
frame is active. This allows `UseTheme()` inside child component composition.

### Theme systems

Core semantic theme values include:

```cpp
struct ThemeSpec {
  ColorScheme colors;
  TypographyScheme typography;
  ShapeScheme shapes;
  SpacingScheme spacing;
  ElevationScheme elevation;
  MotionScheme motion;
  InteractionScheme interactions;
};
```

Component styles are typed Environment values:

```text
TextStyle
ButtonStyle
DialogStyle
ToastStyle
ScrollBarStyle
```

Third-party components can define their own style keys without extending a
single global style registry.

Material, flat, liquid, and third-party themes are theme provider functions,
not Runtime types and not subclasses:

```cpp
template <class Factory>
View MaterialTheme(Factory&& content)
{
  return Theme(
      MaterialThemeDefinition(),
      std::forward<Factory>(content));
}
```

```cpp
template <class Factory>
View XxxTheme(Factory&& content)
{
  return Theme(
      BuildXxxTheme(),
      std::forward<Factory>(content));
}
```

The built-in Flat and Material systems provide complete light and dark
boundaries:

```cpp
HUXERUI_THEME(FlatTheme, Content())
HUXERUI_THEME(FlatDarkTheme, Content())
HUXERUI_THEME(MaterialTheme, Content())
HUXERUI_THEME(MaterialDarkTheme, Content())
```

`FlatLightThemeSpec()` and `FlatDarkThemeSpec()` return mutable token values
that applications can use as the starting point for a branded flat theme.
`MaterialLightThemeSpec()` and `MaterialDarkThemeSpec()` provide the token
subset consumed by the current built-in components. Passing a customized
Material ThemeSpec to `MaterialTheme(theme, factory)` rebuilds the Material
component StyleKeys from those tokens.

### Theme syntax

The direct syntax is:

```cpp
return MaterialTheme([=] {
  return AppContent();
});
```

`HUXERUI_THEME` hides the content lambda while remaining an expression that
can be nested:

```cpp
#define HUXERUI_THEME(ThemeProvider, ...)                              \
  (ThemeProvider)([=]() -> ::huxerui::View { return (__VA_ARGS__); })
```

Root usage:

```cpp
View App()
{
  return HUXERUI_THEME(
      MaterialTheme,
      AppContent());
}
```

Nested usage:

```cpp
return HUXERUI_THEME(
    MaterialTheme,
    Column{
        Header(),
        HUXERUI_THEME(
            LiquidTheme,
            LiquidPanel()),
        Footer(),
    });
```

### Theme resolution

Theme resolution follows a fixed order:

```text
explicit View modifier
    ↓
nearest component style
    ↓
nearest Theme override
    ↓
nearest complete Theme
    ↓
root Theme
    ↓
platform defaults
```

A complete Theme establishes a design system boundary. A Theme override
inherits unspecified values from its parent. Runtime does not branch on
Material, flat, liquid, or third-party theme identity.

`ThemeDefinition{ThemeSpec}` establishes a complete boundary.
`ThemeDefinition{}` only contributes its typed component values, so a nested
style override does not replace the parent `ThemeSpec`. Text, Button, Dialog,
Toast, ScrollBar, and default indications derive their semantic defaults from
the nearest complete `ThemeSpec`. Component StyleKey lookup stops at that
complete boundary, while a component-only `ThemeDefinition` continues to
inherit from its parent. Explicit View modifiers run after semantic style
resolution and win without a separate runtime style branch.

Text uses `TextRole::Body`, `TextRole::Label`, and `TextRole::Title` to select
the corresponding typography token. A component StyleKey can still replace
the complete Text style for a local subtree.

Theme switching initially updates values directly. Per-frame animated Theme
interpolation is intentionally deferred.

## RuntimeRoot and LayerHost

`RuntimeRoot` owns the application content and one shared LayerHost. LayerHost
is the only global presentation container:

```cpp
enum class LayerKind {
  Popup,
  Modal,
  Toast,
  System,
};
```

Each entry owns independent identity and composition:

```cpp
struct LayerEntry {
  LayerId id;
  LayerKind kind;
  ViewFactory content;
  EnvironmentFrame environment;
  InputPolicy input_policy;
  DismissPolicy dismiss_policy;
  TransitionSpec transition;
};
```

Layer entries have their own `RecomposeScope`. Showing a Toast or Dialog does
not invalidate the application root scope.

Paint follows layer order. Hit testing walks layers in reverse paint order:

- A Toast passes input through by default.
- A Popup only intercepts input inside its bounds.
- A modal barrier prevents input from reaching lower layers.
- The System layer is reserved for framework and diagnostic UI.

Removed entries remain mounted until their exit transition completes.

## RootHook

A RootHook installs per-window services or persistent global components before
the first application composition:

```cpp
using RootHook = std::function<void(RootContext&)>;
```

`RootContext` has two capabilities:

```cpp
class RootContext {
public:
  template <class Service>
  void Provide(std::shared_ptr<Service> service);

  LayerController& Layers();
};
```

Installation uses `AppOptions`:

```cpp
HUXERUI_APP(
    App,
    {
        .root_hooks = {
            InstallXxxToast(),
            InstallDebugPanel(),
        },
    })
```

A service hook can be a function:

```cpp
RootHook InstallXxxToast(XxxToastOptions options = {})
{
  return [options](RootContext& root) {
    root.Provide(
        std::make_shared<XxxToastService>(
            root.Layers(),
            options));
  };
}
```

A persistent global component can attach to LayerHost:

```cpp
RootHook InstallDebugPanel()
{
  return [](RootContext& root) {
    root.Layers().Attach(
        LayerKind::System,
        DebugPanel);
  };
}
```

Services are stored in the root Environment and retrieved through a typed
helper:

```cpp
auto service = UseService<XxxToastService>();
```

Duplicate service types are rejected rather than silently replaced.

Root hooks run once in declaration order. Runtime owns the provided services
and attached entries. On window destruction, Runtime removes content and
layers before destroying services in reverse registration order. A service
uses its destructor to release external subscriptions.

RootHook does not provide:

- Direct Runtime access.
- Direct MountedNode insertion.
- Per-frame callbacks.
- Root replacement.
- Dynamic installation and removal.

## Toast

Toast is naturally command-oriented:

```cpp
auto toast = UseToast();

return Button("Save")
    .OnClick([toast] {
      toast.Show("Saved");
    });
```

`UseToast()` returns a lightweight handle bound to the current window and
captures the current Environment frame. A Toast shown from a nested Theme uses
that Theme by default.

The Toast service manages queueing, deduplication, duration, and LayerEntry
creation. The LayerHost owns composition, input behavior, transitions, and
removal.

There is no process-global `Toast::Show()` because it would be ambiguous in
multi-window and multi-Runtime applications.

## Dialog

Dialog supports both declarative and command-oriented usage.

Declarative presentation is a modifier:

```cpp
return Content().With(
    Dialog{
        .visible = show_dialog,
        .content = ConfirmDialog,
        .dismiss_on_outside_press = true,
        .on_dismiss_request = [show_dialog] {
          show_dialog = false;
        },
    });
```

`MountedDialog` owns a LayerEntry handle. Updating the modifier updates the
entry. Destroying the source modifier dismisses the entry, while LayerHost
retains the presentation until its exit transition completes.

An outside press requests dismissal instead of directly removing a
declarative Dialog layer. The callback updates the source State, preserving
one source of truth for both the component and LayerHost. A dismissible
declarative Dialog must provide `on_dismiss_request`.

Command-oriented presentation uses a per-window service:

```cpp
auto dialogs = UseDialogs();

return Button("Delete")
    .OnClick([dialogs] {
      dialogs.Show([](DialogContext dialog) {
        return Column{
            Text("Delete item?"),
            Button("Cancel").OnClick([dialog] {
              dialog.Dismiss();
            }),
        };
      });
    });
```

`DialogContext` identifies the presented instance and lets command-created
content dismiss itself without capturing a `LayerId` before `Show()` returns.

Both forms use the same modal LayerEntry implementation:

- Modal barrier.
- Focus capture and restoration.
- Outside-press dismissal policy.
- Captured Environment and Theme.
- Enter and exit transitions.

Dialog does not own a separate Runtime or presentation host.

## Theme and global presentation

Root services are installed before application composition and inherited
through nested Environment frames.

A global presentation handle obtained inside themed content captures the
caller Environment:

```cpp
View AppContent()
{
  HUXERUI_SCOPE_BEGIN
    auto toast = UseToast();

    return Button("Save")
        .OnClick([toast] {
          toast.Show("Saved");
        });
  HUXERUI_SCOPE_END
}

View App()
{
  return HUXERUI_THEME(
      MaterialTheme,
      AppContent());
}
```

The resulting Toast entry receives the Material Theme frame even though it is
mounted in the window LayerHost outside the normal content layout hierarchy.

A presentation API may explicitly request the root Theme for application-wide
alerts, but caller Theme is the default.

## Extension map

The target extension points are:

| Requirement | Extension mechanism |
| --- | --- |
| Custom layout | `Layout<Derived>`, `LayoutContext`, `LayoutResult` |
| Custom virtual container | `VirtualLayout<Derived>` and `VirtualLayoutContext` |
| Custom event | `On<Key>()`, `UseEvents<Owner>()`, and `Emit<Key>()` |
| Custom View effect | Modifier value and `MountedModifier` |
| Custom animation | `AnimationSpec` or animated modifier value |
| Custom interaction visual | `IndicationSpec` and `MountedModifier` |
| Custom theme | `XxxTheme(factory)` wrapping `Theme()` |
| Per-window service | RootHook and `RootContext::Provide()` |
| Global component | RootHook and LayerHost |
| Toast or Dialog library | A service backed by LayerHost |

Built-in and third-party implementations use the same lifecycle and storage
models.

## Performance rules

The architecture follows these rules:

- Animation advances mounted state and does not recompose components every
  frame.
- Modifier frame traversal skips inactive subtrees.
- Delayed animation work schedules one wake-up instead of polling.
- Environment dependencies are tracked per typed key.
- Layer entries use independent scopes.
- Exit transitions retain only the nodes that are leaving.
- ScrollBar state exists only on Views that install the modifier.
- Pointer interaction state is stored per pointer ID.
- Explicit style values override Theme without mutating Theme.
- A service belongs to one window root.

Incremental Measure and Layout invalidation can be implemented using the same
invalidation flags without changing the public API.

## Deliberately omitted abstractions

The target design does not introduce:

- `ModifierHost`.
- A context class for every modifier lifecycle phase.
- Runtime branches for ScrollBar, Ripple, Dialog, or concrete animations.
- `OverlayBehavior`.
- Separate Overlay and Presentation runtime trees.
- A Host type for every global component.
- `AppFeature` or `MountedRootFeature`.
- `RootRegistration`.
- A public parallel ServiceRegistry.
- Theme class inheritance.
- Runtime checks for Material, flat, liquid, or third-party themes.
- Process-global Toast or Dialog singletons.
- Dynamic RootHook installation and removal.
- Arbitrary numeric layer z-index.
- Animated Theme interpolation in the initial implementation.

## Adoption sequence

The design can be introduced without combining all changes into one rewrite:

- Add the generic modifier descriptor and mounted modifier reconciliation.
- Move ScrollBar frame, pointer, and paint state into a mounted modifier.
- Add generic invalidation flags and prune inactive frame subtrees.
- Add typed Environment frames and direct Theme providers.
- Add the synthetic RuntimeRoot and shared LayerHost.
- Add RootHook service installation.
- Build Dialog and Toast on LayerHost.
- Add interaction indications and public animation values.
- Migrate common View styling to `With()` modifier values.
