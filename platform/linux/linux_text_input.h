#pragma once

#include <memory>

#include "linux_internal.h"

#include <huxerui/text_input.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

// Full XIM (X Input Method) client: preedit and commit composition through an
// input context bound to the host window. Mirrors the Win32 IMM32 adapter.
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
  // Returns the input context for direct key-to-text translation; the adapter
  // uses it when no input session is active so the host owns one XIM only.
  [[nodiscard]] XIC InputContext() const noexcept;
  // Returns true when the IME consumed the key event; false when the adapter
  // should dispatch it to the Runtime as a normal key event.
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
