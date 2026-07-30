#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/event.h>
#include <huxerui/geometry.h>

namespace huxerui {

using TextOffset = std::int64_t;
using TextInputSessionId = std::uint64_t;

enum class TextAffinity {
  Upstream,
  Downstream,
};

struct TextPosition {
  TextOffset offset = 0;
  TextAffinity affinity = TextAffinity::Downstream;

  bool operator==(const TextPosition&) const = default;
};

struct TextRange {
  TextOffset start = 0;
  TextOffset end = 0;

  [[nodiscard]] bool IsValid() const noexcept {
    return start >= 0 && end >= start;
  }

  [[nodiscard]] bool IsCollapsed() const noexcept {
    return start == end;
  }

  [[nodiscard]] TextOffset Length() const noexcept {
    return end - start;
  }

  bool operator==(const TextRange&) const = default;
};

struct TextSelection {
  TextOffset anchor = 0;
  TextOffset active = 0;
  TextAffinity affinity = TextAffinity::Downstream;

  [[nodiscard]] bool IsCollapsed() const noexcept {
    return anchor == active;
  }

  [[nodiscard]] TextRange Range() const noexcept {
    return anchor <= active ? TextRange{anchor, active} : TextRange{active, anchor};
  }

  bool operator==(const TextSelection&) const = default;
};

struct TextEditingValue {
  std::string text;
  TextSelection selection;
  std::optional<TextRange> composition;

  static TextEditingValue FromText(std::string text);

  bool operator==(const TextEditingValue&) const = default;
};

enum class TextInputCommandKind {
  SetSelection,
  BeginComposition,
  UpdateComposition,
  CommitText,
  FinishComposition,
  CancelComposition,
  DeleteSurrounding,
};

enum class TextInputCoordinateSpace {
  Text,
  Composition,
};

enum class TextInputUnit {
  Utf16CodeUnit,
  UnicodeCodePoint,
};

struct TextInputCommand {
  TextInputCommandKind kind = TextInputCommandKind::SetSelection;
  TextInputCoordinateSpace coordinate_space = TextInputCoordinateSpace::Text;
  std::optional<TextRange> target;
  std::optional<TextSelection> selection_after;
  std::string text;
  TextOffset delete_before = 0;
  TextOffset delete_after = 0;
  TextInputUnit delete_unit = TextInputUnit::Utf16CodeUnit;

  bool operator==(const TextInputCommand&) const = default;
};

struct TextInputCommandBatch {
  TextInputSessionId session_id = 0;
  std::vector<TextInputCommand> commands;

  bool operator==(const TextInputCommandBatch&) const = default;
};

enum class TextInputType {
  Text,
  Email,
  Number,
  Decimal,
  Phone,
  Url,
};

enum class TextCapitalization {
  None,
  Characters,
  Words,
  Sentences,
};

enum class TextInputAction {
  Default,
  Done,
  Go,
  Next,
  Search,
  Send,
  Newline,
};

struct TextInputConfiguration {
  TextInputType type = TextInputType::Text;
  TextCapitalization capitalization = TextCapitalization::None;
  TextInputAction action = TextInputAction::Default;
  bool multiline = false;
  bool secure = false;
  bool autocorrect = true;
  bool read_only = false;

  bool operator==(const TextInputConfiguration&) const = default;
};

enum class TextInputResultCode {
  Ok,
  SessionMismatch,
  Rejected,
  ReadOnly,
};

enum class TextInputSyncAction {
  None,
  Update,
  Restart,
};

enum class TextInputEndReason {
  FocusLost,
  ClientRemoved,
  Disabled,
  ReadOnly,
  RuntimeDestroyed,
};

enum class TextInputKeyResult {
  Unhandled,
  Handled,
};

struct TextInputState {
  TextInputSessionId session_id = 0;
  std::uint64_t revision = 0;
  TextSelection selection;
  std::optional<TextRange> composition;

  bool operator==(const TextInputState&) const = default;
};

struct TextInputApplyResult {
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  TextInputSyncAction sync_action = TextInputSyncAction::None;
  bool changed = false;

  bool operator==(const TextInputApplyResult&) const = default;
};

struct TextInputContext {
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  TextInputSessionId session_id = 0;
  TextOffset slice_start = 0;
  TextOffset total_length = 0;
  std::string text;
  TextSelection selection;
  std::optional<TextRange> composition;

  bool operator==(const TextInputContext&) const = default;
};

struct TextInputGeometry {
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  TextInputSessionId session_id = 0;
  Rect caret;
  std::vector<Rect> range_rects;

  bool operator==(const TextInputGeometry& other) const {
    const auto same_rect = [](const Rect& left, const Rect& right) {
      return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
    };
    if (result_code != other.result_code || session_id != other.session_id || !same_rect(caret, other.caret) ||
        range_rects.size() != other.range_rects.size()) {
      return false;
    }
    for (std::size_t index = 0; index < range_rects.size(); ++index) {
      if (!same_rect(range_rects[index], other.range_rects[index])) {
        return false;
      }
    }
    return true;
  }
};

struct TextInputPositionResult {
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  TextInputSessionId session_id = 0;
  TextPosition position;

  bool operator==(const TextInputPositionResult&) const = default;
};

class TextInputClient {
public:
  virtual ~TextInputClient() = default;

  [[nodiscard]] virtual TextInputConfiguration Configuration() const = 0;
  [[nodiscard]] virtual TextInputState State() const = 0;
  virtual TextInputState BeginTextInput(TextInputSessionId session_id) = 0;
  virtual TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) = 0;
  [[nodiscard]] virtual TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const = 0;
  [[nodiscard]] virtual TextInputGeometry
  QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const = 0;
  [[nodiscard]] virtual TextInputPositionResult
  QueryTextInputPosition(TextInputSessionId session_id, Point point) const = 0;
  virtual TextInputKeyResult HandleTextKey(const KeyEvent& event) = 0;
  virtual void EndTextInput(TextInputSessionId session_id, TextInputEndReason reason) = 0;
};

class PlatformTextInput {
public:
  virtual ~PlatformTextInput() = default;

  virtual void
  Start(TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state) = 0;
  virtual void
  Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) = 0;
  virtual void
  Restart(TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state) = 0;
  virtual void Stop(TextInputSessionId session_id) = 0;
  virtual void RequestShow(TextInputSessionId session_id) {
    static_cast<void>(session_id);
  }
};

} // namespace huxerui
