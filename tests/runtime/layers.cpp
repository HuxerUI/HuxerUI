#include "runtime_test_support.h"

#include <algorithm>
#include <limits>
#include <type_traits>

#include "image_test_support.h"

namespace huxerui::test {

namespace {

std::optional<DialogHandle> layer_dialogs;
std::optional<ToastHandle> layer_toast;
std::optional<BottomSheetHandle> layer_bottom_sheet;
std::optional<BottomSheetContext> layer_bottom_sheet_context;
std::optional<PopupHandle> layer_popup;
std::optional<PopupContext> layer_popup_context;
std::optional<MenuHandle> layer_menu;
std::optional<MenuHandle> nested_menu;
std::optional<DialogHandle> retained_environment_dialog;
std::optional<LayerController> raw_layers;
State<float> layer_anchor_offset;
State<bool> layer_anchor_visible;
State<int> layer_environment_value;
State<bool> layer_environment_provider_visible;
int layer_app_compositions = 0;
int layer_background_clicks = 0;
int popup_focus_clicks = 0;
int parent_menu_clicks = 0;
int exiting_layer_clicks = 0;
int exiting_layer_pointer_cancels = 0;
int quarantined_background_pointer_ups = 0;
LayerId reentrant_drag_layer;
std::vector<std::string> reentrant_drag_events;
constexpr Color nested_menu_color = Color::Rgb(40, 150, 90);
constexpr Color section_separator_color = Color::Rgb(210, 70, 40);
constexpr Color bottom_sheet_width_color = Color::Rgb(35, 125, 175);
constexpr Color menu_icon_override = Color::Rgb(30, 130, 210);

VectorAsset MenuTestIcon() {
  static const VectorAsset icon = VectorAsset::Create({16.0F, 16.0F}, [](VectorBuilder& builder) {
    builder.FillPath(Path::RoundedRect({2.0F, 2.0F, 12.0F, 12.0F}, CornerRadii{2.0F}), Color::Black());
  });
  return icon;
}

std::vector<MenuEntry> TestMenu(std::string label, std::function<void()> action = [] {}) {
  return {
      MenuItem(std::move(label), std::move(action)),
  };
}

View ParameterizedLayerContent(std::string label, int value) {
  return Text(label + " " + std::to_string(value));
}

View ParameterizedDialogContent(DialogContext context, std::string label, int value) {
  static_cast<void>(context);
  return ParameterizedLayerContent(std::move(label), value);
}

View ParameterizedBottomSheetContent(BottomSheetContext context, std::string label, int value) {
  layer_bottom_sheet_context = context;
  return ParameterizedLayerContent(std::move(label), value);
}

View ParameterizedPopupContent(PopupContext context, std::string label, int value) {
  layer_popup_context = context;
  return ParameterizedLayerContent(std::move(label), value);
}

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

View MaterialLayerApp() {
  return huxerui::MaterialTheme {LayerApp()};
}

View SectionMenuLayerApp() {
  MenuStyle style = MenuStyle::Default();
  style.separator_color = section_separator_color;
  style.separator_mode = MenuSeparatorMode::BetweenSections;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), LayerApp()};
}

View AnimatedMenuLayerApp() {
  MenuStyle style = MenuStyle::Default();
  style.motion = PresentationMotion{
      .initial_scale = 0.9F,
      .enter = TweenSpec{.duration = 0.2},
      .exit = TweenSpec{.duration = 0.2},
  };
  ThemeDefinition definition;
  definition.Set(std::move(style));
  return Theme {std::move(definition), LayerApp()};
}

View ExitingLayerInputContent() {
  return Button("exiting input")
      .With(huxerui::Frame{100.0F, 36.0F})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Cancel) {
          ++exiting_layer_pointer_cancels;
        }
      })
      .OnClick([] { ++exiting_layer_clicks; });
}

View LayerPointerQuarantineApp() {
  HUXERUI_SCOPE({
    layer_dialogs = UseDialog();
    return Button("background input")
        .With(huxerui::Frame{240.0F, 160.0F})
        .On<ViewEvents::Pointer>([](const PointerEvent& event) {
          if (event.type == PointerEventType::Up) {
            ++quarantined_background_pointer_ups;
          }
        });
  });
}

View ReentrantDragLayerContent() {
  return Text("reentrant drag")
      .With(huxerui::Frame{100.0F, 40.0F}, DragGesture{})
      .On<DragEvents::Started>([](const DragEvent&) {
        reentrant_drag_events.emplace_back("started");
        raw_layers->Dismiss(reentrant_drag_layer);
      })
      .On<DragEvents::Changed>([](const DragEvent&) { reentrant_drag_events.emplace_back("changed"); })
      .On<DragEvents::Canceled>([](const DragEvent&) { reentrant_drag_events.emplace_back("canceled"); });
}

struct LayerEnvironmentValue {
  int value = 0;

  static LayerEnvironmentValue Default() {
    return {};
  }

  bool operator==(const LayerEnvironmentValue&) const = default;
};

View LayerEnvironmentDialogContent() {
  return Text("dialog environment " + std::to_string(UseEnvironment<LayerEnvironmentValue>().value));
}

View LayerEnvironmentApp() {
  layer_environment_value = UseState(1);
  return ProvideEnvironment(LayerEnvironmentValue{layer_environment_value.Get()}, Scope([] {
    return Text("application")
        .With(Dialog{
            .visible = true,
            .content = LayerEnvironmentDialogContent,
        });
  }));
}

View RetainedLayerEnvironmentOwner() {
  HUXERUI_SCOPE({
    retained_environment_dialog = UseDialog();
    return Text("environment owner");
  });
}

View RetainedLayerEnvironmentApp() {
  layer_environment_provider_visible = UseState(true);
  if (!layer_environment_provider_visible.Get()) {
    return Text("provider removed");
  }
  return ProvideEnvironment(LayerEnvironmentValue{7}, RetainedLayerEnvironmentOwner());
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
  MenuStyle style = MenuStyle::Default();
  style.background = nested_menu_color;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), Scope([] {
    auto menu = UseMenu();
    nested_menu = menu;
    return Button("nested menu anchor").With(huxerui::Frame{60.0F, 30.0F}, menu.Anchor());
  })};
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
      .With(Dialog{
          .visible = true,
          .content = [] { return Button("dialog"); },
      });
}

View BottomSheetThemeContent() {
  layer_bottom_sheet = UseBottomSheet();
  return Text("application");
}

View BottomSheetThemeApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.colors.scrim = Color::Rgb(20, 80, 160, 0.25F);
  ThemeDefinition definition = FlatThemeDefinition(spec);
  definition.Set(DialogStyle{.scrim = Color::Rgb(180, 20, 20, 0.75F)});
  return Theme {std::move(definition), Scope(BottomSheetThemeContent)};
}

View BottomSheetWidthApp() {
  BottomSheetStyle style = BottomSheetStyle::Default();
  style.background = bottom_sheet_width_color;
  style.maximum_width = 320.0F;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), Scope(BottomSheetThemeContent)};
}

View ReducedMotionLayerApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme {ThemeDefinition{spec}, LayerApp()};
}

View InvalidMenuShadowApp() {
  MenuStyle style = MenuStyle::Default();
  style.shadow.blur_radius = -1.0F;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), LayerApp()};
}

View InvalidBottomSheetShadowApp() {
  BottomSheetStyle style = BottomSheetStyle::Default();
  style.shadow.spread = std::numeric_limits<float>::infinity();
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), LayerApp()};
}

} // namespace

TEST_CASE("TestLayerMutationsDoNotRecomposeApplicationRoot") {
  layer_app_compositions = 0;
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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

TEST_CASE("PresentationFactoriesBindTypedArguments") {
  raw_layers.reset();
  AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](RootContext& root) { raw_layers = root.Layers(); });

  TestPlatform platform;
  Runtime runtime{LayerApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  REQUIRE(raw_layers.has_value());
  const LayerId layer = raw_layers->Attach({}, ParameterizedLayerContent, std::string{"Layer"}, 1);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Layer 1"));
  REQUIRE(raw_layers->Update(layer, ParameterizedLayerContent, std::string{"Updated"}, 2));
  REQUIRE(ContainsText(runtime.BuildFrame(), "Updated 2"));
  REQUIRE(raw_layers->Dismiss(layer));

  const LayerId dialog = layer_dialogs->Show(ParameterizedDialogContent, std::string{"Dialog"}, 3);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Dialog 3"));
  REQUIRE(layer_dialogs->Dismiss(dialog));
  SettlePresentation(platform, runtime);

  const LayerId sheet = layer_bottom_sheet->Show(ParameterizedBottomSheetContent, std::string{"Sheet"}, 4);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Sheet 4"));
  REQUIRE(layer_bottom_sheet->Dismiss(sheet));
  SettlePresentation(platform, runtime);

  const LayerId popup =
      layer_popup->ShowAt({80.0F, 40.0F}, ParameterizedPopupContent, std::string{"Popup"}, 5);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Popup 5"));
  REQUIRE(layer_popup->Update(popup, ParameterizedPopupContent, std::string{"Updated popup"}, 6));
  const FlattenedScene& updated_popup = runtime.BuildFrame();
  REQUIRE(!ContainsText(updated_popup, "Popup 5"));
  REQUIRE(ContainsText(updated_popup, "Updated popup 6"));
  REQUIRE(layer_popup_context.has_value());
  REQUIRE(layer_popup_context->Id() == popup);
  REQUIRE(layer_popup->Dismiss(popup));
  REQUIRE_FALSE(layer_popup->Update(popup, [] { return Text("missing popup"); }));
}

TEST_CASE("TestPopupAndMenuHandlesReplaceTheirActiveEntries") {
  layer_popup.reset();
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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

  const LayerId first_menu = layer_menu->ShowAt({80.0F, 40.0F}, TestMenu("first menu"));
  runtime.BuildFrame();
  const LayerId second_menu = layer_menu->ShowAt({80.0F, 40.0F}, TestMenu("second menu"));
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  const float infinity = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS_AS(layer_popup->ShowAt({infinity, 0.0F}, [] { return Text("popup"); }), std::invalid_argument);
  REQUIRE_THROWS_AS(layer_menu->ShowAt({0.0F, infinity}, TestMenu("menu")), std::invalid_argument);
  REQUIRE_THROWS_AS(
      layer_popup->ShowAt(
          {0.0F, 0.0F},
          [] { return Text("popup"); },
          PopupOptions{.gap = -1.0F}
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      layer_menu->ShowAt({0.0F, 0.0F}, TestMenu("menu"), MenuOptions{.viewport_margin = -1.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(layer_menu->Show({}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      layer_menu->Show({
          MenuSection{},
          MenuItem("invalid", [] {}),
      }),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(layer_menu->Show({MenuItem("invalid", std::function<void()>{})}), std::invalid_argument);
  REQUIRE_THROWS_AS(layer_menu->Show({MenuItem("invalid", std::vector<MenuEntry>{})}), std::invalid_argument);
  REQUIRE_THROWS_AS(layer_menu->Show({MenuItem(VectorAsset{}, "invalid", [] {})}), std::invalid_argument);
  REQUIRE_THROWS_AS(layer_menu->Show({MenuItem("", [] {})}), std::invalid_argument);
  REQUIRE_THROWS_AS(layer_menu->Show(TestMenu("menu"), MenuOptions{.width = 0.0F}), std::invalid_argument);
}

TEST_CASE("TestPresentationStylesRejectInvalidShadowsBeforeAttachingLayers") {
  {
    layer_menu.reset();
    TestPlatform platform;
    Runtime runtime{InvalidMenuShadowApp, platform};
    runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
    runtime.BuildFrame();
    REQUIRE_THROWS_AS(layer_menu->Show(TestMenu("menu")), std::invalid_argument);
  }

  {
    layer_bottom_sheet.reset();
    TestPlatform platform;
    Runtime runtime{InvalidBottomSheetShadowApp, platform};
    runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
    runtime.BuildFrame();
    REQUIRE_THROWS_AS(layer_bottom_sheet->Show([] { return Text("sheet"); }), std::invalid_argument);
  }
}

TEST_CASE("TestAnchoredPresentationClampsOversizedViewportMargin") {
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {10.0F, 10.0F}});
  runtime.BuildFrame();

  layer_popup->ShowAt(
      {5.0F, 5.0F},
      [] { return Text("tiny popup"); },
      PopupOptions{.viewport_margin = 100.0F}
  );
  REQUIRE_NOTHROW(runtime.BuildFrame());
}

TEST_CASE("TestAnchoredPresentationAlignsAndOffsetsFromAnchor") {
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  constexpr Color popup_color = Color::Rgb(190, 70, 40);
  const LayerId popup = layer_popup->Show(
      [popup_color] {
        return Spacer().With(huxerui::Frame{20.0F, 10.0F}, huxerui::Background{popup_color});
      },
      PopupOptions{
          .placement =
              AnchorPlacement{
                  .side = AnchorSide::Below,
                  .alignment = AnchorAlignment::End,
              },
          .offset = {5.0F, 3.0F},
      }
  );
  const std::optional<Rect> bounds = FindPresentedRectWithColor(runtime.BuildFrame(), popup_color);
  REQUIRE(bounds.has_value());
  REQUIRE(*bounds == Rect{65.0F, 37.0F, 20.0F, 10.0F});
  REQUIRE(layer_popup->Dismiss(popup));
}

TEST_CASE("TestAnchoredPresentationKeepsHostCoordinatesInsideSafeArea") {
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({
      .viewport = {200.0F, 120.0F},
      .safe_area = {.top = 20.0F, .right = 10.0F, .bottom = 10.0F, .left = 10.0F},
  });
  runtime.BuildFrame();

  constexpr Color popup_color = Color::Rgb(180, 80, 30);
  layer_popup->Show(
      [popup_color] { return Spacer().With(huxerui::Frame{20.0F, 10.0F}, huxerui::Background{popup_color}); },
      PopupOptions{
          .placement = AnchorPlacement{.side = AnchorSide::Below, .alignment = AnchorAlignment::End},
          .offset = {5.0F, 3.0F},
      }
  );
  const std::optional<Rect> bounds = FindPresentedRectWithColor(runtime.BuildFrame(), popup_color);
  REQUIRE(bounds.has_value());
  REQUIRE(*bounds == Rect{75.0F, 57.0F, 20.0F, 10.0F});
}

TEST_CASE("TestMenuSectionsAndSubmenusUseSemanticEntries") {
  layer_menu.reset();
  popup_focus_clicks = 0;

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  layer_menu->Show({
      MenuItem("Rename", [] {}),
      MenuSection{},
      MenuItem(
          "Move to",
          {
              MenuItem("Archive", [] { ++popup_focus_clicks; }),
          }
      ),
  });
  const FlattenedScene& root_menu = runtime.BuildFrame();
  REQUIRE(ContainsText(root_menu, "Rename"));
  const DrawRectCommand* separator = nullptr;
  for (const PaintCommand& command : root_menu.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->color == MenuStyle::Default().separator_color && rect->rect.height == 1.0F) {
      separator = rect;
      break;
    }
  }
  REQUIRE(separator != nullptr);
  const std::optional<Rect> submenu_item = FindPresentedTextRect(root_menu, "Move to");
  REQUIRE(submenu_item.has_value());
  const std::optional<Rect> first_item = FindPresentedTextRect(root_menu, "Rename");
  REQUIRE(first_item.has_value());
  REQUIRE(submenu_item->y - first_item->y < 80.0F);

  ClickAt(runtime, {submenu_item->x + submenu_item->width * 0.5F, submenu_item->y + submenu_item->height * 0.5F}, 131);
  const FlattenedScene& submenu = runtime.BuildFrame();
  std::optional<Rect> archive = FindPresentedTextRect(submenu, "Archive");
  REQUIRE(archive.has_value());

  REQUIRE(runtime.HandleBack());
  const FlattenedScene& root_after_back = runtime.BuildFrame();
  REQUIRE(ContainsText(root_after_back, "Rename"));
  REQUIRE(!ContainsText(root_after_back, "Archive"));

  ClickAt(runtime, {submenu_item->x + submenu_item->width * 0.5F, submenu_item->y + submenu_item->height * 0.5F}, 132);
  archive = FindPresentedTextRect(runtime.BuildFrame(), "Archive");
  REQUIRE(archive.has_value());

  ClickAt(runtime, {archive->x + archive->width * 0.5F, archive->y + archive->height * 0.5F}, 133);
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(popup_focus_clicks == 1);
  REQUIRE(!ContainsText(dismissed, "Rename"));
  REQUIRE(!ContainsText(dismissed, "Archive"));
}

TEST_CASE("TestSubmenuLeavesItsParentInteractiveAndOutsideDismissesTheCascade") {
  layer_menu.reset();
  parent_menu_clicks = 0;

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto show_menu = [] {
    layer_menu->Show({
        MenuItem("Close chain", [] { ++parent_menu_clicks; }),
        MenuItem(
            "More",
            {
                MenuItem("Child action", [] {}),
            }
        ),
    });
  };
  show_menu();
  const std::optional<Rect> more = FindPresentedTextRect(runtime.BuildFrame(), "More");
  REQUIRE(more.has_value());
  ClickAt(runtime, {more->x + more->width * 0.5F, more->y + more->height * 0.5F}, 134);
  const FlattenedScene& submenu = runtime.BuildFrame();
  REQUIRE(ContainsText(submenu, "Child action"));

  const std::optional<Rect> parent_action = FindPresentedTextRect(submenu, "Close chain");
  REQUIRE(parent_action.has_value());
  ClickAt(
      runtime,
      {parent_action->x + parent_action->width * 0.5F, parent_action->y + parent_action->height * 0.5F},
      135
  );
  const FlattenedScene& parent_dismissed = runtime.BuildFrame();
  REQUIRE(parent_menu_clicks == 1);
  REQUIRE(!ContainsText(parent_dismissed, "Close chain"));
  REQUIRE(!ContainsText(parent_dismissed, "Child action"));

  show_menu();
  const std::optional<Rect> reopened_more = FindPresentedTextRect(runtime.BuildFrame(), "More");
  REQUIRE(reopened_more.has_value());
  ClickAt(
      runtime,
      {reopened_more->x + reopened_more->width * 0.5F, reopened_more->y + reopened_more->height * 0.5F},
      136
  );
  REQUIRE(ContainsText(runtime.BuildFrame(), "Child action"));
  ClickAt(runtime, {1.0F, 1.0F}, 137);
  const FlattenedScene& outside_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(outside_dismissed, "Close chain"));
  REQUIRE(!ContainsText(outside_dismissed, "Child action"));
}

TEST_CASE("TestMenuCheckedAndDisabledItemsKeepTheirSemantics") {
  layer_menu.reset();
  parent_menu_clicks = 0;

  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  layer_menu->Show({
      MenuItem("Checked", [] {}).Checked(true),
      MenuItem("Disabled", [] { ++parent_menu_clicks; }).Enabled(false),
  });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "Checked"));
  REQUIRE_FALSE(ContainsText(shown, "\xE2\x9C\x93"));
  REQUIRE(FindPresentedStrokePathRect(shown, MenuStyle::Default().foreground).has_value());
  const std::optional<Rect> disabled = FindPresentedTextRect(shown, "Disabled");
  REQUIRE(disabled.has_value());

  ClickAt(runtime, {disabled->x + disabled->width * 0.5F, disabled->y + disabled->height * 0.5F}, 138);
  const FlattenedScene& still_open = runtime.BuildFrame();
  REQUIRE(parent_menu_clicks == 0);
  REQUIRE(ContainsText(still_open, "Checked"));
}

TEST_CASE("TestMenuSeparatorPolicyComesFromTheme") {
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{MaterialLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  layer_menu->Show({
      MenuItem("First", [] {}),
      MenuItem("Second", [] {}),
      MenuSection{},
      MenuItem("Third", [] {}),
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& menu = runtime.BuildFrame();
  REQUIRE(ContainsText(menu, "First"));

  Color separator = huxerui::MaterialLightThemeSpec().colors.on_surface;
  separator.alpha = 0.12F;
  REQUIRE(FindRectWithColor(menu, separator) == nullptr);
}

TEST_CASE("TestMenuSectionMarksThemedSeparatorBoundaries") {
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{SectionMenuLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  layer_menu->Show({
      MenuItem("First", [] {}),
      MenuItem("Second", [] {}),
  });
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), section_separator_color) == nullptr);

  layer_menu->Show({
      MenuItem("First", [] {}),
      MenuSection{},
      MenuItem("Second", [] {}),
  });
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), section_separator_color) != nullptr);
}

TEST_CASE("TestThemedMenuMotionRetainsTheLayerThroughExit") {
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{AnimatedMenuLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const LayerId menu = layer_menu->Show({MenuItem("Animated item", [] {})});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "Animated item"));

  REQUIRE(layer_menu->Dismiss(menu));
  REQUIRE(ContainsText(runtime.BuildFrame(), "Animated item"));
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "Animated item"));
}

TEST_CASE("TestMenuUsesNaturalOrExplicitSurfaceWidthAndOptionalImages") {
  STATIC_REQUIRE(std::is_constructible_v<MenuItem, ImageResource, StringResource, std::function<void()>>);
  STATIC_REQUIRE(std::is_constructible_v<MenuItem, VectorAsset, StringResource, std::function<void()>>);

  layer_menu.reset();
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const ImageAsset icon = ImageAsset::FromEncoded(MakeTestPng(16, 16));
  layer_menu->Show({
      MenuItem(icon, "With icon", [] {}),
      MenuItem("No icon", [] {}),
  });
  const FlattenedScene& natural = runtime.BuildFrame();
  const std::optional<Rect> natural_surface = FindPresentedRectWithColor(natural, MenuStyle::Default().background);
  REQUIRE(natural_surface.has_value());
  REQUIRE(natural_surface->width >= MenuStyle::Default().minimum_width);
  REQUIRE(natural_surface->width < 300.0F);

  const auto menu_clip = std::ranges::find_if(natural.Commands(), [](const PaintCommand& command) {
    const auto* clip = std::get_if<PushClipCommand>(&command);
    return clip && clip->corner_radius == MenuStyle::Default().corner_radius;
  });
  REQUIRE(menu_clip != natural.Commands().end());

  const std::optional<Rect> icon_label = FindPresentedTextRect(natural, "With icon");
  const std::optional<Rect> plain_label = FindPresentedTextRect(natural, "No icon");
  REQUIRE(icon_label.has_value());
  REQUIRE(plain_label.has_value());
  REQUIRE(plain_label->x < icon_label->x);
  REQUIRE(std::ranges::any_of(natural.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<huxerui::DrawImageCommand>(command);
  }));

  layer_menu->Show(TestMenu("Fixed width"), MenuOptions{.width = 240.0F});
  const std::optional<Rect> fixed_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), MenuStyle::Default().background);
  REQUIRE(fixed_surface.has_value());
  REQUIRE(fixed_surface->width == Catch::Approx(240.0F));

  layer_menu->Show(
      {MenuItem("Fixed submenu", std::vector<MenuEntry>{MenuItem("Child", [] {})})},
      MenuOptions{.width = 240.0F}
  );
  const FlattenedScene& fixed_submenu = runtime.BuildFrame();
  const std::optional<Rect> submenu_label = FindPresentedTextRect(fixed_submenu, "Fixed submenu");
  const std::optional<Rect> submenu_arrow =
      FindPresentedStrokePathRect(fixed_submenu, MenuStyle::Default().foreground);
  REQUIRE(submenu_label.has_value());
  REQUIRE(submenu_arrow.has_value());
  REQUIRE(submenu_arrow->x > fixed_surface->x + fixed_surface->width * 0.75F);

  layer_menu->Show({MenuItem("Natural submenu", std::vector<MenuEntry>{MenuItem("Child", [] {})})});
  const FlattenedScene& natural_submenu = runtime.BuildFrame();
  const std::optional<Rect> natural_submenu_surface =
      FindPresentedRectWithColor(natural_submenu, MenuStyle::Default().background);
  const std::optional<Rect> natural_submenu_arrow =
      FindPresentedStrokePathRect(natural_submenu, MenuStyle::Default().foreground);
  REQUIRE(natural_submenu_surface.has_value());
  REQUIRE(natural_submenu_surface->width < 300.0F);
  REQUIRE(natural_submenu_arrow.has_value());
  REQUIRE(natural_submenu_arrow->x > natural_submenu_surface->x + natural_submenu_surface->width * 0.75F);
}

TEST_CASE("TestMenuVectorIconsUseThemedAndItemTint") {
  layer_menu.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  layer_menu->Show({
      MenuItem(MenuTestIcon(), "Themed icon", [] {}),
      MenuItem(MenuTestIcon(), "Override icon", [] {}).IconTint(menu_icon_override),
  });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(FindPresentedRectWithColor(shown, MenuStyle::Default().icon_tint).has_value());
  REQUIRE(FindPresentedRectWithColor(shown, menu_icon_override).has_value());

  const ImageAsset raster = ImageAsset::FromEncoded(MakeTestPng(16, 16));
  layer_menu->Show({MenuItem(raster, "Raster icon", [] {}).IconTint(menu_icon_override)});
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
}

TEST_CASE("TestBottomSheetPlacementContextAndBackDismissal") {
  layer_app_compositions = 0;
  layer_bottom_sheet.reset();
  layer_bottom_sheet_context.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  constexpr Color sheet_color = Color::Rgb(30, 110, 190);
  const LayerId sheet = layer_bottom_sheet->Show([sheet_color](BottomSheetContext context) {
    layer_bottom_sheet_context = context;
    return Text("bottom sheet").With(huxerui::Frame{80.0F, 30.0F}, huxerui::Background{sheet_color});
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(layer_bottom_sheet_context.has_value());
  REQUIRE(layer_bottom_sheet_context->Id() == sheet);
  REQUIRE(ContainsText(shown, "bottom sheet"));
  const Rect expected_bounds{0.0F, 90.0F, 200.0F, 30.0F};
  const std::optional<Rect> sheet_bounds = FindPresentedRectWithColor(shown, sheet_color);
  REQUIRE(sheet_bounds.has_value());
  CAPTURE(sheet_bounds->x, sheet_bounds->y, sheet_bounds->width, sheet_bounds->height);
  REQUIRE(*sheet_bounds == expected_bounds);

  REQUIRE(runtime.HandleBack());
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "bottom sheet"));
  REQUIRE(!runtime.HandleBack());
}

TEST_CASE("TestBottomSheetFillsCompactViewportsAndHonorsItsDesktopWidthLimit") {
  layer_bottom_sheet.reset();

  TestPlatform platform;
  Runtime runtime{BottomSheetWidthApp, platform};
  runtime.SetWindowMetrics({.viewport = {800.0F, 480.0F}});
  runtime.BuildFrame();

  layer_bottom_sheet->Show([] { return Text("limited sheet").With(huxerui::Frame{.height = 80.0F}); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<Rect> surface = FindPresentedRectWithColor(runtime.BuildFrame(), bottom_sheet_width_color);
  REQUIRE(surface.has_value());
  REQUIRE(surface->width == Catch::Approx(320.0F));
  REQUIRE(surface->x == Catch::Approx(240.0F));
}

TEST_CASE("TestMaterialBottomSheetPlacesItsDragHandleWithVerticalPadding") {
  layer_bottom_sheet.reset();
  TestPlatform platform;
  Runtime runtime{MaterialLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  constexpr Color content_color = Color::Rgb(16, 96, 176);
  layer_bottom_sheet->Show([content_color] {
    return Spacer().With(Frame{.height = 40.0F}, Background{content_color});
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& scene = runtime.BuildFrame();
  const BottomSheetStyle style = ThemeDefinitionValue<BottomSheetStyle>(MaterialThemeDefinition());
  const std::optional<Rect> surface = FindPresentedRectWithColor(scene, style.background);
  const std::optional<Rect> handle = FindPresentedRectWithColor(scene, style.drag_handle, style.drag_handle_size);
  const std::optional<Rect> content = FindPresentedRectWithColor(scene, content_color);
  REQUIRE(surface.has_value());
  REQUIRE(handle.has_value());
  REQUIRE(content.has_value());
  REQUIRE(handle->width == style.drag_handle_size.width);
  REQUIRE(handle->height == style.drag_handle_size.height);
  REQUIRE(handle->y - surface->y == style.drag_handle_padding.top);
  REQUIRE(content->y - (handle->y + handle->height) == style.drag_handle_padding.bottom);
}

TEST_CASE("TestMaterialBottomSheetDragHandleMovesSettlesCancelsAndDismisses") {
  layer_bottom_sheet.reset();
  TestPlatform platform;
  Runtime runtime{MaterialLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
  runtime.BuildFrame();

  constexpr Color content_color = Color::Rgb(18, 100, 180);
  layer_bottom_sheet->Show([content_color] {
    return Spacer().With(Frame{.height = 80.0F}, Background{content_color});
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);

  const BottomSheetStyle style = ThemeDefinitionValue<BottomSheetStyle>(MaterialThemeDefinition());
  const std::optional<Rect> initial_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  const std::optional<Rect> initial_handle =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.drag_handle, style.drag_handle_size);
  REQUIRE(initial_surface.has_value());
  REQUIRE(initial_handle.has_value());
  const Point handle_center{
      initial_handle->x + initial_handle->width * 0.5F,
      initial_handle->y + initial_handle->height * 0.5F,
  };

  runtime.HandlePointerEvent({PointerEventType::Down, 901, handle_center, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent(
      {PointerEventType::Move, 901, {handle_center.x, handle_center.y + 24.0F}, PointerDeviceKind::Touch}
  );
  const std::optional<Rect> dragged_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  REQUIRE(dragged_surface.has_value());
  REQUIRE(dragged_surface->y == Catch::Approx(initial_surface->y + 24.0F));

  runtime.HandlePointerEvent(
      {PointerEventType::Up, 901, {handle_center.x, handle_center.y + 24.0F}, PointerDeviceKind::Touch}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<Rect> settled_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  REQUIRE(settled_surface.has_value());
  REQUIRE(settled_surface->y == Catch::Approx(initial_surface->y));

  const std::optional<Rect> settled_handle =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.drag_handle, style.drag_handle_size);
  REQUIRE(settled_handle.has_value());
  const Point settled_handle_center{
      settled_handle->x + settled_handle->width * 0.5F,
      settled_handle->y + settled_handle->height * 0.5F,
  };
  runtime.HandlePointerEvent({PointerEventType::Down, 902, settled_handle_center, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({
      PointerEventType::Move,
      902,
      {settled_handle_center.x, settled_handle_center.y + 24.0F},
      PointerDeviceKind::Touch,
  });
  runtime.BuildFrame();
  runtime.HandlePointerEvent({
      PointerEventType::Cancel,
      902,
      {settled_handle_center.x, settled_handle_center.y + 24.0F},
      PointerDeviceKind::Touch,
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<Rect> cancelled_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  REQUIRE(cancelled_surface.has_value());
  REQUIRE(cancelled_surface->y == Catch::Approx(initial_surface->y));

  runtime.HandlePointerEvent({PointerEventType::Down, 903, settled_handle_center, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({
      PointerEventType::Move,
      903,
      {settled_handle_center.x, settled_handle_center.y + 80.0F},
      PointerDeviceKind::Touch,
  });
  runtime.BuildFrame();
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      903,
      {settled_handle_center.x, settled_handle_center.y + 80.0F},
      PointerDeviceKind::Touch,
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(FindPresentedRectWithColor(runtime.BuildFrame(), content_color) == std::nullopt);
}

TEST_CASE("TestBottomSheetDragUsesDismissRequestCallback") {
  layer_bottom_sheet.reset();
  TestPlatform platform;
  Runtime runtime{MaterialLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
  runtime.BuildFrame();

  int dismiss_requests = 0;
  layer_bottom_sheet->Show(
      [] { return Spacer().With(Frame{.height = 80.0F}); },
      BottomSheetOptions{
          .on_dismiss_request = [&dismiss_requests] { ++dismiss_requests; },
      }
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);

  const BottomSheetStyle style = ThemeDefinitionValue<BottomSheetStyle>(MaterialThemeDefinition());
  const std::optional<Rect> initial_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  const std::optional<Rect> handle =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.drag_handle, style.drag_handle_size);
  REQUIRE(initial_surface.has_value());
  REQUIRE(handle.has_value());
  const Point handle_center{
      handle->x + handle->width * 0.5F,
      handle->y + handle->height * 0.5F,
  };

  runtime.HandlePointerEvent({PointerEventType::Down, 904, handle_center, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent(
      {PointerEventType::Move, 904, {handle_center.x, handle_center.y + 80.0F}, PointerDeviceKind::Touch}
  );
  runtime.BuildFrame();
  runtime.HandlePointerEvent(
      {PointerEventType::Up, 904, {handle_center.x, handle_center.y + 80.0F}, PointerDeviceKind::Touch}
  );
  runtime.BuildFrame();
  REQUIRE(dismiss_requests == 1);

  SettlePresentation(platform, runtime);
  const std::optional<Rect> settled_surface =
      FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  REQUIRE(settled_surface.has_value());
  REQUIRE(settled_surface->y == Catch::Approx(initial_surface->y));
}

TEST_CASE("TestCapturedReducedMotionSettlesBothLayerAndPresentationMotionImmediately") {
  layer_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{ReducedMotionLayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  layer_dialogs->Show([] { return Text("reduced dialog"); });
  const int requests_after_show = platform.requested_frames;
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "reduced dialog"));
  REQUIRE(platform.requested_frames == requests_after_show);
  REQUIRE_FALSE(runtime.LastCommit().next_frame_deadline.has_value());
}

TEST_CASE("TestBackStopsAtTopmostConsumingLayer") {
  layer_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  const LayerId lower = layer_dialogs->Show([] { return Text("dismissible dialog"); });
  const LayerId upper = layer_dialogs->Show(
      [] { return Text("consuming dialog"); },
      DialogOptions{
          .dismiss_on_cancel = false,
      }
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "dismissible dialog"));
  REQUIRE(ContainsText(shown, "consuming dialog"));

  REQUIRE(runtime.HandleBack());
  const FlattenedScene& consumed = runtime.BuildFrame();
  REQUIRE(ContainsText(consumed, "dismissible dialog"));
  REQUIRE(ContainsText(consumed, "consuming dialog"));

  REQUIRE(layer_dialogs->Dismiss(upper));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(runtime.HandleBack());
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "dialog environment 1"));

  layer_environment_value = 2;
  REQUIRE(ContainsText(runtime.BuildFrame(), "dialog environment 2"));
}

TEST_CASE("LayerRetainsTheLastCommittedEnvironmentAfterProviderUnmount") {
  retained_environment_dialog.reset();

  TestPlatform platform;
  Runtime runtime{RetainedLayerEnvironmentApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();
  REQUIRE(retained_environment_dialog.has_value());

  const LayerId dialog = retained_environment_dialog->Show(LayerEnvironmentDialogContent);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "dialog environment 7"));

  layer_environment_provider_visible = false;
  runtime.BuildFrame();
  REQUIRE(retained_environment_dialog->Update(dialog, LayerEnvironmentDialogContent));
  REQUIRE(ContainsText(runtime.BuildFrame(), "dialog environment 7"));
  REQUIRE(retained_environment_dialog->Dismiss(dialog));
}

TEST_CASE("TestPopupContextAndMenuActionDismissTheirOwnLayers") {
  layer_app_compositions = 0;
  popup_focus_clicks = 0;
  layer_popup_context.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  const LayerId popup = layer_popup->Show([](PopupContext context) {
    layer_popup_context = context;
    return Text("context popup");
  });
  REQUIRE(ContainsText(runtime.BuildFrame(), "context popup"));
  REQUIRE(layer_popup_context->Id() == popup);
  REQUIRE(layer_popup_context->Dismiss());
  REQUIRE(!ContainsText(runtime.BuildFrame(), "context popup"));

  const LayerId menu = layer_menu->Show(TestMenu("context menu", [] { ++popup_focus_clicks; }));
  REQUIRE(ContainsText(runtime.BuildFrame(), "context menu"));
  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(popup_focus_clicks == 1);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "context menu"));
  REQUIRE(!layer_menu->Dismiss(menu));
  REQUIRE(layer_app_compositions == 1);
}

TEST_CASE("TestAnchoredPopupTracksPresentationBounds") {
  layer_app_compositions = 0;
  layer_popup.reset();

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
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
  runtime.SetWindowMetrics({.viewport = {400.0F, 240.0F}});
  runtime.BuildFrame();

  layer_popup->Show(NestedAnchorContent);
  runtime.BuildFrame();
  REQUIRE(nested_menu.has_value());
  nested_menu->Show(TestMenu("nested menu"));
  const std::optional<Rect> initial_bounds = FindPresentedRectWithColor(runtime.BuildFrame(), nested_menu_color);
  REQUIRE(initial_bounds.has_value());

  layer_anchor_offset = 50.0F;
  const std::optional<Rect> moved_bounds = FindPresentedRectWithColor(runtime.BuildFrame(), nested_menu_color);
  REQUIRE(moved_bounds.has_value());
  REQUIRE(moved_bounds->x == initial_bounds->x + 30.0F);
}

TEST_CASE("TestMenuTrapsFocusAndDismissesOnBack") {
  layer_menu.reset();
  popup_focus_clicks = 0;

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  layer_menu->Show({
      MenuItem("first menu item", [] { ++popup_focus_clicks; }),
      MenuItem("second menu item", [] {}),
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
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  runtime.BuildFrame();

  layer_popup->ShowAt(
      {120.0F, 40.0F},
      [] {
        return Button("popup focus").With(huxerui::Frame{80.0F, 30.0F}).OnClick([] { ++popup_focus_clicks; });
      },
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();
  runtime.HandleKeyEvent({KeyEventType::Down, Key::Tab});

  const LayerId dialog = layer_dialogs->Show([] { return Button("dialog focus"); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const LayerId menu = layer_menu->Show(TestMenu("menu focus"));
  runtime.BuildFrame();

  REQUIRE(layer_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  REQUIRE(layer_menu->Dismiss(menu));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Enter});
  REQUIRE(layer_background_clicks == 1);
}

TEST_CASE("TestExitingLayerCancelsInputUntilRemoval") {
  layer_dialogs.reset();
  layer_background_clicks = 0;
  exiting_layer_clicks = 0;
  exiting_layer_pointer_cancels = 0;

  TestPlatform platform;
  Runtime runtime{LayerApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  const LayerId dialog = layer_dialogs->Show(ExitingLayerInputContent);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<Rect> button = FindPresentedTextRect(runtime.BuildFrame(), "exiting input");
  REQUIRE(button.has_value());
  const Point pointer{
      button->x + button->width * 0.5F,
      button->y + button->height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 126, pointer});
  REQUIRE(layer_dialogs->Dismiss(dialog));
  REQUIRE(exiting_layer_pointer_cancels == 1);

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Up, 126, pointer});
  runtime.HandleKeyEvent(KeyEvent{KeyEventType::Down, Key::Enter});
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{KeyEventType::Down, Key::Tab});
  runtime.HandleKeyEvent(KeyEvent{KeyEventType::Down, Key::Enter});
  REQUIRE(exiting_layer_clicks == 0);
  REQUIRE(layer_background_clicks == 0);

  SettlePresentation(platform, runtime);
  runtime.HandleKeyEvent(KeyEvent{KeyEventType::Down, Key::Tab});
  runtime.HandleKeyEvent(KeyEvent{KeyEventType::Down, Key::Enter});
  REQUIRE(layer_background_clicks == 1);
}

TEST_CASE("TestExitingLayerQuarantinesThePhysicalPointerSequence") {
  layer_dialogs.reset();
  exiting_layer_pointer_cancels = 0;
  quarantined_background_pointer_ups = 0;

  TestPlatform platform;
  Runtime runtime{LayerPointerQuarantineApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  const LayerId dialog = layer_dialogs->Show(ExitingLayerInputContent);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<Rect> button = FindPresentedTextRect(runtime.BuildFrame(), "exiting input");
  REQUIRE(button.has_value());
  const Point pointer{
      button->x + button->width * 0.5F,
      button->y + button->height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 127, pointer});
  REQUIRE(layer_dialogs->Dismiss(dialog));
  REQUIRE(exiting_layer_pointer_cancels == 1);

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Up, 127, pointer});
  REQUIRE(quarantined_background_pointer_ups == 0);
}

TEST_CASE("TestDragStartLayerDismissalDoesNotPublishAfterCancellation") {
  raw_layers.reset();
  reentrant_drag_events.clear();
  AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](RootContext& root) { raw_layers = root.Layers(); });

  TestPlatform platform;
  Runtime runtime{LayerApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  reentrant_drag_layer = raw_layers->Attach({}, ReentrantDragLayerContent);
  const std::optional<Rect> target = FindPresentedTextRect(runtime.BuildFrame(), "reentrant drag");
  REQUIRE(target.has_value());
  const Point pointer{
      target->x + target->width * 0.5F,
      target->y + target->height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 128, pointer});
  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Move, 128, {pointer.x + 20.0F, pointer.y}});

  REQUIRE(reentrant_drag_events == std::vector<std::string>{"started", "canceled"});
}

TEST_CASE("TestDebugOverlayUsesSystemLayerScope") {
  layer_app_compositions = 0;
  layer_background_clicks = 0;
  AppOptions options;
  options.show_debug_overlay = true;
  options.window.content_mode = WindowContentMode::EdgeToEdge;

  TestPlatform platform;
  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 1.0,
      .memory_usage_bytes = 64ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  Runtime runtime{DebugOverlayApp, platform, std::move(options)};
  runtime.SetWindowMetrics({
      .viewport = {360.0F, 260.0F},
      .safe_area = {.top = 72.0F, .right = 40.0F, .bottom = 24.0F, .left = 64.0F},
      .title_bar = WindowTitleBarMetrics{.height = 80.0F, .left_inset = 20.0F, .right_inset = 100.0F},
  });
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(ContainsText(initial, "DEBUG"));
  REQUIRE(!ContainsText(initial, "HuxerUI Performance"));
  const std::shared_ptr<const SemanticFrame> initial_semantics = runtime.LastCommit().semantic_frame;
  const auto debug_semantics = std::ranges::find(initial_semantics->nodes, std::string("DEBUG"), &SemanticNode::label);
  REQUIRE(debug_semantics != initial_semantics->nodes.end());
  REQUIRE(debug_semantics->role == SemanticRole::Button);
  REQUIRE(debug_semantics->expanded == false);
  REQUIRE(debug_semantics->children.empty());
  REQUIRE(std::ranges::count(initial_semantics->nodes, std::string("DEBUG"), &SemanticNode::label) == 1);
  const std::optional<Rect> banner_text = FindPresentedTextRect(initial, "DEBUG");
  REQUIRE(banner_text.has_value());
  REQUIRE(banner_text->x > 300.0F);
  REQUIRE(banner_text->x + banner_text->width <= 360.0F);
  REQUIRE(banner_text->y < 56.0F);
  REQUIRE(FindRectWithColor(initial, Color::Rgb(155, 38, 52)) != nullptr);
  REQUIRE(std::ranges::any_of(initial.Commands(), [](const PaintCommand& command) {
    const auto* shadow = std::get_if<DrawShadowCommand>(&command);
    return shadow != nullptr && shadow->color == Color::Rgb(0, 0, 0, 0.32F) && shadow->offset == Point{} &&
           shadow->blur_radius == 12.0F;
  }));
  REQUIRE(layer_app_compositions == 1);

  ClickAt(runtime, {20.0F, 220.0F}, 123);
  REQUIRE(layer_background_clicks == 1);

  ClickAt(runtime, {330.0F, 30.0F}, 124);
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
  REQUIRE(panel_title->x < 64.0F);
  REQUIRE(panel_title->y < 72.0F);
  REQUIRE(FindRectWithColor(expanded, Color::Rgb(17, 22, 31, 0.97F)) != nullptr);
  REQUIRE(layer_app_compositions == 1);
  REQUIRE(runtime.LastCommit().next_frame_deadline.has_value());
  const auto expanded_debug_semantics = std::ranges::find(
      runtime.LastCommit().semantic_frame->nodes,
      std::string("DEBUG"),
      &SemanticNode::label
  );
  REQUIRE(expanded_debug_semantics != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(expanded_debug_semantics->expanded == true);

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

  ClickAt(runtime, {330.0F, 30.0F}, 125);
  const FlattenedScene& collapsed = runtime.BuildFrame();
  REQUIRE(ContainsText(collapsed, "DEBUG"));
  REQUIRE(!ContainsText(collapsed, "HuxerUI Performance"));
  REQUIRE(layer_app_compositions == 1);
  REQUIRE(layer_background_clicks == 1);
  platform.AdvanceTime(1.0);
  runtime.BuildFrame();
  REQUIRE(!runtime.LastCommit().next_frame_deadline.has_value());

  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 5.0,
      .memory_usage_bytes = 96ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  ClickAt(runtime, {330.0F, 30.0F}, 127);
  const FlattenedScene& reopened = runtime.BuildFrame();
  REQUIRE(ContainsText(reopened, "HuxerUI Performance"));
  REQUIRE(!ContainsText(reopened, "72.0 MiB"));
  REQUIRE(!ContainsText(reopened, "5.0%"));

  const FlattenedScene& reset_baseline = runtime.BuildFrame();
  REQUIRE(ContainsText(reset_baseline, "96.0 MiB"));
  REQUIRE(!ContainsText(reset_baseline, "5.0%"));

  platform.AdvanceTime(1.0);
  platform.process_metrics = ProcessMetrics{
      .cpu_time_seconds = 5.4,
      .memory_usage_bytes = 96ULL * 1024ULL * 1024ULL,
      .processor_count = 4,
  };
  runtime.BuildFrame();
  REQUIRE(ContainsText(runtime.BuildFrame(), "10.0%"));
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  layer_bottom_sheet->Show([] { return Text("sheet").With(huxerui::Frame{80.0F, 30.0F}); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  constexpr Color scrim_color = Color::Rgb(20, 80, 160, 0.25F);
  const DrawRectCommand* scrim = FindRectWithColor(runtime.BuildFrame(), scrim_color);
  REQUIRE(scrim != nullptr);
  constexpr Rect expected_scrim{0.0F, 0.0F, 200.0F, 120.0F};
  REQUIRE(scrim->rect == expected_scrim);
}

TEST_CASE("TestRuntimeDestructionDoesNotScheduleLayerFrames") {
  TestPlatform platform;
  auto runtime = std::make_unique<Runtime>(DestructionApp, platform);
  runtime->SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime->BuildFrame();
  const int requested_frames = platform.requested_frames;

  runtime.reset();
  REQUIRE(platform.requested_frames == requested_frames);
}

} // namespace huxerui::test
