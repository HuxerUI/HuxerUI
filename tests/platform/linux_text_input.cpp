#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>

#include <string>
#include <utility>
#include <vector>

#include "linux_text_input_internal.h"

namespace huxerui::test {

TEST_CASE("LinuxTextInputMapsUtf8BytesToUtf16Offsets") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 0) == TextOffset{0});
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 1) == TextOffset{1});
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 4) == TextOffset{2});
  REQUIRE(detail::LinuxUtf8ByteToUtf16(text, 8) == TextOffset{4});
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16(text, 2).has_value());
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16(text, 9).has_value());
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16(text, -1).has_value());
  REQUIRE_FALSE(detail::LinuxUtf8ByteToUtf16("\xFF", 1).has_value());
}

TEST_CASE("LinuxTextInputMapsUtf16OffsetsToUtf8Bytes") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 0) == 0);
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 1) == 1);
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 2) == 4);
  REQUIRE(detail::LinuxUtf16ToUtf8Byte(text, 4) == 8);
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte(text, 3).has_value());
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte(text, 5).has_value());
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte(text, -1).has_value());
  REQUIRE_FALSE(detail::LinuxUtf16ToUtf8Byte("\xFF", 1).has_value());
}

TEST_CASE("LinuxTextInputMapsSdlEditingCharacterRangesToUtf16") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80z";
  REQUIRE((detail::LinuxTextEditingRangeToUtf16(text, 0, 0) == TextRange{0, 0}));
  REQUIRE((detail::LinuxTextEditingRangeToUtf16(text, 1, 2) == TextRange{1, 4}));
  REQUIRE((detail::LinuxTextEditingRangeToUtf16(text, 3, 1) == TextRange{4, 5}));
  REQUIRE_FALSE(detail::LinuxTextEditingRangeToUtf16(text, -1, 1).has_value());
  REQUIRE_FALSE(detail::LinuxTextEditingRangeToUtf16(text, 1, -1).has_value());
  REQUIRE_FALSE(detail::LinuxTextEditingRangeToUtf16(text, 5, 0).has_value());
  REQUIRE_FALSE(detail::LinuxTextEditingRangeToUtf16("\xFF", 0, 1).has_value());
}

TEST_CASE("LinuxTextInputTranslatesCompositionAndCommitEventsIntoRuntimeCommands") {
  detail::LinuxTextInputCommandHandler handler;
  handler.Start({}, {.selection = {5, 5}});
  std::vector<std::vector<TextInputCommand>> applied;
  const auto apply = [&applied](std::vector<TextInputCommand> commands) {
    applied.push_back(std::move(commands));
    return TextInputApplyResult{.result_code = TextInputResultCode::Ok};
  };

  const std::string editing = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  handler.HandleTextEditing(editing, 1, 2, apply);

  REQUIRE(handler.Composing());
  REQUIRE(applied.size() == 2);
  REQUIRE(applied[0].size() == 1);
  REQUIRE(applied[0][0].kind == TextInputCommandKind::BeginComposition);
  REQUIRE((applied[0][0].target == TextRange{5, 5}));
  REQUIRE(applied[1].size() == 1);
  REQUIRE(applied[1][0].kind == TextInputCommandKind::UpdateComposition);
  REQUIRE(applied[1][0].text == editing);
  REQUIRE((applied[1][0].selection_after == TextSelection{6, 9, TextAffinity::Downstream}));

  handler.HandleTextEditing({}, 0, 0, apply);
  REQUIRE_FALSE(handler.Composing());
  REQUIRE(applied.size() == 3);
  REQUIRE(applied.back()[0].kind == TextInputCommandKind::FinishComposition);

  handler.HandleTextInput("\xE4\xBD\xA0", apply);
  REQUIRE(applied.size() == 4);
  REQUIRE(applied.back()[0].kind == TextInputCommandKind::CommitText);
  REQUIRE(applied.back()[0].text == "\xE4\xBD\xA0");
}

TEST_CASE("LinuxTextInputStopsCompositionWhenTheRuntimeRejectsBegin") {
  detail::LinuxTextInputCommandHandler handler;
  handler.Start({}, {.selection = {2, 4}});
  std::vector<TextInputCommand> applied;

  handler.HandleTextEditing("candidate", 0, 9, [&applied](std::vector<TextInputCommand> commands) {
    applied.insert(applied.end(), commands.begin(), commands.end());
    return TextInputApplyResult{.result_code = TextInputResultCode::Rejected};
  });

  REQUIRE_FALSE(handler.Composing());
  REQUIRE(applied.size() == 1);
  REQUIRE(applied[0].kind == TextInputCommandKind::BeginComposition);
  REQUIRE((applied[0].target == TextRange{2, 4}));
}

TEST_CASE("LinuxTextInputPreservesCompositionWhenTheRuntimeRejectsLaterCommands") {
  detail::LinuxTextInputCommandHandler handler;
  handler.Start({}, {.selection = {2, 2}, .composition = TextRange{1, 3}});
  std::vector<TextInputCommandKind> applied;
  const auto reject = [&applied](std::vector<TextInputCommand> commands) {
    REQUIRE(commands.size() == 1);
    applied.push_back(commands[0].kind);
    return TextInputApplyResult{.result_code = TextInputResultCode::Rejected};
  };

  handler.HandleTextEditing("candidate", 0, 9, reject);
  REQUIRE(handler.Composing());
  handler.HandleTextEditing({}, 0, 0, reject);
  REQUIRE(handler.Composing());
  handler.HandleTextInput("commit", reject);
  REQUIRE(handler.Composing());
  REQUIRE(
      (applied == std::vector{
                      TextInputCommandKind::UpdateComposition,
                      TextInputCommandKind::FinishComposition,
                      TextInputCommandKind::CommitText,
                  })
  );
}

TEST_CASE("LinuxTextInputKeepsSynchronousRuntimeStateAuthoritative") {
  detail::LinuxTextInputCommandHandler handler;
  handler.Start({}, {.selection = {2, 2}, .composition = TextRange{1, 3}});

  handler.HandleTextEditing("candidate", 0, 9, [&handler](std::vector<TextInputCommand> commands) {
    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0].kind == TextInputCommandKind::UpdateComposition);
    handler.Update({.selection = {3, 3}});
    return TextInputApplyResult{.result_code = TextInputResultCode::Ok};
  });

  REQUIRE_FALSE(handler.Composing());
}

TEST_CASE("LinuxTextInputEnforcesSecureAndReadOnlyEventPolicies") {
  detail::LinuxTextInputCommandHandler handler;
  std::vector<TextInputCommand> applied;
  const auto apply = [&applied](std::vector<TextInputCommand> commands) {
    applied.insert(applied.end(), commands.begin(), commands.end());
    return TextInputApplyResult{.result_code = TextInputResultCode::Ok};
  };

  handler.Start({.secure = true}, {.selection = {1, 1}});
  handler.HandleTextEditing("secret", 0, 6, apply);
  REQUIRE(applied.empty());
  handler.HandleTextInput("secret", apply);
  REQUIRE(applied.size() == 1);
  REQUIRE(applied[0].kind == TextInputCommandKind::CommitText);

  applied.clear();
  handler.Start({.read_only = true}, {.selection = {1, 1}});
  handler.HandleTextEditing("blocked", 0, 7, apply);
  handler.HandleTextInput("blocked", apply);
  REQUIRE(applied.empty());
}

} // namespace huxerui::test
