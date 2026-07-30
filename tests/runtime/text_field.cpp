#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

State<TextEditingValue> text_field_value;
ScrollController text_field_scroll;
std::vector<TextEditingValue> text_field_changes;
int text_field_submissions = 0;
State<TextEditingValue> multiline_text_field_value;
State<TextEditingValue> nested_multiline_text_field_value;
State<TextEditingValue> keyboard_text_field_value;
State<TextEditingValue> undo_text_field_value;
State<TextEditingValue> secure_text_field_value;
State<TextEditingValue> limited_text_field_value;
TextInputAction submission_action = TextInputAction::Default;
int first_action_submissions = 0;
int second_action_submissions = 0;

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

  void RequestShow(TextInputSessionId session_id) override {
    show_requests.push_back(session_id);
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
  std::vector<TextInputSessionId> show_requests;
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

View InvalidTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText(""));
  text_field_value = value;
  return Stack {
      TextField(value)
          .Placeholder("Email")
          .Validation(Validate(value.Get().text, Required("Email is required")))
          .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
          .With(huxerui::Frame{.width = 160.0F}),
  };
}

View ValidTextFieldApp() {
  return Stack {
      TextField(TextEditingValue::FromText(""))
          .Placeholder("Email")
          .Validation(ValidationResult::Valid())
          .With(huxerui::Frame{.width = 160.0F}),
  };
}

View PendingTextFieldApp() {
  return Stack {
      TextField(TextEditingValue::FromText(""))
          .Placeholder("Email")
          .Validation(ValidationResult::Pending("Checking"))
          .With(huxerui::Frame{.width = 160.0F}),
  };
}

View KeyboardTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("alpha beta gamma"));
  keyboard_text_field_value = value;
  return Stack{
      TextField(value)
          .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
          .With(huxerui::Frame{240.0F, 40.0F}),
  };
}

View UndoTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText(""));
  undo_text_field_value = value;
  return Stack{
      TextField(value)
          .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
          .With(huxerui::Frame{240.0F, 40.0F}),
  };
}

View UndoDeletionTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("abcd"));
  undo_text_field_value = value;
  return Stack{
      TextField(value)
          .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
          .With(huxerui::Frame{240.0F, 40.0F}),
  };
}

View SecureTextFieldApp() {
  auto value = UseState(
      TextEditingValue::FromText(
          "a\xF0\x9F\x98\x80"
          "e\xCC\x81"
      )
  );
  secure_text_field_value = value;
  return Stack{
      TextField(value)
          .Secure()
          .Placeholder("Password")
          .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
          .With(huxerui::Frame{240.0F, 40.0F}),
  };
}

View SubmissionTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText(""));
  return TextField(value)
      .InputConfiguration({.action = submission_action})
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
      .OnSubmitted([] { ++first_action_submissions; })
      .With(huxerui::Frame{240.0F, 40.0F});
}

View NextTextFieldApp() {
  auto first = UseState(TextEditingValue::FromText(""));
  auto second = UseState(TextEditingValue::FromText(""));
  return Column {
    TextField(first)
        .InputConfiguration({.action = TextInputAction::Next})
        .OnChanged([first](const TextEditingValue& changed) mutable { first = changed; })
        .OnSubmitted([] { ++first_action_submissions; })
        .With(huxerui::Frame{240.0F, 40.0F}),
    TextField(second)
        .InputConfiguration({.action = TextInputAction::Next})
        .OnChanged([second](const TextEditingValue& changed) mutable { second = changed; })
        .OnSubmitted([] { ++second_action_submissions; })
        .With(huxerui::Frame{240.0F, 40.0F}),
  };
}

View MultilineTextFieldApp() {
  auto value = UseState(TextEditingValue{
      "ab\ncd\nef",
      {0, 0},
  });
  multiline_text_field_value = value;
  return Stack {
      TextField(value)
          .LineLimits(TextFieldLineLimits::MultiLine())
          .Placeholder("Message")
          .OnChanged([value](const TextEditingValue& changed) mutable {
            text_field_changes.push_back(changed);
            value = changed;
          })
          .OnSubmitted([] { ++text_field_submissions; })
          .With(huxerui::Frame{80.0F, 56.0F}),
  };
}

View GrowingMultilineTextFieldApp() {
  TextEditingValue value{
      "abcdefgh",
      {0, 0},
  };
  return Stack {
      TextField(std::move(value))
          .LineLimits(TextFieldLineLimits::MultiLine())
          .With(huxerui::Frame{.width = 80.0F}),
  };
}

View MinimumLinesTextFieldApp() {
  return Stack {
      TextField(TextEditingValue::FromText(""))
          .LineLimits(TextFieldLineLimits::MultiLine(3))
          .With(huxerui::Frame{.width = 80.0F}),
  };
}

View MaximumLinesTextFieldApp() {
  return Stack {
      TextField(TextEditingValue::FromText("a\nb\nc\nd\ne"))
          .LineLimits(TextFieldLineLimits::MultiLine(2, 3))
          .InputConfiguration({
              .action = TextInputAction::Done,
              .multiline = true,
          })
          .With(huxerui::Frame{.width = 80.0F}),
  };
}

View FixedHeightLinesTextFieldApp() {
  return Stack {
      TextField(TextEditingValue::FromText(""))
          .LineLimits(TextFieldLineLimits::MultiLine(3, 5))
          .With(huxerui::Frame{.width = 80.0F, .height = 48.0F}),
  };
}

View LimitedTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText(""));
  limited_text_field_value = value;
  return TextField(value)
      .MaxLength(3)
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
      .With(huxerui::Frame{160.0F, 40.0F});
}

View OverLimitTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("abcd"));
  limited_text_field_value = value;
  return TextField(value)
      .MaxLength(3)
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
      .With(huxerui::Frame{160.0F, 40.0F});
}

View NestedMultilineTextFieldApp() {
  auto value = UseState(TextEditingValue{
      "aa\nbb\ncc\ndd\nee\nff",
      {0, 0},
  });
  nested_multiline_text_field_value = value;
  return ScrollView {
      Column {
          TextField(value)
              .LineLimits(TextFieldLineLimits::MultiLine())
              .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
              .With(huxerui::Frame{80.0F, 56.0F}),
          Spacer{}.With(huxerui::Frame{80.0F, 160.0F}),
      },
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
  multiline_text_field_value = {};
  nested_multiline_text_field_value = {};
  keyboard_text_field_value = {};
  undo_text_field_value = {};
  secure_text_field_value = {};
  limited_text_field_value = {};
  submission_action = TextInputAction::Default;
  first_action_submissions = 0;
  second_action_submissions = 0;
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

TEST_CASE("TestTextFieldValidationRendersSupportingMessageAndErrorBorder") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{InvalidTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  const DisplayList& display_list = runtime.BuildFrame();

  const TextFieldStyle style = TextFieldStyleKey::Default();
  const DrawTextCommand* message = FindText(display_list, "Email is required");
  REQUIRE(message != nullptr);
  REQUIRE(message->color.red == style.validation_error.red);
  REQUIRE(message->color.green == style.validation_error.green);
  REQUIRE(message->color.blue == style.validation_error.blue);
  REQUIRE(message->font_size == style.validation_font_size);
  REQUIRE(runtime.RootNode()->children.front()->frame.height == 80.0F);

  const DrawBorderCommand* border = FindBorderWithColor(display_list, style.validation_error);
  REQUIRE(border != nullptr);
  REQUIRE(border->width == style.validation_border_width);
  REQUIRE(border->rect.height == style.minimum_height);

  Pointer(runtime, PointerEventType::Down, 20.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  const DisplayList& valid_display_list = runtime.BuildFrame();
  REQUIRE(text_field_value.Get() == TextEditingValue::FromText("x"));
  REQUIRE(FindText(valid_display_list, "Email is required") == nullptr);
  REQUIRE(runtime.RootNode()->children.front()->frame.height == style.minimum_height);
}

TEST_CASE("TestInvalidTextFieldDoesNotDrawASecondFocusRingAroundSupportingMessage") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{InvalidTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
  });
  const DisplayList& display_list = runtime.BuildFrame();

  const TextFieldStyle style = TextFieldStyleKey::Default();
  REQUIRE(FindBorderWithColor(display_list, style.validation_error) != nullptr);
  REQUIRE(FindBorderWithColor(display_list, style.focused_border) == nullptr);
}

TEST_CASE("TestTextFieldDoesNotApplyGenericHoverIndication") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{InvalidTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Move,
      701,
      {20.0F, 20.0F},
      PointerDeviceKind::Mouse,
  });
  platform.AdvanceTime(FlatLightThemeSpec().motion.fast);
  const DisplayList& display_list = runtime.BuildFrame();

  REQUIRE(FindText(display_list, "Email is required") != nullptr);
  REQUIRE(FindRectWithColor(display_list, FlatLightThemeSpec().interactions.hover_overlay) == nullptr);
}

TEST_CASE("TestTextFieldValidResultDoesNotReserveSupportingSpace") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{ValidTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  const DisplayList& display_list = runtime.BuildFrame();

  REQUIRE(FindText(display_list, "Email is required") == nullptr);
  REQUIRE(runtime.RootNode()->children.front()->frame.height == TextFieldStyleKey::Default().minimum_height);
  REQUIRE(FindBorderWithColor(display_list, TextFieldStyleKey::Default().validation_error) == nullptr);
}

TEST_CASE("TestTextFieldPendingResultRendersNeutralSupportingMessage") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{PendingTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  const DisplayList& display_list = runtime.BuildFrame();

  const TextFieldStyle style = TextFieldStyleKey::Default();
  const DrawTextCommand* message = FindText(display_list, "Checking");
  REQUIRE(message != nullptr);
  REQUIRE(message->color.red == style.placeholder.red);
  REQUIRE(message->color.green == style.placeholder.green);
  REQUIRE(message->color.blue == style.placeholder.blue);
  REQUIRE(FindBorderWithColor(display_list, style.validation_error) == nullptr);
  REQUIRE(runtime.RootNode()->children.front()->frame.height == 60.0F);
}

TEST_CASE("TestSecureTextFieldMasksGraphemesAndPreservesEditingOffsets") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  clipboard.text = "paste";
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  platform.platform_text_input = &text_input;
  Runtime runtime{SecureTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  const DisplayList& display_list = runtime.BuildFrame();

  REQUIRE(FindText(display_list, "a\xF0\x9F\x98\x80"
                                 "e\xCC\x81") == nullptr);
  REQUIRE(FindText(display_list, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") != nullptr);

  Pointer(runtime, PointerEventType::Down, 230.0F);
  REQUIRE(text_input.started_configurations.back().secure);
  REQUIRE(secure_text_field_value.Get().selection == TextSelection{5, 5});
  REQUIRE(runtime.QueryTextInputGeometry(1, {5, 5}).caret.x == 40.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
  });
  REQUIRE(
      secure_text_field_value.Get() ==
      TextEditingValue::FromText(
          "a\xF0\x9F\x98\x80"
      )
  );

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(
      secure_text_field_value.Get() ==
      TextEditingValue::FromText(
          "a\xF0\x9F\x98\x80"
          "e\xCC\x81"
      )
  );

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::A,
      {},
      {.control = true},
  });
  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Cut));
  REQUIRE(runtime.CanPerformTextEditingAction(TextEditingAction::Paste));
  REQUIRE_FALSE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "paste");
}

TEST_CASE("TestSecureTextFieldRejectsMultilineConfiguration") {
  REQUIRE_THROWS_AS(
      TextField(TextEditingValue::FromText(""))
          .Secure()
          .LineLimits(TextFieldLineLimits::MultiLine()),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      TextField(TextEditingValue::FromText(""))
          .LineLimits(TextFieldLineLimits::MultiLine())
          .Secure(),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      TextField(TextEditingValue::FromText(""))
          .InputConfiguration({
              .multiline = true,
              .secure = true,
          }),
      std::invalid_argument
  );
}

TEST_CASE("TestTextFieldSubmissionActionsUseOneRuntimePath") {
  for (const TextInputAction action : {
           TextInputAction::Default,
           TextInputAction::Done,
           TextInputAction::Go,
           TextInputAction::Search,
           TextInputAction::Send,
       }) {
    ResetTextFieldState();
    submission_action = action;
    TextFieldPlatformInput text_input;
    TestPlatform platform;
    platform.platform_text_input = &text_input;
    Runtime runtime{SubmissionTextFieldApp, platform};
    runtime.SetViewport({280.0F, 80.0F});
    runtime.BuildFrame();
    Pointer(runtime, PointerEventType::Down, 20.0F);

    const TextInputAction requested = action == TextInputAction::Default ? TextInputAction::Done : action;
    REQUIRE(runtime.PerformTextInputAction(1, requested));
    REQUIRE(first_action_submissions == 1);
    REQUIRE(runtime.QueryTextInputContext(1, 0, 0).result_code == TextInputResultCode::Ok);
  }
}

TEST_CASE("TestTextFieldNextSubmitsAndMovesFocusWithoutWrapping") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{NextTextFieldApp, platform};
  runtime.SetViewport({280.0F, 100.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 20.0F);

  REQUIRE(runtime.PerformTextInputAction(1, TextInputAction::Next));
  REQUIRE(first_action_submissions == 1);
  REQUIRE(second_action_submissions == 0);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1, 2});
  REQUIRE(runtime.QueryTextInputContext(1, 0, 0).result_code == TextInputResultCode::SessionMismatch);
  REQUIRE(runtime.QueryTextInputContext(2, 0, 0).result_code == TextInputResultCode::Ok);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Enter,
      {},
      {},
      true,
  });
  REQUIRE(second_action_submissions == 0);

  REQUIRE_FALSE(runtime.PerformTextInputAction(1, TextInputAction::Next));
  REQUIRE_FALSE(runtime.PerformTextInputAction(2, TextInputAction::Done));
  REQUIRE(runtime.PerformTextInputAction(2, TextInputAction::Next));
  REQUIRE(second_action_submissions == 1);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1, 2});
  REQUIRE(runtime.QueryTextInputContext(2, 0, 0).result_code == TextInputResultCode::Ok);
}

TEST_CASE("TestTextFieldNewlineActionRequiresMultilineInput") {
  REQUIRE_THROWS_AS(
      TextField(TextEditingValue::FromText(""))
          .InputConfiguration({
              .action = TextInputAction::Newline,
          }),
      std::invalid_argument
  );
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

TEST_CASE("TestTextFieldUsesPlatformWordNavigationAndDeletion") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{KeyboardTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 230.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowLeft,
      {},
      {.control = true},
  });
  REQUIRE(keyboard_text_field_value.Get().selection == TextSelection{11, 11});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowLeft,
      {},
      {.alt = true},
  });
  REQUIRE(keyboard_text_field_value.Get().selection == TextSelection{6, 6});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowRight,
      {},
      {
          .shift = true,
          .alt = true,
      },
  });
  REQUIRE(keyboard_text_field_value.Get().selection == TextSelection{6, 10});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowRight,
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowRight,
      {},
      {.control = true},
  });
  REQUIRE(keyboard_text_field_value.Get().selection == TextSelection{11, 11});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
      {},
      {.control = true},
  });
  REQUIRE(keyboard_text_field_value.Get().text == "alpha gamma");
  REQUIRE(keyboard_text_field_value.Get().selection == TextSelection{6, 6});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Delete,
      {},
      {.alt = true},
  });
  REQUIRE(keyboard_text_field_value.Get().text == "alpha ");
  REQUIRE(keyboard_text_field_value.Get().selection == TextSelection{6, 6});
}

TEST_CASE("TestTextFieldUndoRedoMergesContinuousTyping") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  for (const char* text : {"a", "b", "c"}) {
    runtime.HandleKeyEvent({
        KeyEventType::Down,
        Key::Unknown,
        text,
    });
    platform.AdvanceTime(0.1);
  }
  runtime.BuildFrame();
  REQUIRE(undo_text_field_value.Get().text == "abc");

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText(""));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {
          .shift = true,
          .meta = true,
      },
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("abc"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.meta = true},
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Y,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("abc"));
}

TEST_CASE("TestTextFieldUndoSeparatesTimedAndRepositionedEdits") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "a",
  });
  platform.AdvanceTime(2.0);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "b",
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("a"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowLeft,
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  REQUIRE(undo_text_field_value.Get().text == "xa");

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(
      undo_text_field_value.Get() ==
      TextEditingValue{
          "a",
          {0, 0},
      }
  );

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText(""));
}

TEST_CASE("TestTextFieldUndoRedoMergesAdjacentDeletion") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{UndoDeletionTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 230.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("ab"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.meta = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("abcd"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Y,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("ab"));
}

TEST_CASE("TestTextFieldUndoTreatsCompositionAsOneEdit") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  TextInputCommand first;
  first.kind = TextInputCommandKind::UpdateComposition;
  first.text = "n";
  runtime.HandleTextInputCommands({1, {first}});

  TextInputCommand second;
  second.kind = TextInputCommandKind::UpdateComposition;
  second.text = "ni";
  runtime.HandleTextInputCommands({1, {second}});

  TextInputCommand commit;
  commit.kind = TextInputCommandKind::CommitText;
  commit.text = "你";
  runtime.HandleTextInputCommands({1, {commit}});
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("你"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText(""));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Y,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("你"));
}

TEST_CASE("TestTextFieldUndoCancelsActiveCompositionFirst") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "ni";
  runtime.HandleTextInputCommands({1, {update}});
  REQUIRE(undo_text_field_value.Get().composition == TextRange{0, 2});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText(""));
}

TEST_CASE("TestTextFieldExternalTextReplacementClearsUndoHistory") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "a",
  });
  runtime.BuildFrame();
  undo_text_field_value = TextEditingValue::FromText("server");
  runtime.BuildFrame();

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("server"));
}

TEST_CASE("TestTextFieldExternalSelectionPreservesHistoryAndNewEditsClearRedo") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "a",
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "b",
  });
  runtime.BuildFrame();

  undo_text_field_value = TextEditingValue{
      "ab",
      {0, 0},
  };
  runtime.BuildFrame();
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText(""));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Y,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("x"));
}

TEST_CASE("TestTextFieldUndoKeepsPasteSeparateFromTyping") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  clipboard.text = "p";
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{UndoTextFieldApp, platform};
  runtime.SetViewport({280.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Paste));
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("px"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText("p"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(undo_text_field_value.Get() == TextEditingValue::FromText(""));
}

TEST_CASE("TestTextFieldMaxLengthCountsGraphemeClusters") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{LimitedTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "a\xF0\x9F\x98\x80"
      "e\xCC\x81"
      "b",
  });
  REQUIRE(
      limited_text_field_value.Get() ==
      TextEditingValue::FromText(
          "a\xF0\x9F\x98\x80"
          "e\xCC\x81"
      )
  );
}

TEST_CASE("TestTextFieldMaxLengthTruncatesReplacementAndPreservesUndo") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{LimitedTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "abc",
  });
  TextInputCommand replace;
  replace.kind = TextInputCommandKind::CommitText;
  replace.target = TextRange{1, 2};
  replace.text = "xy";
  const TextInputApplyResult result = runtime.HandleTextInputCommands({1, {replace}});
  REQUIRE(result.result_code == TextInputResultCode::Ok);
  REQUIRE(result.changed);
  REQUIRE(
      limited_text_field_value.Get() ==
      TextEditingValue{
          "axc",
          {2, 2},
      }
  );

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Z,
      {},
      {.control = true},
  });
  REQUIRE(limited_text_field_value.Get() == TextEditingValue::FromText("abc"));
}

TEST_CASE("TestTextFieldMaxLengthPreservesExternalOverLimitValuesAndAllowsDeletion") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{OverLimitTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 150.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  REQUIRE(limited_text_field_value.Get() == TextEditingValue::FromText("abcd"));

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
  });
  REQUIRE(limited_text_field_value.Get() == TextEditingValue::FromText("abc"));
}

TEST_CASE("TestTextFieldMaxLengthAllowsCompositionOverflowUntilFinish") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{LimitedTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "abcd";
  REQUIRE(runtime.HandleTextInputCommands({1, {update}}).changed);
  REQUIRE(limited_text_field_value.Get().text == "abcd");
  REQUIRE(limited_text_field_value.Get().composition == TextRange{0, 4});

  TextInputCommand finish;
  finish.kind = TextInputCommandKind::FinishComposition;
  REQUIRE(runtime.HandleTextInputCommands({1, {finish}}).changed);
  REQUIRE(limited_text_field_value.Get() == TextEditingValue::FromText("abc"));
}

TEST_CASE("TestMultilineTextFieldWrapsAndGrowsWithoutAHeight") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{GrowingMultilineTextFieldApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  const DisplayList& display_list = runtime.BuildFrame();

  const DrawTextCommand* text = FindText(display_list, "abcdefgh");
  REQUIRE(text != nullptr);
  REQUIRE(text->rect.width == 60.0F);
  REQUIRE(text->rect.height == 40.0F);
  REQUIRE(std::ranges::any_of(display_list.Commands(), [](const DisplayCommand& command) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    return border != nullptr && border->rect.width == 80.0F && border->rect.height == 56.0F;
  }));
}

TEST_CASE("TestMultilineTextFieldAppliesIntrinsicLineLimits") {
  ResetTextFieldState();
  TestPlatform platform;

  Runtime minimum{MinimumLinesTextFieldApp, platform};
  minimum.SetViewport({200.0F, 120.0F});
  minimum.BuildFrame();
  REQUIRE(minimum.RootNode()->children.front()->frame.height == 76.0F);

  Runtime maximum{MaximumLinesTextFieldApp, platform};
  maximum.SetViewport({200.0F, 140.0F});
  maximum.BuildFrame();
  const auto* field = maximum.RootNode()->children.front().get();
  REQUIRE(field->frame.height == 76.0F);
  REQUIRE(field->scroll != nullptr);
  REQUIRE(field->scroll->content_height == 100.0F);
}

TEST_CASE("TestTextFieldParentHeightOverridesIntrinsicLineLimits") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{FixedHeightLinesTextFieldApp, platform};
  runtime.SetViewport({200.0F, 120.0F});
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode()->children.front()->frame.height == 48.0F);
}

TEST_CASE("TestTextFieldRejectsInvalidLineLimits") {
  REQUIRE_THROWS_AS(TextFieldLineLimits::MultiLine(0), std::invalid_argument);
  REQUIRE_THROWS_AS(TextFieldLineLimits::MultiLine(1, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(TextFieldLineLimits::MultiLine(3, 2), std::invalid_argument);
}

TEST_CASE("TestMultilineTextFieldNavigatesLinesAndKeepsCaretVisible") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);
  REQUIRE(text_input.started_configurations.size() == 1);
  REQUIRE(text_input.started_configurations.front().multiline);
  REQUIRE(text_input.started_configurations.front().action == huxerui::TextInputAction::Newline);
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{1, 1});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowDown,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{4, 4});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowDown,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{7, 7});

  runtime.BuildFrame();
  const TextInputGeometry geometry = runtime.QueryTextInputGeometry(1, {7, 7});
  REQUIRE(geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(geometry.caret.y >= 8.0F);
  REQUIRE(geometry.caret.y + geometry.caret.height <= 48.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowUp,
      {},
      {.shift = true},
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{7, 4});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowDown,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{7, 7});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Home,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{6, 6});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::End,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{8, 8});
}

TEST_CASE("TestMultilineTextFieldNavigatesLinePageAndDocumentBoundaries") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 38.0F);
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{4, 4});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowLeft,
      {},
      {.meta = true},
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{3, 3});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowRight,
      {},
      {.meta = true},
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{5, 5});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Home,
      {},
      {.control = true},
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{0, 0});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::PageDown,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{6, 6});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::PageUp,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{0, 0});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::End,
      {},
      {.control = true},
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{8, 8});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Home,
      {},
      {.control = true},
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowDown,
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowRight,
  });
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{4, 4});

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Backspace,
      {},
      {.meta = true},
  });
  REQUIRE(multiline_text_field_value.Get().text == "ab\nd\nef");
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{3, 3});
}

TEST_CASE("TestMultilineTextFieldEnterInsertsNewlineInsteadOfSubmitting") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Enter,
  });

  REQUIRE(multiline_text_field_value.Get().text == "a\nb\ncd\nef");
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{2, 2});
  REQUIRE(text_field_submissions == 0);
}

TEST_CASE("TestMultilineTextFieldWheelScrollDoesNotRevealCaretUntilEditingResumes") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);

  runtime.HandleScrollEvent({
      {20.0F, 20.0F},
      0.0F,
      20.0F,
  });
  runtime.BuildFrame();

  const auto* field = runtime.RootNode()->children.front().get();
  REQUIRE(field->scroll != nullptr);
  REQUIRE(field->scroll->offset_y == 20.0F);
  REQUIRE(runtime.QueryTextInputGeometry(1, {1, 1}).caret.y < 8.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Home,
  });
  runtime.BuildFrame();
  REQUIRE(field->scroll->offset_y == 0.0F);
}

TEST_CASE("TestMultilineTextFieldPassesRemainingWheelDeltaToParent") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{NestedMultilineTextFieldApp, platform};
  runtime.SetViewport({80.0F, 80.0F});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);
  Pointer(runtime, PointerEventType::Up, 20.0F, 18.0F);

  runtime.HandleScrollEvent({
      {40.0F, 20.0F},
      0.0F,
      100.0F,
  });

  const auto* root = runtime.RootNode();
  const auto* field = root->children.front()->children.front().get();
  REQUIRE(field->scroll != nullptr);
  REQUIRE(field->scroll->offset_y == 80.0F);
  REQUIRE(root->scroll->offset_y == 20.0F);

  runtime.BuildFrame();
  REQUIRE(field->scroll->offset_y == 80.0F);
  REQUIRE(root->scroll->offset_y == 20.0F);
}

TEST_CASE("TestMultilineTextFieldUsesTouchDragForScrollAndMouseDragForSelection") {
  ResetTextFieldState();
  TestPlatform touch_platform;
  Runtime touch{MultilineTextFieldApp, touch_platform};
  touch.SetViewport({200.0F, 100.0F});
  touch.BuildFrame();
  touch.HandlePointerEvent({
      PointerEventType::Down,
      710,
      {20.0F, 38.0F},
      PointerDeviceKind::Touch,
  });
  touch.HandlePointerEvent({
      PointerEventType::Move,
      710,
      {20.0F, 8.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(touch.RootNode()->children.front()->scroll->offset_y == 20.0F);

  ResetTextFieldState();
  TestPlatform mouse_platform;
  Runtime mouse{MultilineTextFieldApp, mouse_platform};
  mouse.SetViewport({200.0F, 100.0F});
  mouse.BuildFrame();
  mouse.HandlePointerEvent({
      PointerEventType::Down,
      711,
      {20.0F, 18.0F},
      PointerDeviceKind::Mouse,
  });
  mouse.HandlePointerEvent({
      PointerEventType::Move,
      711,
      {20.0F, 38.0F},
      PointerDeviceKind::Mouse,
  });
  REQUIRE(mouse.RootNode()->children.front()->scroll->offset_y == 0.0F);
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{1, 4});
}

TEST_CASE("TestMultilineTextFieldSelectionDragScrollsAtViewportEdge") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      712,
      {20.0F, 18.0F},
      PointerDeviceKind::Mouse,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Move,
      712,
      {20.0F, 72.0F},
      PointerDeviceKind::Mouse,
  });

  REQUIRE(runtime.RootNode()->children.front()->scroll->offset_y == 20.0F);
  REQUIRE(multiline_text_field_value.Get().selection.active > 1);
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

  const TextInputPositionResult position = runtime.QueryTextInputPosition(1, {45.0F, 20.0F});
  REQUIRE(position.result_code == TextInputResultCode::Ok);
  REQUIRE(position.position.offset == 4);
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
  touch.HandlePointerEvent({
      PointerEventType::Up,
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

TEST_CASE("TestTouchScrollOverTextFieldDoesNotFocusOrStartTextInput") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{ScrollableTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      720,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(text_input.started_sessions.empty());
  REQUIRE(text_field_value.Get().selection == TextSelection{6, 6});

  runtime.HandlePointerEvent({
      PointerEventType::Move,
      720,
      {20.0F, -20.0F},
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      720,
      {20.0F, -20.0F},
      PointerDeviceKind::Touch,
  });

  REQUIRE(text_input.started_sessions.empty());
  REQUIRE(text_input.show_requests.empty());
  REQUIRE(text_field_value.Get().selection == TextSelection{6, 6});
  REQUIRE(text_field_scroll.Metrics().offset > 0.0F);
}

TEST_CASE("TestTouchTapStartsTextInputOnReleaseAndRetapRequestsKeyboard") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{ScrollableTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      721,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(text_input.started_sessions.empty());
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      721,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.started_states.back().selection == TextSelection{1, 1});
  REQUIRE(text_input.show_requests.empty());

  platform.AdvanceTime(0.5);
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      722,
      {30.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(text_input.show_requests.empty());
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      722,
      {30.0F, 20.0F},
      PointerDeviceKind::Touch,
  });

  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.show_requests == std::vector<TextInputSessionId>{1});
  REQUIRE(text_field_value.Get().selection == TextSelection{2, 2});
}

TEST_CASE("TestTouchDragOverFocusedTextFieldDoesNotRequestKeyboard") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{ScrollableTextFieldApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      723,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      723,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      724,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Move,
      724,
      {20.0F, -20.0F},
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      724,
      {20.0F, -20.0F},
      PointerDeviceKind::Touch,
  });

  REQUIRE(text_input.show_requests.empty());
  REQUIRE(text_field_value.Get().selection == TextSelection{1, 1});
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
