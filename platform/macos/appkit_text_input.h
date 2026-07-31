#pragma once

#import <AppKit/AppKit.h>

#include <memory>

#include <huxerui/app.h>

namespace huxerui::detail {

class MacTextInputState;

[[nodiscard]] KeyEvent MakeMacKeyEvent(NSEvent* event, KeyEventType type);

class MacTextInput final : public PlatformTextInput {
public:
  MacTextInput(Runtime& runtime, NSView* view);
  ~MacTextInput() override;

  MacTextInput(const MacTextInput&) = delete;
  MacTextInput& operator=(const MacTextInput&) = delete;

  [[nodiscard]] NSTextInputContext* InputContext() const noexcept;
  [[nodiscard]] bool HandleEvent(NSEvent* event);
  [[nodiscard]] bool IsActive() const noexcept;
  void InvalidateGeometry();
  void ApplicationActiveChanged(bool active);

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
  std::unique_ptr<MacTextInputState> state_;
};

} // namespace huxerui::detail
