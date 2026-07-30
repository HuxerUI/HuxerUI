# Architecture Design

Status: implemented foundation with deferred follow-up work

This document describes the implemented modifier, animation, interaction, theme, presentation, and root extension foundation, followed by explicitly identified follow-up work. Code examples in implemented sections match the current public API.

Current implementation status:

- Generic View modifiers, node extension reconciliation, frame callbacks, pointer observation, foreground painting, and third-party descriptors are implemented.
- ScrollBar animation, hit testing, dragging, and painting are implemented as a node extension without Runtime feature branches.
- Typed Environment, direct Theme providers, nested Theme propagation, and reduced-motion animation resolution are implemented.
- The synthetic RuntimeRoot, fixed LayerHost ordering, RootHook services, Toast, command and declarative Dialog presentation are implemented.
- Tween and spring animated Offset, Opacity, Scale, and Rotation values, state-overlay indication, and multi-pointer ripple indication are implemented.
- Retained exit transitions, keyframes, decay animation, focus restoration, platform Back handling, and advanced Toast queue policy remain follow-up work.

The design has four goals:

- Keep the common View API small and declarative.
- Give built-in and third-party features the same extension mechanisms.
- Preserve mounted state across recomposition without adding feature-specific branches to `Runtime`.
- Reuse the existing Scope, Composer, reconciliation, layout, event, and virtual layout systems.

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

`RuntimeRoot` is synthesized by the Runtime. It is not a public layout component and does not require applications to wrap their root View.

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
MountedNode and NodeExtension
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

`OnClick()` remains a high-frequency convenience wrapper for the typed event API:

```cpp
template <class Handler>
View OnClick(Handler&& handler) &&
{
  return std::move(*this).On<ViewEvents::Click>(
      std::forward<Handler>(handler));
}
```

Visual effects, interaction behavior, animation, presentation, and parent layout data are modifier values passed to `With()`:

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

The target API does not require a dedicated `View` member function for every new modifier type.

### Modifier order

Modifiers are processed from left to right, but the current property modifiers do not form wrapper nodes. `Padding`, `Frame`, `Background`, `Foreground`, `FontSize`, alignment, spacing, and similar values apply directly to `ViewSpec`. A later modifier that writes the same property wins. `Frame` merges only explicitly supplied width, height, minimum, and maximum fields so independent declarations can constrain separate axes.

```cpp
view.With(
    Padding{12.0F},
    Background{Colors::Blue});
view.With(
    Background{Colors::Blue},
    Padding{12.0F});
```

These declarations currently produce the same padding and background. They do not express inner and outer backgrounds.

`Frame(width, height)` is the positional fixed-size form. Its six optional fields also support a single preferred axis and independent minimum or maximum bounds. The runtime validates local bounds when the modifier is applied, then intersects them with the parent `Constraints` before measuring content. Preferred dimensions collapse the resulting range to the closest permitted value. Frame constraints describe the outer node size, so Padding is deflated only after the frame range has been resolved.

Grow is a parent layout policy rather than a frame constraint. Row and Column divide finite remaining main-axis space by grow factor and pass each grow child a tight allocation. An unbounded main axis has no remaining extent to divide, so Grow does not expand there.

Flow uses the same public `Layout<Derived>` protocol as Row, Column, and Stack. It first measures children at their natural widths to form horizontal lines, then distributes finite remaining width among Grow children within each line. Main alignment is resolved separately per line, cross alignment applies within the line height, and the common Spacing value is used for both item and line gaps. An unbounded width produces one intrinsic-width line without Grow expansion.

Retained modifiers such as `ScrollBar`, `Indication`, animated `Opacity`, and third-party modifiers with an extension preserve their relative order. Compatible retained entries reconcile by descriptor and position. Frame and foreground paint callbacks run in declaration order, while extension hit testing runs in reverse order.

## Modifier descriptions and node extensions

There are two modifier categories:

- A property modifier applies its value directly to `ViewSpec` and is not retained afterward.
- A retained modifier stores a type-erased `ModifierSpec` in `ViewSpec` and a persistent `NodeExtension` in `MountedNode`.

Conceptually:

```text
Padding / Background ── apply ──▶ ViewSpec properties

Ripple / ScrollBar / Glow
    └── type erasure ──▶ ModifierSpec
                            └── reconciliation ──▶ NodeExtension
```

Each modifier type has a stable descriptor identity. Reconciliation compares modifier type and position:

- A compatible modifier updates its existing node extension.
- An incompatible modifier destroys the previous node extension and creates a new one.
- Reusing a `MountedNode` also preserves compatible modifier animation, gesture, and presentation state.
- Reordering modifiers is a semantic change and may recreate affected node extensions.

A third-party modifier can expose its node extension without changing `View`:

```cpp
class GlowExtension;

struct Glow {
  using Extension = GlowExtension;

  Color color;
  float radius = 12.0F;
};
```

The framework-provided adapter performs type erasure and dispatches typed updates:

```cpp
class GlowExtension final : public NodeExtension {
public:
  GlowExtension(MountedNode& node, const Glow& spec);

  void Update(MountedNode& node, const Glow& spec);

  void Paint(
      const MountedNode& node,
      DisplayList& display_list) const override;
};
```

The framework detects `Glow::Extension`, creates the node extension, and dispatches typed updates without requiring the modifier to expose descriptor or type-erasure details.

## NodeExtension lifecycle

`NodeExtension` operates directly on a controlled public `MountedNode`. There is no separate `ModifierHost` and no context object for every phase.

The current public lifecycle is:

```cpp
class NodeExtension {
public:
  struct FrameResult {
    bool needs_frame;
    std::optional<double> wake_after;
  };

  enum class PointerResult {
    Ignored,
    Observe,
    Handled,
    Capture,
  };

  virtual ~NodeExtension() = default;

  virtual FrameResult OnFrame(
      MountedNode& node,
      const FrameInfo& frame);

  virtual void OnScrollActivity(MountedNode& node);
  virtual void OnScrollGesture(MountedNode& node, bool active);

  virtual bool HitTest(
      MountedNode& node,
      Point position) const;

  virtual bool HoverHitTest(
      MountedNode& node,
      Point position) const;

  virtual void OnHoverChanged(MountedNode& node, bool hovered);
  virtual void OnFocusChanged(MountedNode& node, bool focused);
  virtual void OnKey(MountedNode& node, const KeyEvent& event);

  virtual PointerResult OnPointer(
      MountedNode& node,
      const PointerEvent& event);

  virtual void Paint(
      const MountedNode& node,
      DisplayList& display_list) const;
};
```

`Paint()` is currently a foreground pass after the View content and children. `NodeExtension` does not wrap measure, layout, or paint, and it has no `Next` continuations. Custom child measurement and placement belong to `Layout<Derived>` or `VirtualLayout<Derived>`.

During `Paint()`, the DisplayList already contains the node's inherited presentation transform, so extension drawing uses `MountedNode::Frame()`. `PresentationFrame()` is the transformed axis-aligned window-space bounds. Pointer positions delivered to `NodeExtension::HitTest()` and `OnPointer()` are mapped back into the coordinate space of `Frame()`.

The existing `LayoutContext` and `VirtualLayoutContext` remain because they represent real child measurement sessions.

## MountedNode capabilities

The public `MountedNode` surface exposes controlled operations needed by layouts and modifiers:

```cpp
class MountedNode {
public:
  Rect Frame() const;
  Rect PresentationFrame() const;
  float PresentationOpacity() const;
  Size MeasuredSize() const;
  bool IsEnabled() const;
  bool IsFocused() const;

  std::size_t ChildCount() const;
  MountedNode& ChildAt(std::size_t index);
  const MountedNode& ChildAt(std::size_t index) const;

  template <class Key>
  const typename Key::Value* LayoutValue() const;

  template <class T, class... Arguments>
  T& Cache(Arguments&&... arguments);
};
```

It does not expose Runtime ownership, Environment storage, reconciliation internals, or direct child insertion and removal. A `NodeExtension` requests a continuing frame or a delayed wake-up through the `FrameResult` returned from `OnFrame()`. A general public measure/layout/paint invalidation API is deferred.

## Frame lifecycle

The target frame sequence is:

```text
apply State invalidations
recompose dirty scopes
reconcile ViewSpec and MountedNode
measure
layout
advance retained node extensions
paint
schedule the next frame or delayed wake-up
```

The current Runtime measures and lays out the mounted tree on every produced frame. Node-extension frame traversal caches whether a subtree contains any extensions and skips extension-free subtrees. A modifier that is waiting for a delayed transition schedules one wake-up rather than running empty frames.

Runtime calls fixed node and modifier lifecycle functions. It does not contain branches for concrete features such as ScrollBar, Ripple, Dialog, or a particular animation.

## Animation model

Animation is separated into motion parameters, animated modifier values, and visibility transitions.

### AnimationSpec

`AnimationSpec` describes how a value moves:

```cpp
using AnimationSpec = std::variant<
    SnapSpec,
    TweenSpec,
    SpringSpec>;
```

Keyframes, decay animation, and visibility transitions remain follow-up work.

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

`AnimationSpec` is a value. It is not a modifier and does not own runtime state.

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
    },
    Scale{
        AnimateTo(
            visible ? 1.0F : 0.92F,
            SpringSpec{}),
    },
    Rotation{
        AnimateTo(
            selected ? 8.0F : 0.0F,
            TweenSpec{.duration = 0.2}),
    });
```

The current value, velocity, start time, and target are stored in the compatible `NodeExtension`. Retargeting starts from the current presentation value. Advancing an animation does not recompose the component. Scale and Rotation default to the View center, use a normalized `TransformOrigin`, and share their transform with descendant drawing, clipping, foreground extensions, and pointer hit testing without changing Measure or Layout.

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

When a node with an exit transition disappears from the incoming tree, it enters a retained exit state:

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

Accessibility and platform preferences enter through Environment. Theme motion resolution can replace animations with `SnapSpec` or shorter motion without changing each component.

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

A Press records the pointer ID and local press position. Release and Cancel refer to the corresponding Press. This supports multiple simultaneous pointers and multiple active ripple instances.

`OnClick()` and `.On<ViewEvents::Click>()` register the same typed event. Adding a Click handler makes the View participate in click interaction. Flat themes use a state-overlay indication, while Material themes select a ripple with a hover state layer. Both resolve their colors from `InteractionScheme` and their transition durations from `MotionScheme`. Reduced-motion themes snap those transitions.

`Enabled` is a semantic modifier. Effective enabled state is resolved from the root toward its descendants, so a child cannot re-enable itself beneath a disabled parent. Disabled controls remain hit-test barriers without receiving pointer, scroll, focus, or Click interaction. The renderer applies disabled opacity once at the boundary instead of repeatedly dimming every descendant.

`Focusable` lets a custom View participate in the window focus order. Button is focusable by default. Runtime owns one focused mounted-node identity, dispatches `FocusChanged`, `KeyDown`, and `KeyUp`, and moves focus for Tab or Shift+Tab. Enter activates a focused Button on key down; Space shows pressed indication and activates on key up. Focus ring color, width, disabled opacity, and key indication timing resolve from Theme.

The topmost modal Layer is the active focus traversal root. Opening a nested modal captures the current focus, and dismissing it restores the previously focused mounted node when that node still exists and remains enabled.

When a pointer drag crosses the scroll threshold, the selected scroll container wins gesture arbitration. The original click target receives PointerCancel, Click is suppressed, and its indication runs the cancellation animation.

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

A ripple is one mounted instance per Press. It continues expanding and fading after Release or Cancel until its configured transition finishes. Its DisplayList clip uses the resolved component corner radius.

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

`ScrollBarExtension` owns:

- Opacity animation state.
- Delayed hide scheduling.
- Hover and drag state.
- Thumb geometry and pointer handling.
- Foreground painting.

Scroll activity, hover, and drag update this modifier. Runtime does not retain ScrollBar-specific animation or pointer functions.

## Environment

Environment is a typed, hierarchical value system:

```cpp
template <EnvironmentValue Value>
const Value& UseEnvironment();
```

The Environment value type is also its lookup identity and provides its fallback through `Value::Default()`.

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

Use a semantic wrapper when two ambient values share the same underlying representation. Primitive or third-party representation types are not separate Environment keys by themselves.

Each Environment frame stores only local overrides and points to its parent:

```cpp
struct EnvironmentFrame {
  std::shared_ptr<const EnvironmentFrame> parent;
  EnvironmentValues overrides;
};
```

Each composed subtree captures its current Environment frame. A nested provider shadows only the value type it supplies and inherits every other value from its parent frame.

Environment carries:

- Theme values.
- Platform and accessibility values.
- Per-window services.
- Other typed third-party values.

Theme and services reuse Environment rather than introducing parallel tree propagation systems.

## Theme

Theme is a direct, deferred subtree provider built on Environment:

```cpp
template <class Factory>
View Theme(
    ThemeDefinition definition,
    Factory&& content);
```

The content factory is stored and invoked only after the Theme Environment frame is active. This allows `UseTheme()` inside child component composition.

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

Third-party components can define their own style keys without extending a single global style registry.

Material, flat, liquid, and third-party themes are theme provider functions, not Runtime types and not subclasses:

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

The built-in Flat and Material systems provide complete light and dark boundaries:

```cpp
FlatTheme(Content)
FlatDarkTheme(Content)
MaterialTheme(Content)
MaterialDarkTheme(Content)
```

`FlatLightThemeSpec()` and `FlatDarkThemeSpec()` return mutable token values that applications can use as the starting point for a branded flat theme. `MaterialLightThemeSpec()` and `MaterialDarkThemeSpec()` provide the token subset consumed by the current built-in components. Passing a customized Material ThemeSpec to `MaterialTheme(theme, factory)` rebuilds the Material component styles from those tokens.

### Theme syntax

Pass a component function directly in the common case:

```cpp
return MaterialTheme(AppContent);
```

A content factory remains available when arguments must be captured:

```cpp
return MaterialTheme([=] {
  return AppContent(user_id);
});
```

`HUXERUI_THEME` is optional syntax sugar for an inline View expression or a component call with arguments:

```cpp
#define HUXERUI_THEME(ThemeProvider, ...)                              \
  (ThemeProvider)([=]() -> ::huxerui::View { return (__VA_ARGS__); })
```

Root usage:

```cpp
View App()
{
  return MaterialTheme(AppContent);
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

A complete Theme establishes a design system boundary. A Theme override inherits unspecified values from its parent. Runtime does not branch on Material, flat, liquid, or third-party theme identity.

`ThemeDefinition{ThemeSpec}` establishes a complete boundary. `ThemeDefinition{}` only contributes its typed component values, so a nested style override does not replace the parent `ThemeSpec`. Text, Button, Dialog, Toast, ScrollBar, and default indications derive their semantic defaults from the nearest complete `ThemeSpec`. Component style lookup stops at that complete boundary, while a component-only `ThemeDefinition` continues to inherit from its parent. Explicit View modifiers run after semantic style resolution and win without a separate runtime style branch.

Text uses `TextRole::Body`, `TextRole::Label`, and `TextRole::Title` to select the corresponding typography token. A component `TextStyle` value can still replace the complete Text style for a local subtree.

Theme switching initially updates values directly. Per-frame animated Theme interpolation is intentionally deferred.

## RuntimeRoot and LayerHost

`RuntimeRoot` owns the application content and one shared LayerHost. LayerHost is the only global presentation container:

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

Layer entries have their own `RecomposeScope`. Showing a Toast or Dialog does not invalidate the application root scope.

Paint follows layer order. Hit testing walks layers in reverse paint order:

- A Toast passes input through by default.
- A Popup only intercepts input inside its bounds.
- A modal barrier prevents input from reaching lower layers.
- The System layer is reserved for framework and diagnostic UI.

Removed entries remain mounted until their exit transition completes.

## RootHook

A RootHook installs per-window services or persistent global components before the first application composition:

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

Services are stored in the root Environment and retrieved through a typed helper:

```cpp
auto service = UseService<XxxToastService>();
```

Duplicate service types are rejected rather than silently replaced.

Root hooks run once in declaration order. Runtime owns the provided services and attached entries. On window destruction, Runtime removes content and layers before destroying services in reverse registration order. A service uses its destructor to release external subscriptions.

HuxerUI installs its built-in Toast and Dialog services for every Runtime before application root hooks run. Applications use `UseToast()` and `UseDialog()` directly; root hooks remain the extension mechanism for third-party services and global components.

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

`UseToast()` returns a lightweight handle bound to the current window and captures the current Environment frame. A Toast shown from a nested Theme uses that Theme by default.

The Toast service manages queueing, deduplication, duration, and LayerEntry creation. The LayerHost owns composition, input behavior, transitions, and removal.

There is no process-global `Toast::Show()` because it would be ambiguous in multi-window and multi-Runtime applications.

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

`DialogExtension` owns a LayerEntry handle. Updating the modifier updates the entry. Destroying the source modifier dismisses the entry, while LayerHost retains the presentation until its exit transition completes.

An outside press requests dismissal instead of directly removing a declarative Dialog layer. The callback updates the source State, preserving one source of truth for both the component and LayerHost. A dismissible declarative Dialog must provide `on_dismiss_request`.

Command-oriented presentation uses a per-window service:

```cpp
auto dialog = UseDialog();

return Button("Delete")
    .OnClick([dialog] {
      dialog.Show([](DialogContext dialog) {
        return Column{
            Text("Delete item?"),
            Button("Cancel").OnClick([dialog] {
              dialog.Dismiss();
            }),
        };
      });
    });
```

`DialogContext` identifies the presented instance and lets command-created content dismiss itself without capturing a `LayerId` before `Show()` returns.

Both forms use the same modal LayerEntry implementation:

- Modal barrier.
- Focus capture and restoration.
- Outside-press dismissal policy.
- Captured Environment and Theme.
- Enter and exit transitions.

Dialog does not own a separate Runtime or presentation host.

## Theme and global presentation

Root services are installed before application composition and inherited through nested Environment frames.

A global presentation handle obtained inside themed content captures the caller Environment:

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
  return MaterialTheme(AppContent);
}
```

The resulting Toast entry receives the Material Theme frame even though it is mounted in the window LayerHost outside the normal content layout hierarchy.

A presentation API may explicitly request the root Theme for application-wide alerts, but caller Theme is the default.

## Extension map

The target extension points are:

| Requirement | Extension mechanism |
| --- | --- |
| Custom layout | `Layout<Derived>`, `LayoutContext`, `LayoutResult` |
| Custom virtual container | `VirtualLayout<Derived>` and `VirtualLayoutContext` |
| Custom event | `Event<Arguments...>`, `On<Key>()`, `UseEvents()`, and `Emit<Key>()` |
| Custom View effect | Modifier value and `NodeExtension` |
| Custom animation | `AnimationSpec` or animated modifier value |
| Custom interaction visual | `IndicationSpec` and `NodeExtension` |
| Custom theme | `XxxTheme(factory)` wrapping `Theme()` |
| Per-window service | RootHook and `RootContext::Provide()` |
| Global component | RootHook and LayerHost |
| Toast or Dialog library | A service backed by LayerHost |

Built-in and third-party implementations use the same lifecycle and storage models.

## Performance rules

The architecture follows these rules:

- Animation advances mounted state and does not recompose components every frame.
- Node extension frame traversal skips subtrees that contain no retained extensions after the extension-tree cache is rebuilt.
- Delayed animation work schedules one wake-up instead of polling.
- Environment values are captured during composition; the current runtime does not maintain per-key Environment dependency subscriptions.
- Layer entries use independent scopes.
- ScrollBar state exists only on Views that install the modifier.
- Pointer interaction state is stored per pointer ID.
- Explicit style values override Theme without mutating Theme.
- A service belongs to one window root.

Incremental Measure and Layout invalidation can be implemented using the same invalidation flags without changing the public API.

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

- Add the generic modifier descriptor and node extension reconciliation.
- Move ScrollBar frame, pointer, and paint state into a node extension.
- Add generic invalidation flags and prune inactive frame subtrees.
- Add typed Environment frames and direct Theme providers.
- Add the synthetic RuntimeRoot and shared LayerHost.
- Add RootHook service installation.
- Build Dialog and Toast on LayerHost.
- Add interaction indications and public animation values.
- Migrate common View styling to `With()` modifier values.
