#pragma once

#include <memory>

#include <huxerui/app.h>

@class HuxerUIView;
@protocol UITextInput;

namespace huxerui::detail {

class UIKitTextInputState;

class UIKitTextInput final : public PlatformTextInput {
public:
  UIKitTextInput(Runtime& runtime, HuxerUIView* view);
  ~UIKitTextInput() override;

  UIKitTextInput(const UIKitTextInput&) = delete;
  UIKitTextInput& operator=(const UIKitTextInput&) = delete;

  [[nodiscard]] bool IsActive() const noexcept;
  [[nodiscard]] const TextInputConfiguration& Configuration() const noexcept;
  [[nodiscard]] TextInputContext QueryContext(TextOffset start = 0, TextOffset length = 0) const;
  [[nodiscard]] TextInputGeometry QueryGeometry(TextRange range) const;
  [[nodiscard]] TextInputPositionResult QueryPosition(Point point) const;
  [[nodiscard]] id<UITextInput> Responder() const noexcept;
  TextInputApplyResult Apply(TextInputCommand command);
  bool PerformAction(TextInputAction action);

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
  void RequestShow(TextInputSessionId session_id) override;

private:
  std::unique_ptr<UIKitTextInputState> state_;
};

} // namespace huxerui::detail
