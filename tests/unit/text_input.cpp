#include <catch2/catch_amalgamated.hpp>

#include <huxerui/text_input.h>

#include <cstdint>
#include <string>
#include <vector>

#include "text_input_internal.h"

namespace huxerui::test {
namespace {

detail::TextFieldEditingState EditingState(TextEditingValue value) {
  return {
      std::move(value),
      std::nullopt,
  };
}

TextInputCommand Command(TextInputCommandKind kind) {
  TextInputCommand command;
  command.kind = kind;
  return command;
}

detail::TextInputReductionResult
Reduce(const detail::TextFieldEditingState& state, std::vector<TextInputCommand> commands) {
  return detail::ReduceTextInputCommands(state, commands);
}

} // namespace

TEST_CASE("TextEditingValueUsesUtf16Offsets") {
  const TextEditingValue value = TextEditingValue::FromText("A\xF0\x9F\x98\x80中");

  REQUIRE(value.selection == TextSelection{4, 4});
  REQUIRE(value.selection.IsCollapsed());
  REQUIRE(value.selection.Range() == TextRange{4, 4});
  REQUIRE(detail::Utf16Length(value.text) == 4);
  REQUIRE(detail::IsValidTextEditingValue(value));

  const TextSelection backward{4, 1, TextAffinity::Upstream};
  REQUIRE(backward.Range() == TextRange{1, 4});
  REQUIRE_FALSE(backward.IsCollapsed());

  const std::string invalid_utf8{"\xF0\x28\x8C\x28", 4};
  REQUIRE_THROWS_AS(TextEditingValue::FromText(invalid_utf8), std::invalid_argument);
}

TEST_CASE("TextWordBoundariesUseUtf16Offsets") {
  const std::string text = "go \xF0\x9F\x98\x80 now";

  REQUIRE(detail::PreviousWordStart(text, 9) == 6);
  REQUIRE(detail::PreviousWordStart(text, 6) == 3);
  REQUIRE(detail::PreviousWordStart(text, 3) == 0);
  REQUIRE(detail::NextWordEnd(text, 0) == 2);
  REQUIRE(detail::NextWordEnd(text, 2) == 5);
  REQUIRE(detail::NextWordStart(text, 0) == 3);
  REQUIRE(detail::NextWordStart(text, 3) == 6);
  REQUIRE_FALSE(detail::PreviousWordStart(text, 4).has_value());
  REQUIRE_FALSE(detail::NextWordEnd(text, 4).has_value());
  REQUIRE_FALSE(detail::NextWordStart(text, 4).has_value());
}

TEST_CASE("TextInputCommitReplacesSelection") {
  TextEditingValue value = TextEditingValue::FromText("Hello");
  value.selection = {1, 4};

  TextInputCommand commit = Command(TextInputCommandKind::CommitText);
  commit.text = "i";

  const auto result = Reduce(EditingState(value), {commit});

  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.changed);
  REQUIRE(
      result.state.value == TextEditingValue{
                                "Hio",
                                {2, 2},
                                std::nullopt,
                            }
  );
}

TEST_CASE("TextInputBatchIsAtomic") {
  TextEditingValue value = TextEditingValue::FromText("A\xF0\x9F\x98\x80");

  TextInputCommand selection = Command(TextInputCommandKind::SetSelection);
  selection.selection_after = TextSelection{1, 1};

  TextInputCommand invalid_commit = Command(TextInputCommandKind::CommitText);
  invalid_commit.target = TextRange{2, 3};
  invalid_commit.text = "x";

  const detail::TextFieldEditingState initial = EditingState(value);
  const auto result = Reduce(initial, {selection, invalid_commit});

  REQUIRE(result.status == detail::TextInputReductionStatus::Rejected);
  REQUIRE_FALSE(result.changed);
  REQUIRE(result.state == initial);
}

TEST_CASE("TextInputCompositionUpdatesAndCommits") {
  TextEditingValue value = TextEditingValue::FromText("ab");
  value.selection = {1, 1};

  TextInputCommand first_update = Command(TextInputCommandKind::UpdateComposition);
  first_update.text = "你";

  auto result = Reduce(EditingState(value), {first_update});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "a你b");
  REQUIRE(result.state.value.selection == TextSelection{2, 2});
  REQUIRE(result.state.value.composition == TextRange{1, 2});

  TextInputCommand second_update = Command(TextInputCommandKind::UpdateComposition);
  second_update.text = "你好";

  result = Reduce(result.state, {second_update});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "a你好b");
  REQUIRE(result.state.value.selection == TextSelection{3, 3});
  REQUIRE(result.state.value.composition == TextRange{1, 3});

  TextInputCommand commit = Command(TextInputCommandKind::CommitText);
  commit.text = "好";

  result = Reduce(result.state, {commit});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "a好b");
  REQUIRE(result.state.value.selection == TextSelection{2, 2});
  REQUIRE_FALSE(result.state.value.composition.has_value());
  REQUIRE_FALSE(result.state.composition_baseline.has_value());
}

TEST_CASE("TextInputCancelRestoresCompositionBaseline") {
  TextEditingValue value = TextEditingValue::FromText("abcd");
  value.selection = {1, 3};

  TextInputCommand begin = Command(TextInputCommandKind::BeginComposition);
  begin.target = TextRange{1, 3};

  TextInputCommand update = Command(TextInputCommandKind::UpdateComposition);
  update.text = "XY";

  TextInputCommand cancel = Command(TextInputCommandKind::CancelComposition);

  const auto result = Reduce(EditingState(value), {begin, update, cancel});

  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value == value);
  REQUIRE_FALSE(result.state.composition_baseline.has_value());
}

TEST_CASE("TextInputUpdatesPartOfComposition") {
  TextEditingValue value = TextEditingValue::FromText("abc");
  value.selection = {1, 1};

  TextInputCommand initial = Command(TextInputCommandKind::UpdateComposition);
  initial.text = "xy";

  auto result = Reduce(EditingState(value), {initial});
  REQUIRE(result.state.value.text == "axybc");

  TextInputCommand partial = Command(TextInputCommandKind::UpdateComposition);
  partial.coordinate_space = TextInputCoordinateSpace::Composition;
  partial.target = TextRange{1, 2};
  partial.text = "z";

  result = Reduce(result.state, {partial});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "axzbc");
  REQUIRE(result.state.value.composition == TextRange{1, 3});
  REQUIRE(result.state.value.selection == TextSelection{3, 3});
}

TEST_CASE("TextInputUpdateCanStartAtExplicitTarget") {
  TextEditingValue value = TextEditingValue::FromText("abcd");
  value.selection = {4, 4};

  TextInputCommand update = Command(TextInputCommandKind::UpdateComposition);
  update.target = TextRange{1, 3};
  update.text = "X";
  update.selection_after = TextSelection{2, 2, TextAffinity::Upstream};

  const auto result = Reduce(EditingState(value), {update});

  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "aXd");
  REQUIRE(result.state.value.selection == TextSelection{2, 2, TextAffinity::Upstream});
  REQUIRE(result.state.value.composition == TextRange{1, 2});
}

TEST_CASE("TextInputFinishKeepsProvisionalText") {
  TextEditingValue value = TextEditingValue::FromText("ab");
  value.selection = {1, 1};

  TextInputCommand update = Command(TextInputCommandKind::UpdateComposition);
  update.text = "中";
  TextInputCommand finish = Command(TextInputCommandKind::FinishComposition);

  const auto result = Reduce(EditingState(value), {update, finish});

  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "a中b");
  REQUIRE(result.state.value.selection == TextSelection{2, 2});
  REQUIRE_FALSE(result.state.value.composition.has_value());
}

TEST_CASE("TextInputDeletesSurroundingUtf16Text") {
  TextEditingValue value = TextEditingValue::FromText("abcdef");
  value.selection = {3, 3};

  TextInputCommand deletion = Command(TextInputCommandKind::DeleteSurrounding);
  deletion.delete_before = 2;
  deletion.delete_after = 1;

  const auto result = Reduce(EditingState(value), {deletion});

  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "aef");
  REQUIRE(result.state.value.selection == TextSelection{1, 1});
}

TEST_CASE("TextInputCodePointDeletionPreservesUtf8") {
  TextEditingValue value = TextEditingValue::FromText(
      "A\xF0\x9F\x98\x80"
      "B"
  );
  value.selection = {3, 3};

  TextInputCommand deletion = Command(TextInputCommandKind::DeleteSurrounding);
  deletion.delete_before = 1;
  deletion.delete_unit = TextInputUnit::UnicodeCodePoint;

  const auto result = Reduce(EditingState(value), {deletion});

  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "AB");
  REQUIRE(result.state.value.selection == TextSelection{1, 1});
}

TEST_CASE("TextInputRejectsDeletionInsideSurrogatePair") {
  TextEditingValue value = TextEditingValue::FromText(
      "A\xF0\x9F\x98\x80"
      "B"
  );
  value.selection = {3, 3};

  TextInputCommand deletion = Command(TextInputCommandKind::DeleteSurrounding);
  deletion.delete_before = 1;

  const detail::TextFieldEditingState initial = EditingState(value);
  const auto result = Reduce(initial, {deletion});

  REQUIRE(result.status == detail::TextInputReductionStatus::Rejected);
  REQUIRE(result.state == initial);
}

TEST_CASE("TextInputRejectsUnknownAffinity") {
  TextEditingValue value = TextEditingValue::FromText("text");

  TextInputCommand selection = Command(TextInputCommandKind::SetSelection);
  selection.selection_after = TextSelection{
      1,
      1,
      static_cast<TextAffinity>(42),
  };

  const detail::TextFieldEditingState initial = EditingState(value);
  const auto result = Reduce(initial, {selection});

  REQUIRE(result.status == detail::TextInputReductionStatus::Rejected);
  REQUIRE(result.state == initial);
}

TEST_CASE("TextInputCompositionDeletionCanBeCancelled") {
  TextEditingValue value = TextEditingValue::FromText("ab");
  value.selection = {1, 1};

  TextInputCommand update = Command(TextInputCommandKind::UpdateComposition);
  update.text = "xy";

  auto result = Reduce(EditingState(value), {update});
  REQUIRE(result.state.value.text == "axyb");

  TextInputCommand deletion = Command(TextInputCommandKind::DeleteSurrounding);
  deletion.delete_before = 1;
  deletion.delete_unit = TextInputUnit::UnicodeCodePoint;

  result = Reduce(result.state, {deletion});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "axb");
  REQUIRE(result.state.value.composition == TextRange{1, 2});

  TextInputCommand cancel = Command(TextInputCommandKind::CancelComposition);
  result = Reduce(result.state, {cancel});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value == value);
}

TEST_CASE("TextInputCancelPreservesDeletionOutsideComposition") {
  TextEditingValue value = TextEditingValue::FromText("0ab");
  value.selection = {2, 3};

  TextInputCommand begin = Command(TextInputCommandKind::BeginComposition);
  begin.target = TextRange{2, 3};
  TextInputCommand update = Command(TextInputCommandKind::UpdateComposition);
  update.text = "XY";

  auto result = Reduce(EditingState(value), {begin, update});
  REQUIRE(result.state.value.text == "0aXY");

  TextInputCommand selection = Command(TextInputCommandKind::SetSelection);
  selection.selection_after = TextSelection{2, 2};
  TextInputCommand deletion = Command(TextInputCommandKind::DeleteSurrounding);
  deletion.delete_before = 1;
  deletion.delete_unit = TextInputUnit::UnicodeCodePoint;
  TextInputCommand cancel = Command(TextInputCommandKind::CancelComposition);

  result = Reduce(result.state, {selection, deletion, cancel});
  REQUIRE(result.status == detail::TextInputReductionStatus::Accepted);
  REQUIRE(result.state.value.text == "0b");
  REQUIRE(result.state.value.selection == TextSelection{1, 2});
}

} // namespace huxerui::test
