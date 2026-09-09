# Themes and Presentation

## Theme providers

HuxerUI ships Flat and Material light and dark theme definitions.
Theme providers are transparent Environment boundaries: they supply colors, typography, component styles, motion, and presentation policy without adding layout geometry.

```cpp
return MaterialTheme {
  Content(),
};
```

```cpp
return FlatDarkTheme {
  Content(),
};
```

Pass a customized `ThemeSpec` when an application needs different tokens while preserving the selected theme system.
Nested themes affect only their subtree.

`ColorScheme::primary_container` and `on_primary_container` describe a tonal primary surface and its foreground; they are distinct from the stronger `primary` and `on_primary` pair.
`tertiary_container` and `on_tertiary_container` provide a contrasting tonal accent, used by the Material TimePicker's AM/PM selection.
Customize each container and its foreground together to preserve readable contrast in light and dark themes.

## Component styles

Each built-in component resolves its typed style from the closest Theme.
Styles cover geometry, color, typography, indication, and motion owned by that component.

Component and presentation surface fills use `VisualFill`, so a Theme can supply a solid color, gradient, image asset, or image resource without changing the component API.
Ordinary rectangular outlines use `Border`; styles whose default has no outline expose `std::optional<Border>`.
Surface shape uses the pure geometry value `CornerRadii`, including asymmetric values such as `CornerRadii::Top(16.0F)`.
`CornerRadius` remains the View modifier that applies those radii to a declaration:

```cpp
ButtonStyle style = ButtonStyle::Default();
style.background = LinearGradient{
    .stops = {{0.0F, Color::Rgb(75, 92, 255)}, {1.0F, Color::Rgb(142, 68, 255)}},
};
style.corner_radii = CornerRadii{16.0F, 4.0F, 16.0F, 4.0F};

ThemeDefinition definition;
definition.Set(style);
```

Foregrounds, indicators, dividers, scrims, tracks, and colors used by retained animation or opacity calculations remain `Color`.
`Indication` owns only transient focus, hover, press, and ripple presentation; normal backgrounds, borders, and surface geometry remain in the component style.

Flat and Material are independent systems, not a shared style with a few color substitutions.
For example, a Flat `TextFieldStyle` can use an outlined surface without a visible floating label while keeping the label in semantics.
Flat pickers use compact geometry, restrained outlines, and rounded-square date selections; Material pickers use larger tonal surfaces, circular date selections, and separate time-field, period-group, and dial selection colors.

## Interaction indication

Interactive components derive visual state from shared interaction facts such as hovered, focused, pressed, dragged, selected, and disabled.
The active indication can paint a state layer, state border, content tint, focus ring, or ripple without changing layout.

Application code can provide a custom `Indication` or remove transient indication where that is semantically appropriate.
Normal component appearance remains in its component style rather than being duplicated as an interaction state.

## Animation

Retained animation updates mounted presentation state without recomposing application state every frame.
Motion controllers, theme-owned transitions, and scene transitions use the same frame scheduling and reduced-motion policy.

TransitionSpec combines a copied, equality-comparable effect value, an AnimationSpec, and an optional delay in seconds.
The built-in effects are FadeTransition, SlideTransition, ScaleFadeTransition, and CircularRevealTransition.
A default TransitionSpec completes immediately; transition descriptions do not repeat and reject undamped springs.
MotionController retains its broader playback support for ordinary animations.

Scene transitions customize outgoing and incoming content while keeping both sides on one progress clock.
Use `RunFromCurrentInteraction` inside a synchronous component event when a circular reveal should originate at the pointer position or keyboard/accessibility activation center:

```cpp
Button("Next").OnClick([transition, page] {
  transition.RunFromCurrentInteraction(
      TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}},
      [page] { page += 1; }
  );
});
```

The implicit origin exists only for the duration of the interaction callback.
Use `RunAt` with retained geometry for asynchronous work, and use `Anchor` plus `Run` when the reveal belongs to stable View geometry rather than the triggering interaction.
When reduced motion is enabled, the runtime selects the documented reduced or immediate path rather than leaving each component to interpret the system setting independently.

Try the standalone [Transition Studio example](../../examples/transition/main.cpp) for ten selectable effects, including circular reveal, circular close, and the fragmented Crimson rift, slow playback, reversed page navigation, replacement, and interaction-origin scene changes. Its [custom effects](../../examples/transition/effects.cpp) use only public APIs.

### Custom effects

An effect returns a TransitionFrame containing outgoing and incoming TransitionSamples plus their drawing order:

```cpp
struct LiftTransition {
  float distance = 32.0F;

  TransitionFrame Evaluate(const TransitionContext& context) const {
    const float progress = std::clamp(context.progress, 0.0F, 1.0F);
    auto frame = FadeTransition{}.Evaluate(context);
    frame.incoming.transform.translate_y = distance * (1.0F - progress);
    frame.incoming.clip = ClipShape::Rectangle(context.bounds);
    return frame;
  }

  bool operator==(const LiftTransition&) const = default;
};
```

Include `<algorithm>` for std::clamp.
Evaluate must be pure: do not call composition hooks, mutate application state, or start another scene transition.
Progress can overshoot during spring motion; preserve finite overshoot or clamp it deliberately.
Join the stable outgoing presentation at zero and stable incoming presentation at one.
The executor owns completion, even when an underdamped spring crosses one before settling.

ClipShape provides Rectangle, RoundedRectangle, Circle, and FromPath factories.
Transforms apply in container-local logical coordinates, followed by clipping in those coordinates; an absent clip adds no restriction.
Use TransitionOrder::OutgoingAbove when the departing content should cover the incoming content.
Reversed() exchanges both sides and drawing order while evaluating at one minus progress; Reversed(animation) also selects a different reverse timing specification.
IsImmediate() reports whether the description completes without elapsed time or delay; reduced-motion policy is applied separately by its executor.

### Fragments and synchronized decoration

Set a side's `fragments` to draw independently moving regions of the same page or scene without duplicating its state or event handlers.
An empty collection draws the original content once; a non-empty collection replaces that drawing with the listed fragments, painted from first to last.
Each `TransitionFragment::source_clip` cuts the original container-local content before `fragment.transform`; the sample's common transform, opacity factor, and final clip still apply to all fragments.
Clipping does not relocate a fragment's origin, and regions may overlap or leave gaps.

```cpp
TransitionFrame frame;
frame.order = TransitionOrder::OutgoingAbove;
frame.outgoing.fragments = {
  TransitionFragment{
      .source_clip = ClipShape::Rectangle({0.0F, 0.0F, 160.0F, 240.0F}),
      .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, -40.0F, 0.0F},
  },
  TransitionFragment{
      .source_clip = ClipShape::Rectangle({160.0F, 0.0F, 160.0F, 240.0F}),
      .transform = Transform2D{1.0F, 0.0F, 0.0F, 1.0F, 40.0F, 0.0F},
  },
};
```

An effect can additionally define `void Paint(PaintContext&, const TransitionContext&) const` to record decoration above both sides using exactly the accompanying sample's progress, bounds, and origin.
Include `<huxerui/paint.h>` when implementing this method.
The executor owns and finishes the context; balance every clip and transform you push, do not retain or finish the context, and do not mutate application state.
Decoration is clipped to the transition container, has no input or semantic targets, and is independent of either side's transform or opacity.
`TransitionSpec::Paint()` forwards this optional capability and records nothing for effects without it.
`Reversed()` exchanges complete sides including fragments and supplies `1 - progress` to both evaluation and painting.
Use fixed seeds and trajectories calculated directly from progress for reproducible particles and reversible effects; do not start a second clock or integrate particle positions across calls.

Fragmentation replays retained render content rather than guaranteeing a cached bitmap or GPU texture.
There are no fixed count limits or automatic effect changes based on fragment count or recorded output; additional fragments increase copying and rendering costs.
Native PlatformViews cannot be replicated as fragments. When a fragmented page contains one, Navigation uses a fade without decoration for the remainder of that operation, including predictive cancellation.
External textures retain their existing producer-backed pixel behavior.

### Page transitions

Attach the complete policy to the page root:

```cpp
const TransitionSpec enter{LiftTransition{}, TweenSpec{0.28}};
return Column {
  Text("Product details"),
}.With(PageTransition{
    .push = enter,
    .pop = enter.Reversed(TweenSpec{0.22}),
    .replace = TransitionSpec{FadeTransition{}, TweenSpec{0.18}},
});
```

Push selects the incoming page's push value, Pop selects the departing page's pop value, and Replace selects the incoming page's replace value.
The whole page declaration overrides NavigationStyle::motion; PageTransition{} explicitly disables all three animations.
Declare the modifier at the root, including a transparent Scope or Environment boundary, rather than on an arbitrary descendant.
The selected description is frozen for an operation, and predictive Back seeks that operation's progress.
Page content is disabled for interaction while an operation is active and restored after completion or cancellation; the NavigationStack's Back gesture and cancellation remain available.
Fragments are visual-only and do not introduce per-fragment hit testing, focus, or duplicate accessibility nodes.
Page effects currently receive no implicit interaction origin; use context.bounds for geometry or capture explicit application-owned origin data in a custom effect.
Shared-element matching and SharedBounds are not yet available.

Scene and page transitions compose when a Scene mutation explicitly navigates.
Select immediate page motion when only the Scene effect is desired.
Scene requests replace active visual work, reject recursive requests from mutations or effect evaluation, and preserve already-performed state writes when a mutation throws.
Reduced motion skips visual retention while still executing the mutation.
Native PlatformView content uses the frozen-scene fade fallback; retaining an external texture does not freeze its producer's pixels.
Repeated scene replacement captures the committed composite and can increase retained memory; it has no count-based cutoff, so assess interruption frequency and effect cost on the intended devices.

## Tooltips

Attach `Tooltip` to an anchor that needs short contextual help.
Hover, focus, dismissal, placement, and delay are owned by the presentation service and active theme.
The hover delay begins again after each pointer movement over the anchor, and a visible hover-owned Tooltip hides immediately when the pointer moves.
Keyboard focus keeps its independently owned Tooltip visible.

## Toast and SnackBar

Use the typed Toast service for passive transient feedback.
Each call creates an independent non-interactive notification that can be dismissed through its returned id.

Use SnackBar when the feedback needs one optional action:

```cpp
auto snack_bar = UseSnackBar();

return Button("Delete")
    .OnClick([snack_bar] {
      DeleteItem();
      snack_bar.Show("Item deleted", "Undo", [snack_bar] {
        RestoreItem();
        snack_bar.Show("Item restored");
      });
    });
```

Only one SnackBar is active per window.
A new request atomically replaces the previous one, and the action dismisses its owning SnackBar before invoking the callback.
The default duration is four seconds; pass `SnackBarOptions{std::nullopt}` for an indefinite presentation that is dismissed explicitly or by its action.
Timed dismissal pauses while the surface or action is hovered, while the action is focused or pressed, and while the application is inactive.

## Dialog and BottomSheet

Dialogs and bottom sheets are modal presentations owned by root services.
Their content is an ordinary View factory that captures the caller's Environment.

The service owns modality, focus trapping, barrier interaction, animation, and focus restoration.
Application code owns controlled values and action outcomes.

## Popup and Menu

Popups are anchored non-modal layers with typed placement and dismissal policy.
Menus build on the same anchor and layer infrastructure while adding items, sections, keyboard navigation, semantic roles, and submenus.
`PopupHandle::Show` follows the complete bounds of the View carrying `Anchor()`, while `ShowAtAnchor` follows a node-local rectangle inside that View through layout, scrolling, and presentation transforms.
`PopupHandle::UpdateAnchor` moves a Popup created by `ShowAtAnchor` without changing its layer id or recreating its content; it returns false for a stale id or another anchor mode.
`ShowAt` remains the fixed window-point path for context menus and pointer-position Popups.
`PopupHandle::Update` replaces an existing popup's content factory and captured Environment while retaining its layer id, anchor, placement, and dismissal policy.
Set `PopupOptions::retain_anchor_focus` when non-focusable popup content must accept pointer input without ending the anchor's editing or keyboard session; a focusable popup descendant still receives focus normally.

Menu icons accept `ImageVariant` and use the resolved menu content tint unless their visual source deliberately provides its own color behavior.

## Layer ordering

Application content, non-modal presentation, modal presentation, selection UI, and debug UI have a defined root-owned order.
Layers preserve the Environment captured at presentation time and do not create another application Runtime.

Use public services rather than directly mutating the layer stack.
See [Architecture Design](../design/architecture.md) for ownership and [Animation and Scene Transition Design](../design/animation.md) for motion behavior.
