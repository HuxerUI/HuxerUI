#include "runtime_test_support.h"

#include <algorithm>
#include <limits>

namespace huxerui::test {

namespace {

std::optional<DialogHandle> layer_dialogs;
std::optional<ToastHandle> layer_toast;
std::optional<BottomSheetHandle> layer_bottom_sheet;
std::optional<BottomSheetContext> layer_bottom_sheet_context;
std::optional<PopupHandle> layer_popup;
std::optional<PopupContext> layer_popup_context;
std::optional<MenuHandle> layer_menu;
std::optional<MenuContext> layer_menu_context;
std::optional<MenuHandle> nested_menu;
std::optional<LayerController> raw_layers;
State<float> layer_anchor_offset;
State<bool> layer_anchor_visible;
State<int> layer_environment_value;
int layer_app_compositions = 0;
int layer_background_clicks = 0;
int popup_focus_clicks = 0;

View LayerApp() {
  HUXERUI_SCOPE({
    ++layer_app_compositions;
    layer_toast = UseToast();
    layer_dialogs = UseDialog();
    layer_bottom_sheet = UseBottomSheet();
    auto popup = UsePopup();
    auto menu = UseMenu();
    layer_popup = popup;
    layer_menu = menu;
    layer_anchor_offset = UseState(20.0F);
    return Column {
      Button("popup anchor")
          .With(huxerui::Frame{60.0F, 30.0F}, Offset{Point{layer_anchor_offset.Get(), 0.0F}}, popup.Anchor())
          .OnClick([] { ++layer_background_clicks; }),
      Button("menu anchor").With(huxerui::Frame{60.0F, 30.0F}, menu.Anchor()),
      Text("application content"),
    };
  });
}

struct LayerEnvironmentValue {
  int value = 0;

  static LayerEnvironmentValue Default() {
    return {};
  }
};

View LayerEnvironmentDialogContent() {
  return Text("dialog environment " + std::to_string(UseEnvironment<LayerEnvironmentValue>().value));
}

View LayerEnvironmentApp() {
  layer_environment_value = UseState(1);
  return ProvideEnvironment(
      LayerEnvironmentValue{layer_environment_value.Get()},
      [] {
        return Text("application").With(
            Dialog{
                .visible = true,
                .content = LayerEnvironmentDialogContent,
            }
        );
      }
  );
}

View DebugOverlayApp() {
  ++layer_app_compositions;
  return Button("debug application").With(huxerui::Frame{240.0F, 120.0F}).OnClick([] { ++layer_background_clicks; });
}

View LayerKeyCollisionApp() {
  layer_popup = UsePopup();
  return Text("keyed application").Key("__huxerui_layer_stack");
}

View NestedAnchorContent() {
  auto menu = UseMenu();
  nested_menu = menu;
  return Button("nested menu anchor").With(huxerui::Frame{60.0F, 30.0F}, menu.Anchor());
}

View NestedAnchorApp() {
  auto popup = UsePopup();
  layer_popup = popup;
  layer_anchor_offset = UseState(20.0F);
  return Button("outer popup anchor")
      .With(huxerui::Frame{60.0F, 30.0F}, Offset{Point{layer_anchor_offset.Get(), 0.0F}}, popup.Anchor());
}

View RemovableAnchorApp() {
  auto popup = UsePopup();
  layer_popup = popup;
  layer_anchor_visible = UseState(true);
  if (!layer_anchor_visible.Get()) {
    return Text("anchor removed");
  }
  return Button("removable popup anchor").With(huxerui::Frame{60.0F, 30.0F}, popup.Anchor());
}

View FocusTrapApp() {
  layer_popup = UsePopup();
  return Button("application focus").With(huxerui::Frame{80.0F, 30.0F}).OnClick([] { ++layer_background_clicks; });
}

View DestructionApp() {
  return Text("application")
      .With(
          Dialog{
              .visible = true,
              .content = [] { return Button("dialog"); },
          }
      );
}

View BottomSheetThemeContent() {
  layer_bottom_sheet = UseBottomSheet();
  return Text("application");
}

View BottomSheetThemeApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.colors.scrim = Color::Rgb(20, 80, 160, 0.25F);
  ThemeDefinition definition{spec};
  definition.Set(DialogStyle{.scrim = Color::Rgb(180, 20, 20, 0.75F)});
  return Theme(std::move(definition), BottomSheetThemeContent);
}

std::optional<Rect> FindPresentedTextRect(const FlattenedScene& scene, std::string_view expected) {
  std::vector<Transform2D> transform_stack{Transform2D{}};
  for (const PaintCommand& command : scene.Commands()) {
    if (const auto* transform = std::get_if<PushTransformCommand>(&command)) {
      transform_stack.push_back(detail::ComposeTransform(transform_stack.back(), transform->transform));
      continue;
    }
    if (std::holds_alternative<PopTransformCommand>(command)) {
      transform_stack.pop_back();
      continue;
    }
    const auto* text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return detail::TransformBounds(transform_stack.back(), text->rect);
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("TestLayerMutationsDoNotRecomposeApplicationRoot") {
  layer_app_compositions = 0;
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();
  REQUIRE(layer_app_compositions == 1);

  const LayerId popup = layer_popup->ShowAt({80.0F, 40.0F}, [] { return Text("detached popup"); });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "detached popup"));
  REQUIRE(layer_app_compositions == 1);

  REQUIRE(layer_popup->Dismiss(popup));
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "detached popup"));
  REQUIRE(layer_app_compositions == 1);
}

TEST_CASE("TestPopupAndMenuHandlesReplaceTheirActiveEntries") {
  layer_popup.reset();
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  const LayerId first_popup = layer_popup->ShowAt({80.0F, 40.0F}, [] { return Text("first popup"); });
  runtime.BuildFrame();
  const LayerId second_popup = layer_popup->ShowAt({80.0F, 40.0F}, [] { return Text("second popup"); });
  const FlattenedScene& popup_replaced = runtime.BuildFrame();
  REQUIRE(!ContainsText(popup_replaced, "first popup"));
  REQUIRE(ContainsText(popup_replaced, "second popup"));
  REQUIRE(!layer_popup->Dismiss(first_popup));
  REQUIRE(layer_popup->Dismiss(second_popup));
  runtime.BuildFrame();

  const LayerId first_menu = layer_menu->ShowAt({80.0F, 40.0F}, [] { return Text("first menu"); });
  runtime.BuildFrame();
  const LayerId second_menu = layer_menu->ShowAt({80.0F, 40.0F}, [] { return Text("second menu"); });
  const FlattenedScene& menu_replaced = runtime.BuildFrame();
  REQUIRE(!ContainsText(menu_replaced, "first menu"));
  REQUIRE(ContainsText(menu_replaced, "second menu"));
  REQUIRE(!layer_menu->Dismiss(first_menu));
  REQUIRE(layer_menu->Dismiss(second_menu));
}

TEST_CASE("TestLayerStackIdentityDoesNotCollideWithViewKeys") {
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerKeyCollisionApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(ContainsText(initial, "keyed application"));

  layer_popup->ShowAt({80.0F, 40.0F}, [] { return Text("key-safe popup"); });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "keyed application"));
  REQUIRE(ContainsText(shown, "key-safe popup"));
}

TEST_CASE("TestLayerConfigurationValidation") {
  raw_layers.reset();
  AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](RootContext& root) { raw_layers = root.Layers(); });

  TestPlatform platform;
  Runtime runtime{LayerApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  REQUIRE(raw_layers.has_value());
  REQUIRE_THROWS_AS(raw_layers->Attach({}, {}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      raw_layers->Attach(
          LayerOptions{
              .pointer_policy = LayerPointerPolicy::Content,
              .dismiss_on_outside_press = true,
          },
          [] { return Text("invalid outside dismissal"); }
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      raw_layers->Attach(
          LayerOptions{
              .pointer_policy = LayerPointerPolicy::Content,
              .barrier_color = Color::Black(),
          },
          [] { return Text("invalid barrier color"); }
      ),
      std::invalid_argument
  );
}

TEST_CASE("TestAnchoredPresentationRejectsInvalidGeometry") {
  layer_popup.reset();
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  const float infinity = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS_AS(layer_popup->ShowAt({infinity, 0.0F}, [] { return Text("popup"); }), std::invalid_argument);
  REQUIRE_THROWS_AS(layer_menu->ShowAt({0.0F, infinity}, [] { return Text("menu"); }), std::invalid_argument);
  REQUIRE_THROWS_AS(
      layer_popup->ShowAt({0.0F, 0.0F}, [] { return Text("popup"); }, PopupOptions{.gap = -1.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      layer_menu->ShowAt({0.0F, 0.0F}, [] { return Text("menu"); }, MenuOptions{.viewport_margin = -1.0F}),
      std::invalid_argument
  );
}

TEST_CASE("TestAnchoredPresentationClampsOversizedViewportMargin") {
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({10.0F, 10.0F});
  runtime.BuildFrame();

  layer_popup->ShowAt({5.0F, 5.0F}, [] { return Text("tiny popup"); }, PopupOptions{.viewport_margin = 100.0F});
  REQUIRE_NOTHROW(runtime.BuildFrame());
}

TEST_CASE("TestBottomSheetPlacementContextAndBackDismissal") {
  layer_app_compositions = 0;
  layer_bottom_sheet.reset();
  layer_bottom_sheet_context.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  constexpr Color sheet_color = Color::Rgb(30, 110, 190);
  const LayerId sheet = layer_bottom_sheet->Show([sheet_color](BottomSheetContext context) {
    layer_bottom_sheet_context = context;
    return Text("bottom sheet").With(huxerui::Frame{80.0F, 30.0F}, huxerui::Background{sheet_color});
  });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(layer_bottom_sheet_context.has_value());
  REQUIRE(layer_bottom_sheet_context->Id() == sheet);
  REQUIRE(ContainsText(shown, "bottom sheet"));
  const Rect expected_bounds{60.0F, 90.0F, 80.0F, 30.0F};
  REQUIRE(FindPresentedRectWithColor(shown, sheet_color) == expected_bounds);

  REQUIRE(runtime.HandleBack());
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "bottom sheet"));
  REQUIRE(!runtime.HandleBack());
}

TEST_CASE("TestBackStopsAtTopmostConsumingLayer") {
  layer_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  const LayerId lower = layer_dialogs->Show([] { return Text("dismissible dialog"); });
  const LayerId upper = layer_dialogs->Show(
      [] { return Text("consuming dialog"); },
      DialogOptions{
          .dismiss_on_cancel = false,
      }
  );
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "dismissible dialog"));
  REQUIRE(ContainsText(shown, "consuming dialog"));

  REQUIRE(runtime.HandleBack());
  const FlattenedScene& consumed = runtime.BuildFrame();
  REQUIRE(ContainsText(consumed, "dismissible dialog"));
  REQUIRE(ContainsText(consumed, "consuming dialog"));

  REQUIRE(layer_dialogs->Dismiss(upper));
  REQUIRE(runtime.HandleBack());
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "dismissible dialog"));
  REQUIRE(!ContainsText(dismissed, "consuming dialog"));
  REQUIRE(!layer_dialogs->Dismiss(lower));
}

TEST_CASE("TestBackPassesThroughNotificationLayers") {
  layer_dialogs.reset();
  layer_toast.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  layer_dialogs->Show([] { return Text("dialog below toast"); });
  layer_toast->Show("toast above dialog", ToastOptions{10.0});
  runtime.BuildFrame();

  REQUIRE(runtime.HandleBack());
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "dialog below toast"));
  REQUIRE(ContainsText(dismissed, "toast above dialog"));
}

TEST_CASE("TestDeclarativeDialogUpdatesCapturedEnvironment") {
  TestPlatform platform;
  Runtime runtime{LayerEnvironmentApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  REQUIRE(ContainsText(runtime.BuildFrame(), "dialog environment 1"));

  layer_environment_value = 2;
  REQUIRE(ContainsText(runtime.BuildFrame(), "dialog environment 2"));
}

TEST_CASE("TestPopupAndMenuContextsDismissTheirOwnLayers") {
  layer_app_compositions = 0;
  layer_popup_context.reset();
  layer_menu_context.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  const LayerId popup = layer_popup->Show([](PopupContext context) {
    layer_popup_context = context;
    return Text("context popup");
  });
  REQUIRE(ContainsText(runtime.BuildFrame(), "context popup"));
  REQUIRE(layer_popup_context->Id() == popup);
  REQUIRE(layer_popup_context->Dismiss());
  REQUIRE(!ContainsText(runtime.BuildFrame(), "context popup"));

  const LayerId menu = layer_menu->Show([](MenuContext context) {
    layer_menu_context = context;
    return Button("context menu");
  });
  REQUIRE(ContainsText(runtime.BuildFrame(), "context menu"));
  REQUIRE(layer_menu_context->Id() == menu);
  REQUIRE(layer_menu_context->Dismiss());
  REQUIRE(!ContainsText(runtime.BuildFrame(), "context menu"));
  REQUIRE(layer_app_compositions == 1);
}

TEST_CASE("TestAnchoredPopupTracksPresentationBounds") {
  layer_app_compositions = 0;
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  constexpr Color popup_color = Color::Rgb(180, 60, 90);
  layer_popup->Show([popup_color] {
    return Text("anchored popup").With(huxerui::Frame{50.0F, 20.0F}, huxerui::Background{popup_color});
  });
  const std::optional<Rect> initial_bounds = FindPresentedRectWithColor(runtime.BuildFrame(), popup_color);
  REQUIRE(initial_bounds.has_value());

  layer_anchor_offset = 50.0F;
  const std::optional<Rect> moved_bounds = FindPresentedRectWithColor(runtime.BuildFrame(), popup_color);
  REQUIRE(moved_bounds.has_value());
  REQUIRE(moved_bounds->x == initial_bounds->x + 30.0F);

  ClickAt(runtime, {190.0F, 110.0F}, 121);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "anchored popup"));
}

TEST_CASE("TestAnchoredPresentationDismissesWhenAnchorUnmounts") {
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{RemovableAnchorApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  const LayerId popup = layer_popup->Show([] { return Text("attached popup"); });
  REQUIRE(ContainsText(runtime.BuildFrame(), "attached popup"));

  layer_anchor_visible = false;
  REQUIRE(!ContainsText(runtime.BuildFrame(), "attached popup"));
  REQUIRE(!layer_popup->Dismiss(popup));
}

TEST_CASE("TestNestedAnchorsSettleInOneFrame") {
  layer_popup.reset();
  nested_menu.reset();

  TestPlatform platform;
  Runtime runtime{NestedAnchorApp, platform};
  runtime.SetViewport({400.0F, 240.0F});
  runtime.BuildFrame();

  layer_popup->Show(NestedAnchorContent);
  runtime.BuildFrame();
  REQUIRE(nested_menu.has_value());

  constexpr Color menu_color = Color::Rgb(40, 150, 90);
  nested_menu->Show([menu_color] {
    return Text("nested menu").With(huxerui::Frame{50.0F, 20.0F}, huxerui::Background{menu_color});
  });
  const std::optional<Rect> initial_bounds = FindPresentedRectWithColor(runtime.BuildFrame(), menu_color);
  REQUIRE(initial_bounds.has_value());

  layer_anchor_offset = 50.0F;
  const std::optional<Rect> moved_bounds = FindPresentedRectWithColor(runtime.BuildFrame(), menu_color);
  REQUIRE(moved_bounds.has_value());
  REQUIRE(moved_bounds->x == initial_bounds->x + 30.0F);
}

TEST_CASE("TestMenuTrapsFocusAndDismissesOnBack") {
  layer_menu.reset();
  popup_focus_clicks = 0;

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  layer_menu->Show([] {
    return Column {
      Button("first menu item").OnClick([] { ++popup_focus_clicks; }),
      Button("second menu item"),
    };
  });
  REQUIRE(ContainsText(runtime.BuildFrame(), "first menu item"));

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(popup_focus_clicks == 1);

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Escape});
  REQUIRE(!ContainsText(runtime.BuildFrame(), "first menu item"));
}

TEST_CASE("TestPointerFocusDoesNotEscapeTrappedLayer") {
  layer_background_clicks = 0;
  popup_focus_clicks = 0;
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{FocusTrapApp, platform};
  runtime.SetViewport({240.0F, 120.0F});
  runtime.BuildFrame();

  layer_popup->ShowAt(
      {120.0F, 40.0F},
      [] { return Button("popup focus").With(huxerui::Frame{80.0F, 30.0F}).OnClick([] { ++popup_focus_clicks; }); },
      PopupOptions{
          .dismiss_on_outside_press = false,
          .trap_focus = true,
      }
  );
  runtime.BuildFrame();

  ClickAt(runtime, {20.0F, 15.0F}, 122);
  REQUIRE(layer_background_clicks == 1);
  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(layer_background_clicks == 1);
  REQUIRE(popup_focus_clicks == 1);
}

TEST_CASE("TestNestedLayerFocusRestoresAcrossRemovedLowerLayer") {
  layer_dialogs.reset();
  layer_menu.reset();
  layer_background_clicks = 0;

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();
  runtime.HandleKeyEvent({KeyEventType::Down, Key::Tab});

  const LayerId dialog = layer_dialogs->Show([] { return Button("dialog focus"); });
  runtime.BuildFrame();
  const LayerId menu = layer_menu->Show([] { return Button("menu focus"); });
  runtime.BuildFrame();

  REQUIRE(layer_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  REQUIRE(layer_menu->Dismiss(menu));
  runtime.BuildFrame();

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(layer_background_clicks == 1);
}

TEST_CASE("TestDebugOverlayUsesSystemLayerScope") {
  layer_app_compositions = 0;
  layer_background_clicks = 0;
  AppOptions options;
  options.show_debug_overlay = true;

  TestPlatform platform;
  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 1.0,
      .memory_usage_bytes = 64ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  Runtime runtime{DebugOverlayApp, platform, std::move(options)};
  runtime.SetViewport({360.0F, 260.0F});
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(ContainsText(initial, "DEBUG"));
  REQUIRE(!ContainsText(initial, "HuxerUI Performance"));
  const std::optional<Rect> banner_text = FindPresentedTextRect(initial, "DEBUG");
  REQUIRE(banner_text.has_value());
  REQUIRE(banner_text->x > 300.0F);
  REQUIRE(banner_text->x + banner_text->width <= 360.0F);
  REQUIRE(banner_text->y < 56.0F);
  REQUIRE(FindRectWithColor(initial, Color::Rgb(183, 28, 28)) != nullptr);
  REQUIRE(std::ranges::any_of(initial.Commands(), [](const PaintCommand& command) {
    const auto* shadow = std::get_if<DrawShadowCommand>(&command);
    return shadow != nullptr && shadow->color == Color::Rgb(0, 0, 0, 0.32F) && shadow->offset == Point{} &&
           shadow->blur_radius == 8.0F;
  }));
  REQUIRE(layer_app_compositions == 1);

  ClickAt(runtime, {20.0F, 220.0F}, 123);
  REQUIRE(layer_background_clicks == 1);

  ClickAt(runtime, {332.0F, 28.0F}, 124);
  const FlattenedScene& expanded = runtime.BuildFrame();
  REQUIRE(ContainsText(expanded, "DEBUG"));
  REQUIRE(ContainsText(expanded, "HuxerUI Performance"));
  REQUIRE(ContainsText(expanded, "FPS"));
  REQUIRE(ContainsText(expanded, "COMMIT"));
  REQUIRE(ContainsText(expanded, "CPU"));
  REQUIRE(ContainsText(expanded, "MEMORY"));
  const std::optional<Rect> panel_title = FindPresentedTextRect(expanded, "HuxerUI Performance");
  REQUIRE(panel_title.has_value());
  REQUIRE(panel_title->x >= 16.0F);
  REQUIRE(panel_title->y >= 16.0F);
  REQUIRE(FindRectWithColor(expanded, Color::Rgb(17, 22, 31, 0.97F)) != nullptr);
  REQUIRE(layer_app_compositions == 1);
  REQUIRE(runtime.LastCommit().next_frame_deadline.has_value());

  const FlattenedScene& initialized = runtime.BuildFrame();
  REQUIRE(ContainsText(initialized, "64.0 MiB"));
  REQUIRE(ContainsText(initialized, "Damage 0.0%  /  360 x 260"));

  platform.AdvanceTime(1.0);
  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 1.2,
      .memory_usage_bytes = 72ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  runtime.BuildFrame();
  const FlattenedScene& sampled = runtime.BuildFrame();
  REQUIRE(ContainsText(sampled, "2"));
  REQUIRE(ContainsText(sampled, "5.0%"));
  REQUIRE(ContainsText(sampled, "72.0 MiB"));
  REQUIRE(layer_app_compositions == 1);

  ClickAt(runtime, {332.0F, 28.0F}, 125);
  const FlattenedScene& collapsed = runtime.BuildFrame();
  REQUIRE(ContainsText(collapsed, "DEBUG"));
  REQUIRE(!ContainsText(collapsed, "HuxerUI Performance"));
  REQUIRE(layer_app_compositions == 1);
  REQUIRE(layer_background_clicks == 1);
  platform.AdvanceTime(1.0);
  runtime.BuildFrame();
  REQUIRE(!runtime.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestDebugMetricsSamplesPaintedFramesAndProcessUsage") {
  TestPlatform platform;
  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 2.0,
      .memory_usage_bytes = 48ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  detail::DebugMetricsState metrics{platform};
  static_cast<void>(metrics.Sample(0.0));

  metrics.RecordCommit(0.004, DamageRegion{.full = true}, {200.0F, 100.0F});
  metrics.RecordCommit(0.001, {}, {200.0F, 100.0F});
  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 2.2,
      .memory_usage_bytes = 56ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  const detail::DebugMetricsSnapshot sampled = metrics.Sample(1.0);
  REQUIRE(sampled.painted_frame_count == 1);
  REQUIRE(sampled.fps == 1.0F);
  REQUIRE(sampled.average_commit_time_ms == 4.0F);
  REQUIRE(sampled.maximum_commit_time_ms == 4.0F);
  REQUIRE(sampled.cpu_percent == 5.0F);
  REQUIRE(sampled.memory_usage_bytes == 56ULL * 1024ULL * 1024ULL);
  REQUIRE(sampled.average_damage_ratio == 1.0F);
  REQUIRE(sampled.viewport == Size{200.0F, 100.0F});

  metrics.RecordCommit(0.003, DamageRegion{.full = true}, {200.0F, 100.0F});
  const detail::DebugMetricsSnapshot next_sample = metrics.Sample(2.0);
  REQUIRE(next_sample.painted_frame_count == 1);
  REQUIRE(next_sample.fps == 1.0F);
  REQUIRE(next_sample.average_commit_time_ms == 3.0F);
}

TEST_CASE("TestDebugOverlayDefaultMatchesBuildConfiguration") {
  const AppOptions options;
#if defined(NDEBUG)
  REQUIRE(!options.show_debug_overlay);
#else
  REQUIRE(options.show_debug_overlay);
#endif
}

TEST_CASE("TestBottomSheetDoesNotUseDialogStyleScrim") {
  layer_bottom_sheet.reset();

  TestPlatform platform;
  Runtime runtime{BottomSheetThemeApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  layer_bottom_sheet->Show([] { return Text("sheet").With(huxerui::Frame{80.0F, 30.0F}); });
  constexpr Color scrim_color = Color::Rgb(20, 80, 160, 0.25F);
  const DrawRectCommand* scrim = FindRectWithColor(runtime.BuildFrame(), scrim_color);
  REQUIRE(scrim != nullptr);
  constexpr Rect expected_scrim{0.0F, 0.0F, 200.0F, 120.0F};
  REQUIRE(scrim->rect == expected_scrim);
}

TEST_CASE("TestRuntimeDestructionDoesNotScheduleLayerFrames") {
  TestPlatform platform;
  auto runtime = std::make_unique<Runtime>(DestructionApp, platform);
  runtime->SetViewport({200.0F, 120.0F});
  runtime->BuildFrame();
  const int requested_frames = platform.requested_frames;

  runtime.reset();
  REQUIRE(platform.requested_frames == requested_frames);
}

} // namespace huxerui::test
