#pragma once

#include <windows.h>

#include <memory>

#include <huxerui/text_input.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class Win32TextInput final : public PlatformTextInput {
public:
  Win32TextInput();
  ~Win32TextInput() override;

  Win32TextInput(const Win32TextInput&) = delete;
  Win32TextInput& operator=(const Win32TextInput&) = delete;

  void SetRuntime(Runtime* runtime) noexcept;
  void SetWindow(HWND window) noexcept;
  void SetDpiScale(float scale) noexcept;
  void Reset() noexcept;

  [[nodiscard]] bool Active() const noexcept;
  [[nodiscard]] bool Composing() const noexcept;
  void ClearPendingResult() noexcept;
  [[nodiscard]] bool BeginComposition();
  [[nodiscard]] bool UpdateComposition(LPARAM flags);
  [[nodiscard]] bool EndComposition();
  [[nodiscard]] bool CommitCharacter(wchar_t character);
  [[nodiscard]] bool SuppressCharacter(wchar_t character);

  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override;
  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override;
  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override;
  void Stop(TextInputSessionId session_id) override;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
