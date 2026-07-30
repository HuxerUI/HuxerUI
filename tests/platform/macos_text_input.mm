#import <AppKit/AppKit.h>

#include "appkit_text_input.h"
#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

State<TextEditingValue> mac_text_field_value;
int mac_first_submissions = 0;

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
    auto runtime = std::make_unique<Runtime>(MacTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->NativeRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetViewport({200.0F, 80.0F});
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
    auto runtime = std::make_unique<Runtime>(MacTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->NativeRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetViewport({200.0F, 80.0F});
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
    auto runtime = std::make_unique<Runtime>(MacTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->NativeRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetViewport({200.0F, 80.0F});
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
    auto runtime = std::make_unique<Runtime>(MacSecureTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 80.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->NativeRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetViewport({200.0F, 80.0F});
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
    auto runtime = std::make_unique<Runtime>(MacNextTextFieldApp, platform);
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 100.0)];
    auto input = std::make_unique<detail::MacTextInput>(runtime->NativeRuntime(), view);
    platform.platform_text_input = input.get();

    runtime->SetViewport({200.0F, 100.0F});
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
} // namespace huxerui::test
