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

class ChangingTextPlatform final : public TestPlatform {
public:
  TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style, float width,
      const TextLayoutOptions& options) override {
    const float layout_scale = scale * style.font.Size() / 20.0F;
    auto result = TestPlatform::MeasureText(text, style, width / layout_scale, options);
    result.size.width *= layout_scale;
    return result;
  }
  std::unique_ptr<detail::TextLayout> CreateTextLayout(const AttributedText& text, const TextStyle& style, float width,
      const TextLayoutOptions& options) override {
    class ScaledLayout final : public detail::TextLayout {
    public:
      ScaledLayout(std::unique_ptr<detail::TextLayout> inner, float scale) : inner_(std::move(inner)), scale_(scale) {}
      Size Measure() const override { auto size = inner_->Measure(); size.width *= scale_; return size; }
      TextPosition HitTest(Point point) const override { point.x /= scale_; return inner_->HitTest(point); }
      Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
        auto rect = inner_->CaretRect(offset, affinity); rect.x *= scale_; return rect;
      }
      std::vector<Rect> RangeRects(TextRange range) const override {
        auto rects = inner_->RangeRects(range);
        for (auto& rect : rects) { rect.x *= scale_; rect.width *= scale_; }
        return rects;
      }
      TextOffset PreviousCaretOffset(TextOffset offset) const override { return inner_->PreviousCaretOffset(offset); }
      TextOffset NextCaretOffset(TextOffset offset) const override { return inner_->NextCaretOffset(offset); }
    private:
      std::unique_ptr<detail::TextLayout> inner_;
      float scale_;
    };
    ++layouts;
    const float layout_scale = scale * style.font.Size() / 20.0F;
    return std::make_unique<ScaledLayout>(
        TestPlatform::CreateTextLayout(text, style, width / layout_scale, options), layout_scale);
  }
  float scale = 1.0F;
  int layouts = 0;
};

std::function<View()> metric_root;
View MetricApp() { return metric_root(); }

void ResetParagraph() {
  paragraph = AttributedText::FromRanges("Go first then second.", {},
      {{{3, 8}, Uri("https://example.com/first")}, {{14, 20}, Uri("https://example.com/second")}});
  paragraph_enabled = true;
  link_activations = 0;
  activated_link.reset();
}

void ParagraphPointer(UiWindow& runtime, PointerEventType type, Point position, KeyModifiers modifiers = {}) {
  runtime.HandlePointerEvent({type, 41, position, PointerDeviceKind::Mouse,
      type == PointerEventType::Down || type == PointerEventType::Up ? PointerButton::Primary : PointerButton::None,
      type == PointerEventType::Up || type == PointerEventType::Cancel ? PointerButton::None : PointerButton::Primary,
      modifiers});
}

} // namespace

TEST_CASE("AttributedParagraphLinksUseTypedPointerKeyboardAndSemanticActions") {
  ResetParagraph();
  ParagraphPlatform platform;
  UiWindow runtime{AttributedParagraphApp, platform};
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
  UiWindow runtime{AttributedParagraphApp, platform};
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
  UiWindow runtime{AttributedParagraphApp, platform};
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
  UiWindow runtime{AttributedParagraphApp, platform};
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
  UiWindow runtime{AttributedParagraphApp, platform};
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
  UiWindow runtime{AttributedParagraphApp, platform};
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
  UiWindow runtime{AttributedParagraphApp, platform};
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

TEST_CASE("TextFontChangesRefreshObservationSelectionAndTextFieldWithoutResettingState") {
  class Input final : public PlatformTextInput {
  public:
    void Start(TextInputSessionId id, const TextInputConfiguration&, const TextInputState&,
        const TextInputGeometry& value) override { session = id; geometry = value; ++starts; }
    void Update(TextInputSessionId id, const TextInputState&, const TextInputGeometry& value) override {
      REQUIRE(id == session);
      geometry = value;
    }
    void Restart(TextInputSessionId, const TextInputConfiguration&, const TextInputState&,
        const TextInputGeometry&) override { ++restarts; }
    void Stop(TextInputSessionId) override { ++stops; }
    TextInputSessionId session = 0;
    TextInputGeometry geometry;
    int starts = 0;
    int restarts = 0;
    int stops = 0;
  } input;
  ChangingTextPlatform platform;
  platform.platform_text_input = &input;
  State<TextEditingValue> value;
  State<Font> font;
  float observed_width = 0.0F;
  int compositions = 0;
  metric_root = [&]() -> View {
    ++compositions;
    value = UseState(TextEditingValue::FromText("edit"));
    font = UseState(Font::System(20.0F));
    const TextStyle style{font.Get(), Color::Black()};
    observed_width = UseTextMeasurer().MeasureText("measure", style).size.width;
    return Column {
      SelectionArea(Text("paragraph").Style(style)),
      TextField(value)
          .OnChanged([state = value](const TextEditingValue& changed) mutable { state = changed; })
          .With(FontSize{style.font.Size()}, Frame{.width = 200.0F, .height = 48.0F}),
    };
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {300.0F, 160.0F}});
  window.BuildFrame();
  auto selection = value.Get();
  selection.selection = {1, 3};
  value = selection;
  const auto& before = window.BuildFrame();
  const auto width = FindText(before, "paragraph")->rect.width;
  const auto field = FindText(before, "edit")->rect;
  const auto observed = observed_width;
  ClickAt(window, {field.x + 10.0F, field.y + 10.0F});
  TextInputCommand begin;
  begin.kind = TextInputCommandKind::BeginComposition;
  begin.target = TextRange{1, 3};
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = "di";
  update.selection_after = TextSelection{2, 2};
  REQUIRE(window.HandleTextInputCommands({input.session, {begin, update}}).result_code == TextInputResultCode::Ok);
  const auto editing = value.Get();
  REQUIRE(editing.composition == TextRange{1, 3});
  window.BuildFrame();
  const auto count = platform.layouts;
  const auto old_compositions = compositions;
  const auto before_caret = input.geometry.caret;
  float scale = 2.0F;
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  bool platform_metrics_changed = false;
#endif
  SECTION("Font value changes") {
    font = font.Get().WithSize(40.0F);
  }
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  SECTION("Platform text metrics change") {
    platform_metrics_changed = true;
    SECTION("Larger system fonts") { scale = 1.5F; }
    SECTION("Smaller system fonts") { scale = 0.75F; }
    platform.scale = scale;
    detail::InternalAccess::InvalidateTextMetrics(platform);
  }
#endif
  const auto& after = window.BuildFrame();
  REQUIRE(observed_width == Catch::Approx(observed * scale));
  REQUIRE(FindText(after, "paragraph")->rect.width == Catch::Approx(width * scale));
  REQUIRE(platform.layouts > count);
  REQUIRE(compositions == old_compositions + 1);
  REQUIRE(value.Get() == editing);
  REQUIRE(input.starts == 1);
  REQUIRE(input.restarts == 0);
  REQUIRE(input.stops == 0);
  REQUIRE(input.geometry.caret.x - field.x == Catch::Approx((before_caret.x - field.x) * scale));
#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
  if (platform_metrics_changed) {
    REQUIRE(font.Get().Size() == 20.0F);
    platform.scale = 1.0F;
    detail::InternalAccess::InvalidateTextMetrics(platform);
    const auto& restored = window.BuildFrame();
    REQUIRE(observed_width == Catch::Approx(observed));
    REQUIRE(FindText(restored, "paragraph")->rect.width == Catch::Approx(width));
    REQUIRE(input.geometry.caret == before_caret);
    REQUIRE(value.Get() == editing);
    REQUIRE(input.starts == 1);
    REQUIRE(input.restarts == 0);
    REQUIRE(input.stops == 0);
  }
#endif
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowRight}));
  REQUIRE(value.Get().selection.active == editing.selection.active + 1);
}

#if defined(__EMSCRIPTEN__) || (defined(__linux__) && !defined(__ANDROID__))
TEST_CASE("PlatformTextMetricsChangesRemainLocalToOneWindow") {
  TestPlatform application_platform;
  ChangingTextPlatform first_platform;
  ChangingTextPlatform second_platform;
  float observed[2]{};
  int compositions[2]{};
  int slot = 0;
  metric_root = [&]() -> View {
    ++compositions[slot];
    const TextStyle style{Font::System(20.0F), Color::Black()};
    observed[slot] = UseTextMeasurer().MeasureText("measure", style).size.width;
    return Column {
      SelectionArea(Text("paragraph").Style(style)),
    };
  };
  Application application(MetricApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, application_platform);
  UiWindow first(runtime, first_platform);
  UiWindow second(runtime, second_platform, ResourceConfiguration{.display_scale = 2.0F});
  first.SetWindowMetrics({.viewport = {300.0F, 160.0F}});
  second.SetWindowMetrics({.viewport = {300.0F, 160.0F}});
  const auto initial_width = FindText(first.BuildFrame(), "paragraph")->rect.width;
  slot = 1;
  REQUIRE(FindText(second.BuildFrame(), "paragraph")->rect.width == initial_width);
  const auto initial_observed = observed[0];
  const auto second_layouts = second_platform.layouts;

  first_platform.scale = 1.5F;
  detail::InternalAccess::InvalidateTextMetrics(first_platform);
  slot = 0;
  REQUIRE(FindText(first.BuildFrame(), "paragraph")->rect.width == Catch::Approx(initial_width * 1.5F));
  slot = 1;
  REQUIRE(FindText(second.BuildFrame(), "paragraph")->rect.width == initial_width);
  REQUIRE(observed[0] == Catch::Approx(initial_observed * 1.5F));
  REQUIRE(observed[1] == initial_observed);
  REQUIRE(compositions[0] == 2);
  REQUIRE(compositions[1] == 1);
  REQUIRE(second_platform.layouts == second_layouts);
}
#endif

} // namespace huxerui::test
