#pragma once

#include <memory>

#include "linux_internal.h"

#include <huxerui/text_input.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

// True for keys that must bypass the XIM composition path: editing,
// navigation, shortcut, modifier, and unknown keys never produce composed
// text and are routed to the focused view instead.
[[nodiscard]] bool IsEditingKeySym(KeySym keysym) noexcept;

class LinuxTextInput final : public PlatformTextInput {
public:
  LinuxTextInput();
  ~LinuxTextInput() override;

  LinuxTextInput(const LinuxTextInput&) = delete;
  LinuxTextInput& operator=(const LinuxTextInput&) = delete;

  void SetRuntime(Runtime* runtime) noexcept;
  void SetDisplayAndWindow(Display* display, Window window);
  void SetDpiScale(float scale) noexcept;
  void Reset() noexcept;

  [[nodiscard]] bool Active() const noexcept;
  [[nodiscard]] bool Composing() const noexcept;
  void SetFocus(bool focused);
  [[nodiscard]] XIC InputContext() const noexcept;
  [[nodiscard]] bool HandleXKeyEvent(const XKeyEvent& event);

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
