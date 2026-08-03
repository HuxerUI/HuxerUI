#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <huxerui/text_input.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class WebTextInput final : public PlatformTextInput {
public:
  explicit WebTextInput(std::uintptr_t web_session_id);
  ~WebTextInput() override;

  WebTextInput(const WebTextInput&) = delete;
  WebTextInput& operator=(const WebTextInput&) = delete;

  void SetRuntime(Runtime* runtime) noexcept;
  void Reset() noexcept;

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

  void CommitEdit(std::uint32_t event_token, TextRange target, std::string text, TextSelection selection);
  void SetSelection(std::uint32_t event_token, TextSelection selection);
  void BeginComposition(std::uint32_t event_token, TextRange target);
  void UpdateComposition(std::uint32_t event_token, std::string text, TextSelection selection);
  void EndComposition(std::uint32_t event_token, std::string text, TextSelection selection);
  void PerformAction(std::uint32_t event_token, TextInputAction action);

private:
  [[nodiscard]] bool Accepts(std::uint32_t event_token) const noexcept;
  [[nodiscard]] std::uint32_t NextEventToken() noexcept;
  void Activate(const TextInputGeometry& geometry);
  void Synchronize(const TextInputGeometry& geometry);
  void Apply(std::vector<TextInputCommand> commands);

  std::uintptr_t web_session_id_ = 0;
  Runtime* runtime_ = nullptr;
  TextInputSessionId session_id_ = 0;
  std::uint32_t event_token_ = 0;
  TextInputConfiguration configuration_;
  TextInputState state_;
};

} // namespace huxerui::detail
