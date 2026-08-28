#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <huxerui/text_input.h>

namespace huxerui::detail {

[[nodiscard]] std::optional<TextOffset> LinuxUtf8ByteToUtf16(std::string_view text, int byte_offset) noexcept;

[[nodiscard]] std::optional<int> LinuxUtf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept;

[[nodiscard]] std::optional<TextRange>
LinuxTextEditingRangeToUtf16(std::string_view text, int start, int length) noexcept;

class LinuxTextInputCommandHandler final {
public:
  using ApplyCommands = std::function<TextInputApplyResult(std::vector<TextInputCommand>)>;

  void Start(const TextInputConfiguration& configuration, const TextInputState& state) noexcept;
  void Update(const TextInputState& state) noexcept;
  void Reset() noexcept;

  [[nodiscard]] bool AcceptsInput() const noexcept;
  [[nodiscard]] bool Composing() const noexcept;

  void HandleTextEditing(std::string_view text, int start, int length, const ApplyCommands& apply);
  void HandleTextInput(std::string_view text, const ApplyCommands& apply);

private:
  TextInputConfiguration configuration_;
  TextInputState state_;
  std::uint64_t synchronization_revision_ = 0;
  bool active_ = false;
  bool composing_ = false;
};

} // namespace huxerui::detail
