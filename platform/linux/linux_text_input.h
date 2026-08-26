#pragma once

#include <memory>

#include <gtk/gtk.h>

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
  void SetClientWidget(GtkWidget* widget);
  void SetFocus(bool focused);
  void Reset() noexcept;

  [[nodiscard]] bool Active() const noexcept;
  [[nodiscard]] bool Composing() const noexcept;
  [[nodiscard]] bool FilterKeyEvent(GdkEvent* event);

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
