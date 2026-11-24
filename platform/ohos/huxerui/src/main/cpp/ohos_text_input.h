#pragma once

#include <arkui/native_type.h>
#include <napi/native_api.h>

#include <initializer_list>
#include <span>
#include <string>

#include <huxerui/text_input.h>

namespace huxerui {
class UiWindow;

namespace detail {

class OhosTextInput final : public PlatformTextInput {
public:
  OhosTextInput(UiWindow& window, ArkUI_NodeHandle node, const float& scale, napi_env env, napi_value host);
  ~OhosTextInput() override;

  void Start(TextInputSessionId session_id, const TextInputConfiguration& configuration,
             const TextInputState& state, const TextInputGeometry& geometry) override;
  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override;
  void Restart(TextInputSessionId session_id, const TextInputConfiguration& configuration,
               const TextInputState& state, const TextInputGeometry& geometry) override;
  void Stop(TextInputSessionId session_id) override;
  void RequestShowKeyboard(TextInputSessionId session_id) override;
  void RequestHideKeyboard(TextInputSessionId session_id) override;

  // Requests the system keyboard for the current editable session, if one has started.
  void ShowKeyboard();
  bool HandleBack();
  void SetFocused(bool focused);
  napi_value HandleCommand(std::span<const napi_value, 5> args);
  napi_value ReadContext(std::span<const napi_value, 3> args);
  void ReleaseHost() noexcept;

private:
  napi_value Number(double value) const;
  void Call(const char* name, std::initializer_list<napi_value> arguments) const;
  std::string ReadString(napi_value value) const;
  bool Matches(napi_value token) const;

  UiWindow& window_;
  ArkUI_NodeHandle node_;
  const float& scale_;
  napi_env env_;
  napi_ref host_ = nullptr;
  TextInputState state_;
  TextInputConfiguration configuration_;
  std::string token_;
  bool selection_sent_ = false;
  bool show_keyboard_ = false;
  bool focused_ = false;
};

} // namespace detail
} // namespace huxerui
