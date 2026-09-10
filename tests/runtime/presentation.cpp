#include "runtime_test_support.h"

#include <limits>

namespace huxerui::test {
namespace {

struct TestRootService {
  LayerController* layers = nullptr;
  int value = 0;
};

std::shared_ptr<TestRootService> installed_root_service;
int observed_root_service_value = 0;
int root_app_clicks = 0;
ViewportClass observed_layer_viewport_class = ViewportClass::Compact;
int layer_viewport_compositions = 0;
std::optional<ToastHandle> saved_toast;
std::optional<SnackBarHandle> saved_snack_bar;
std::optional<DialogHandle> saved_dialogs;
std::optional<DialogContext> saved_dialog_context;
State<bool> declarative_dialog_visible;
State<int> declarative_dialog_value;
State<bool> declarative_dialog_motion_enabled;
State<bool> alternate_dialog_update_environment;
int first_dialog_clicks = 0;
int positive_dialog_clicks = 0;
constexpr Color policy_dialog_background = Color::Rgb(32, 84, 132);
constexpr Color policy_dialog_separator = Color::Rgb(220, 120, 30);
constexpr Color policy_toast_background = Color::Rgb(42, 52, 62);
constexpr Color initial_dialog_update_theme = Color::Rgb(40, 100, 220);
constexpr Color updated_dialog_update_theme = Color::Rgb(220, 70, 50);
constexpr Color initial_dialog_update_scrim = Color::Rgb(20, 30, 40, 0.2F);
constexpr Color updated_dialog_update_scrim = Color::Rgb(80, 20, 100, 0.45F);

bool PaintsText(const PaintSequence& sequence, std::string_view text) {
  return std::ranges::any_of(sequence.Commands(), [text](const PaintCommand& command) {
    const auto* draw_text = std::get_if<DrawTextCommand>(&command);
    return draw_text != nullptr && draw_text->text.PlainText() == text;
  });
}

std::optional<float>
RenderedTextOpacity(const RenderNode& node, std::string_view text, float inherited_opacity = 1.0F) {
  const float opacity = inherited_opacity * node.opacity;
  if (PaintsText(node.content, text) || PaintsText(node.foreground, text)) {
    return opacity;
  }
  for (const RenderNode* child : node.children) {
    if (child == nullptr) {
      continue;
    }
    if (const std::optional<float> found = RenderedTextOpacity(*child, text, opacity); found.has_value()) {
      return found;
    }
  }
  return std::nullopt;
}

View RootHookApp() {
  HUXERUI_SCOPE({
    observed_root_service_value = UseService<TestRootService>()->value;
    return Button("application").OnClick([] { ++root_app_clicks; });
  });
}

View PresentationApp() {
  HUXERUI_SCOPE({
    saved_toast = UseToast();
    saved_dialogs = UseDialog();
    return Text("content");
  });
}

View SnackBarApp() {
  HUXERUI_SCOPE({
    saved_snack_bar = UseSnackBar();
    return Button("background action");
  });
}

View PresentationThemeApp() {
  ThemeDefinition definition;
  definition.Set(
      huxerui::ToastStyle{
          .background = Color::Rgb(20, 30, 40, 0.9F),
          .text_style = TextStyle{Font::System(14.0F), Color::Rgb(240, 245, 250)},
          .padding = EdgeInsets::All(10.0F),
          .corner_radii = CornerRadii{9.0F},
      }
  );
  definition.Set(
      huxerui::DialogStyle{
          .scrim = Color::Rgb(180, 20, 20, 0.3F),
      }
  );
  return Theme {std::move(definition), PresentationApp()};
}

View MaterialPresentationApp() {
  return huxerui::MaterialTheme {PresentationApp()};
}

View MaterialSnackBarApp() {
  return huxerui::MaterialTheme {SnackBarApp()};
}

View DialogUpdateEnvironmentContent() {
  return Text(
      UseTheme().colors.primary == updated_dialog_update_theme ? "updated dialog environment"
                                                               : "initial dialog environment"
  );
}

View DialogUpdateEnvironmentApp() {
  alternate_dialog_update_environment = UseState(false);
  ThemeSpec theme = FlatLightThemeSpec();
  theme.colors.primary =
      alternate_dialog_update_environment ? updated_dialog_update_theme : initial_dialog_update_theme;

  DialogStyle dialog_style = DialogStyle::Default();
  dialog_style.scrim = alternate_dialog_update_environment ? updated_dialog_update_scrim : initial_dialog_update_scrim;
  dialog_style.motion.reset();

  ThemeDefinition definition{std::move(theme)};
  definition.Set(std::move(dialog_style));
  return Theme {std::move(definition), PresentationApp()};
}

View ThemedPresentationPolicyApp() {
  DialogStyle dialog_style = DialogStyle::Default();
  dialog_style.background = policy_dialog_background;
  dialog_style.action_separator_color = policy_dialog_separator;
  dialog_style.action_separator_thickness = 2.0F;
  dialog_style.placement = VerticalPlacement::Bottom;
  dialog_style.action_layout = Axis::Vertical;
  dialog_style.action_alignment = HorizontalAlignment::Stretch;
  dialog_style.viewport_margin = 10.0F;
  dialog_style.motion.reset();

  ToastStyle toast_style = ToastStyle::Default();
  toast_style.background = policy_toast_background;
  toast_style.placement = VerticalPlacement::Top;
  toast_style.motion.reset();

  ThemeDefinition definition;
  definition.Set(std::move(dialog_style));
  definition.Set(std::move(toast_style));
  return Theme {std::move(definition), PresentationApp()};
}

View FlatDarkPresentationApp() {
  return huxerui::FlatDarkTheme {PresentationApp()};
}

View DeclarativeDialogApp() {
  declarative_dialog_visible = UseState(false);
  declarative_dialog_value = UseState(1);
  const std::string label = "declarative dialog " + std::to_string(declarative_dialog_value.Get());
  return Text("content").With(
      Dialog {
          .visible = declarative_dialog_visible,
          .content = [label] { return Text(label); },
          .dismiss_on_outside_press = true,
          .on_dismiss_request = [visible = declarative_dialog_visible] { visible = false; },
      }
  );
}

View DeclarativeDialogMotionApp() {
  declarative_dialog_visible = UseState(false);
  declarative_dialog_motion_enabled = UseState(false);

  DialogStyle style = DialogStyle::Default();
  if (!declarative_dialog_motion_enabled) {
    style.motion.reset();
  }
  ThemeDefinition definition;
  definition.Set(std::move(style));
  return Theme {
    std::move(definition),
    Text("content").With(
        Dialog {
            .visible = declarative_dialog_visible,
            .content = [] { return Text("motion dialog"); },
        }
    ),
  };
}

} // namespace

TEST_CASE("TestRootHooksServicesAndLayers") {
  installed_root_service.reset();
  observed_root_service_value = 0;
  root_app_clicks = 0;
  int toast_compositions = 0;
  int modal_compositions = 0;

  huxerui::AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](huxerui::RootContext& root) {
    installed_root_service = std::make_shared<TestRootService>(TestRootService{
        &root.Layers(),
        42,
    });
    root.Provide(installed_root_service);
  });

  TestPlatform platform;
  Runtime runtime{RootHookApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(observed_root_service_value == 42);
  REQUIRE(ContainsText(initial, "application"));

  const LayerId toast = installed_root_service->layers->Attach(
      LayerOptions{
          .level = LayerLevel::Notification,
          .pointer_policy = LayerPointerPolicy::PassThrough,
      },
      [&toast_compositions] {
        ++toast_compositions;
        return Text("toast");
      }
  );
  const FlattenedScene& with_toast = runtime.BuildFrame();
  REQUIRE(toast_compositions == 1);
  REQUIRE(ContainsText(with_toast, "application"));
  REQUIRE(ContainsText(with_toast, "toast"));

  const LayerId modal = installed_root_service->layers->Attach(
      LayerOptions{
          .level = LayerLevel::Presentation,
          .pointer_policy = LayerPointerPolicy::Barrier,
          .trap_focus = true,
      },
      [&modal_compositions] {
        ++modal_compositions;
        return Text("modal");
      }
  );
  const FlattenedScene& with_modal = runtime.BuildFrame();
  REQUIRE(toast_compositions == 1);
  REQUIRE(modal_compositions == 1);
  std::vector<std::string> painted_text;
  for (const PaintCommand& command : with_modal.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      painted_text.push_back(text->text.PlainText());
    }
  }
  const auto modal_position = std::ranges::find(painted_text, "modal");
  const auto toast_position = std::ranges::find(painted_text, "toast");
  REQUIRE(modal_position != painted_text.end());
  REQUIRE(toast_position != painted_text.end());
  REQUIRE(modal_position < toast_position);
  ClickAt(runtime, {20.0F, 20.0F}, 82);
  REQUIRE(root_app_clicks == 0);

  REQUIRE(installed_root_service->layers->Update(modal, [&modal_compositions] {
    ++modal_compositions;
    return Text("updated modal");
  }));
  const FlattenedScene& updated_modal = runtime.BuildFrame();
  REQUIRE(ContainsText(updated_modal, "updated modal"));
  REQUIRE(toast_compositions == 1);
  REQUIRE(modal_compositions == 2);

  REQUIRE(installed_root_service->layers->Dismiss(modal));
  runtime.BuildFrame();
  REQUIRE(toast_compositions == 1);
  ClickAt(runtime, {20.0F, 20.0F}, 83);
  REQUIRE(root_app_clicks == 1);

  REQUIRE(installed_root_service->layers->Dismiss(toast));
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "toast"));
}

TEST_CASE("TestViewportClassRecomposesExistingLayersAcrossBreakpoints") {
  installed_root_service.reset();
  observed_layer_viewport_class = ViewportClass::Compact;
  layer_viewport_compositions = 0;

  AppOptions options;
  options.show_debug_overlay = false;
  options.root_hooks.push_back([](RootContext& root) {
    installed_root_service = std::make_shared<TestRootService>(TestRootService{
        &root.Layers(),
        0,
    });
    root.Provide(installed_root_service);
  });

  TestPlatform platform;
  Runtime runtime{RootHookApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {480.0F, 600.0F}});
  runtime.BuildFrame();

  installed_root_service->layers->Attach({}, [] {
    ++layer_viewport_compositions;
    observed_layer_viewport_class = UseViewportClass();
    return Text("responsive layer");
  });
  runtime.BuildFrame();
  REQUIRE(layer_viewport_compositions == 1);
  REQUIRE(observed_layer_viewport_class == ViewportClass::Compact);

  runtime.SetWindowMetrics({.viewport = {560.0F, 600.0F}});
  runtime.BuildFrame();
  REQUIRE(layer_viewport_compositions == 1);

  runtime.SetWindowMetrics({.viewport = {600.0F, 600.0F}});
  runtime.BuildFrame();
  REQUIRE(layer_viewport_compositions == 2);
  REQUIRE(observed_layer_viewport_class == ViewportClass::Medium);
}

TEST_CASE("TestToastAndDialogPresentation") {
  saved_toast.reset();
  saved_dialogs.reset();
  saved_dialog_context.reset();

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE(saved_toast.has_value());
  REQUIRE(saved_dialogs.has_value());

  saved_toast->Show("saved", huxerui::ToastOptions{0.5});
  const FlattenedScene& toast = runtime.BuildFrame();
  REQUIRE(ContainsText(toast, "saved"));
  const DrawRectCommand* toast_background = FindRectWithColor(toast, Color::Rgb(20, 30, 40, 0.9F));
  REQUIRE(toast_background != nullptr);
  REQUIRE(toast_background->rect.width == 70.0F);
  REQUIRE(toast_background->rect.height == 40.0F);
  const std::optional<Rect> presented_toast_background =
      FindPresentedRectWithColor(toast, Color::Rgb(20, 30, 40, 0.9F));
  REQUIRE(presented_toast_background.has_value());
  REQUIRE(presented_toast_background->x == 65.0F);
  REQUIRE(presented_toast_background->y == 36.0F);
  const DrawTextCommand* toast_text = FindText(toast, "saved");
  REQUIRE(toast_text != nullptr);
  REQUIRE(toast_text->style.foreground.green == Color::Rgb(240, 245, 250).green);
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  const FlattenedScene& expired = runtime.BuildFrame();
  REQUIRE(!ContainsText(expired, "saved"));

  const LayerId dialog = saved_dialogs->Show(
      [] { return Text("command dialog"); },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "command dialog"));
  const DrawRectCommand* scrim = FindRect(shown, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  REQUIRE(scrim != nullptr);
  REQUIRE(SolidBrushColor(scrim->brush) != nullptr);
  REQUIRE(SolidBrushColor(scrim->brush)->red == Color::Rgb(180, 20, 20, 0.3F).red);
  REQUIRE(SolidBrushColor(scrim->brush)->alpha == 0.3F);
  REQUIRE(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(dismissed, "command dialog"));

  const LayerId contextual_dialog = saved_dialogs->Show(
      [](DialogContext dialog_context) {
        saved_dialog_context = dialog_context;
        return Text("context dialog");
      },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& contextual = runtime.BuildFrame();
  REQUIRE(ContainsText(contextual, "context dialog"));
  REQUIRE(saved_dialog_context.has_value());
  REQUIRE(saved_dialog_context->Id() == contextual_dialog);

  saved_dialog_context.reset();
  REQUIRE(saved_dialogs->Update(contextual_dialog, [](DialogContext dialog_context) {
    saved_dialog_context = dialog_context;
    return Text("updated context dialog");
  }));
  const FlattenedScene& updated_contextual = runtime.BuildFrame();
  REQUIRE(ContainsText(updated_contextual, "updated context dialog"));
  REQUIRE(saved_dialog_context.has_value());
  REQUIRE(saved_dialog_context->Id() == contextual_dialog);
  REQUIRE(saved_dialog_context->Dismiss());
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& context_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(context_dismissed, "updated context dialog"));

  const LayerId outside_dialog = saved_dialogs->Show([] { return Text("outside dismiss dialog"); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& outside_shown = runtime.BuildFrame();
  REQUIRE(ContainsText(outside_shown, "outside dismiss dialog"));
  ClickAt(runtime, {1.0F, 1.0F}, 85);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& outside_dismissed = runtime.BuildFrame();
  REQUIRE(!ContainsText(outside_dismissed, "outside dismiss dialog"));
  REQUIRE(!saved_dialogs->Dismiss(outside_dialog));
}

TEST_CASE("TestToastRejectsAnEmptyLiteralBeforeAttachingALayer") {
  saved_toast.reset();

  TestPlatform platform;
  Runtime runtime{PresentationApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_toast->Show(""), std::invalid_argument);
  REQUIRE_NOTHROW(runtime.BuildFrame());
}

TEST_CASE("TestToastRetainsItsLayerUntilExitMotionCompletes") {
  saved_toast.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  const LayerId toast = saved_toast->Show("animated toast", ToastOptions{10.0});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "animated toast"));

  REQUIRE(saved_toast->Dismiss(toast));
  REQUIRE(ContainsText(runtime.BuildFrame(), "animated toast"));
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "animated toast"));
}

TEST_CASE("TestSnackBarValidatesRequestsBeforePresentation") {
  saved_snack_bar.reset();

  TestPlatform platform;
  Runtime runtime{SnackBarApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 200.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_snack_bar->Show(""), std::invalid_argument);
  REQUIRE_THROWS_AS(saved_snack_bar->Show("message", SnackBarOptions{0.0}), std::invalid_argument);
  const SnackBarOptions infinite_duration{std::numeric_limits<double>::infinity()};
  REQUIRE_THROWS_AS(saved_snack_bar->Show("message", infinite_duration), std::invalid_argument);
  REQUIRE_THROWS_AS(saved_snack_bar->Show("message", "", [] {}), std::invalid_argument);
  REQUIRE_THROWS_AS(saved_snack_bar->Show("message", "Action", std::function<void()>{}), std::invalid_argument);
  REQUIRE_NOTHROW(runtime.BuildFrame());
}

TEST_CASE("TestSnackBarAtomicallyReplacesRequestsAndGuardsReentrantActions") {
  saved_snack_bar.reset();
  int actions = 0;
  std::optional<LayerId> replacement;

  TestPlatform platform;
  Runtime runtime{MaterialSnackBarApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 220.0F}});
  runtime.BuildFrame();

  const LayerId first = saved_snack_bar->Show("first message", SnackBarOptions{0.75});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "first message"));

  const LayerId second = saved_snack_bar->Show(
      "second message",
      "Undo",
      [&] {
        ++actions;
        replacement = saved_snack_bar->Show("restored", SnackBarOptions{std::nullopt});
      },
      SnackBarOptions{std::nullopt}
  );
  const FlattenedScene& replaced = runtime.BuildFrame();
  REQUIRE(!ContainsText(replaced, "first message"));
  REQUIRE_FALSE(saved_snack_bar->Dismiss(first));

  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "second message"));
  platform.AdvanceTime(1.0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "second message"));
  const std::optional<Rect> action = FindPresentedTextRect(runtime.BuildFrame(), "Undo");
  REQUIRE(action.has_value());
  ClickAt(runtime, {action->x + action->width * 0.5F, action->y + action->height * 0.5F}, 201);
  REQUIRE(actions == 1);
  REQUIRE(replacement.has_value());
  const FlattenedScene& reentrant = runtime.BuildFrame();
  REQUIRE(!ContainsText(reentrant, "second message"));
  REQUIRE_FALSE(saved_snack_bar->Dismiss(second));
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "restored"));
  REQUIRE(saved_snack_bar->Dismiss(*replacement));
}

TEST_CASE("TestSnackBarTimeoutPausesForHoverFocusAndApplicationLifecycle") {
  saved_snack_bar.reset();

  TestPlatform platform;
  Runtime runtime{SnackBarApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 220.0F}});
  runtime.BuildFrame();

  saved_snack_bar->Show("hover pause", SnackBarOptions{1.0});
  const FlattenedScene& initial = runtime.BuildFrame();
  const std::optional<Rect> message = FindPresentedTextRect(initial, "hover pause");
  REQUIRE(message.has_value());
  platform.AdvanceTime(0.4);
  runtime.BuildFrame();
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Move,
      .pointer_id = 202,
      .position = {message->x + message->width * 0.5F, message->y + message->height * 0.5F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(2.0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "hover pause"));
  runtime.HandlePointerEvent(PointerEvent{.type = PointerEventType::Move, .pointer_id = 202, .position = {1.0F, 1.0F}});
  runtime.BuildFrame();
  platform.AdvanceTime(0.7);
  runtime.BuildFrame();
  REQUIRE(!ContainsText(runtime.BuildFrame(), "hover pause"));

  saved_snack_bar->Show("focus pause", "Action", [] {}, SnackBarOptions{1.0});
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  runtime.BuildFrame();
  platform.AdvanceTime(2.0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "focus pause"));
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  runtime.BuildFrame();
  platform.AdvanceTime(1.1);
  runtime.BuildFrame();
  REQUIRE(!ContainsText(runtime.BuildFrame(), "focus pause"));

  saved_snack_bar->Show("lifecycle pause", SnackBarOptions{0.5});
  runtime.BuildFrame();
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Background);
  runtime.BuildFrame();
  platform.AdvanceTime(2.0);
  REQUIRE(ContainsText(runtime.BuildFrame(), "lifecycle pause"));
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  runtime.BuildFrame();
  platform.AdvanceTime(0.6);
  runtime.BuildFrame();
  REQUIRE(!ContainsText(runtime.BuildFrame(), "lifecycle pause"));
}

TEST_CASE("TestCommandDialogUpdateRefreshesCapturedEnvironmentAndBarrier") {
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{DialogUpdateEnvironmentApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const LayerId dialog = saved_dialogs->Show(
      DialogUpdateEnvironmentContent,
      DialogOptions{
          .dismiss_on_outside_press = false,
      }
  );
  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(ContainsText(initial, "initial dialog environment"));
  REQUIRE(FindRectWithColor(initial, initial_dialog_update_scrim) != nullptr);

  alternate_dialog_update_environment = true;
  runtime.BuildFrame();
  REQUIRE(saved_dialogs->Update(dialog, DialogUpdateEnvironmentContent));
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(!ContainsText(updated, "initial dialog environment"));
  REQUIRE(ContainsText(updated, "updated dialog environment"));
  REQUIRE(FindRectWithColor(updated, updated_dialog_update_scrim) != nullptr);

  ClickAt(runtime, {1.0F, 1.0F}, 146);
  REQUIRE(ContainsText(runtime.BuildFrame(), "updated dialog environment"));
}

TEST_CASE("TestStandardDialogUsesDefaultLabelsAndTwoActions") {
  saved_dialogs.reset();
  positive_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  saved_dialogs->Show("Save changes?", "The current document has unsaved changes.", "OK", [] {
    ++positive_dialog_clicks;
  });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shortcut = runtime.BuildFrame();
  REQUIRE(ContainsText(shortcut, "Save changes?"));
  REQUIRE(ContainsText(shortcut, "The current document has unsaved changes."));
  const std::optional<Rect> positive = FindPresentedTextRect(shortcut, "OK");
  REQUIRE(positive.has_value());

  ClickAt(runtime, {positive->x + positive->width * 0.5F, positive->y + positive->height * 0.5F}, 141);
  REQUIRE(positive_dialog_clicks == 1);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "Save changes?"));

  saved_dialogs->Show("Remove item?", "This action cannot be undone.", "Remove", "Cancel");
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& two_actions = runtime.BuildFrame();
  REQUIRE(ContainsText(two_actions, "Remove item?"));
  REQUIRE(ContainsText(two_actions, "Cancel"));
  REQUIRE(ContainsText(two_actions, "Remove"));
  const std::optional<Rect> negative = FindPresentedTextRect(two_actions, "Cancel");
  REQUIRE(negative.has_value());
  ClickAt(runtime, {negative->x + negative->width * 0.5F, negative->y + negative->height * 0.5F}, 142);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "Remove item?"));
}

TEST_CASE("TestStandardDialogKeepsNaturalWidthAndRejectsEmptyLiteralContent") {
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetWindowMetrics({.viewport = {800.0F, 480.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(saved_dialogs->Show("", "Message", "OK"), std::invalid_argument);
  REQUIRE_THROWS_AS(saved_dialogs->Show("Title", "", "OK"), std::invalid_argument);

  saved_dialogs->Show("Short", "Message", "OK");
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const DialogStyle style = ThemeDefinitionValue<DialogStyle>(MaterialThemeDefinition());
  const std::optional<Rect> surface = FindPresentedRectWithColor(runtime.BuildFrame(), style.background);
  REQUIRE(surface.has_value());
  REQUIRE(surface->width < style.maximum_width);
}

TEST_CASE("TestMaterialDialogActionUsesThemeRipple") {
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{MaterialPresentationApp, platform};
  runtime.SetWindowMetrics({.viewport = {640.0F, 360.0F}});
  runtime.BuildFrame();

  saved_dialogs->Show("Save changes?", "The current document has unsaved changes.", "Save");
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  const std::optional<Rect> save = FindPresentedTextRect(shown, "Save");
  REQUIRE(save.has_value());

  const Point pointer{
      save->x + save->width * 0.5F,
      save->y + save->height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      143,
      pointer,
  });
  runtime.BuildFrame();
  const ThemeSpec material = MaterialLightThemeSpec();
  platform.AdvanceTime(material.motion.slow * 0.5);
  const FlattenedScene& pressed = runtime.BuildFrame();

  const Indication expected =
      ThemeDefinitionValue<DialogStyle>(MaterialThemeDefinition()).positive_action_indication;
  REQUIRE(expected.ripple.has_value());
  const DrawCircleCommand* ripple = nullptr;
  for (const auto& command : pressed.Commands()) {
    const auto* circle = std::get_if<DrawCircleCommand>(&command);
    if (circle && circle->radius > 0.0F && circle->color == expected.ripple->color) {
      ripple = circle;
      break;
    }
  }
  REQUIRE(ripple != nullptr);
}

TEST_CASE("TestPresentationThemeControlsDialogLayoutAndVerticalPlacement") {
  saved_toast.reset();
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{ThemedPresentationPolicyApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const LayerId toast_id = saved_toast->Show("top toast", ToastOptions{10.0});
  const FlattenedScene& toast = runtime.BuildFrame();
  const std::optional<Rect> toast_surface = FindPresentedRectWithColor(toast, policy_toast_background);
  REQUIRE(toast_surface.has_value());
  REQUIRE(toast_surface->y < 40.0F);
  REQUIRE(saved_toast->Dismiss(toast_id));

  const LayerId policy_dialog =
      saved_dialogs->Show("Policy dialog", "Theme-owned placement and actions", "Second", "First");
  const FlattenedScene& dialog = runtime.BuildFrame();
  const std::optional<Rect> dialog_surface = FindPresentedRectWithColor(dialog, policy_dialog_background);
  REQUIRE(dialog_surface.has_value());
  REQUIRE(dialog_surface->y + dialog_surface->height <= 230.0F);
  REQUIRE(dialog_surface->y + dialog_surface->height > 220.0F);
  REQUIRE(FindRectWithColor(dialog, policy_dialog_separator) != nullptr);

  const std::optional<Rect> first = FindPresentedTextRect(dialog, "First");
  const std::optional<Rect> second = FindPresentedTextRect(dialog, "Second");
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->y < second->y);

  REQUIRE(saved_dialogs->Dismiss(policy_dialog));
  const LayerId custom_dialog = saved_dialogs->Show([] { return Text("before update"); });
  REQUIRE(ContainsText(runtime.BuildFrame(), "before update"));
  REQUIRE(saved_dialogs->Update(custom_dialog, [] { return Text("after update"); }));
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(!ContainsText(updated, "before update"));
  REQUIRE(ContainsText(updated, "after update"));
}

TEST_CASE("TestDialogRetainsExitPresentationWithoutRetainingInput") {
  saved_dialogs.reset();
  first_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PresentationThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const LayerId dialog =
      saved_dialogs->Show([] { return Button("animated dialog").OnClick([] { ++first_dialog_clicks; }); });
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "animated dialog"));

  REQUIRE(saved_dialogs->Dismiss(dialog));
  const FlattenedScene& exiting = runtime.BuildFrame();
  REQUIRE(ContainsText(exiting, "animated dialog"));
  ClickAt(runtime, {100.0F, 50.0F}, 133);
  REQUIRE(first_dialog_clicks == 0);

  REQUIRE(saved_dialogs->Update(dialog, [] { return Text("revived dialog"); }));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "revived dialog"));

  REQUIRE(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "animated dialog"));
  REQUIRE(!ContainsText(runtime.BuildFrame(), "revived dialog"));
}

TEST_CASE("TestFlatDarkPresentationStyles") {
  saved_toast.reset();
  saved_dialogs.reset();

  TestPlatform platform;
  Runtime runtime{FlatDarkPresentationApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  Color toast_background = dark.colors.on_surface;
  toast_background.alpha *= 0.94F;
  saved_toast->Show("dark toast", huxerui::ToastOptions{10.0});
  const FlattenedScene& toast = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(toast, toast_background) != nullptr);
  const DrawTextCommand* toast_text = FindText(toast, "dark toast");
  REQUIRE(toast_text != nullptr);
  REQUIRE(toast_text->style.foreground.red == dark.colors.surface.red);

  saved_dialogs->Show([] { return Text("dark dialog"); }, huxerui::DialogOptions{.dismiss_on_outside_press = false});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& dialog = runtime.BuildFrame();
  const DrawRectCommand* scrim = FindRect(dialog, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  REQUIRE(scrim != nullptr);
  REQUIRE(SolidBrushColor(scrim->brush) != nullptr);
  REQUIRE(SolidBrushColor(scrim->brush)->alpha == dark.colors.scrim.alpha);
}

TEST_CASE("TestDeclarativeDialogModifier") {
  TestPlatform platform;
  Runtime runtime{DeclarativeDialogApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "declarative dialog 1"));

  declarative_dialog_value = 2;
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(ContainsText(updated, "declarative dialog 2"));

  ClickAt(runtime, {1.0F, 1.0F}, 84);
  REQUIRE(!declarative_dialog_visible);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const FlattenedScene& hidden = runtime.BuildFrame();
  REQUIRE(!ContainsText(hidden, "declarative dialog 2"));

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "declarative dialog 2"));

  ClickAt(runtime, {1.0F, 1.0F}, 85);
  REQUIRE(!declarative_dialog_visible);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(!ContainsText(runtime.BuildFrame(), "declarative dialog 2"));

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "declarative dialog 2"));
}

TEST_CASE("TestDeclarativeDialogMotionStyleUpdatesWithoutReentering") {
  TestPlatform platform;
  Runtime runtime{DeclarativeDialogMotionApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  const std::optional<float> unanimated_opacity =
      RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog");
  REQUIRE(unanimated_opacity.has_value());
  REQUIRE(*unanimated_opacity == Catch::Approx(1.0F));

  declarative_dialog_motion_enabled = true;
  runtime.BuildFrame();
  const std::optional<float> updated_opacity =
      RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog");
  REQUIRE(updated_opacity.has_value());
  REQUIRE(*updated_opacity == Catch::Approx(1.0F));

  declarative_dialog_visible = false;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE_FALSE(RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog").has_value());

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  runtime.BuildFrame();
  const std::optional<float> entering_opacity =
      RenderedTextOpacity(*runtime.LastCommit().render_frame.scene.root, "motion dialog");
  REQUIRE(entering_opacity.has_value());
  REQUIRE(*entering_opacity < 1.0F);
}

TEST_CASE("TestDeclarativeDialogCanRemoveMotionWhileReentering") {
  TestPlatform platform;
  Runtime runtime{DeclarativeDialogMotionApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  declarative_dialog_motion_enabled = true;
  declarative_dialog_visible = true;
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "motion dialog"));

  declarative_dialog_visible = false;
  runtime.BuildFrame();
  platform.AdvanceTime(1.0);
  declarative_dialog_motion_enabled = false;
  declarative_dialog_visible = true;
  runtime.BuildFrame();
  REQUIRE(ContainsText(runtime.BuildFrame(), "motion dialog"));
}

} // namespace huxerui::test
