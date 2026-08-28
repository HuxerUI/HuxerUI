#pragma once

#include <memory>
#include <string_view>

#include <SDL3/SDL.h>

#include <huxerui/text_input.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class LinuxTextInput final : public PlatformTextInput {
public:
  LinuxTextInput();
  ~LinuxTextInput() override;

  LinuxTextInput(const LinuxTextInput&) = delete;
  LinuxTextInput& operator=(const LinuxTextInput&) = delete;

  void SetRuntime(Runtime* runtime) noexcept;
  void SetWindow(SDL_Window* window) noexcept;
  void SetFocus(bool focused);
  void Reset() noexcept;

  [[nodiscard]] bool Active() const noexcept;
  [[nodiscard]] bool Composing() const noexcept;
  void HandleTextEditing(std::string_view text, int start, int length);
  void HandleTextInput(std::string_view text);

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
