#import <AppKit/AppKit.h>

#include "appkit_text_input.h"
#include "appkit_renderer.h"
#include "text/text_internal.h"
#include "runtime_test_support.h"
#include "font_test_support.h"

namespace huxerui::test {
namespace {

State<TextEditingValue> mac_text_field_value;
int mac_first_submissions = 0;

class NativeLayoutPlatform final : public TestPlatform {
public:
  FontMetrics Metrics(const Font& font) override { return renderer_.Metrics(font); }
  TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style, float width,
      const TextLayoutOptions& options) override {
    return renderer_.MeasureText(text, style, width, options);
  }
  std::unique_ptr<detail::TextLayoutBackend> CreatePlatformTextLayout(
      const AttributedText& text, const TextStyle& style, float width, const TextLayoutOptions& options) override {
    return renderer_.CreatePlatformTextLayout(text, style, width, options);
  }

private:
  detail::AppKitRenderer renderer_;
};

View MacTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText(""));
  mac_text_field_value = value;
  return TextField(value)
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
      .With(huxerui::Frame{160.0F, 40.0F});
}

View MacSecureTextFieldApp() {
  auto value = UseState(TextEditingValue::FromText("secret"));
  mac_text_field_value = value;
  return TextField(value)
      .Secure()
      .OnChanged([value](const TextEditingValue& changed) mutable { value = changed; })
      .With(huxerui::Frame{160.0F, 40.0F});
}

View MacNextTextFieldApp() {
  auto first = UseState(TextEditingValue::FromText(""));
  auto second = UseState(TextEditingValue::FromText(""));
  return Column {
    TextField(first)
        .InputConfiguration({.action = TextInputAction::Next})
        .OnChanged([first](const TextEditingValue& changed) mutable { first = changed; })
        .OnSubmitted([] { ++mac_first_submissions; })
        .With(huxerui::Frame{160.0F, 40.0F}),
    TextField(second)
        .OnChanged([second](const TextEditingValue& changed) mutable { second = changed; })
        .With(huxerui::Frame{160.0F, 40.0F}),
  };
}

TEST_CASE("TestMacTextInputClientMapsCompositionAndCommands") {
  @autoreleasepool {
    TestPlatform platform;
    auto runtime = std::make_unique<UiWindow>(MacTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->CoreRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime->BuildFrame();
    ClickAt(*runtime, {20.0F, 20.0F});
    REQUIRE(input->IsActive());

    id<NSTextInputClient> client = input->InputContext().client;
    REQUIRE(client != nil);

    [client setMarkedText:@"ni"
            selectedRange:NSMakeRange(2, 0)
          replacementRange:NSMakeRange(NSNotFound, 0)];
    TextInputContext context = runtime->QueryTextInputContext(1, 0, 2);
    REQUIRE(context.text == "ni");
    REQUIRE(context.composition == TextRange{0, 2});
    REQUIRE(context.selection == TextSelection{2, 2});
    REQUIRE([client markedRange].location == 0);
    REQUIRE([client markedRange].length == 2);

    [client setMarkedText:@"你"
            selectedRange:NSMakeRange(1, 0)
          replacementRange:NSMakeRange(NSNotFound, 0)];
    context = runtime->QueryTextInputContext(1, 0, 1);
    REQUIRE(context.text == "你");
    REQUIRE(context.composition == TextRange{0, 1});

    [client insertText:@"你" replacementRange:NSMakeRange(NSNotFound, 0)];
    context = runtime->QueryTextInputContext(1, 0, 1);
    REQUIRE(context.text == "你");
    REQUIRE_FALSE(context.composition.has_value());

    [client setMarkedText:@"hao"
            selectedRange:NSMakeRange(3, 0)
          replacementRange:NSMakeRange(NSNotFound, 0)];
    [client unmarkText];
    context = runtime->QueryTextInputContext(1, 0, 4);
    REQUIRE(context.text == "你hao");
    REQUIRE_FALSE(context.composition.has_value());

    [client doCommandBySelector:@selector(deleteBackward:)];
    context = runtime->QueryTextInputContext(1, 0, 3);
    REQUIRE(context.text == "你ha");

    runtime.reset();
    platform.platform_text_input = nullptr;
    input.reset();
  }
}

TEST_CASE("TestMacTextInputClientPreservesWordEditingSelectors") {
  @autoreleasepool {
    TestPlatform platform;
    auto runtime = std::make_unique<UiWindow>(MacTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->CoreRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime->BuildFrame();
    ClickAt(*runtime, {20.0F, 20.0F});

    id<NSTextInputClient> client = input->InputContext().client;
    REQUIRE(client != nil);
    [client insertText:@"alpha beta" replacementRange:NSMakeRange(NSNotFound, 0)];
    [client doCommandBySelector:@selector(moveWordBackward:)];
    REQUIRE(runtime->QueryTextInputContext(1, 0, 10).selection == TextSelection{6, 6});

    [client doCommandBySelector:@selector(moveToEndOfDocument:)];
    [client doCommandBySelector:@selector(deleteWordBackward:)];
    const TextInputContext context = runtime->QueryTextInputContext(1, 0, 10);
    REQUIRE(context.text == "alpha ");
    REQUIRE(context.selection == TextSelection{6, 6});

    runtime.reset();
    platform.platform_text_input = nullptr;
    input.reset();
  }
}

TEST_CASE("TestMacTextInputRoutesHistoryShortcutsBeforeInterpretation") {
  @autoreleasepool {
    TestPlatform platform;
    auto runtime = std::make_unique<UiWindow>(MacTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->CoreRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime->BuildFrame();
    ClickAt(*runtime, {20.0F, 20.0F});

    id<NSTextInputClient> client = input->InputContext().client;
    REQUIRE(client != nil);
    [client insertText:@"abc" replacementRange:NSMakeRange(NSNotFound, 0)];
    REQUIRE(runtime->QueryTextInputContext(1, 0, 3).text == "abc");

    NSEvent* undo = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:NSEventModifierFlagCommand
                                   timestamp:0.0
                                windowNumber:0
                                     context:nil
                                  characters:@"z"
                 charactersIgnoringModifiers:@"z"
                                   isARepeat:NO
                                     keyCode:6];
    REQUIRE(input->HandleEvent(undo));
    REQUIRE(runtime->QueryTextInputContext(1, 0, 3).text.empty());

    NSEvent* redo = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:NSEventModifierFlagCommand | NSEventModifierFlagShift
                                   timestamp:0.0
                                windowNumber:0
                                     context:nil
                                  characters:@"Z"
                 charactersIgnoringModifiers:@"z"
                                   isARepeat:NO
                                     keyCode:6];
    REQUIRE(input->HandleEvent(redo));
    REQUIRE(runtime->QueryTextInputContext(1, 0, 3).text == "abc");

    runtime.reset();
    platform.platform_text_input = nullptr;
    input.reset();
  }
}

TEST_CASE("TestMacSecureTextInputDoesNotExposeAttributedText") {
  @autoreleasepool {
    TestPlatform platform;
    auto runtime = std::make_unique<UiWindow>(MacSecureTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->CoreRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetWindowMetrics({.viewport = {200.0F, 80.0F}});
    runtime->BuildFrame();
    ClickAt(*runtime, {20.0F, 20.0F});

    id<NSTextInputClient> client = input->InputContext().client;
    REQUIRE(client != nil);
    NSRange actual = NSMakeRange(0, 0);
    REQUIRE([client attributedSubstringForProposedRange:NSMakeRange(0, 6) actualRange:&actual] == nil);
    REQUIRE(actual.location == NSNotFound);
    REQUIRE(runtime->QueryTextInputContext(1, 0, 6).text == "secret");

    runtime.reset();
    platform.platform_text_input = nullptr;
    input.reset();
  }
}

TEST_CASE("TestMacTextInputNextSelectorUsesRuntimeFocusAction") {
  @autoreleasepool {
    mac_first_submissions = 0;
    TestPlatform platform;
    auto runtime = std::make_unique<UiWindow>(MacNextTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 100.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->CoreRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetWindowMetrics({.viewport = {200.0F, 100.0F}});
    runtime->BuildFrame();
    ClickAt(*runtime, {20.0F, 20.0F});

    id<NSTextInputClient> client = input->InputContext().client;
    REQUIRE(client != nil);
    [client doCommandBySelector:@selector(insertNewline:)];
    REQUIRE(mac_first_submissions == 1);
    REQUIRE(runtime->QueryTextInputContext(1, 0, 0).result_code == TextInputResultCode::SessionMismatch);
    REQUIRE(runtime->QueryTextInputContext(2, 0, 0).result_code == TextInputResultCode::Ok);

    runtime.reset();
    platform.platform_text_input = nullptr;
    input.reset();
  }
}

} // namespace

TEST_CASE("MacTextLayoutAlignsExplicitNoWrapLinesWithinAvailableWidth") {
  @autoreleasepool {
    detail::AppKitRenderer renderer;
    const TextStyle style{Font::Monospace(20.0F), Color::Black()};
    for (const TextDirection direction : {TextDirection::LeftToRight, TextDirection::RightToLeft}) {
      const std::string text = direction == TextDirection::LeftToRight ? "ab\nabcd" : "אב\nאבגד";
      for (const TextAlign align : {TextAlign::Leading, TextAlign::Center, TextAlign::Trailing}) {
        const TextLayoutOptions options{.shaping = {.direction = direction}, .align = align, .wrap = TextWrap::NoWrap};
        const auto narrow = renderer.CreatePlatformTextLayout(text, style, 160.0F, options);
        const auto wide = renderer.CreatePlatformTextLayout(text, style, 240.0F, options);
        const float shift = align == TextAlign::Center ? 40.0F
            : ((align == TextAlign::Trailing) == (direction == TextDirection::LeftToRight) ? 80.0F : 0.0F);
        REQUIRE(narrow->Measure() == wide->Measure());
        for (const TextOffset offset : {TextOffset{0}, TextOffset{3}}) {
          const Rect first = narrow->CaretRect(offset, TextAffinity::Downstream);
          const Rect second = wide->CaretRect(offset, TextAffinity::Downstream);
          REQUIRE(second.x - first.x == Catch::Approx(shift));
          REQUIRE(second.y == first.y);
          const TextPosition hit = wide->HitTest({second.x, second.y + second.height * 0.5F});
          REQUIRE(hit.offset == offset);
        }
        const auto first = narrow->RangeRects({0, 7});
        const auto second = wide->RangeRects({0, 7});
        REQUIRE(first.size() == second.size());
        for (std::size_t index = 0; index < first.size(); ++index) {
          REQUIRE(second[index].x - first[index].x == Catch::Approx(shift));
        }
      }
    }
  }
}

TEST_CASE("MacRetainedTextLayoutKeepsEmptyLinesAndWrappedCaretAffinity") {
  @autoreleasepool {
    NativeLayoutPlatform platform;
    UiWindow window(MacTextFieldApp, platform);
    const TextStyle style{Font::Monospace(20.0F), Color::Black()};
    const auto layout = platform.CreateTextLayout(AttributedText("alpha beta\n"), style, 80.0F);
    const auto lines = layout.Lines();
    REQUIRE(lines.size() == 3);
    CAPTURE(lines.back().range.start, lines.back().range.end);
    REQUIRE(lines.front().range.start == 0);
    REQUIRE(lines.front().range.end == lines[1].range.start);
    REQUIRE(lines[1].range.end == 10);
    REQUIRE(lines.back().range == TextRange{11, 11});
    const TextOffset wrap = lines[1].range.start;
    REQUIRE(layout.LineIndex({wrap, TextAffinity::Upstream}) == 0);
    REQUIRE(layout.LineIndex({wrap, TextAffinity::Downstream}) == 1);
    REQUIRE(layout.CaretRect({wrap, TextAffinity::Upstream}).y < layout.CaretRect({wrap, TextAffinity::Downstream}).y);
    const Rect end = layout.CaretRect({11});
    REQUIRE(end.y == Catch::Approx(lines.back().bounds.y));
    REQUIRE(layout.CaretRect({11, TextAffinity::Upstream}) == end);
    REQUIRE(layout.HitTest({end.x, end.y + end.height * 0.5F}).offset == 11);
    const auto empty = platform.CreateTextLayout(AttributedText{}, style, 80.0F);
    REQUIRE(empty.Lines().size() == 1);
    REQUIRE(empty.CaretRect({0}).height > 0.0F);

    for (const std::string separator : {"\n", "\r", "\r\n", "\u2028", "\u2029"}) {
      CAPTURE(separator);
      const TextOffset break_length = separator == "\r\n" ? 2 : 1;
      const auto no_wrap = platform.CreateTextLayout(AttributedText("aa" + separator + "bb" + separator),
          style, 200.0F, {.wrap = TextWrap::NoWrap});
      const auto rows = no_wrap.Lines();
      REQUIRE(rows.size() == 3);
      REQUIRE(rows[0].range == TextRange{0, 2});
      REQUIRE(rows[1].range == TextRange{2 + break_length, 4 + break_length});
      REQUIRE(rows[2].range == TextRange{4 + 2 * break_length, 4 + 2 * break_length});
      const Rect& first = rows.front().bounds;
      REQUIRE(no_wrap.HitTest({200.0F, first.y + first.height * 0.5F}).offset == 2);
      REQUIRE(no_wrap.Metrics().size.height > rows[2].bounds.y);
    }

    const AttributedText crlf_text("🙂\r\n\r\n中\r\n");
    const auto crlf = platform.CreateTextLayout(crlf_text, style, 200.0F);
    const auto native = platform.CreatePlatformTextLayout(crlf_text, style, 200.0F, {});
    const auto rows = crlf.Lines();
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].range == TextRange{0, 2});
    CHECK(rows[1].range == TextRange{4, 4});
    CHECK(rows[2].range == TextRange{6, 7});
    CHECK(rows[3].range == TextRange{9, 9});
    CHECK(crlf.LineIndex({4, TextAffinity::Upstream}) == 1);
    CHECK(crlf.NextCaretOffset(0) == 2);
    CHECK(crlf.PreviousCaretOffset(4) == native->PreviousCaretOffset(4));
  }
}

TEST_CASE("MacInlineObjectLayoutKeepsFractionGeometryAndCaretEdgesTogether") {
  @autoreleasepool {
    NativeLayoutPlatform platform;
    UiWindow window(MacTextFieldApp, platform);
    const TextStyle style{Font::System(16.0F), Color::Black()};
    const auto text = AttributedText("中文👩‍💻\uFFFC继续").WithInlineObjects({{7, 36.0F, 24.0F, 12.0F}});
    const TextLayoutOptions options{.shaping = {.locale = "zh-Hans"}};
    const auto wide = platform.CreateTextLayout(text, style, 300.0F, options);
    REQUIRE(wide.Lines().size() == 1);
    REQUIRE(wide.InlineObjectRect(7).has_value());
    REQUIRE_FALSE(wide.InlineObjectRect(8).has_value());
    REQUIRE(wide.NextCaretOffset(2) == 7);
    REQUIRE(wide.NextCaretOffset(7) == 8);
    REQUIRE(wide.PreviousCaretOffset(8) == 7);

    for (const float width : {300.0F, wide.InlineObjectRect(7)->x + 35.0F, 30.0F}) {
      CAPTURE(width);
      const auto layout = platform.CreateTextLayout(text, style, width, options);
      REQUIRE(layout.InlineObjectRect(7).has_value());
      const Rect box = *layout.InlineObjectRect(7);
      const auto line_index = layout.LineIndex({7});
      const auto& line = layout.Lines()[line_index];
      CHECK((line_index == 0) == (width == 300.0F));
      CHECK(box.width == 36.0F);
      CHECK(box.height == 36.0F);
      CHECK(box.y + 24.0F == Catch::Approx(line.baseline));
      CHECK(line.bounds.height >= box.height);
      CHECK(layout.CaretRect({7, TextAffinity::Downstream}).x == Catch::Approx(box.x));
      CHECK(layout.CaretRect({8, TextAffinity::Upstream}).x == Catch::Approx(box.x + box.width));
      CHECK(layout.HitTest({box.x + 9.0F, box.y + 18.0F}) == TextPosition{7, TextAffinity::Downstream});
      CHECK(layout.HitTest({box.x + 27.0F, box.y + 18.0F}) == TextPosition{8, TextAffinity::Upstream});
      const auto selection = layout.RangeRects({7, 8});
      REQUIRE(selection.size() == 1);
      CHECK(selection[0].x == Catch::Approx(box.x));
      CHECK(selection[0].width == Catch::Approx(box.width));
      CHECK(selection[0].y <= box.y + 0.01F);
      CHECK(selection[0].y + selection[0].height >= box.y + box.height - 0.01F);
      CHECK(platform.MeasureText(text, style, width, options) == layout.Metrics());
    }

    const auto resized = text.WithInlineObjects({{7, 48.0F, 32.0F, 12.0F}});
    const auto changed = platform.CreateTextLayout(resized, style, 300.0F, options);
    CHECK(changed.InlineObjectRect(7).value().width == 48.0F);
    CHECK(changed.Metrics().size.height > wide.Metrics().size.height);
    CHECK(changed.CaretRect({9}).x > wide.CaretRect({9}).x);

    const auto rtl_text = AttributedText("א\uFFFCב").WithInlineObjects({{1, 36.0F, 24.0F, 12.0F}});
    const auto rtl = platform.CreateTextLayout(rtl_text, style, 160.0F,
        {.shaping = {.direction = TextDirection::RightToLeft}, .wrap = TextWrap::NoWrap});
    REQUIRE(rtl.InlineObjectRect(1).has_value());
    const Rect box = *rtl.InlineObjectRect(1);
    CHECK(rtl.CaretRect({1}).x == Catch::Approx(box.x + box.width));
    CHECK(rtl.CaretRect({2, TextAffinity::Upstream}).x == Catch::Approx(box.x));
    CHECK(rtl.HitTest({box.x + 9.0F, box.y + 18.0F}) == TextPosition{2, TextAffinity::Upstream});
    CHECK(rtl.HitTest({box.x + 27.0F, box.y + 18.0F}) == TextPosition{1, TextAffinity::Downstream});

    const auto adjacent = AttributedText("\uFFFC\uFFFC\u0301")
        .WithInlineObjects({{0, 36.0F, 24.0F, 12.0F}, {1, 36.0F, 24.0F, 12.0F}});
    const auto adjacent_layout = platform.CreateTextLayout(adjacent, style, 300.0F);
    CHECK(adjacent_layout.NextCaretOffset(0) == 1);
    CHECK(adjacent_layout.NextCaretOffset(1) == 2);
    CHECK(adjacent_layout.PreviousCaretOffset(2) == 1);
    CHECK(adjacent_layout.InlineObjectRect(1).value().x ==
          Catch::Approx(adjacent_layout.InlineObjectRect(0).value().x + 36.0F));
  }
}

TEST_CASE("MacCustomFontsUseSelectedBytesInMetricsRichTextAndRetainedLayouts") {
  @autoreleasepool {
    auto renderer = std::make_unique<detail::AppKitRenderer>();
    auto family = TestFontFamily();
    const auto font = Font::FromFamily(family, 20.0F);
    const TextStyle style{font, Color::Black()};
    REQUIRE(renderer->MeasureRun("AA", style, {}).advance == Catch::Approx(20.0F));
    REQUIRE(renderer->MeasureRun("AA", TextStyle{font.WithWeight(FontWeight::Bold), Color::Black()}, {}).advance ==
        Catch::Approx(32.0F));
    REQUIRE(renderer->MeasureRun("AA", TextStyle{font.WithSlant(FontSlant::Italic), Color::Black()}, {}).advance ==
        Catch::Approx(26.0F));
    const auto paragraph = AttributedText::FromRanges("AA", {{{1, 2}, {.font_weight = FontWeight::Bold}}});
    auto layout = renderer->CreatePlatformTextLayout(paragraph, style, 200.0F, {.wrap = TextWrap::NoWrap});
    REQUIRE(layout->Measure().width == Catch::Approx(26.0F));
    REQUIRE(layout->CaretRect(1, TextAffinity::Downstream).x == Catch::Approx(10.0F));
    REQUIRE(layout->CaretRect(2, TextAffinity::Upstream).x == Catch::Approx(26.0F));
    renderer.reset();
    family = {};
    float selected_width = 0.0F;
    for (const auto& rect : layout->RangeRects({0, 2})) selected_width += rect.width;
    REQUIRE(selected_width == Catch::Approx(26.0F));
    REQUIRE(layout->HitTest({20.0F, 10.0F}).offset == 2);
  }
}

} // namespace huxerui::test
