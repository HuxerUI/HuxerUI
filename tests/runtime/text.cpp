#include "runtime_test_support.h"

#include "runtime/transition_internal.h"

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
  std::unique_ptr<detail::TextLayoutBackend> CreatePlatformTextLayout(
      const AttributedText& text, const TextStyle& style, float width, const TextLayoutOptions& options) override {
    ++layouts;
    return TestPlatform::CreatePlatformTextLayout(text, style, width, options);
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
  std::unique_ptr<detail::TextLayoutBackend> CreatePlatformTextLayout(
      const AttributedText& text, const TextStyle& style, float width, const TextLayoutOptions& options) override {
    class ScaledLayout final : public detail::TextLayoutBackend {
    public:
      ScaledLayout(std::unique_ptr<detail::TextLayoutBackend> inner, float scale)
          : inner_(std::move(inner)), scale_(scale) {}
      Size Measure() const override { auto size = inner_->Measure(); size.width *= scale_; return size; }
      std::vector<TextLineMetrics> LineMetrics() const override {
        auto lines = inner_->LineMetrics();
        for (auto& line : lines) {
          line.bounds.x *= scale_;
          line.bounds.width *= scale_;
        }
        return lines;
      }
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
      std::unique_ptr<detail::TextLayoutBackend> inner_;
      float scale_;
    };
    ++layouts;
    const float layout_scale = scale * style.font.Size() / 20.0F;
    return std::make_unique<ScaledLayout>(
        TestPlatform::CreatePlatformTextLayout(text, style, width / layout_scale, options), layout_scale);
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

TEST_CASE("RetainedTextLayoutSharesLineGeometryAndPaintInputs") {
  ParagraphPlatform platform;
  UiWindow window{AttributedParagraphApp, platform};
  const AttributedText text("ab🙂c\n");
  const auto style = TextStyle::Default();
  const TextLayoutOptions options{
      .shaping = {.locale = "ja"},
      .vertical_align = TextVerticalAlign::Bottom,
  };
  const auto layout = platform.CreateTextLayout(text, style, 20.0F, options);
  const auto copy = platform.CreateTextLayout(text, style, 20.0F, options);
  REQUIRE(platform.layouts == 1);
  REQUIRE(copy.Lines().data() == layout.Lines().data());
  REQUIRE(layout.Metrics().line_count == 4);
  REQUIRE(layout.Lines()[0].range == TextRange{0, 2});
  REQUIRE(layout.Lines()[1].range == TextRange{2, 4});
  REQUIRE(layout.Lines()[2].range == TextRange{4, 5});
  REQUIRE(layout.Lines()[3].range == TextRange{6, 6});
  REQUIRE(layout.LineIndex({2, TextAffinity::Upstream}) == 0);
  REQUIRE(layout.LineIndex({2, TextAffinity::Downstream}) == 1);
  REQUIRE(layout.CaretRect({2, TextAffinity::Upstream}).y < layout.CaretRect({2, TextAffinity::Downstream}).y);
  REQUIRE(layout.HitTest({18.0F, 10.0F}) == TextPosition{2, TextAffinity::Upstream});
  REQUIRE(layout.HitTest({0.0F, 100.0F}).offset == 6);
  REQUIRE(layout.PreviousCaretOffset(4) == 2);
  REQUIRE(layout.NextCaretOffset(2) == 4);
  REQUIRE(layout.RangeRects({1, 5}).size() == 3);
  REQUIRE(layout.RangeRects({2, 2}).empty());
  REQUIRE_THROWS_AS(layout.CaretRect({3}), std::invalid_argument);

  PaintSequence sequence;
  PaintContext paint(sequence, {0, 0, 100, 120});
  paint.DrawText({3, 7}, layout);
  paint.Finish();
  REQUIRE(sequence.Commands().size() == 1);
  const auto& command = std::get<DrawTextCommand>(sequence.Commands().front());
  REQUIRE(command.text == text);
  REQUIRE(command.rect == Rect{3, 7, 20, layout.Metrics().size.height});
  REQUIRE(command.options.shaping.locale == "ja");
  REQUIRE(command.options.vertical_align == TextVerticalAlign::Top);

  const auto empty = platform.CreateTextLayout(AttributedText{}, style, 20.0F);
  REQUIRE(empty.Lines().size() == 1);
  REQUIRE(empty.CaretRect({0}).height > 0.0F);
  REQUIRE_THROWS_AS(platform.CreateTextLayout(text, style, 0.0F), std::invalid_argument);
}

TEST_CASE("PaintSequenceRetainsEachPreparedTextLayoutUntilRerecorded") {
  PaintSequence sequence;
  const Rect bounds{0, 0, 160, 120};
  const auto style = TextStyle::Default();
  {
    ParagraphPlatform platform;
    UiWindow window{AttributedParagraphApp, platform};
    PaintContext paint(sequence, bounds);
    paint.DrawText({0, 0}, platform.CreateTextLayout(AttributedText("first"), style, 80.0F));
    paint.DrawRect({0, 20, 8, 8}, Color::Black());
    paint.DrawText(Rect{0, 30, 80, 20}, "portable", style);
    paint.DrawText({0, 50}, platform.CreateTextLayout(AttributedText("second"), style, 60.0F));
    paint.Finish();
  }

  const auto& commands = sequence.Commands();
  REQUIRE(commands.size() == 4);
  const auto* first = detail::InternalAccess::TextLayoutForPaint(sequence, commands[0]);
  const auto* second = detail::InternalAccess::TextLayoutForPaint(sequence, commands[3]);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first != second);
  CHECK(first->Measure().width == 50.0F);
  CHECK(second->Measure().width == 60.0F);
  CHECK(detail::InternalAccess::TextLayoutForPaint(sequence, commands[2]) == nullptr);

  const auto frozen = detail::FrozenScene::CopyPaintSequence(sequence);
  REQUIRE(frozen.Commands() == commands);
  CHECK(detail::InternalAccess::TextLayoutForPaint(frozen, frozen.Commands()[0]) == nullptr);
  CHECK(detail::InternalAccess::TextLayoutForPaint(frozen, frozen.Commands()[3]) == nullptr);

  PaintContext replacement(sequence, bounds);
  replacement.DrawText(bounds, "replacement", style);
  replacement.Finish();
  REQUIRE(sequence.Commands().size() == 1);
  CHECK(detail::InternalAccess::TextLayoutForPaint(sequence, sequence.Commands().front()) == nullptr);
}

TEST_CASE("TextOverflowUsesNaturalLineHeightAndCompleteGraphemes") {
  ParagraphPlatform platform;
  metric_root = []() -> View {
    return Column {
      Text("Alignment.md").Overflow(TextOverflow::MiddleEllipsis).LineLimit(1).With(Frame{.width = 70.0F}),
      Text("中文👩‍💻é.md").LineLimit(1).Overflow(TextOverflow::MiddleEllipsis).With(Frame{.width = 100.0F}),
      Text("AéBC").LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Frame{.width = 30.0F}),
      Text("Alignment.md").LineLimit(1).Overflow(TextOverflow::StartEllipsis).With(Frame{.width = 40.0F}),
      Text("one\nlong line\nthree").LineLimit(2).Overflow(TextOverflow::Ellipsis).With(Frame{.width = 70.0F}),
      Text("one\n\nthree").LineLimit(2).With(Frame{.width = 70.0F}),
      Text("Narrow").LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Frame{.width = 5.0F}),
    };
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {220.0F, 200.0F}});
  const auto& frame = window.BuildFrame();
  for (const std::string_view visible : {"Ali….md", "中文👩‍💻…md", "Aé…", "….md"}) {
    const auto* command = FindText(frame, visible);
    REQUIRE(command);
    REQUIRE(command->rect.height == 20.0F);
  }
  for (const std::string_view visible : {"one\nlong l…", "one\n"}) {
    const auto* command = FindText(frame, visible);
    REQUIRE(command);
    REQUIRE(command->rect.height == 40.0F);
  }
  REQUIRE_FALSE(ContainsText(frame, "Narrow"));
  const auto semantics = window.BuildCommit().semantic_frame;
  for (const std::string_view label : {"Alignment.md", "中文👩‍💻é.md", "one\n\nthree", "Narrow"}) {
    REQUIRE(std::ranges::any_of(semantics->nodes, [label](const auto& node) { return node.label == label; }));
  }
}

TEST_CASE("TextOverflowReusesTheMeasuredParagraphWhenTextFits") {
  ParagraphPlatform platform;
  metric_root = [] {
    return Text("Alignment.md").LineLimit(1).Overflow(TextOverflow::MiddleEllipsis).With(Frame{.width = 160.0F});
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {160.0F, 20.0F}});
  const auto& frame = window.BuildCommit().render_frame;
  const auto find_layout =
      [](const auto& self, const RenderNode& node) -> const detail::TextLayoutBackend* {
    for (const auto& command : node.content.Commands()) {
      if (const auto* text = std::get_if<DrawTextCommand>(&command); text && text->text.PlainText() == "Alignment.md") {
        return detail::InternalAccess::TextLayoutForPaint(node.content, command);
      }
    }
    for (const auto* child : node.children) {
      if (auto layout = self(self, *child)) {
        return layout;
      }
    }
    return {};
  };
  const auto layout = find_layout(find_layout, *frame.scene.root);
  REQUIRE(layout);
  REQUIRE(layout->Measure().height == 20.0F);
  REQUIRE(platform.layouts == 1);
  window.InvalidateRoot();
  REQUIRE(find_layout(find_layout, *window.BuildCommit().render_frame.scene.root) == layout);
  REQUIRE(platform.layouts == 1);
}

TEST_CASE("TextOverflowPreservesOriginalParagraphDirection") {
  ParagraphPlatform platform;
  metric_root = [] {
    return Column {
      Text("123 مرحبا example.txt").LineLimit(1).Overflow(TextOverflow::StartEllipsis)
          .With(Frame{.width = 40.0F}),
      Text("file שלום").LineLimit(1).Overflow(TextOverflow::StartEllipsis)
          .With(Frame{.width = 40.0F}),
      Text("مرحبا").LineLimit(1).Overflow(TextOverflow::Ellipsis)
          .Shaping({.direction = TextDirection::LeftToRight}).With(Frame{.width = 10.0F}),
    };
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  const auto& frame = window.BuildFrame();
  const auto* rtl = FindText(frame, "…txt");
  const auto* ltr = FindText(frame, "…לום");
  const auto* explicit_ltr = FindText(frame, "…");
  REQUIRE(rtl);
  REQUIRE(ltr);
  REQUIRE(explicit_ltr);
  REQUIRE(rtl->options.shaping.direction == TextDirection::RightToLeft);
  REQUIRE(ltr->options.shaping.direction == TextDirection::LeftToRight);
  REQUIRE(explicit_ltr->options.shaping.direction == TextDirection::LeftToRight);
}

TEST_CASE("TextOverflowValidatesLineLimitsAtTheDeclarationBoundary") {
  REQUIRE_THROWS_AS(Text("Name").LineLimit(0), std::invalid_argument);
  ParagraphPlatform platform;
  metric_root = [] { return Text("Name").LineLimit(2).Overflow(TextOverflow::MiddleEllipsis); };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {150.0F, 40.0F}});
  REQUIRE_THROWS_AS(window.BuildFrame(), std::invalid_argument);
}

TEST_CASE("TextOverflowReusesLayoutWhenOnlyOmittedColorsChange") {
  ParagraphPlatform platform;
  AttributedText source("Alignment.md");
  metric_root = [&source] {
    return Column {
      Text(source).LineLimit(1).Overflow(TextOverflow::MiddleEllipsis).With(Frame{.width = 70.0F}),
    };
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  REQUIRE(ContainsText(window.BuildFrame(), "Ali….md"));
  const int layouts = platform.layouts;
  const Color accent = Color::Rgb(200, 30, 80);

  source = source.WithStyles({{{3, 9}, {.foreground = accent}}});
  window.InvalidateRoot();
  REQUIRE(ContainsText(window.BuildFrame(), "Ali….md"));
  REQUIRE(platform.layouts == layouts);

  source = source.WithStyles({{{1, 2}, {.foreground = accent}}});
  window.InvalidateRoot();
  const auto* painted = FindText(window.BuildFrame(), "Ali….md");
  REQUIRE(painted);
  REQUIRE(painted->text.StyleRanges().size() == 1);
  REQUIRE(painted->text.StyleRanges().front().range == TextRange{1, 2});
  REQUIRE(painted->text.StyleRanges().front().style.foreground == accent);
  REQUIRE(platform.layouts == layouts + 1);
}

TEST_CASE("TextOverflowPreservesSourceOffsetsAndLinkSemantics") {
  ParagraphPlatform platform;
  ParagraphClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  const auto source = AttributedText::FromRanges("Alignment.md", {},
      {{{3, 8}, Uri("https://example.com/hidden")}, {{8, 12}, Uri("https://example.com/extension")}});
  activated_link.reset();
  metric_root = [source] {
    return SelectionArea(Column {
      Text(source).LineLimit(1).Overflow(TextOverflow::MiddleEllipsis)
          .On<TextEvents::LinkActivated>([](const auto& link) { activated_link = link; })
          .With(Frame{.width = 70.0F}),
    });
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  const auto semantics = window.BuildCommit().semantic_frame;
  REQUIRE(std::ranges::any_of(semantics->nodes, [](const auto& node) { return node.label == "Alignment.md"; }));
  REQUIRE(std::ranges::count_if(semantics->nodes, [](const auto& node) {
    return node.role == SemanticRole::Link;
  }) == 1);
  ParagraphPointer(window, PointerEventType::Down, {34.0F, 10.0F});
  ParagraphPointer(window, PointerEventType::Up, {34.0F, 10.0F});
  REQUIRE_FALSE(activated_link);
  platform.AdvanceTime(1.0);
  ParagraphPointer(window, PointerEventType::Down, {45.0F, 10.0F});
  ParagraphPointer(window, PointerEventType::Up, {45.0F, 10.0F});
  REQUIRE(activated_link);
  REQUIRE(activated_link->range == TextRange{8, 12});
  platform.AdvanceTime(1.0);
  ParagraphPointer(window, PointerEventType::Down, {1.0F, 10.0F});
  ParagraphPointer(window, PointerEventType::Move, {69.0F, 10.0F});
  ParagraphPointer(window, PointerEventType::Up, {69.0F, 10.0F});
  REQUIRE(window.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "Alignment.md");
}

TEST_CASE("TextOverflowPreservesKeyboardAndSemanticActionsForFrameClippedLinks") {
  ParagraphPlatform platform;
  const auto source = AttributedText::FromRanges("first\nsecond", {}, {
      {{6, 12}, Uri("https://example.com/second")},
  });
  activated_link.reset();
  metric_root = [source] {
    return Column {
      Text(source).On<TextEvents::LinkActivated>([](const auto& link) { activated_link = link; })
          .With(Frame{.width = 110.0F, .height = 20.0F}),
    };
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  const auto semantics = window.BuildCommit().semantic_frame;
  const auto link = std::ranges::find(semantics->nodes, SemanticRole::Link, &SemanticNode::role);
  REQUIRE(link != semantics->nodes.end());
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter}));
  REQUIRE(activated_link);
  REQUIRE(activated_link->range == TextRange{6, 12});
  activated_link.reset();
  REQUIRE(window.CoreRuntime().PerformSemanticAction(link->id, {SemanticActionKind::Activate, {}}));
  REQUIRE(activated_link);
  REQUIRE(activated_link->range == TextRange{6, 12});
}

TEST_CASE("TextOverflowSkipsParagraphWhenAllLinksAreOmitted") {
  ParagraphPlatform platform;
  const auto source = AttributedText::FromRanges("first\nsecond", {}, {
      {{6, 12}, Uri("https://example.com/second")},
  });
  bool show_link = false;
  bool link_focused = false;
  bool button_focused = false;
  metric_root = [&] {
    return Column {
      Text(source).LineLimit(show_link ? 2 : 1).With(Frame{.width = 110.0F})
          .On<ViewEvents::FocusChanged>([&](bool focused) { link_focused = focused; }),
      Button("Next").On<ViewEvents::FocusChanged>([&](bool focused) { button_focused = focused; }),
    };
  };
  UiWindow window(MetricApp, platform);
  window.SetWindowMetrics({.viewport = {220.0F, 100.0F}});
  const auto semantics = window.BuildCommit().semantic_frame;
  const auto paragraph = std::ranges::find(semantics->nodes, "first\nsecond", &SemanticNode::label);
  REQUIRE(paragraph != semantics->nodes.end());
  REQUIRE((paragraph->actions & SemanticActionMask(SemanticActionKind::Focus)) == 0);
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(button_focused);
  REQUIRE_FALSE(link_focused);

  show_link = true;
  window.InvalidateRoot();
  window.BuildFrame();
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab, .modifiers = {.shift = true}}));
  REQUIRE(link_focused);

  show_link = false;
  window.InvalidateRoot();
  window.BuildFrame();
  REQUIRE_FALSE(link_focused);
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab}));
  REQUIRE(button_focused);
}

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
  std::optional<TextLayout> snapshots[2];
  int compositions[2]{};
  int slot = 0;
  metric_root = [&]() -> View {
    ++compositions[slot];
    const TextStyle style{Font::System(20.0F), Color::Black()};
    auto& measurer = UseTextMeasurer();
    observed[slot] = measurer.MeasureText("measure", style).size.width;
    snapshots[slot] = measurer.CreateTextLayout(AttributedText("measure"), style, 300.0F);
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
  const TextLayout first_snapshot = *snapshots[0];
  const TextLayout second_snapshot = *snapshots[1];
  const auto second_layouts = second_platform.layouts;

  first_platform.scale = 1.5F;
  detail::InternalAccess::InvalidateTextMetrics(first_platform);
  slot = 0;
  REQUIRE(FindText(first.BuildFrame(), "paragraph")->rect.width == Catch::Approx(initial_width * 1.5F));
  slot = 1;
  REQUIRE(FindText(second.BuildFrame(), "paragraph")->rect.width == initial_width);
  REQUIRE(observed[0] == Catch::Approx(initial_observed * 1.5F));
  REQUIRE(observed[1] == initial_observed);
  REQUIRE(snapshots[0]->Metrics().size.width == Catch::Approx(first_snapshot.Metrics().size.width * 1.5F));
  REQUIRE(snapshots[1]->Lines().data() == second_snapshot.Lines().data());
  REQUIRE(compositions[0] == 2);
  REQUIRE(compositions[1] == 1);
  REQUIRE(second_platform.layouts == second_layouts);
}
#endif

} // namespace huxerui::test
