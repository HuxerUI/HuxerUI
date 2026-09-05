#include "text_input_internal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "text_internal.h"

namespace huxerui::detail {
namespace {

struct WordSpan {
  TextRange range;
  bool word = false;
};

std::optional<std::size_t> ByteOffsetAt(std::string_view text, TextOffset utf16_offset) noexcept {
  if (utf16_offset < 0) {
    return std::nullopt;
  }

  TextOffset current = 0;
  std::size_t byte_offset = 0;
  if (utf16_offset == 0) {
    return byte_offset;
  }

  while (byte_offset < text.size()) {
    Utf8CodePoint code_point;
    if (!DecodeCodePoint(text, byte_offset, code_point)) {
      return std::nullopt;
    }
    const TextOffset width = code_point.value > 0xFFFFU ? 2 : 1;
    if (utf16_offset < current + width) {
      return std::nullopt;
    }
    current += width;
    byte_offset += code_point.byte_length;
    if (utf16_offset == current) {
      return byte_offset;
    }
  }

  return std::nullopt;
}

bool IsBoundary(std::string_view text, TextOffset offset) noexcept {
  return ByteOffsetAt(text, offset).has_value();
}

bool IsWordCodePoint(std::uint32_t value) noexcept {
  return value >= 0x80U || value == static_cast<std::uint32_t>('_') ||
         (value <= 0x7FU && std::isalnum(static_cast<unsigned char>(value)) != 0);
}

std::optional<std::vector<WordSpan>> BuildWordSpans(std::string_view text) {
  std::vector<WordSpan> spans;
  TextOffset current = 0;
  for (std::size_t byte_offset = 0; byte_offset < text.size();) {
    Utf8CodePoint code_point;
    if (!DecodeCodePoint(text, byte_offset, code_point)) {
      return std::nullopt;
    }
    const TextOffset width = code_point.value > 0xFFFFU ? 2 : 1;
    spans.push_back({
        {current, current + width},
        IsWordCodePoint(code_point.value),
    });
    current += width;
    byte_offset += code_point.byte_length;
  }
  return spans;
}

std::optional<std::string> TextInRange(std::string_view text, TextRange range) {
  const std::optional<std::size_t> start = ByteOffsetAt(text, range.start);
  const std::optional<std::size_t> end = ByteOffsetAt(text, range.end);
  if (!start.has_value() || !end.has_value()) {
    return std::nullopt;
  }
  return std::string(text.substr(*start, *end - *start));
}

bool ReplaceText(std::string& text, TextRange range, std::string_view replacement, TextOffset& inserted_length) {
  const std::optional<TextOffset> replacement_length = Utf16Length(replacement);
  const std::optional<std::size_t> start = ByteOffsetAt(text, range.start);
  const std::optional<std::size_t> end = ByteOffsetAt(text, range.end);
  if (!replacement_length.has_value() || !start.has_value() || !end.has_value()) {
    return false;
  }

  text.replace(*start, *end - *start, replacement);
  inserted_length = *replacement_length;
  return true;
}

enum class TransformBias {
  Before,
  After,
};

TextOffset
TransformOffset(TextOffset offset, TextRange replaced, TextOffset inserted_length, TransformBias bias) noexcept {
  const TextOffset delta = inserted_length - replaced.Length();
  if (replaced.IsCollapsed()) {
    if (offset < replaced.start) {
      return offset;
    }
    if (offset > replaced.start) {
      return offset + delta;
    }
    return bias == TransformBias::Before ? replaced.start : replaced.start + inserted_length;
  }
  if (offset < replaced.start) {
    return offset;
  }
  if (offset > replaced.end) {
    return offset + delta;
  }
  if (offset == replaced.start && bias == TransformBias::Before) {
    return replaced.start;
  }
  return replaced.start + inserted_length;
}

TextSelection TransformSelection(TextSelection selection, TextRange replaced, TextOffset inserted_length) noexcept {
  selection.anchor = TransformOffset(selection.anchor, replaced, inserted_length, TransformBias::After);
  selection.active = TransformOffset(selection.active, replaced, inserted_length, TransformBias::After);
  return selection;
}

TextRange TransformRange(TextRange range, TextRange replaced, TextOffset inserted_length) noexcept {
  return {
      TransformOffset(range.start, replaced, inserted_length, TransformBias::Before),
      TransformOffset(range.end, replaced, inserted_length, TransformBias::After),
  };
}

bool IsKnown(TextInputCommandKind kind) noexcept {
  switch (kind) {
  case TextInputCommandKind::SetSelection:
  case TextInputCommandKind::BeginComposition:
  case TextInputCommandKind::UpdateComposition:
  case TextInputCommandKind::CommitText:
  case TextInputCommandKind::FinishComposition:
  case TextInputCommandKind::CancelComposition:
  case TextInputCommandKind::DeleteSurrounding:
    return true;
  }
  return false;
}

bool IsKnown(TextInputCoordinateSpace space) noexcept {
  return space == TextInputCoordinateSpace::Text || space == TextInputCoordinateSpace::Composition;
}

bool IsKnown(TextInputUnit unit) noexcept {
  return unit == TextInputUnit::Utf16CodeUnit || unit == TextInputUnit::UnicodeCodePoint;
}

bool IsKnown(TextAffinity affinity) noexcept {
  return affinity == TextAffinity::Upstream || affinity == TextAffinity::Downstream;
}

bool HasDefaultDeletionFields(const TextInputCommand& command) noexcept {
  return command.delete_before == 0 && command.delete_after == 0 && command.delete_unit == TextInputUnit::Utf16CodeUnit;
}

bool IsValidCommandShape(const TextInputCommand& command) noexcept {
  if (!IsKnown(command.kind) || !IsKnown(command.coordinate_space) || !IsKnown(command.delete_unit)) {
    return false;
  }
  if (command.target.has_value() && !command.target->IsValid()) {
    return false;
  }
  if (command.selection_after.has_value() &&
      (command.selection_after->anchor < 0 || command.selection_after->active < 0)) {
    return false;
  }

  switch (command.kind) {
  case TextInputCommandKind::SetSelection:
    return !command.target.has_value() && command.selection_after.has_value() && command.text.empty() &&
           command.coordinate_space == TextInputCoordinateSpace::Text && HasDefaultDeletionFields(command);
  case TextInputCommandKind::BeginComposition:
    return command.target.has_value() && !command.selection_after.has_value() && command.text.empty() &&
           command.coordinate_space == TextInputCoordinateSpace::Text && HasDefaultDeletionFields(command);
  case TextInputCommandKind::UpdateComposition:
  case TextInputCommandKind::CommitText:
    return (command.target.has_value() || command.coordinate_space == TextInputCoordinateSpace::Text) &&
           HasDefaultDeletionFields(command);
  case TextInputCommandKind::FinishComposition:
  case TextInputCommandKind::CancelComposition:
    return !command.target.has_value() && !command.selection_after.has_value() && command.text.empty() &&
           command.coordinate_space == TextInputCoordinateSpace::Text && HasDefaultDeletionFields(command);
  case TextInputCommandKind::DeleteSurrounding:
    return !command.target.has_value() && !command.selection_after.has_value() && command.text.empty() &&
           command.coordinate_space == TextInputCoordinateSpace::Text && command.delete_before >= 0 &&
           command.delete_after >= 0;
  }
  return false;
}

bool IsValidSelection(const std::string& text, const TextSelection& selection) noexcept {
  return selection.anchor >= 0 && selection.active >= 0 && IsKnown(selection.affinity) &&
         IsBoundary(text, selection.anchor) && IsBoundary(text, selection.active);
}

std::optional<TextRange>
ResolveTarget(const TextEditingValue& value, TextInputCoordinateSpace space, TextRange target) noexcept {
  if (!target.IsValid()) {
    return std::nullopt;
  }

  TextRange resolved = target;
  if (space == TextInputCoordinateSpace::Composition) {
    if (!value.composition.has_value() || target.end > value.composition->Length()) {
      return std::nullopt;
    }
    resolved.start += value.composition->start;
    resolved.end += value.composition->start;
  } else if (space != TextInputCoordinateSpace::Text) {
    return std::nullopt;
  }

  if (!IsBoundary(value.text, resolved.start) || !IsBoundary(value.text, resolved.end)) {
    return std::nullopt;
  }
  return resolved;
}

bool SetResultSelection(TextEditingValue& value, const std::optional<TextSelection>& selection, TextOffset fallback) {
  value.selection = selection.value_or(TextSelection{fallback, fallback});
  return IsValidSelection(value.text, value.selection);
}

bool BeginComposition(TextFieldEditingState& state, TextRange target) {
  if (state.value.composition.has_value() || state.composition_baseline.has_value()) {
    return false;
  }

  const std::optional<std::string> original_text = TextInRange(state.value.text, target);
  if (!original_text.has_value()) {
    return false;
  }

  state.composition_baseline = TextCompositionBaseline{
      target,
      *original_text,
      state.value.selection,
  };
  state.value.composition = target;
  return true;
}

bool ApplySetSelection(TextFieldEditingState& state, const TextInputCommand& command) {
  if (!command.selection_after.has_value() || !IsValidSelection(state.value.text, *command.selection_after)) {
    return false;
  }
  state.value.selection = *command.selection_after;
  return true;
}

bool ApplyBeginComposition(TextFieldEditingState& state, const TextInputCommand& command) {
  if (!command.target.has_value()) {
    return false;
  }
  const std::optional<TextRange> target = ResolveTarget(state.value, TextInputCoordinateSpace::Text, *command.target);
  return target.has_value() && BeginComposition(state, *target);
}

bool ApplyUpdateComposition(TextFieldEditingState& state, const TextInputCommand& command) {
  if (!Utf16Length(command.text).has_value()) {
    return false;
  }

  bool began_composition = false;
  if (!state.value.composition.has_value()) {
    if (command.target.has_value() && command.coordinate_space != TextInputCoordinateSpace::Text) {
      return false;
    }
    const std::optional<TextRange> target =
        command.target.has_value() ? ResolveTarget(state.value, TextInputCoordinateSpace::Text, *command.target)
                                   : std::optional<TextRange>{state.value.selection.Range()};
    if (!target.has_value() || !BeginComposition(state, *target)) {
      return false;
    }
    began_composition = true;
  }

  const TextRange composition_before = *state.value.composition;
  std::optional<TextRange> target;
  if (began_composition || !command.target.has_value()) {
    target = composition_before;
  } else {
    if (command.coordinate_space != TextInputCoordinateSpace::Composition) {
      return false;
    }
    target = ResolveTarget(state.value, command.coordinate_space, *command.target);
  }
  if (!target.has_value()) {
    return false;
  }

  TextOffset inserted_length = 0;
  if (!ReplaceText(state.value.text, *target, command.text, inserted_length)) {
    return false;
  }

  state.value.composition = TransformRange(composition_before, *target, inserted_length);
  return SetResultSelection(state.value, command.selection_after, state.value.composition->end);
}

bool ApplyCommitText(TextFieldEditingState& state, const TextInputCommand& command) {
  if (!Utf16Length(command.text).has_value()) {
    return false;
  }

  TextRange target;
  if (state.value.composition.has_value()) {
    if (command.target.has_value()) {
      return false;
    }
    target = *state.value.composition;
  } else if (command.target.has_value()) {
    if (command.coordinate_space != TextInputCoordinateSpace::Text) {
      return false;
    }
    const std::optional<TextRange> resolved = ResolveTarget(state.value, command.coordinate_space, *command.target);
    if (!resolved.has_value()) {
      return false;
    }
    target = *resolved;
  } else {
    target = state.value.selection.Range();
  }

  TextOffset inserted_length = 0;
  if (!ReplaceText(state.value.text, target, command.text, inserted_length)) {
    return false;
  }
  state.value.composition.reset();
  state.composition_baseline.reset();
  return SetResultSelection(state.value, command.selection_after, target.start + inserted_length);
}

bool ApplyFinishComposition(TextFieldEditingState& state) {
  state.value.composition.reset();
  state.composition_baseline.reset();
  return true;
}

bool ApplyCancelComposition(TextFieldEditingState& state) {
  if (!state.value.composition.has_value()) {
    return !state.composition_baseline.has_value();
  }
  if (!state.composition_baseline.has_value()) {
    return false;
  }

  const TextCompositionBaseline baseline = *state.composition_baseline;
  TextOffset inserted_length = 0;
  if (!ReplaceText(state.value.text, *state.value.composition, baseline.text, inserted_length)) {
    return false;
  }
  state.value.selection = baseline.selection;
  state.value.composition.reset();
  state.composition_baseline.reset();
  return IsValidSelection(state.value.text, state.value.selection);
}

std::optional<std::vector<TextOffset>> CodePointBoundaries(std::string_view text) {
  std::vector<TextOffset> boundaries{0};
  TextOffset utf16_offset = 0;
  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    Utf8CodePoint code_point;
    if (!DecodeCodePoint(text, byte_offset, code_point)) {
      return std::nullopt;
    }
    const TextOffset width = code_point.value > 0xFFFFU ? 2 : 1;
    if (utf16_offset > std::numeric_limits<TextOffset>::max() - width) {
      return std::nullopt;
    }
    utf16_offset += width;
    byte_offset += code_point.byte_length;
    boundaries.push_back(utf16_offset);
  }
  return boundaries;
}

std::optional<TextOffset>
MoveByCodePoints(const std::vector<TextOffset>& boundaries, TextOffset offset, TextOffset distance) noexcept {
  const auto found = std::lower_bound(boundaries.begin(), boundaries.end(), offset);
  if (found == boundaries.end() || *found != offset) {
    return std::nullopt;
  }

  const auto index = static_cast<std::size_t>(found - boundaries.begin());
  if (distance < 0) {
    const TextOffset magnitude =
        distance == std::numeric_limits<TextOffset>::min() ? std::numeric_limits<TextOffset>::max() : -distance;
    const auto count = static_cast<std::uint64_t>(magnitude);
    return count >= index ? boundaries.front() : boundaries[index - static_cast<std::size_t>(count)];
  }

  const auto count = static_cast<std::uint64_t>(distance);
  const std::size_t remaining = boundaries.size() - 1 - index;
  return count >= remaining ? boundaries.back() : boundaries[index + static_cast<std::size_t>(count)];
}

bool ApplyDeletion(TextFieldEditingState& state, TextRange range) {
  if (range.IsCollapsed()) {
    return true;
  }

  const std::optional<TextRange> composition_before = state.value.composition;
  if (composition_before.has_value() && state.composition_baseline.has_value()) {
    TextCompositionBaseline& baseline = *state.composition_baseline;
    if (range.end <= composition_before->start) {
      baseline.range = TransformRange(baseline.range, range, 0);
      baseline.selection = TransformSelection(baseline.selection, range, 0);
    } else if (range.start >= composition_before->end) {
      const TextOffset coordinate_delta = baseline.range.end - composition_before->end;
      const TextRange baseline_range{
          range.start + coordinate_delta,
          range.end + coordinate_delta,
      };
      baseline.range = TransformRange(baseline.range, baseline_range, 0);
      baseline.selection = TransformSelection(baseline.selection, baseline_range, 0);
    } else if (!(composition_before->start <= range.start && range.end <= composition_before->end)) {
      return false;
    }
  }

  TextOffset inserted_length = 0;
  if (!ReplaceText(state.value.text, range, {}, inserted_length)) {
    return false;
  }
  state.value.selection = TransformSelection(state.value.selection, range, 0);
  if (composition_before.has_value()) {
    state.value.composition = TransformRange(*composition_before, range, 0);
  }
  return true;
}

bool ApplyDeleteSurrounding(TextFieldEditingState& state, const TextInputCommand& command) {
  const TextRange selection = state.value.selection.Range();
  const std::optional<TextOffset> text_length = Utf16Length(state.value.text);
  if (!text_length.has_value()) {
    return false;
  }

  TextOffset before_start = 0;
  TextOffset after_end = *text_length;
  if (command.delete_unit == TextInputUnit::Utf16CodeUnit) {
    before_start = command.delete_before >= selection.start ? 0 : selection.start - command.delete_before;
    const TextOffset remaining = *text_length - selection.end;
    after_end = command.delete_after >= remaining ? *text_length : selection.end + command.delete_after;
    if (!IsBoundary(state.value.text, before_start) || !IsBoundary(state.value.text, after_end)) {
      return false;
    }
  } else {
    const std::optional<std::vector<TextOffset>> boundaries = CodePointBoundaries(state.value.text);
    if (!boundaries.has_value()) {
      return false;
    }
    const TextOffset before_distance = command.delete_before == std::numeric_limits<TextOffset>::min()
                                           ? std::numeric_limits<TextOffset>::min()
                                           : -command.delete_before;
    const std::optional<TextOffset> moved_before = MoveByCodePoints(*boundaries, selection.start, before_distance);
    const std::optional<TextOffset> moved_after = MoveByCodePoints(*boundaries, selection.end, command.delete_after);
    if (!moved_before.has_value() || !moved_after.has_value()) {
      return false;
    }
    before_start = *moved_before;
    after_end = *moved_after;
  }

  const TextRange after{selection.end, after_end};
  const TextRange before{before_start, selection.start};
  return ApplyDeletion(state, after) && ApplyDeletion(state, before);
}

bool ApplyCommand(TextFieldEditingState& state, const TextInputCommand& command) {
  if (!IsValidCommandShape(command)) {
    return false;
  }

  switch (command.kind) {
  case TextInputCommandKind::SetSelection:
    return ApplySetSelection(state, command);
  case TextInputCommandKind::BeginComposition:
    return ApplyBeginComposition(state, command);
  case TextInputCommandKind::UpdateComposition:
    return ApplyUpdateComposition(state, command);
  case TextInputCommandKind::CommitText:
    return ApplyCommitText(state, command);
  case TextInputCommandKind::FinishComposition:
    return ApplyFinishComposition(state);
  case TextInputCommandKind::CancelComposition:
    return ApplyCancelComposition(state);
  case TextInputCommandKind::DeleteSurrounding:
    return ApplyDeleteSurrounding(state, command);
  }
  return false;
}

} // namespace

std::optional<TextOffset> Utf16Length(std::string_view text) noexcept {
  TextOffset length = 0;
  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    Utf8CodePoint code_point;
    if (!DecodeCodePoint(text, byte_offset, code_point)) {
      return std::nullopt;
    }
    const TextOffset width = code_point.value > 0xFFFFU ? 2 : 1;
    if (length > std::numeric_limits<TextOffset>::max() - width) {
      return std::nullopt;
    }
    length += width;
    byte_offset += code_point.byte_length;
  }
  return length;
}

std::optional<std::string> Utf8TextInRange(std::string_view text, TextRange range) {
  return TextInRange(text, range);
}

std::optional<TextRange> WordRangeAt(std::string_view text, TextOffset offset) {
  if (offset < 0 || text.empty()) {
    return std::nullopt;
  }
  const std::optional<std::vector<WordSpan>> built = BuildWordSpans(text);
  if (!built.has_value() || built->empty() || offset > built->back().range.end) {
    return std::nullopt;
  }
  const std::vector<WordSpan>& spans = *built;

  auto found = std::find_if(spans.begin(), spans.end(), [offset](const WordSpan& span) {
    return offset >= span.range.start && offset < span.range.end;
  });
  if (found == spans.end()) {
    found = std::prev(spans.end());
  }
  if (!found->word) {
    return found->range;
  }

  auto first = found;
  while (first != spans.begin() && std::prev(first)->word) {
    --first;
  }
  auto last = std::next(found);
  while (last != spans.end() && last->word) {
    ++last;
  }
  return TextRange{first->range.start, std::prev(last)->range.end};
}

std::optional<TextOffset> PreviousWordStart(std::string_view text, TextOffset offset) {
  const std::optional<std::vector<WordSpan>> built = BuildWordSpans(text);
  if (!built.has_value() || !IsBoundary(text, offset)) {
    return std::nullopt;
  }
  const std::vector<WordSpan>& spans = *built;
  std::size_t index = spans.size();
  while (index > 0 && spans[index - 1].range.start >= offset) {
    --index;
  }
  while (index > 0 && !spans[index - 1].word) {
    --index;
  }
  while (index > 0 && spans[index - 1].word) {
    --index;
  }
  return index < spans.size() ? spans[index].range.start : TextOffset{0};
}

std::optional<TextOffset> NextWordEnd(std::string_view text, TextOffset offset) {
  const std::optional<std::vector<WordSpan>> built = BuildWordSpans(text);
  if (!built.has_value() || !IsBoundary(text, offset)) {
    return std::nullopt;
  }
  const std::vector<WordSpan>& spans = *built;
  std::size_t index = 0;
  while (index < spans.size() && spans[index].range.end <= offset) {
    ++index;
  }
  while (index < spans.size() && !spans[index].word) {
    ++index;
  }
  while (index < spans.size() && spans[index].word) {
    ++index;
  }
  return index == 0 ? TextOffset{0} : spans[index - 1].range.end;
}

std::optional<TextOffset> NextWordStart(std::string_view text, TextOffset offset) {
  const std::optional<std::vector<WordSpan>> built = BuildWordSpans(text);
  if (!built.has_value() || !IsBoundary(text, offset)) {
    return std::nullopt;
  }
  const std::vector<WordSpan>& spans = *built;
  std::size_t index = 0;
  while (index < spans.size() && spans[index].range.end <= offset) {
    ++index;
  }
  while (index < spans.size() && spans[index].word) {
    ++index;
  }
  while (index < spans.size() && !spans[index].word) {
    ++index;
  }
  if (index < spans.size()) {
    return spans[index].range.start;
  }
  return spans.empty() ? TextOffset{0} : spans.back().range.end;
}

bool IsValidTextEditingValue(const TextEditingValue& value) noexcept {
  if (!Utf16Length(value.text).has_value() || !IsValidSelection(value.text, value.selection)) {
    return false;
  }
  return !value.composition.has_value() ||
         (value.composition->IsValid() && IsBoundary(value.text, value.composition->start) &&
          IsBoundary(value.text, value.composition->end));
}

TextInputReductionResult
ReduceTextInputCommands(const TextFieldEditingState& state, const std::vector<TextInputCommand>& commands) {
  TextInputReductionResult result;
  result.state = state;
  if (commands.empty() || !IsValidTextEditingValue(state.value) ||
      state.value.composition.has_value() != state.composition_baseline.has_value()) {
    return result;
  }

  TextFieldEditingState staged = state;
  for (const TextInputCommand& command : commands) {
    if (!ApplyCommand(staged, command) || !IsValidTextEditingValue(staged.value) ||
        staged.value.composition.has_value() != staged.composition_baseline.has_value()) {
      return result;
    }
  }

  result.status = TextInputReductionStatus::Accepted;
  result.changed = staged != state;
  result.state = std::move(staged);
  return result;
}

} // namespace huxerui::detail

namespace huxerui {

TextEditingValue TextEditingValue::FromText(std::string text) {
  const std::optional<TextOffset> length = detail::Utf16Length(text);
  if (!length.has_value()) {
    throw std::invalid_argument("HuxerUI text must contain valid UTF-8");
  }
  return {
      std::move(text),
      {*length, *length},
      std::nullopt,
  };
}

} // namespace huxerui
