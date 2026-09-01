#include "runtime_test_support.h"
#include "image_test_support.h"
#include "path_internal.h"

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
State<TextEditingValue> single_line_scroll_value;
State<int> text_field_recompose_trigger;
State<bool> text_field_offset;
State<bool> text_field_dark_theme;
TextInputAction submission_action = TextInputAction::Default;
int first_action_submissions = 0;
int second_action_submissions = 0;

class TextFieldPlatformInput final : public PlatformTextInput {
public:
  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    started_sessions.push_back(session_id);
    started_configurations.push_back(configuration);
    started_states.push_back(state);
    started_geometry.push_back(geometry);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    updated_sessions.push_back(session_id);
    updated_states.push_back(state);
    updated_geometry.push_back(geometry);
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    restarted_sessions.push_back(session_id);
    restarted_configurations.push_back(configuration);
    restarted_states.push_back(state);
    restarted_geometry.push_back(geometry);
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
  std::vector<TextInputGeometry> started_geometry;
  std::vector<TextInputSessionId> updated_sessions;
  std::vector<TextInputState> updated_states;
  std::vector<TextInputGeometry> updated_geometry;
  std::vector<TextInputSessionId> restarted_sessions;
  std::vector<TextInputConfiguration> restarted_configurations;
  std::vector<TextInputState> restarted_states;
  std::vector<TextInputGeometry> restarted_geometry;
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

class RecordingTextLayoutPlatform final : public TestPlatform {
public:
  std::unique_ptr<detail::TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    if (text == "Centered value that overflows the editor" && options.align == TextAlign::Center) {
      centered_layout_width = max_width;
    }
    return TestPlatform::CreateTextLayout(text, style, max_width, options);
  }

  float centered_layout_width = std::numeric_limits<float>::infinity();
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
  return Stack {
      TextField(TextEditingValue::FromText("")).Placeholder("Name").With(huxerui::Frame{160.0F, 40.0F}),
  };
}

View ScrollableSingleLineTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("012345678901234567890123456789"));
  single_line_scroll_value = value;
  return TextField(value)
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
      .With(huxerui::Frame{100.0F, 40.0F});
}

View IndependentPlaceholderFontApp() {
  TextFieldStyle style = TextFieldStyle::Default();
  style.text_style.font = Font::System(18.0F);
  style.placeholder_style.font = Font::Monospace(11.0F).WithWeight(FontWeight::Bold);
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    TextField(TextEditingValue::FromText("")).Placeholder("Independent").With(huxerui::Frame{160.0F, 40.0F}),
  };
}

View DefaultVariantTextFieldStyleApp() {
  TextFieldStyle style = TextFieldStyle::Default();
  style.variant = TextFieldVariant::Filled;
  style.filled.background = Color::Rgb(38, 50, 56);
  style.filled.minimum_height = 52.0F;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    Column {
      TextField(TextEditingValue::FromText("Value")).With(huxerui::Frame{.width = 160.0F}),
    },
  };
}

View StableTextFieldApp() {
  auto trigger = UseState(0);
  text_field_recompose_trigger = trigger;
  static_cast<void>(trigger.Get());
  return TextField(TextEditingValue::FromText("stable")).With(huxerui::Frame{160.0F, 40.0F});
}

View OffsetTextFieldApp() {
  auto offset = UseState(false);
  text_field_offset = offset;
  return Stack{
      TextField(TextEditingValue::FromText("moving"))
          .With(huxerui::Frame{160.0F, 40.0F}, Offset{Point{offset.Get() ? 40.0F : 0.0F, 0.0F}}),
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

View MaterialSecureInvalidTextFieldApp() {
  return MaterialTheme {
    Column {
      TextField(TextEditingValue::FromText(""))
          .Secure()
          .Placeholder("Password")
          .Validation(ValidationResult::Invalid("Password is required"))
          .With(huxerui::Frame{.width = 160.0F}),
    },
  };
}

View MaterialEmptyTextFieldApp() {
  return MaterialTheme {
    TextField(TextEditingValue::FromText(""))
        .Placeholder("Material placeholder")
        .With(huxerui::Frame{.width = 160.0F}),
  };
}

View MaterialLabeledTextFieldApp() {
  return MaterialTheme {
    Column {
      TextField(TextEditingValue::FromText(""))
          .Label("Email")
          .Placeholder("name@example.com")
          .With(huxerui::Frame{.width = 160.0F}),
    },
  };
}

View MaterialOutlinedLabeledTextFieldApp() {
  return MaterialTheme {
    Column {
      TextField(TextEditingValue::FromText("Value"))
          .Label("Email")
          .Variant(TextFieldVariant::Outlined)
          .With(huxerui::Frame{.width = 160.0F}),
    },
  };
}

View MaterialStandardLabeledTextFieldApp() {
  return MaterialTheme {
    Column {
      TextField(TextEditingValue::FromText("Value"))
          .Label("Email")
          .Variant(TextFieldVariant::Standard)
          .With(huxerui::Frame{.width = 160.0F}),
    },
  };
}

ImageAsset TextFieldRasterIcon() {
  static const ImageAsset icon = ImageAsset::FromEncoded(MakeTestPng(20, 10));
  return icon;
}

VectorAsset TextFieldVectorIcon() {
  static const VectorAsset icon = VectorAsset::Create({10.0F, 10.0F}, [](VectorBuilder& builder) {
    Path path;
    path.MoveTo({1.0F, 1.0F}).LineTo({9.0F, 5.0F}).LineTo({1.0F, 9.0F}).Close();
    builder.FillPath(std::move(path), Color::Black());
  });
  return icon;
}

View FlatLabeledIconTextFieldApp() {
  return Column {
    TextField(TextEditingValue::FromText("Value"))
        .Label("Account")
        .Placeholder("Placeholder")
        .LeadingIcon(TextFieldRasterIcon())
        .TrailingIcon(TextFieldVectorIcon())
        .With(huxerui::Frame{.width = 180.0F}),
  };
}

View FlatLabeledTextFieldApp() {
  return Column {
    TextField(TextEditingValue::FromText("Value"))
        .Label("Account")
        .With(huxerui::Frame{.width = 180.0F}),
  };
}

View FlatFilledTextFieldApp() {
  return Column {
    TextField(TextEditingValue::FromText("Value"))
        .Label("Account")
        .Variant(TextFieldVariant::Filled)
        .With(huxerui::Frame{.width = 180.0F}),
  };
}

View FlatHiddenVisualLabelTextFieldApp() {
  TextFieldStyle style = TextFieldStyle::Default();
  style.variant = TextFieldVariant::Outlined;
  style.show_label = false;
  style.outlined.background = Color::Rgb(248, 249, 250);
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    TextField(TextEditingValue::FromText(""))
        .Label("Search mail")
        .Placeholder("Sender or subject")
        .LeadingIcon(TextFieldVectorIcon())
        .With(huxerui::Frame{.width = 180.0F}),
  };
}

View MaterialInvalidIconTextFieldApp() {
  return MaterialTheme {
    TextField(TextEditingValue::FromText("Value"))
        .Label("Account")
        .LeadingIcon(TextFieldVectorIcon())
        .TrailingIcon(TextFieldVectorIcon())
        .Validation(ValidationResult::Invalid("Invalid account"))
        .With(huxerui::Frame{.width = 180.0F}),
  };
}

View ReducedMotionMaterialLabeledTextFieldApp() {
  ThemeSpec theme = MaterialLightThemeSpec();
  theme.motion.reduced_motion = true;
  return MaterialTheme {
    std::move(theme),
    TextField(TextEditingValue::FromText(""))
        .Label("Email")
        .Placeholder("name@example.com")
        .With(huxerui::Frame{.width = 160.0F}),
  };
}

View MaterialDisabledTextFieldApp() {
  return MaterialTheme {
    TextField(TextEditingValue::FromText(""))
        .Placeholder("Disabled placeholder")
        .Validation(ValidationResult::Pending("Disabled supporting"))
        .With(huxerui::Frame{.width = 160.0F}, Enabled{false}),
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
  TextFieldStyle style = TextFieldStyle::Default();
  style.caret = Color::Rgb(214, 55, 48);
  ThemeDefinition definition = FlatThemeDefinition();
  definition.Set(style);
  return Theme {
    std::move(definition),
    ProvideEnvironment(
        TextSelectionMenuLabels{
            .cut = "剪切",
            .copy = "复制",
            .paste = "粘贴",
            .select_all = "全选",
        },
        TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F})
    ),
  };
}

View MaterialTextSelectionOverlayApp() {
  return MaterialTheme {
    TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F}),
  };
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
  single_line_scroll_value = {};
  text_field_recompose_trigger = State<int>{};
  text_field_offset = State<bool>{};
  text_field_dark_theme = State<bool>{};
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

const RenderNode* FindRenderNodeById(const RenderNode& node, std::uint64_t id) {
  if (node.id == id) {
    return &node;
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      if (const RenderNode* found = FindRenderNodeById(*child, id)) {
        return found;
      }
    }
  }
  return nullptr;
}

View MaterialDisabledSelectedTextFieldApp() {
  TextEditingValue value = TextEditingValue::FromText("Selected text");
  value.selection = {0, 8};
  return MaterialTheme {TextField(std::move(value)).With(huxerui::Frame{.width = 160.0F}, Enabled{false})};
}

View ThemeSwitchTextFieldApp() {
  auto dark = UseState(false);
  auto value = UseState(TextEditingValue::FromText("theme"));
  text_field_dark_theme = dark;
  text_field_value = value;
  View field = TextField(value)
                   .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
                   .With(huxerui::Frame{.width = 160.0F});
  if (dark.Get()) {
    return MaterialDarkTheme {std::move(field)};
  }
  return MaterialTheme {std::move(field)};
}

const detail::MountedNode* FindMountedNodeKind(const detail::MountedNode& node, detail::NodeKind kind) {
  if (node.kind == kind) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (child) {
      if (const detail::MountedNode* found = FindMountedNodeKind(*child, kind)) {
        return found;
      }
    }
  }
  return nullptr;
}

const DrawRectCommand* FindTextFieldIndicator(
    const FlattenedScene& scene,
    Color color,
    float width,
    float height
) {
  for (const PaintCommand& command : scene.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && BrushIsColor(rect->brush, color) && rect->rect.width == width && rect->rect.height == height) {
      return rect;
    }
  }
  return nullptr;
}

const DrawRectCommand* FindTextFieldCaret(const FlattenedScene& scene, Color color, float width) {
  for (const PaintCommand& command : scene.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && BrushIsColor(rect->brush, color) && rect->rect.width == width && rect->rect.height > width) {
      return rect;
    }
  }
  return nullptr;
}

const StrokePathCommand* FindTextFieldOutline(const FlattenedScene& scene, Color color, float width) {
  for (const PaintCommand& command : scene.Commands()) {
    const auto* path = std::get_if<StrokePathCommand>(&command);
    if (path && BrushIsColor(path->brush, color) && path->style.width == width) {
      return path;
    }
  }
  return nullptr;
}

template <TextVerticalAlign vertical_align>
View AlignedPlaceholderApp() {
  return TextField(TextEditingValue::FromText(""))
      .Placeholder("Aligned placeholder")
      .Align(TextAlign::Trailing)
      .VerticalAlign(vertical_align)
      .With(huxerui::Frame{160.0F, 80.0F});
}

View DefaultAlignedTextFieldsApp() {
  return Column {
    TextField(TextEditingValue::FromText("")).Placeholder("Single line"),
    TextField(TextEditingValue::FromText(""))
        .Placeholder("Multiple lines")
        .LineLimits(TextFieldLineLimits::MultiLine(2)),
  };
}

View CenterAlignedTextFieldApp() {
  return TextField(TextEditingValue::FromText("Centered value that overflows the editor"))
      .Align(TextAlign::Center)
      .With(huxerui::Frame{160.0F, 56.0F});
}

} // namespace

TEST_CASE("TestTextFieldRendersPlaceholderAndThemeStyle") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{EmptyTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const DrawTextCommand* placeholder = FindText(scene, "Name");
  REQUIRE(placeholder != nullptr);
  REQUIRE(placeholder->style.font == TextFieldStyle::Default().placeholder_style.font);
  REQUIRE(
      placeholder->style.foreground.alpha == TextFieldStyle::Default().placeholder_style.foreground.alpha
  );

  const TextFieldStyle flat_style = TextFieldStyle::Default();
  REQUIRE(flat_style.variant == TextFieldVariant::Standard);
  REQUIRE(flat_style.caret_width == 2.0F);
  REQUIRE(FindTextFieldIndicator(scene, flat_style.standard.border, 160.0F, flat_style.border_width) != nullptr);

  const ThemeDefinition material = huxerui::MaterialThemeDefinition();
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(material);
  const ThemeSpec material_theme = huxerui::MaterialLightThemeSpec();
  REQUIRE(style.variant == TextFieldVariant::Filled);
  REQUIRE(style.standard.minimum_height == 56.0F);
  REQUIRE(style.filled.minimum_height == 56.0F);
  REQUIRE(style.outlined.minimum_height == 56.0F);
  REQUIRE(style.filled.border == material_theme.colors.on_surface_variant);
  REQUIRE(style.outlined.border == material_theme.colors.outline);
  REQUIRE(style.padding == EdgeInsets::All(16.0F));
  REQUIRE(style.selection.alpha == 0.4F);
  REQUIRE(style.caret_width == 2.0F);
  REQUIRE(style.leading_icon_size == 24.0F);
  REQUIRE(style.trailing_icon_size == 24.0F);
  REQUIRE(style.filled.disabled_background.has_value());
  Color disabled_background = material_theme.colors.on_surface;
  disabled_background.alpha *= 0.04F;
  REQUIRE(*style.filled.disabled_background == disabled_background);
  REQUIRE(style.error_leading_icon == material_theme.colors.on_surface_variant);
  REQUIRE(style.error_trailing_icon == material_theme.colors.error);
  REQUIRE(style.border_width == 1.0F);
  REQUIRE(style.validation_border_width == 1.0F);
  REQUIRE(style.focused_validation_border_width == 2.0F);
  REQUIRE(style.filled.focused_border == material_theme.colors.primary);
}

TEST_CASE("TextFieldAlignmentUsesOneLayoutContractForPlaceholderGeometry") {
  const auto render = [](RootFactory factory) {
    TestPlatform platform;
    Runtime runtime{factory, platform};
    runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
    const DrawTextCommand* placeholder = FindText(runtime.BuildFrame(), "Aligned placeholder");
    REQUIRE(placeholder != nullptr);
    return *placeholder;
  };

  const DrawTextCommand top = render(AlignedPlaceholderApp<TextVerticalAlign::Top>);
  const DrawTextCommand center = render(AlignedPlaceholderApp<TextVerticalAlign::Center>);
  const DrawTextCommand bottom = render(AlignedPlaceholderApp<TextVerticalAlign::Bottom>);
  REQUIRE(top.options.align == TextAlign::Trailing);
  REQUIRE(top.options.vertical_align == TextVerticalAlign::Top);
  REQUIRE(center.options.vertical_align == TextVerticalAlign::Center);
  REQUIRE(bottom.options.vertical_align == TextVerticalAlign::Bottom);
  REQUIRE(top.rect.y < center.rect.y);
  REQUIRE(center.rect.y < bottom.rect.y);
}

TEST_CASE("TextFieldDefaultsSingleLineToCenterAndMultilineToTop") {
  TestPlatform platform;
  Runtime runtime{DefaultAlignedTextFieldsApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 200.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const DrawTextCommand* single_line = FindText(scene, "Single line");
  const DrawTextCommand* multiline = FindText(scene, "Multiple lines");
  REQUIRE(single_line != nullptr);
  REQUIRE(multiline != nullptr);
  REQUIRE(single_line->options.vertical_align == TextVerticalAlign::Center);
  REQUIRE(single_line->options.wrap == TextWrap::NoWrap);
  REQUIRE(multiline->options.vertical_align == TextVerticalAlign::Top);
  REQUIRE(multiline->options.wrap == TextWrap::Word);
}

TEST_CASE("TextFieldUsesTheBoundedEditorWidthForAlignedSingleLineGeometry") {
  RecordingTextLayoutPlatform platform;
  Runtime runtime{CenterAlignedTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const DrawTextCommand* text = FindText(scene, "Centered value that overflows the editor");

  REQUIRE(std::isfinite(platform.centered_layout_width));
  REQUIRE(platform.centered_layout_width > 0.0F);
  REQUIRE(platform.centered_layout_width < 200.0F);
  REQUIRE(text != nullptr);
  REQUIRE(text->rect.width == Catch::Approx(platform.centered_layout_width));
  REQUIRE(text->paragraph_offset.x < 0.0F);
}

TEST_CASE("TestTextFieldUsesThemeCaretWidth") {
  TestPlatform flat_platform;
  Runtime flat{EmptyTextFieldApp, flat_platform};
  flat.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  flat.BuildFrame();
  Pointer(flat, PointerEventType::Down, 20.0F);
  const TextFieldStyle flat_style = TextFieldStyle::Default();
  REQUIRE(FindTextFieldCaret(flat.BuildFrame(), flat_style.caret, flat_style.caret_width) != nullptr);

  TestPlatform material_platform;
  Runtime material{MaterialLabeledTextFieldApp, material_platform};
  material.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  material.BuildFrame();
  Pointer(material, PointerEventType::Down, 40.0F);
  const TextFieldStyle material_style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());
  REQUIRE(FindTextFieldCaret(material.BuildFrame(), material_style.caret, material_style.caret_width) != nullptr);
}

TEST_CASE("TestTextFieldPreservesIndependentPlaceholderFont") {
  TestPlatform platform;
  Runtime runtime{IndependentPlaceholderFontApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const DrawTextCommand* placeholder = FindText(scene, "Independent");
  REQUIRE(placeholder != nullptr);
  REQUIRE(placeholder->style.font.FamilyKind() == FontFamilyKind::Monospace);
  REQUIRE(placeholder->style.font.Size() == 11.0F);
  REQUIRE(placeholder->style.font.Weight() == FontWeight::Bold);
}

TEST_CASE("TestTextFieldResolvesThemeDefaultVariantStyle") {
  TestPlatform platform;
  Runtime runtime{DefaultVariantTextFieldStyleApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->bounds.height == 52.0F);
  REQUIRE(field->properties.corner_radii == CornerRadii::Top(TextFieldStyle::Default().corner_radius));
  REQUIRE(FindPresentedRectWithColor(scene, Color::Rgb(38, 50, 56), Size{160.0F, 52.0F}).has_value());
}

TEST_CASE("TestMaterialTextFieldFloatsLabelAndRevealsPlaceholderOnFocus") {
  TestPlatform platform;
  Runtime runtime{MaterialLabeledTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());

  const FlattenedScene& resting = runtime.BuildFrame();
  const DrawTextCommand* expanded_label = FindText(resting, "Email");
  REQUIRE(expanded_label != nullptr);
  REQUIRE(expanded_label->style.font == style.label_style.font);
  REQUIRE(FindText(resting, "name@example.com") == nullptr);
  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->bounds.height == style.filled.minimum_height);
  REQUIRE(field->properties.corner_radii == CornerRadii::Top(style.corner_radius));
  REQUIRE(FindPresentedRectWithColor(resting, style.filled.background, Size{160.0F, 56.0F}).has_value());
  REQUIRE(FindTextFieldIndicator(resting, style.filled.border, 160.0F, style.border_width) != nullptr);
  REQUIRE(FindBorderWithColor(resting, style.filled.border) == nullptr);
  const float expanded_label_y = expanded_label->rect.y;

  Pointer(runtime, PointerEventType::Down, 40.0F);
  runtime.BuildFrame();
  platform.AdvanceTime(style.label_animation_duration * 0.5);
  const DrawTextCommand* moving_label = FindText(runtime.BuildFrame(), "Email");
  REQUIRE(moving_label != nullptr);
  REQUIRE(moving_label->rect.y < expanded_label_y);
  REQUIRE(moving_label->style.font.Size() < style.label_style.font.Size());
  REQUIRE(moving_label->style.font.Size() > style.floating_label_style.font.Size());

  platform.AdvanceTime(style.label_animation_duration * 0.5);
  const FlattenedScene& focused = runtime.BuildFrame();
  const DrawTextCommand* floating_label = FindText(focused, "Email");
  const DrawTextCommand* placeholder = FindText(focused, "name@example.com");
  REQUIRE(floating_label != nullptr);
  REQUIRE(placeholder != nullptr);
  REQUIRE(floating_label->style.font == style.floating_label_style.font);
  REQUIRE(floating_label->style.foreground == style.focused_label);
  REQUIRE(placeholder->style.font == style.placeholder_style.font);
  REQUIRE(FindTextFieldIndicator(focused, style.filled.focused_border, 160.0F, style.focused_border_width) != nullptr);
  REQUIRE(FindBorderWithColor(focused, style.filled.focused_border) == nullptr);
}

TEST_CASE("TestReducedMotionTextFieldFloatsLabelWithoutTransition") {
  TestPlatform platform;
  Runtime runtime{ReducedMotionMaterialLabeledTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  ThemeSpec theme = MaterialLightThemeSpec();
  theme.motion.reduced_motion = true;
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition(theme));

  const DrawTextCommand* resting_label = FindText(runtime.BuildFrame(), "Email");
  REQUIRE(resting_label != nullptr);
  REQUIRE(resting_label->style.font == style.label_style.font);

  Pointer(runtime, PointerEventType::Down, 40.0F);
  const FlattenedScene& focused = runtime.BuildFrame();
  const DrawTextCommand* floating_label = FindText(focused, "Email");
  REQUIRE(floating_label != nullptr);
  REQUIRE(floating_label->style.font == style.floating_label_style.font);
  REQUIRE(FindText(focused, "name@example.com") != nullptr);
}

TEST_CASE("TestMaterialTextFieldSupportsOutlinedVariant") {
  TestPlatform platform;
  Runtime runtime{MaterialOutlinedLabeledTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());

  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->bounds.height == style.outlined.minimum_height);
  REQUIRE(field->properties.corner_radii == CornerRadii(style.corner_radius));
  REQUIRE(FindRectWithColor(scene, style.filled.background) == nullptr);
  const DrawTextCommand* label = FindText(scene, "Email");
  const StrokePathCommand* outline = FindTextFieldOutline(scene, style.outlined.border, style.border_width);
  REQUIRE(label != nullptr);
  REQUIRE(outline != nullptr);
  const auto elements = detail::PathAccess::Elements(outline->path);
  REQUIRE(elements.size() >= 2);
  REQUIRE(elements.front().verb == detail::PathVerb::MoveTo);
  REQUIRE(elements.back().verb == detail::PathVerb::LineTo);
  REQUIRE(elements.front().points[0].x > label->rect.x + label->rect.width);
  REQUIRE(elements.back().points[0].x < label->rect.x);
}

TEST_CASE("TestMaterialTextFieldSupportsStandardVariant") {
  TestPlatform platform;
  Runtime runtime{MaterialStandardLabeledTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());

  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->bounds.height == style.standard.minimum_height);
  REQUIRE(field->properties.corner_radii == CornerRadii{});
  REQUIRE_FALSE(FindPresentedRectWithColor(scene, style.filled.background).has_value());
  REQUIRE(FindTextFieldIndicator(scene, style.standard.border, 160.0F, style.border_width) != nullptr);
  REQUIRE(FindBorderWithColor(scene, style.standard.border) == nullptr);
}

TEST_CASE("TestFlatTextFieldLaysOutAndTintsDecorativeIcons") {
  TestPlatform platform;
  Runtime runtime{FlatLabeledIconTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const TextFieldStyle style = TextFieldStyle::Default();

  const DrawTextCommand* label = FindText(scene, "Account");
  const DrawTextCommand* text = FindText(scene, "Value");
  REQUIRE(label != nullptr);
  REQUIRE(text != nullptr);
  REQUIRE(label->style.font == style.floating_label_style.font);
  REQUIRE(label->rect.y + label->rect.height <= text->rect.y);
  REQUIRE(FindText(scene, "Placeholder") == nullptr);
  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->bounds.height == style.standard.minimum_height);
  REQUIRE(field->properties.corner_radii == CornerRadii{});

  const auto raster = std::ranges::find_if(scene.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<DrawImageCommand>(command);
  });
  REQUIRE(raster != scene.Commands().end());
  const DrawImageCommand& raster_command = std::get<DrawImageCommand>(*raster);
  REQUIRE(raster_command.destination.width == style.leading_icon_size);
  REQUIRE(raster_command.destination.height == style.leading_icon_size * 0.5F);
  REQUIRE(text->rect.x >= raster_command.destination.x + raster_command.destination.width + style.icon_spacing);
  REQUIRE(std::ranges::any_of(scene.Commands(), [&style](const PaintCommand& command) {
    const auto* path = std::get_if<FillPathCommand>(&command);
    return path && BrushIsColor(path->brush, style.trailing_icon);
  }));
  REQUIRE(FindTextFieldIndicator(scene, style.standard.border, 180.0F, style.border_width) != nullptr);
  REQUIRE(FindBorderWithColor(scene, style.standard.border) == nullptr);
}

TEST_CASE("TestTextFieldCanHideItsVisualLabelWithoutDroppingSemantics") {
  TestPlatform platform;
  Runtime runtime{FlatHiddenVisualLabelTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const TextFieldStyle style = TextFieldStyle::Default();

  REQUIRE(FindText(scene, "Search mail") == nullptr);
  REQUIRE(FindText(scene, "Sender or subject") != nullptr);
  REQUIRE(FindBorderWithColor(scene, style.outlined.border) != nullptr);
  REQUIRE(FindTextFieldOutline(scene, style.outlined.border, style.border_width) == nullptr);

  const detail::MountedNode* mounted = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(mounted != nullptr);
  Rect content = mounted->ContentBounds();
  const float leading_slot = style.leading_icon_size + style.icon_spacing;
  content.x += leading_slot;
  content.width -= leading_slot;
  REQUIRE(std::ranges::any_of(scene.Commands(), [mounted, &style](const PaintCommand& command) {
    const auto* clip = std::get_if<PushClipCommand>(&command);
    return clip && clip->rect == mounted->bounds && clip->corner_radius == style.corner_radius;
  }));
  REQUIRE(std::ranges::any_of(scene.Commands(), [content](const PaintCommand& command) {
    const auto* clip = std::get_if<PushClipCommand>(&command);
    return clip && clip->rect == content && clip->corner_radius == 0.0F;
  }));

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const auto field = std::ranges::find(frame->nodes, SemanticRole::TextField, &SemanticNode::role);
  REQUIRE(field != frame->nodes.end());
  REQUIRE(field->label == "Search mail");
}

TEST_CASE("TestTextFieldIconsOffsetSharedInputGeometry") {
  TextFieldPlatformInput plain_text_input;
  TestPlatform plain_platform;
  plain_platform.platform_text_input = &plain_text_input;
  Runtime plain{FlatLabeledTextFieldApp, plain_platform};
  plain.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  plain.BuildFrame();
  Pointer(plain, PointerEventType::Down, 50.0F);
  const TextInputGeometry plain_geometry = plain.QueryTextInputGeometry(1, {0, 1});

  TextFieldPlatformInput icon_text_input;
  TestPlatform icon_platform;
  icon_platform.platform_text_input = &icon_text_input;
  Runtime icons{FlatLabeledIconTextFieldApp, icon_platform};
  icons.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  icons.BuildFrame();
  Pointer(icons, PointerEventType::Down, 50.0F);
  const TextInputGeometry icon_geometry = icons.QueryTextInputGeometry(1, {0, 1});

  REQUIRE(plain_geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(icon_geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(plain_geometry.range_rects.size() == 1);
  REQUIRE(icon_geometry.range_rects.size() == 1);
  const TextFieldStyle style = TextFieldStyle::Default();
  const float leading_slot = style.leading_icon_size + style.icon_spacing;
  REQUIRE(icon_geometry.caret.x - plain_geometry.caret.x == leading_slot);
  REQUIRE(icon_geometry.range_rects.front().x - plain_geometry.range_rects.front().x == leading_slot);
}

TEST_CASE("TestMaterialTextFieldUsesIndependentIconStateColors") {
  TestPlatform platform;
  Runtime runtime{MaterialInvalidIconTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 120.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());

  REQUIRE(std::ranges::any_of(scene.Commands(), [&style](const PaintCommand& command) {
    const auto* path = std::get_if<FillPathCommand>(&command);
    return path && BrushIsColor(path->brush, style.error_leading_icon);
  }));
  REQUIRE(std::ranges::any_of(scene.Commands(), [&style](const PaintCommand& command) {
    const auto* path = std::get_if<FillPathCommand>(&command);
    return path && BrushIsColor(path->brush, style.error_trailing_icon);
  }));
  REQUIRE(style.error_leading_icon != style.error_trailing_icon);
}

TEST_CASE("TestFlatTextFieldSupportsFilledVariant") {
  TestPlatform platform;
  Runtime runtime{FlatFilledTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const TextFieldStyle style = TextFieldStyle::Default();

  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->bounds.height == style.filled.minimum_height);
  REQUIRE(field->properties.corner_radii == CornerRadii::Top(style.corner_radius));
  REQUIRE(FindPresentedRectWithColor(scene, style.filled.background, Size{180.0F, 44.0F}).has_value());
  REQUIRE(FindTextFieldIndicator(scene, style.filled.border, 180.0F, style.border_width) != nullptr);
  REQUIRE(FindBorderWithColor(scene, style.filled.border) == nullptr);
}

TEST_CASE("TestTextFieldRejectsEmptyIconAssets") {
  REQUIRE_THROWS_AS(
      TextField(TextEditingValue::FromText("")).LeadingIcon(ImageAsset{}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      TextField(TextEditingValue::FromText("")).TrailingIcon(VectorAsset{}),
      std::invalid_argument
  );
}

TEST_CASE("TestTextFieldValidationRendersSupportingMessageAndErrorBorder") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{InvalidTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const TextFieldStyle style = TextFieldStyle::Default();
  const DrawTextCommand* message = FindText(scene, "Email is required");
  REQUIRE(message != nullptr);
  REQUIRE(message->style.foreground.red == style.validation_text_style.foreground.red);
  REQUIRE(message->style.foreground.green == style.validation_text_style.foreground.green);
  REQUIRE(message->style.foreground.blue == style.validation_text_style.foreground.blue);
  REQUIRE(message->style.font == style.validation_text_style.font);
  REQUIRE(runtime.RootNode()->children.front()->bounds.height == style.standard.minimum_height + 44.0F);

  REQUIRE(FindTextFieldIndicator(scene, style.validation_error, 160.0F, style.validation_border_width) != nullptr);

  Pointer(runtime, PointerEventType::Down, 20.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Unknown,
      "x",
  });
  const FlattenedScene& valid_scene = runtime.BuildFrame();
  REQUIRE(text_field_value.Get() == TextEditingValue::FromText("x"));
  REQUIRE(FindText(valid_scene, "Email is required") == nullptr);
  REQUIRE(runtime.RootNode()->children.front()->bounds.height == style.standard.minimum_height);
}

TEST_CASE("TestMaterialSecureTextFieldReservesValidationHeight") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MaterialSecureInvalidTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());
  const DrawTextCommand* message = FindText(scene, "Password is required");
  REQUIRE(message != nullptr);

  const detail::MountedNode* field = runtime.RootNode()->children.front()->children.front().get();
  const float validation_height = style.validation_spacing + message->rect.height;
  REQUIRE(field->bounds.height == style.filled.minimum_height + validation_height);
  REQUIRE(message->rect.y + message->rect.height == field->bounds.y + field->bounds.height);

  REQUIRE(FindPresentedRectWithColor(scene, style.filled.background, Size{160.0F, 56.0F}).has_value());
  REQUIRE_FALSE(
      FindPresentedRectWithColor(scene, style.filled.background, Size{160.0F, field->bounds.height}).has_value()
  );
  REQUIRE(FindTextFieldIndicator(scene, style.validation_error, 160.0F, style.validation_border_width) != nullptr);
}

TEST_CASE("TestInvalidTextFieldDoesNotDrawASecondFocusRingAroundSupportingMessage") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{InvalidTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  const FlattenedScene& scene = runtime.BuildFrame();

  const TextFieldStyle style = TextFieldStyle::Default();
  REQUIRE(
      FindTextFieldIndicator(scene, style.validation_error, 160.0F, style.focused_validation_border_width) != nullptr
  );
  REQUIRE(FindBorderWithColor(scene, style.standard.focused_border) == nullptr);
}

TEST_CASE("TestMaterialTextFieldUsesHoverErrorAndDisabledStateColors") {
  TestPlatform hover_platform;
  Runtime hovered{MaterialEmptyTextFieldApp, hover_platform};
  hovered.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  hovered.BuildFrame();
  hovered.HandlePointerEvent({
      PointerEventType::Move,
      702,
      {20.0F, 20.0F},
      PointerDeviceKind::Mouse,
  });
  const TextFieldStyle style = ThemeDefinitionValue<TextFieldStyle>(MaterialThemeDefinition());
  const FlattenedScene& hovered_scene = hovered.BuildFrame();
  const detail::MountedNode* hovered_field =
      FindMountedNodeKind(*hovered.RootNode(), detail::NodeKind::TextField);
  REQUIRE(hovered_field != nullptr);
  REQUIRE(
      FindTextFieldIndicator(
          hovered_scene,
          style.filled.hovered_border,
          hovered_field->bounds.width,
          style.border_width
      ) != nullptr
  );

  TestPlatform invalid_platform;
  Runtime invalid{MaterialSecureInvalidTextFieldApp, invalid_platform};
  invalid.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  invalid.BuildFrame();
  Pointer(invalid, PointerEventType::Down, 20.0F);
  const FlattenedScene& focused_invalid = invalid.BuildFrame();
  REQUIRE(
      FindTextFieldIndicator(
          focused_invalid,
          style.validation_error,
          160.0F,
          style.focused_validation_border_width
      ) != nullptr
  );
  const DrawRectCommand* error_caret = FindTextFieldCaret(focused_invalid, style.error_caret, style.caret_width);
  REQUIRE(error_caret != nullptr);

  TestPlatform disabled_platform;
  Runtime disabled{MaterialDisabledTextFieldApp, disabled_platform};
  disabled.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  const FlattenedScene& disabled_scene = disabled.BuildFrame();
  const DrawTextCommand* placeholder = FindText(disabled_scene, "Disabled placeholder");
  const DrawTextCommand* supporting = FindText(disabled_scene, "Disabled supporting");
  REQUIRE(placeholder != nullptr);
  REQUIRE(supporting != nullptr);
  REQUIRE(placeholder->style.foreground == style.disabled_placeholder);
  REQUIRE(supporting->style.foreground == style.disabled_supporting_text);
  const detail::MountedNode* field = FindMountedNodeKind(*disabled.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(style.filled.disabled_background.has_value());
  const auto disabled_background_rect = FindPresentedRectWithColor(disabled_scene, *style.filled.disabled_background);
  REQUIRE(disabled_background_rect.has_value());
  REQUIRE(
      FindTextFieldIndicator(
          disabled_scene,
          style.filled.disabled_border,
          field->bounds.width,
          style.border_width
      ) != nullptr
  );
  REQUIRE(field->render_node.opacity == 1.0F);

  TestPlatform selected_platform;
  Runtime selected{MaterialDisabledSelectedTextFieldApp, selected_platform};
  selected.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  REQUIRE(FindRectWithColor(selected.BuildFrame(), style.selection) == nullptr);
}

TEST_CASE("TestTextFieldDoesNotApplyGenericHoverIndication") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{InvalidTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Move,
      701,
      {20.0F, 20.0F},
      PointerDeviceKind::Mouse,
  });
  platform.AdvanceTime(FlatLightThemeSpec().motion.fast);
  const FlattenedScene& scene = runtime.BuildFrame();

  REQUIRE(FindText(scene, "Email is required") != nullptr);
  const auto& hover = FlatLightThemeSpec().interactions.indication.hover;
  REQUIRE(hover.has_value());
  REQUIRE(hover->fill.has_value());
  const Brush* hover_brush = std::get_if<Brush>(&hover->fill->Get());
  REQUIRE(hover_brush != nullptr);
  const Color* hover_color = std::get_if<Color>(&hover_brush->Get());
  REQUIRE(hover_color != nullptr);
  REQUIRE(FindRectWithColor(scene, *hover_color) == nullptr);
}

TEST_CASE("TestTextFieldValidResultDoesNotReserveSupportingSpace") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{ValidTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  REQUIRE(FindText(scene, "Email is required") == nullptr);
  REQUIRE(runtime.RootNode()->children.front()->bounds.height == TextFieldStyle::Default().standard.minimum_height);
  REQUIRE(FindBorderWithColor(scene, TextFieldStyle::Default().validation_error) == nullptr);
}

TEST_CASE("TestTextFieldPendingResultRendersNeutralSupportingMessage") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{PendingTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const TextFieldStyle style = TextFieldStyle::Default();
  const DrawTextCommand* message = FindText(scene, "Checking");
  REQUIRE(message != nullptr);
  REQUIRE(message->style.foreground.red == style.placeholder_style.foreground.red);
  REQUIRE(message->style.foreground.green == style.placeholder_style.foreground.green);
  REQUIRE(message->style.foreground.blue == style.placeholder_style.foreground.blue);
  REQUIRE(FindBorderWithColor(scene, style.validation_error) == nullptr);
  REQUIRE(runtime.RootNode()->children.front()->bounds.height == style.standard.minimum_height + 24.0F);
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  REQUIRE(FindText(scene, "a\xF0\x9F\x98\x80"
                          "e\xCC\x81") == nullptr);
  REQUIRE(FindText(scene, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") != nullptr);

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
    runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 34.0F);

  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.started_states.front().selection == TextSelection{3, 3});
  REQUIRE(
      text_input.started_geometry.front() ==
      runtime.QueryTextInputGeometry(1, text_input.started_states.front().selection.Range())
  );
  REQUIRE(text_field_value.Get().selection == TextSelection{3, 3});
}

TEST_CASE("TestAccessibleTextFieldEditingSynchronizesAnActivePlatformSession") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const auto field = std::ranges::find(frame->nodes, SemanticRole::TextField, &SemanticNode::role);
  REQUIRE(field != frame->nodes.end());

  Pointer(runtime, PointerEventType::Down, 34.0F);
  text_input.updated_states.clear();
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      field->id,
      {SemanticActionKind::SetSelection, TextRange{0, 1}}
  ));

  REQUIRE(text_field_value.Get().selection == TextSelection{0, 1});
  REQUIRE(text_input.updated_states.size() == 1);
  REQUIRE(text_input.updated_states.back().selection == TextSelection{0, 1});

  TextInputCommand composition;
  composition.kind = TextInputCommandKind::BeginComposition;
  composition.target = TextRange{0, 1};
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "x";
  update.selection_after = TextSelection{1, 1};
  REQUIRE(runtime.HandleTextInputCommands({1, {composition, update}}).result_code == TextInputResultCode::Ok);
  REQUIRE(text_field_value.Get().composition == TextRange{0, 1});

  text_input.restarted_sessions.clear();
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(field->id, {SemanticActionKind::SetText, std::string("reset")}));
  REQUIRE(text_field_value.Get() == TextEditingValue::FromText("reset"));
  REQUIRE(text_input.restarted_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE_FALSE(text_input.restarted_states.back().composition.has_value());
}

TEST_CASE("ThemeSwitchPreservesFocusedTextFieldEditingStateAndExtensions") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{ThemeSwitchTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE_FALSE(field->extensions.empty());
  const std::uint64_t identity = field->identity;
  const NodeExtension* extension = field->extensions.front().extension.get();

  Pointer(runtime, PointerEventType::Down, 34.0F);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  TextInputCommand begin;
  begin.kind = TextInputCommandKind::BeginComposition;
  begin.target = TextRange{1, 3};
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "xy";
  update.selection_after = TextSelection{3, 3};
  REQUIRE(runtime.HandleTextInputCommands({1, {begin, update}}).result_code == TextInputResultCode::Ok);
  const TextEditingValue editing = text_field_value.Get();
  REQUIRE(editing.composition.has_value());

  text_input.started_sessions.clear();
  text_input.restarted_sessions.clear();
  text_input.stopped_sessions.clear();
  text_field_dark_theme = true;
  runtime.BuildFrame();

  field = FindMountedNodeKind(*runtime.RootNode(), detail::NodeKind::TextField);
  REQUIRE(field != nullptr);
  REQUIRE(field->identity == identity);
  REQUIRE(field->extensions.front().extension.get() == extension);
  REQUIRE(text_field_value.Get() == editing);
  REQUIRE(runtime.QueryTextInputContext(1, 0, editing.text.size()).selection == editing.selection);
  REQUIRE(runtime.QueryTextInputContext(1, 0, editing.text.size()).composition == editing.composition);
  REQUIRE(text_input.started_sessions.empty());
  REQUIRE(text_input.restarted_sessions.empty());
  REQUIRE(text_input.stopped_sessions.empty());
}

TEST_CASE("TestTextFieldPresentationMovementUpdatesImeGeometryWithoutLayout") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{OffsetTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 80.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  const auto* field = runtime.RootNode()->children.front().get();
  const std::uint64_t layout_revision = field->layout_revision;
  const TextInputState initial_state = text_input.started_states.back();
  const TextInputGeometry initial_geometry = text_input.started_geometry.back();
  const TextInputPositionResult initial_position =
      runtime.QueryTextInputPosition(1, {initial_geometry.caret.x, initial_geometry.caret.y});
  text_input.updated_states.clear();
  text_input.updated_geometry.clear();

  text_field_offset = true;
  runtime.BuildFrame();

  field = runtime.RootNode()->children.front().get();
  REQUIRE(field->layout_revision == layout_revision);
  REQUIRE(text_input.updated_states.size() == 1);
  REQUIRE(text_input.updated_states.back() == initial_state);
  REQUIRE(text_input.updated_geometry.back().caret.x == initial_geometry.caret.x + 40.0F);
  REQUIRE(
      runtime.QueryTextInputPosition(
          1,
          {text_input.updated_geometry.back().caret.x, text_input.updated_geometry.back().caret.y}
      ) == initial_position
  );

  runtime.BuildFrame();
  REQUIRE(text_input.updated_states.size() == 1);
}

TEST_CASE("TestTextFieldHardwareEditingUsesTextClusters") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F);

  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "abcd";
  REQUIRE(runtime.HandleTextInputCommands({1, {update}}).result_code == TextInputResultCode::Ok);
  REQUIRE(limited_text_field_value.Get().text == "abcd");
  REQUIRE(limited_text_field_value.Get().composition == TextRange{0, 4});

  TextInputCommand finish;
  finish.kind = TextInputCommandKind::FinishComposition;
  REQUIRE(runtime.HandleTextInputCommands({1, {finish}}).result_code == TextInputResultCode::Ok);
  REQUIRE(limited_text_field_value.Get() == TextEditingValue::FromText("abc"));
}

TEST_CASE("TestMultilineTextFieldWrapsAndGrowsWithoutAHeight") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{GrowingMultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const DrawTextCommand* text = FindText(scene, "abcdefgh");
  REQUIRE(text != nullptr);
  REQUIRE(text->rect.width == 60.0F);
  REQUIRE(text->rect.height == 40.0F);
  const TextFieldStyle style = TextFieldStyle::Default();
  REQUIRE(FindTextFieldIndicator(scene, style.standard.border, 80.0F, style.border_width) != nullptr);
}

TEST_CASE("TestMultilineTextFieldEditingInvalidatesLayout") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{GrowingMultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  const auto* field = runtime.RootNode()->children.front().get();
  const float initial_height = field->bounds.height;
  const std::uint64_t initial_measure_revision = field->measure_revision;
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);

  TextInputCommand insert;
  insert.kind = TextInputCommandKind::CommitText;
  insert.text = "\nabcdefgh";
  REQUIRE(runtime.HandleTextInputCommands({1, {insert}}).result_code == TextInputResultCode::Ok);
  runtime.BuildFrame();

  field = runtime.RootNode()->children.front().get();
  REQUIRE(field->measure_revision > initial_measure_revision);
  REQUIRE(field->bounds.height > initial_height);
}

TEST_CASE("TestIdenticalTextFieldRecompositionKeepsLayoutCache") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{StableTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  const std::uint64_t initial_measure_revision = runtime.RootNode()->measure_revision;
  text_field_recompose_trigger = 1;
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode()->measure_revision == initial_measure_revision);
}

TEST_CASE("TestTextFieldSelectionChangeKeepsLayoutCache") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{KeyboardTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {280.0F, 80.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 100.0F);
  runtime.BuildFrame();

  const auto* field = runtime.RootNode()->children.front().get();
  const std::uint64_t measure_revision = field->measure_revision;
  const TextSelection selection = keyboard_text_field_value.Get().selection;
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowLeft,
  });
  runtime.BuildFrame();

  field = runtime.RootNode()->children.front().get();
  REQUIRE(keyboard_text_field_value.Get().selection != selection);
  REQUIRE(field->measure_revision == measure_revision);
}

TEST_CASE("TestMultilineTextFieldAppliesIntrinsicLineLimits") {
  ResetTextFieldState();
  TestPlatform platform;

  Runtime minimum{MinimumLinesTextFieldApp, platform};
  minimum.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  minimum.BuildFrame();
  REQUIRE(minimum.RootNode()->children.front()->bounds.height == 76.0F);

  Runtime maximum{MaximumLinesTextFieldApp, platform};
  maximum.SetWindowMetrics({.viewport = {200.0F, 140.0F}});
  maximum.BuildFrame();
  const auto* field = maximum.RootNode()->children.front().get();
  REQUIRE(field->bounds.height == 76.0F);
  REQUIRE(field->scroll_state != nullptr);
  REQUIRE(field->scroll_state->content_height == 100.0F);
}

TEST_CASE("TestTextFieldParentHeightOverridesIntrinsicLineLimits") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{FixedHeightLinesTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.RootNode()->children.front()->bounds.height == 48.0F);
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

TEST_CASE("TestTextFieldPaintDoesNotMutateScrollState") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowDown,
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::ArrowDown,
  });

  const auto& text_field = *runtime.RootNode()->children.front();
  REQUIRE(text_field.scroll_state);
  REQUIRE(text_field.scroll_state->offset_y == 0.0F);

  PaintSequence sequence;
  PaintContext context{sequence, text_field.bounds};
  for (const detail::NodeExtensionEntry& entry : text_field.extensions) {
    if (entry.extension) {
      entry.extension->PaintBehindContent(text_field, context);
      entry.extension->PaintAboveContent(text_field, context);
    }
  }
  context.Finish();

  REQUIRE(text_field.scroll_state->offset_y == 0.0F);
}

TEST_CASE("TestMultilineTextFieldNavigatesLinePageAndDocumentBoundaries") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

TEST_CASE("TestSingleLineTextFieldUsesTouchDragForHorizontalScroll") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{ScrollableSingleLineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  const auto* field = runtime.RootNode();
  REQUIRE(field->scroll_state != nullptr);
  REQUIRE(field->scroll_state->axis == Axis::Horizontal);
  const float initial_offset = field->scroll_state->offset_x;
  REQUIRE(initial_offset > 0.0F);

  runtime.HandlePointerEvent({PointerEventType::Down, 705, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 705, {80.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 705, {80.0F, 20.0F}, PointerDeviceKind::Touch});
  REQUIRE(field->scroll_state->offset_x < initial_offset);
  REQUIRE(single_line_scroll_value.Get().selection == TextSelection{30, 30});
}

TEST_CASE("TestSingleLineTextFieldKeepsMouseDragSelectionSemantics") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{ScrollableSingleLineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  const auto* field = runtime.RootNode();
  const float initial_offset = field->scroll_state->offset_x;
  runtime.HandlePointerEvent({PointerEventType::Down, 706, {80.0F, 20.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Move, 706, {40.0F, 20.0F}, PointerDeviceKind::Mouse});

  REQUIRE(!single_line_scroll_value.Get().selection.IsCollapsed());
  REQUIRE(field->scroll_state->offset_x == initial_offset);
}

TEST_CASE("TestSingleLineTextFieldScrollUpdatesImeGeometryAndEditingRevealsCaret") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{ScrollableSingleLineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 707, {80.0F, 20.0F}, PointerDeviceKind::Mouse});
  runtime.HandlePointerEvent({PointerEventType::Up, 707, {80.0F, 20.0F}, PointerDeviceKind::Mouse});
  runtime.BuildFrame();
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

  const auto* field = runtime.RootNode();
  const float initial_offset = field->scroll_state->offset_x;
  const TextInputGeometry initial_geometry =
      runtime.QueryTextInputGeometry(1, single_line_scroll_value.Get().selection.Range());
  text_input.updated_geometry.clear();

  runtime.HandlePointerEvent({PointerEventType::Down, 708, {50.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 708, {80.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 708, {80.0F, 20.0F}, PointerDeviceKind::Touch});
  const float manual_offset = field->scroll_state->offset_x;
  REQUIRE(manual_offset < initial_offset);
  runtime.BuildFrame();

  REQUIRE(!text_input.updated_geometry.empty());
  const TextInputGeometry manual_geometry =
      runtime.QueryTextInputGeometry(1, single_line_scroll_value.Get().selection.Range());
  REQUIRE(manual_geometry.caret.x > initial_geometry.caret.x);

  TextInputCommand insert;
  insert.kind = TextInputCommandKind::CommitText;
  insert.text = "x";
  REQUIRE(runtime.HandleTextInputCommands({1, {insert}}).result_code == TextInputResultCode::Ok);
  runtime.BuildFrame();

  REQUIRE(field->scroll_state->offset_x > manual_offset);
  const TextInputGeometry revealed =
      runtime.QueryTextInputGeometry(1, single_line_scroll_value.Get().selection.Range());
  const Rect viewport = detail::ScrollViewport(*field);
  REQUIRE(revealed.caret.x >= viewport.x);
  REQUIRE(revealed.caret.x + revealed.caret.width <= viewport.x + viewport.width);
}

TEST_CASE("TestMultilineTextFieldEnterInsertsNewlineInsteadOfSubmitting") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);

  runtime.HandleScrollInput({
      {20.0F, 20.0F},
      0.0F,
      20.0F,
  });
  runtime.BuildFrame();

  const auto* field = runtime.RootNode()->children.front().get();
  REQUIRE(field->scroll_state != nullptr);
  REQUIRE(field->scroll_state->offset_y == 20.0F);
  REQUIRE(runtime.QueryTextInputGeometry(1, {1, 1}).caret.y < 8.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Home,
  });
  runtime.BuildFrame();
  REQUIRE(field->scroll_state->offset_y == 0.0F);
}

TEST_CASE("TestMultilineTextFieldScrollUpdatesImeGeometry") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);
  runtime.BuildFrame();

  const TextInputGeometry initial = runtime.QueryTextInputGeometry(1, {1, 1});
  const TextSelection selection = runtime.QueryTextInputContext(1, 0, 8).selection;
  text_input.updated_states.clear();
  text_input.updated_geometry.clear();

  runtime.HandleScrollInput({
      {20.0F, 20.0F},
      0.0F,
      20.0F,
  });
  runtime.BuildFrame();

  REQUIRE(text_input.updated_states.size() == 1);
  REQUIRE(text_input.updated_states.back().selection == selection);
  REQUIRE(text_input.updated_geometry.back().caret.y == initial.caret.y - 20.0F);
}

TEST_CASE("TestMultilineTextFieldScrollDuringCompositionDoesNotRestartInput") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);

  TextInputCommand begin;
  begin.kind = TextInputCommandKind::BeginComposition;
  begin.target = TextRange{1, 1};
  REQUIRE(runtime.HandleTextInputCommands({1, {begin}}).result_code == TextInputResultCode::Ok);
  text_input.updated_states.clear();
  text_input.updated_geometry.clear();
  text_input.restarted_sessions.clear();

  runtime.HandleScrollInput({
      {20.0F, 20.0F},
      0.0F,
      20.0F,
  });
  runtime.BuildFrame();

  REQUIRE(text_input.restarted_sessions.empty());
  REQUIRE(text_input.updated_states.size() == 1);
  REQUIRE(text_input.updated_states.back().composition == TextRange{1, 1});
}

TEST_CASE("TestMultilineTextFieldPassesRemainingWheelDeltaToParent") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{NestedMultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {80.0F, 80.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 20.0F, 18.0F);
  Pointer(runtime, PointerEventType::Up, 20.0F, 18.0F);

  runtime.HandleScrollInput({
      {40.0F, 20.0F},
      0.0F,
      100.0F,
  });

  const auto* root = runtime.RootNode();
  const auto* field = root->children.front()->children.front().get();
  REQUIRE(field->scroll_state != nullptr);
  REQUIRE(field->scroll_state->offset_y == 80.0F);
  REQUIRE(root->scroll_state->offset_y == 20.0F);

  runtime.BuildFrame();
  REQUIRE(field->scroll_state->offset_y == 80.0F);
  REQUIRE(root->scroll_state->offset_y == 20.0F);
}

TEST_CASE("TestMultilineTextFieldUsesTouchDragForScrollAndMouseDragForSelection") {
  ResetTextFieldState();
  TestPlatform touch_platform;
  Runtime touch{MultilineTextFieldApp, touch_platform};
  touch.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  REQUIRE(touch.RootNode()->children.front()->scroll_state->offset_y == 20.0F);

  ResetTextFieldState();
  TestPlatform mouse_platform;
  Runtime mouse{MultilineTextFieldApp, mouse_platform};
  mouse.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  REQUIRE(mouse.RootNode()->children.front()->scroll_state->offset_y == 0.0F);
  REQUIRE(multiline_text_field_value.Get().selection == TextSelection{1, 4});
}

TEST_CASE("TestMultilineTextFieldSelectionDragScrollsAtViewportEdge") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{MultilineTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

  REQUIRE(runtime.RootNode()->children.front()->scroll_state->offset_y == 20.0F);
  REQUIRE(multiline_text_field_value.Get().selection.active > 1);
}

TEST_CASE("TestTextFieldClipboardShortcutsUseEditingActions") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F);
  Pointer(runtime, PointerEventType::Move, 55.0F);
  REQUIRE(text_field_value.Get().selection == TextSelection{1, 4});

  const FlattenedScene& scene = runtime.BuildFrame();
  const DrawRectCommand* selection = FindRect(scene, {20.0F, 10.0F, 30.0F, 20.0F});
  REQUIRE(selection != nullptr);
  REQUIRE(SolidBrushColor(selection->brush) != nullptr);
  REQUIRE(SolidBrushColor(selection->brush)->alpha == TextFieldStyle::Default().selection.alpha);

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
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      701,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const FlattenedScene& overlay = runtime.BuildFrame();
  const DrawTextCommand* copy = FindText(overlay, "复制");
  REQUIRE(copy != nullptr);
  const MenuStyle menu_style = ThemeDefinitionValue<MenuStyle>(FlatThemeDefinition());
  REQUIRE(std::ranges::any_of(overlay.Commands(), [&menu_style](const PaintCommand& command) {
    const auto* shadow = std::get_if<DrawShadowCommand>(&command);
    return shadow != nullptr && shadow->color == menu_style.shadow.color &&
           shadow->blur_radius == menu_style.shadow.blur_radius && shadow->corner_radius == menu_style.corner_radius;
  }));
  const std::size_t themed_handles = std::ranges::count_if(overlay.Commands(), [](const PaintCommand& command) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    return circle != nullptr && circle->color.red == Color::Rgb(214, 55, 48).red &&
           circle->color.green == Color::Rgb(214, 55, 48).green;
  });
  REQUIRE(themed_handles == 2);

  const RenderFrame& first_render_frame = runtime.LastCommit().render_frame;
  REQUIRE(first_render_frame.scene.root != nullptr);
  const RenderNode* selection_overlay = FindRenderNodeById(*first_render_frame.scene.root, 0);
  REQUIRE(selection_overlay != nullptr);
  const std::uint64_t overlay_revision = selection_overlay->revision;
  const RenderFrame& stable_frame = runtime.BuildRenderFrame();
  selection_overlay = FindRenderNodeById(*stable_frame.scene.root, 0);
  REQUIRE(selection_overlay != nullptr);
  REQUIRE(selection_overlay->revision == overlay_revision);

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

  const FlattenedScene& feedback = runtime.BuildFrame();
  REQUIRE(FindText(feedback, "复制") != nullptr);
  REQUIRE(menu_style.item_indication.press.has_value());
  REQUIRE(menu_style.item_indication.press->fill.has_value());
  const Brush* pressed_brush = std::get_if<Brush>(&menu_style.item_indication.press->fill->Get());
  REQUIRE(pressed_brush != nullptr);
  const Color* pressed = std::get_if<Color>(&pressed_brush->Get());
  REQUIRE(pressed != nullptr);
  REQUIRE(FindRectWithColor(feedback, *pressed) != nullptr);

  platform.AdvanceTime(0.3);
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(FindText(dismissed, "复制") == nullptr);
  REQUIRE(std::ranges::none_of(dismissed.Commands(), [](const PaintCommand& command) {
    const auto* circle = std::get_if<huxerui::DrawCircleCommand>(&command);
    return circle != nullptr && circle->color.red == Color::Rgb(214, 55, 48).red &&
           circle->color.green == Color::Rgb(214, 55, 48).green;
  }));
}

TEST_CASE("TestTextSelectionOverlayKeepsSelectionActionsAfterSelectAll") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{TextSelectionOverlayApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      703,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const DrawTextCommand* select_all = FindText(runtime.BuildFrame(), "全选");
  REQUIRE(select_all != nullptr);
  const Point select_all_center{
      select_all->rect.x + select_all->rect.width * 0.5F,
      select_all->rect.y + select_all->rect.height * 0.5F,
  };

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      704,
      select_all_center,
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      704,
      select_all_center,
      PointerDeviceKind::Touch,
  });
  REQUIRE(runtime.QueryTextInputContext(1, 0, 10).selection == TextSelection{0, 10});

  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(FindText(updated, "全选") == nullptr);
  REQUIRE(FindText(updated, "复制") != nullptr);
  const std::size_t selection_handles =
      std::ranges::count_if(updated.Commands(), [](const PaintCommand& command) {
        const auto* circle = std::get_if<DrawCircleCommand>(&command);
        return circle != nullptr && circle->radius == 6.0F;
      });
  REQUIRE(selection_handles == 2);
}

TEST_CASE("TestTextSelectionOverlayHandlesBackBeforePlatformFallback") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{TextSelectionOverlayApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      709,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(FindText(shown, "复制") != nullptr);

  REQUIRE(runtime.HandleBack());
  const FlattenedScene& dismissed = runtime.BuildFrame();
  REQUIRE(FindText(dismissed, "复制") == nullptr);
}

TEST_CASE("TestTextFieldSelectionHandleDragExtendsSelection") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{TextSelectionOverlayApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      710,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const FlattenedScene& overlay = runtime.BuildFrame();
  std::vector<Point> handles;
  for (const PaintCommand& command : overlay.Commands()) {
    const auto* circle = std::get_if<DrawCircleCommand>(&command);
    if (circle != nullptr && circle->radius == 6.0F) {
      handles.push_back(circle->center);
    }
  }
  REQUIRE(handles.size() == 2);
  const Point end_handle = *std::ranges::max_element(handles, {}, &Point::x);

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      711,
      end_handle,
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Move,
      711,
      {end_handle.x + 40.0F, end_handle.y},
      PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent({
      PointerEventType::Up,
      711,
      {end_handle.x + 40.0F, end_handle.y},
      PointerDeviceKind::Touch,
  });

  const TextSelection selection = runtime.QueryTextInputContext(1, 0, 10).selection;
  REQUIRE(selection.Range().start == 0);
  REQUIRE(selection.Range().end > 5);
}

TEST_CASE("TestEmptyTextFieldLongPressShowsPasteAtCaret") {
  ResetTextFieldState();
  TextFieldClipboard clipboard;
  clipboard.text = "pasted";
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{EmptyTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      703,
      {20.0F, 20.0F},
      PointerDeviceKind::Touch,
  });
  platform.AdvanceTime(0.5);
  const FlattenedScene& overlay = runtime.BuildFrame();
  const DrawTextCommand* paste = FindText(overlay, "Paste");
  REQUIRE(paste != nullptr);
  REQUIRE(std::ranges::none_of(overlay.Commands(), [](const PaintCommand& command) {
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
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{MaterialTextSelectionOverlayApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
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
  const FlattenedScene& pressed = runtime.BuildFrame();
  const MenuStyle material_menu = ThemeDefinitionValue<MenuStyle>(MaterialThemeDefinition());
  REQUIRE(material_menu.item_indication.ripple.has_value());
  const Color expected_ripple = material_menu.item_indication.ripple->color;
  REQUIRE(std::ranges::any_of(pressed.Commands(), [expected_ripple](const PaintCommand& command) {
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
  const FlattenedScene& released = runtime.BuildFrame();
  REQUIRE(FindText(released, "Copy") != nullptr);
  REQUIRE(std::ranges::any_of(released.Commands(), [expected_ripple](const PaintCommand& command) {
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
  mouse.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  mouse.BuildFrame();
  ClickAt(mouse, {70.0F, 20.0F}, 707);
  platform.AdvanceTime(0.2);
  mouse.HandlePointerEvent({
      PointerEventType::Down,
      708,
      {70.0F, 20.0F},
      PointerDeviceKind::Mouse,
  });
  REQUIRE(mouse.QueryTextInputContext(1, 0, 10).selection == TextSelection{6, 10});
  REQUIRE(FindText(mouse.BuildFrame(), "复制") == nullptr);

  TextFieldClipboard touch_clipboard;
  TestPlatform touch_platform;
  touch_platform.platform_clipboard = &touch_clipboard;
  Runtime touch{TextSelectionOverlayApp, touch_platform};
  touch.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
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
  const TextSelection double_tap_selection = touch.QueryTextInputContext(1, 0, 10).selection;
  REQUIRE(double_tap_selection == TextSelection{0, 5});
  REQUIRE(FindText(touch.BuildFrame(), "复制") != nullptr);
}

TEST_CASE("TestTextFieldImeCommandsAndAuthoritativeReplacement") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F);
  Pointer(runtime, PointerEventType::Move, 20.0F, -20.0F);
  Pointer(runtime, PointerEventType::Move, 55.0F, -40.0F);
  runtime.BuildFrame();

  REQUIRE(text_field_value.Get().selection == TextSelection{1, 1});
  REQUIRE(text_field_scroll.Metrics().offset > 0.0F);
}

TEST_CASE("TestFocusedSingleLineTextFieldAllowsAncestorWheelScrollUntilEditingResumes") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{ScrollableTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F);
  Pointer(runtime, PointerEventType::Up, 20.0F);
  runtime.BuildFrame();

  runtime.HandleScrollInput({
      {20.0F, 20.0F},
      0.0F,
      60.0F,
  });
  runtime.BuildFrame();
  REQUIRE(text_field_scroll.Metrics().offset == 60.0F);

  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::Home,
  });
  runtime.BuildFrame();
  REQUIRE(text_field_scroll.Metrics().offset < 60.0F);
}

TEST_CASE("TestTouchMovementDoesNotFocusANonScrollableTextField") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{TextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 719, {20.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Move, 719, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 719, {20.0F, 20.0F}, PointerDeviceKind::Touch});

  REQUIRE(text_input.started_sessions.empty());
  REQUIRE(text_input.show_requests.empty());
}

TEST_CASE("TestTouchScrollOverTextFieldDoesNotFocusOrStartTextInput") {
  ResetTextFieldState();
  TextFieldPlatformInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  Runtime runtime{ScrollableTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
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
  runtime.BuildFrame();

  REQUIRE(text_input.show_requests.empty());
  REQUIRE(text_field_value.Get().selection == TextSelection{1, 1});
  REQUIRE(text_field_scroll.Metrics().offset > 0.0F);
}

TEST_CASE("TestTextFieldScrollsIntoReducedViewport") {
  ResetTextFieldState();
  TestPlatform platform;
  Runtime runtime{OccludedTextFieldApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20.0F, 160.0F);
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  REQUIRE(text_field_scroll.Metrics().offset > 0.0F);
  const TextInputGeometry geometry = runtime.QueryTextInputGeometry(1, {7, 7});
  REQUIRE(geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(geometry.caret.y + geometry.caret.height <= 72.0F);
}

} // namespace huxerui::test
