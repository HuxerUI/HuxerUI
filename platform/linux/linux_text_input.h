#pragma once

#include <poll.h>

#include <memory>
#include <vector>

#include "linux_internal.h"

#include <huxerui/text_input.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

enum class XimKeyEventResult {
  Unhandled,
  Consumed,
  DispatchWithoutText,
};

struct LinuxDeferredKeyEvent {
  XEvent event{};
  XimKeyEventResult result = XimKeyEventResult::Unhandled;
};

// XIM sees every key first. Only explicit application shortcuts and bare
// modifier events bypass lookup after the input method declines the event.
[[nodiscard]] bool ShouldBypassXimLookup(KeySym keysym, unsigned int state) noexcept;

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
  [[nodiscard]] bool FilterEvent(XEvent& event) noexcept;
  [[nodiscard]] XimKeyEventResult HandleXKeyEvent(XEvent& event);
  [[nodiscard]] int PreparePoll(std::vector<pollfd>& descriptors, int timeout_ms);
  void DispatchPoll(const std::vector<pollfd>& descriptors, bool poll_succeeded) noexcept;
  void TakeDeferredKeyEvents(std::vector<LinuxDeferredKeyEvent>& events);

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
