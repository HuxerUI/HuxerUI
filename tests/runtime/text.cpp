#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

AttributedText paragraph;
bool paragraph_enabled = true;
int link_activations = 0;
std::optional<TextLinkActivation> activated_link;

View AttributedParagraphApp() {
  return SelectionArea(Column {
    Text(paragraph).On<TextEvents::LinkActivated>([](const TextLinkActivation& link) {
      ++link_activations;
      activated_link = link;
    }),
    Button("After"),
  }).With(Enabled{paragraph_enabled});
}

class ParagraphClipboard final : public PlatformClipboard {
public:
  std::optional<std::string> ReadText() override { return text; }
  bool WriteText(std::string_view value) override {
    text = value;
    return true;
  }
  std::optional<std::string> text;
};

class ParagraphPlatform final : public TestPlatform {
public:
  std::unique_ptr<detail::TextLayout> CreateTextLayout(const AttributedText& text, const TextStyle& style, float width,
      const TextLayoutOptions& options) override {
    ++layouts;
    return TestPlatform::CreateTextLayout(text, style, width, options);
  }
  TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style, float width,
      const TextLayoutOptions& options) override {
    ++measurements;
    return TestPlatform::MeasureText(text, style, width, options);
  }
  int measurements = 0;
  int layouts = 0;
};

void ResetParagraph() {
  paragraph = AttributedText::FromRanges("Go first then second.", {},
      {{{3, 8}, Uri("https://example.com/first")}, {{14, 20}, Uri("https://example.com/second")}});
  paragraph_enabled = true;
  link_activations = 0;
  activated_link.reset();
}

void ParagraphPointer(Runtime& runtime, PointerEventType type, Point position, KeyModifiers modifiers = {}) {
  runtime.HandlePointerEvent({type, 41, position, PointerDeviceKind::Mouse,
      type == PointerEventType::Down || type == PointerEventType::Up ? PointerButton::Primary : PointerButton::None,
      type == PointerEventType::Up || type == PointerEventType::Cancel ? PointerButton::None : PointerButton::Primary,
      modifiers});
}

} // namespace

TEST_CASE("AttributedParagraphLinksUseTypedPointerKeyboardAndSemanticActions") {
  ResetParagraph();
  ParagraphPlatform platform;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE(platform.layouts == 1);
  ParagraphPointer(runtime, PointerEventType::Down, {40.0F, 10.0F});
  ParagraphPointer(runtime, PointerEventType::Up, {40.0F, 10.0F});
  REQUIRE(link_activations == 1);
  REQUIRE(activated_link->range == TextRange{3, 8});

  REQUIRE(activated_link->target == Uri("https://example.com/first"));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(link_activations == 2);
  REQUIRE(activated_link->range == TextRange{14, 20});
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab, .modifiers = {.shift = true}}));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(activated_link->range == TextRange{3, 8});

  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab, .modifiers = {.shift = true}}));
  REQUIRE(runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(activated_link->range == TextRange{14, 20});

  const auto frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(frame);
  std::vector<SemanticNode> links;
  for (const auto& node : frame->nodes) {
    if (node.role == SemanticRole::Link) {
      links.push_back(node);
    }
  }
  REQUIRE(links.size() == 2);
  REQUIRE(links[0].label == "first");
  REQUIRE(links[1].label == "second");
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(links[1].id, {SemanticActionKind::Activate}));
  REQUIRE(activated_link->range == TextRange{14, 20});
}

TEST_CASE("AttributedParagraphDragSelectionDoesNotActivateLinks") {
  ResetParagraph();
  ParagraphPlatform platform;
  ParagraphClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  ParagraphPointer(runtime, PointerEventType::Down, {40.0F, 10.0F});
  ParagraphPointer(runtime, PointerEventType::Move, {180.0F, 10.0F});
  ParagraphPointer(runtime, PointerEventType::Up, {180.0F, 10.0F});
  REQUIRE(link_activations == 0);
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == paragraph.TextInRange({4, 18}));
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::SelectAll));
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == paragraph.PlainText());
}

TEST_CASE("AttributedParagraphRejectsCanceledDisabledAndReplacedLinkTargets") {
  ResetParagraph();
  ParagraphPlatform platform;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  ParagraphPointer(runtime, PointerEventType::Down, {40.0F, 10.0F});
  SECTION("Canceled") {
    ParagraphPointer(runtime, PointerEventType::Cancel, {40.0F, 10.0F});
  }
  SECTION("Disabled") {
    paragraph_enabled = false;
    runtime.InvalidateRoot();
    const auto frame = runtime.BuildCommit().semantic_frame;
    for (const auto& node : frame->nodes) {
      if (node.role == SemanticRole::Link) {
        REQUIRE_FALSE(node.enabled);
        REQUIRE(node.actions == 0);
      }
    }
  }
  SECTION("Replaced target") {
    paragraph = AttributedText::FromRanges(paragraph.PlainText(), {}, {{{3, 8}, Uri("https://example.com/replaced")}});
    runtime.InvalidateRoot();
    runtime.BuildFrame();
  }
  ParagraphPointer(runtime, PointerEventType::Up, {40.0F, 10.0F});
  REQUIRE(link_activations == 0);
}

TEST_CASE("AttributedParagraphPaintOnlyChangesKeepMeasuredAncestors") {
  ResetParagraph();
  ParagraphPlatform platform;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  const int original = platform.measurements;
  paragraph = paragraph.WithStyles({{{0, 2}, {.foreground = Color::Rgb(200, 30, 80)}}});
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(platform.measurements == original);
  paragraph = paragraph.WithStyles({{{0, 2}, {.font_size = 32.0F}}});
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(platform.measurements > original);
}

TEST_CASE("AttributedParagraphShiftClickExtendsExistingSelection") {
  ResetParagraph();
  ParagraphPlatform platform;
  ParagraphClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  ParagraphPointer(runtime, PointerEventType::Down, {10.0F, 10.0F});
  ParagraphPointer(runtime, PointerEventType::Up, {10.0F, 10.0F});
  Point target{120.0F, 10.0F};
  SECTION("Plain text") {}
  SECTION("Link") { target.x = 40.0F; }
  ParagraphPointer(runtime, PointerEventType::Down, target, {.shift = true});
  ParagraphPointer(runtime, PointerEventType::Up, target, {.shift = true});
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == paragraph.TextInRange({1, static_cast<TextOffset>(target.x / 10.0F)}));
  REQUIRE(link_activations == 0);
}

TEST_CASE("AttributedParagraphLongPressSelectsWithoutActivatingLink") {
  ResetParagraph();
  ParagraphPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  ParagraphClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 44, {40.0F, 10.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.8);
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Up, 44, {40.0F, 10.0F}, PointerDeviceKind::Touch});
  REQUIRE(link_activations == 0);
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "first");
}

TEST_CASE("AttributedParagraphDoubleClickSelectsLinkWordWithoutSecondActivation") {
  ResetParagraph();
  ParagraphPlatform platform;
  ParagraphClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{AttributedParagraphApp, platform};
  runtime.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  runtime.BuildFrame();
  ParagraphPointer(runtime, PointerEventType::Down, {40.0F, 10.0F});
  ParagraphPointer(runtime, PointerEventType::Up, {40.0F, 10.0F});
  REQUIRE(link_activations == 1);
  platform.AdvanceTime(0.1);
  ParagraphPointer(runtime, PointerEventType::Down, {40.0F, 10.0F});
  ParagraphPointer(runtime, PointerEventType::Up, {40.0F, 10.0F});
  REQUIRE(link_activations == 1);
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "first");
}

} // namespace huxerui::test
