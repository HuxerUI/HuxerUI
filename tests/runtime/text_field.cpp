#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

State<TextEditingValue> text_field_value;
ScrollController text_field_scroll;
std::vector<TextEditingValue> text_field_changes;
int text_field_submissions = 0;

class TextFieldPlatformInput final : public PlatformTextInput {
public:
  void Start(
      TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state
  ) override {
    started_sessions.push_back(session_id);
    started_configurations.push_back(configuration);
    started_states.push_back(state);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    updated_sessions.push_back(session_id);
    updated_states.push_back(state);
    updated_geometry.push_back(geometry);
  }

  void Restart(
      TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state
  ) override {
    restarted_sessions.push_back(session_id);
    restarted_configurations.push_back(configuration);
    restarted_states.push_back(state);
  }

  void Stop(TextInputSessionId session_id) override {
    stopped_sessions.push_back(session_id);
  }

  std::vector<TextInputSessionId> started_sessions;
  std::vector<TextInputConfiguration> started_configurations;
  std::vector<TextInputState> started_states;
  std::vector<TextInputSessionId> updated_sessions;
  std::vector<TextInputState> updated_states;
  std::vector<TextInputGeometry> updated_geometry;
  std::vector<TextInputSessionId> restarted_sessions;
  std::vector<TextInputConfiguration> restarted_configurations;
  std::vector<TextInputState> restarted_states;
  std::vector<TextInputSessionId> stopped_sessions;
};

class TextFieldClipboard final : public PlatformClipboard {
public:
  std::optional<std::string> ReadText() override {
    return text;
  }

  bool WriteText(std::string_view value) override {
    text = std::string(value);
    return true;
  }

  std::optional<std::string> text;
};

View TextFieldApp() {
  auto value = UseState(
      TextEditingValue::FromText(
          "a\xF0\x9F\x98\x80"
          "b"
      )
  );
  text_field_value = value;
  return Stack{
      TextField(value)
          .Placeholder("Name")
          .OnChanged([value](const TextEditingValue& changed) mutable {
            text_field_changes.push_back(changed);
            value = changed;
          })
          .OnSubmitted([] { ++text_field_submissions; })
          .With(huxerui::Frame{160.0F, 40.0F}),
  };
}

View EmptyTextFieldApp() {
  return Stack{
      TextField(TextEditingValue::FromText("")).Placeholder("Name").With(huxerui::Frame{160.0F, 40.0F}),
  };
}

View TextSelectionOverlayApp() {
  TextFieldStyle style = TextFieldStyleKey::Default();
  style.caret = Color::Rgb(214, 55, 48);
  ThemeDefinition definition = FlatThemeDefinition();
  definition.Set<TextFieldStyleKey>(style);
  return Theme(std::move(definition), [] {
    return ProvideEnvironment<TextSelectionMenuLabelsKey>(
        {
            .cut = "剪切",
            .copy = "复制",
            .paste = "粘贴",
            .select_all = "全选",
        },
        [] { return TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F}); }
    );
  });
}

View MaterialTextSelectionOverlayApp() {
  return MaterialTheme([] {
    return TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F});
  });
}

View ScrollableTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("abcdef"));
  auto scroll = UseScrollController();
  text_field_value = value;
  text_field_scroll = scroll;
  return ScrollView{
      Column{
          TextField(value).OnChanged(
                              [value](const TextEditingValue& changed) mutable { value = changed; }
          ).With(huxerui::Frame{160.0F, 40.0F}),
          Text("Tail").With(huxerui::Frame{160.0F, 200.0F}),
      },
  }
      .Controller(scroll);
}

View OccludedTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("visible"));
  auto scroll = UseScrollController();
  text_field_value = value;
  text_field_scroll = scroll;
  return ScrollView{
      Column{
          Spacer{}.With(huxerui::Frame{160.0F, 140.0F}),
          TextField(value).OnChanged(
                              [value](const TextEditingValue& changed) mutable { value = changed; }
          ).With(huxerui::Frame{160.0F, 40.0F}),
          Spacer{}.With(huxerui::Frame{160.0F, 140.0F}),
      },
  }
      .Controller(scroll);
}

void ResetTextFieldState() {
  text_field_value = {};
  text_field_scroll = ScrollController{};
  text_field_changes.clear();
  text_field_submissions = 0;
}

void Pointer(Runtime& runtime, PointerEventType type, float x, float y = 20.0F) {
  runtime.HandlePointerEvent({
      type,
      700,
      {x, y},
  });
}

} // namespace

TEST_CASE("TestTextFieldRendersPlaceholderAndThemeStyle") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{EmptyTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  const DisplayList& display_list = runtime.BuildFrame();

  const DrawTextCommand* placeholder = FindText(display_list, "Name");
  REQUIRE(placeholder != nullptr);
  REQUIRE(placeholder->font_size == TextFieldStyleKey::Default().font_size);
  REQUIRE(placeholder->color.alpha == TextFieldStyleKey::Default().placeholder.alpha);

  const auto border = std::ranges::find_if(display_list.Commands(), [](const DisplayCommand& command) {
    const auto* value = std::get_if<DrawBorderCommand>(&command);
    return value && value->rect.width == 160.0F && value->rect.height == 40.0F;
  });
  REQUIRE(border != display_list.Commands().end());

  const ThemeDefinition material = huxerui::MaterialThemeDefinition();
  const auto* style = std::any_cast<TextFieldStyle>(material.Values().Find(typeid(TextFieldStyleKey)));
  REQUIRE(style != nullptr);
  REQUIRE(style->minimum_height == 56.0F);
  REQUIRE(style->focused_border.red == huxerui::MaterialLightThemeSpec().colors.primary.red);
}

TEST_CASE("TestTextFieldPointerSelectionPrecedesPlatformStart") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 34.0F);

  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.started_states.front().selection == TextSelection{3, 3});
  REQUIRE(text_field_value.Get().selection == TextSelection{3, 3});
}

TEST_CASE("TestTextFieldHardwareEditingUsesTextClusters") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 34.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
  });
  REQUIRE(text_field_value.Get().text == "ab");
  REQUIRE(text_field_value.Get().selection == TextSelection{1, 1});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "!",
  });
  REQUIRE(text_field_value.Get().text == "a!b");
  REQUIRE(text_field_value.Get().selection == TextSelection{2, 2});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Enter,
  });
  REQUIRE(text_field_submissions == 1);
}

TEST_CASE("TestTextFieldClipboardShortcutsUseEditingActions") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 55.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::A,
      {},
      {.control = true},
  });
  REQUIRE(text_field_value.Get().selection == TextSelection{0, 4});
  REQUIRE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::X,
      {},
      {.control = true},
  });
  REQUIRE(
      clipboard.text == "a\xF0\x9F\x98\x80"
                        "b"
  );
  REQUIRE(text_field_value.Get().text.empty());

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::V,
      {},
      {.control = true},
  });
  REQUIRE(text_field_value.Get().text == *clipboard.text);
}

TEST_CASE("TestTextFieldDragSelectionAndGeometry") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F);
  Pointer(runtime, PointerEventType::Move, 55.0F);
  REQUIRE(text_field_value.Get().selection == TextSelection{1, 4});

  const DisplayList& display_list = runtime.BuildFrame();
  const DrawRectCommand* selection = FindRect(display_list, {20.0F, 10.0F, 30.0F, 20.0F});
  REQUIRE(selection != nullptr);
  REQUIRE(selection->color.alpha == TextFieldStyleKey::Default().selection.alpha);

  const TextInputGeometry geometry = runtime.QueryTextInputGeometry(1, {1, 4});
  REQUIRE(geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(geometry.caret.x == 50.0F);
  REQUIRE(geometry.range_rects.size() == 1);
  REQUIRE(geometry.range_rects.front().x == 20.0F);
  REQUIRE(geometry.range_rects.front().y == 10.0F);
  REQUIRE(geometry.range_rects.front().width == 30.0F);
  REQUIRE(geometry.range_rects.front().height == 20.0F);
}

TEST_CASE("TestTextFieldSelectionOverlayUsesThemeAndLocalizedLabels") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{TextSelectionOverlayApp, platform};
  runtime.SetViewport({240.0F, 120.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      701,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const DisplayList& overlay = runtime.BuildFrame();
  const DrawTextCommand* copy = FindText(overlay, "复制");
  REQUIRE(copy != nullptr);
  const std::size_t themed_handles = std::ranges::count_if(overlay.Commands(), [](const DisplayCommand& command) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    return circle != nullptr && circle->color.red == Color::Rgb(214, 55, 48).red &&
           circle->color.green == Color::Rgb(214, 55, 48).green;
  });
  REQUIRE(themed_handles == 2);

  const Point copy_center{
      copy->rect.x + copy->rect.width * 0.5F,
      copy->rect.y + copy->rect.height * 0.5F,
  };
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      702,
      copy_center,
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      702,
      copy_center,
      PointerDeviceKind::Touch,
  });
  REQUIRE(clipboard.text == "alpha");

  const DisplayList& feedback = runtime.BuildFrame();
  REQUIRE(FindText(feedback, "复制") != nullptr);
  REQUIRE(FindRectWithColor(feedback, FlatLightThemeSpec().interactions.pressed_overlay) != nullptr);

  platform.AdvanceTime(0.3);
  const DisplayList& dismissed = runtime.BuildFrame();
  REQUIRE(FindText(dismissed, "复制") == nullptr);
  REQUIRE(std::ranges::none_of(dismissed.Commands(), [](const DisplayCommand& command) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    return circle != nullptr && circle->color.red == Color::Rgb(214, 55, 48).red &&
           circle->color.green == Color::Rgb(214, 55, 48).green;
  }));
}

TEST_CASE("TestEmptyTextFieldLongPressShowsPasteAtCaret") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  clipboard.text = "pasted";
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{EmptyTextFieldApp, platform};
  runtime.SetViewport({240.0F, 120.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      703,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const DisplayList& overlay = runtime.BuildFrame();
  const DrawTextCommand* paste = FindText(overlay, "Paste");
  REQUIRE(paste != nullptr);
  REQUIRE(std::ranges::none_of(overlay.Commands(), [](const DisplayCommand& command) {
    return std::holds_alternative<huxerui::DrawCircleCommand>(command);
  }));

  const Point paste_center{
      paste->rect.x + paste->rect.width * 0.5F,
      paste->rect.y + paste->rect.height * 0.5F,
  };
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      704,
      paste_center,
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      704,
      paste_center,
      PointerDeviceKind::Touch,
  });
  REQUIRE(runtime.QueryTextInputContext(1, 0, 6).text == "pasted");
}

TEST_CASE("TestMaterialTextSelectionMenuKeepsRippleThroughDismissal") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{MaterialTextSelectionOverlayApp, platform};
  runtime.SetViewport({240.0F, 120.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      705,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const DrawTextCommand* copy = FindText(runtime.BuildFrame(), "Copy");
  REQUIRE(copy != nullptr);
  const Point copy_center{
      copy->rect.x + copy->rect.width * 0.5F,
      copy->rect.y + copy->rect.height * 0.5F,
  };
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      706,
      copy_center,
      PointerDeviceKind::Touch,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.05);
  const DisplayList& pressed = runtime.BuildFrame();
  const ThemeSpec material = MaterialLightThemeSpec();
  Color expected_ripple = material.colors.on_surface;
  expected_ripple.alpha = material.interactions.ripple.alpha;
  REQUIRE(std::ranges::any_of(pressed.Commands(), [expected_ripple](const DisplayCommand& command) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    return circle != nullptr && circle->radius > 0.0F && circle->color.red == expected_ripple.red &&
           circle->color.green == expected_ripple.green && circle->color.blue == expected_ripple.blue &&
           circle->color.alpha == expected_ripple.alpha;
  }));

  runtime.HandlePointerEvent({
      PointerEventType::Up,
      706,
      copy_center,
      PointerDeviceKind::Touch,
  });
  REQUIRE(clipboard.text == "alpha");
  runtime.BuildFrame();
  platform.AdvanceTime(0.05);
  const DisplayList& released = runtime.BuildFrame();
  REQUIRE(FindText(released, "Copy") != nullptr);
  REQUIRE(std::ranges::any_of(released.Commands(), [expected_ripple](const DisplayCommand& command) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    return circle != nullptr && circle->color.red == expected_ripple.red &&
           circle->color.green == expected_ripple.green && circle->color.blue == expected_ripple.blue &&
           circle->color.alpha > 0.0F && circle->color.alpha < expected_ripple.alpha;
  }));

  platform.AdvanceTime(0.3);
  REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
}

TEST_CASE("TestTextFieldDoubleClickAndDoubleTapSelectWords") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime mouse{TextSelectionOverlayApp, platform};
  mouse.SetViewport({240.0F, 120.0F});
  mouse.BuildFrame();
  mouse.HandlePointerEvent({
      PointerEventType::Down,
      707,
      {70.0F, 20.0F},
      PointerDeviceKind::Mouse,
      2,
  });
  REQUIRE(mouse.QueryTextInputContext(1, 0, 10).selection == TextSelection{6, 10});
  REQUIRE(FindText(mouse.BuildFrame(), "复制") == nullptr);

  TextFieldClipboard touch_clipboard;
  TestPlatform touch_platform;
  touch_platform.platform_clipboard = &touch_clipboard;
  Runtime touch{TextSelectionOverlayApp, touch_platform};
  touch.SetViewport({240.0F, 120.0F});
  touch.BuildFrame();
  touch.HandlePointerEvent({
      PointerEventType::Down,
      708,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  touch.HandlePointerEvent({
      PointerEventType::Up,
      708,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  touch_platform.AdvanceTime(0.2);
  touch.HandlePointerEvent({
      PointerEventType::Down,
      709,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(touch.QueryTextInputContext(1, 0, 10).selection == TextSelection{0, 5});
  REQUIRE(FindText(touch.BuildFrame(), "复制") != nullptr);
}

TEST_CASE("TestTextFieldImeCommandsAndAuthoritativeReplacement") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 55.0F);

  TextInputCommand begin;
  begin.kind = TextInputCommandKind::BeginComposition;
  begin.target = TextRange{4, 4};
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "x";
  update.selection_after = TextSelection{5, 5};
  const TextInputApplyResult applied = runtime.HandleTextInputCommands({
      1,
      {begin, update},
  });
  REQUIRE(applied.result_code == TextInputResultCode::Ok);
  REQUIRE(applied.changed);
  REQUIRE(
      text_field_value.Get().text == "a\xF0\x9F\x98\x80"
                                     "bx"
  );
  REQUIRE(text_field_value.Get().composition == TextRange{4, 5});

  runtime.BuildFrame();
  REQUIRE(text_input.restarted_sessions.empty());
  REQUIRE(runtime.QueryTextInputContext(1, 0, 5).composition == TextRange{4, 5});

  text_field_value = TextEditingValue::FromText("server");
  runtime.BuildFrame();
  REQUIRE(text_input.restarted_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.restarted_states.back().selection == TextSelection{6, 6});
  REQUIRE(runtime.QueryTextInputContext(1, 0, 6).text == "server");
}

TEST_CASE("TestTextFieldPointerSelectionYieldsToParentScroll") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{ScrollableTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F);
  Pointer(runtime, PointerEventType::Move, 20.0F, -20.0F);
  Pointer(runtime, PointerEventType::Move, 55.0F, -40.0F);
  runtime.BuildFrame();

  REQUIRE(text_field_value.Get().selection == TextSelection{1, 1});
  REQUIRE(text_field_scroll.Metrics().offset > 0.0F);
}

TEST_CASE("TestTextFieldScrollsIntoReducedViewport") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{OccludedTextFieldApp, platform};
  runtime.SetViewport({200.0F, 200.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F, 160.0F);
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  REQUIRE(text_field_scroll.Metrics().offset > 0.0F);
  const TextInputGeometry geometry = runtime.QueryTextInputGeometry(1, {7, 7});
  REQUIRE(geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(geometry.caret.y + geometry.caret.height <= 72.0F);
}

} // namespace huxerui::test
