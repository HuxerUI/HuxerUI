#include "win32_text_input.h"

#include <imm.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/app.h>

#include "text/text_input_internal.h"
#include "win32_internal.h"

namespace huxerui::detail {

struct Win32TextInput::State {
  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    static_cast<void>(configuration);
    text_input_session_id_ = session_id;
    text_input_state_ = state;
    ime_composing_ = state.composition.has_value();
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    UpdateImePosition(geometry);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) {
    if (session_id != text_input_session_id_) {
      return;
    }
    text_input_state_ = state;
    ime_composing_ = state.composition.has_value();
    UpdateImePosition(geometry);
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) {
    static_cast<void>(configuration);
    CancelSystemComposition();
    text_input_session_id_ = session_id;
    text_input_state_ = state;
    ime_composing_ = state.composition.has_value();
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    UpdateImePosition(geometry);
  }

  void Stop(TextInputSessionId session_id) {
    if (session_id != text_input_session_id_) {
      return;
    }
    text_input_session_id_ = 0;
    text_input_state_ = {};
    ime_composing_ = false;
    pending_high_surrogate_ = 0;
    pending_ime_result_.clear();
    CancelSystemComposition();
  }

  TextInputGeometry QueryTextInputGeometry() const {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return {};
    }
    return runtime_->QueryTextInputGeometry(text_input_session_id_, text_input_state_.selection.Range());
  }

  void UpdateImePosition(const TextInputGeometry& geometry) {
    if (window_ == nullptr || geometry.result_code != TextInputResultCode::Ok ||
        geometry.session_id != text_input_session_id_) {
      return;
    }

    HIMC context = ImmGetContext(window_);
    if (context == nullptr) {
      return;
    }

    const float scale = dpi_scale_;
    const LONG left = static_cast<LONG>(std::lround(geometry.caret.x * scale));
    const LONG top = static_cast<LONG>(std::lround(geometry.caret.y * scale));
    const LONG right =
        static_cast<LONG>(std::lround((geometry.caret.x + std::max(geometry.caret.width, 1.0F)) * scale));
    const LONG bottom =
        static_cast<LONG>(std::lround((geometry.caret.y + std::max(geometry.caret.height, 1.0F)) * scale));

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {left, top};
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = {left, bottom};
    candidate.rcArea = {left, top, right, bottom};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window_, context);
  }

  void CancelSystemComposition() {
    if (window_ == nullptr) {
      return;
    }
    HIMC context = ImmGetContext(window_);
    if (context == nullptr) {
      return;
    }
    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    ImmReleaseContext(window_, context);
  }

  std::wstring ReadCompositionString(HIMC context, DWORD index) const {
    const LONG byte_count = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (byte_count <= 0 || byte_count % static_cast<LONG>(sizeof(wchar_t)) != 0) {
      return {};
    }
    std::wstring result(static_cast<std::size_t>(byte_count) / sizeof(wchar_t), L'\0');
    const LONG copied = ImmGetCompositionStringW(context, index, result.data(), static_cast<DWORD>(byte_count));
    if (copied != byte_count) {
      return {};
    }
    return result;
  }

  TextInputApplyResult ApplyTextInputCommands(std::vector<TextInputCommand> commands) {
    if (runtime_ == nullptr || text_input_session_id_ == 0 || commands.empty()) {
      return {};
    }
    TextInputCommandBatch batch;
    batch.session_id = text_input_session_id_;
    batch.commands = std::move(commands);
    return runtime_->HandleTextInputCommands(batch);
  }

  bool BeginImeComposition() {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return false;
    }
    ime_composing_ = true;
    UpdateImePosition(QueryTextInputGeometry());

    const TextInputContext context = runtime_->QueryTextInputContext(text_input_session_id_, 0, 0);
    if (context.result_code != TextInputResultCode::Ok || context.composition.has_value()) {
      return context.result_code == TextInputResultCode::Ok;
    }

    TextInputCommand begin;
    begin.kind = TextInputCommandKind::BeginComposition;
    begin.target = context.selection.Range();
    const TextInputApplyResult result = ApplyTextInputCommands({std::move(begin)});
    return result.result_code == TextInputResultCode::Ok;
  }

  bool UpdateImeComposition(LPARAM flags) {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return false;
    }
    HIMC context = ImmGetContext(window_);
    if (context == nullptr) {
      return false;
    }

    const bool has_result = (flags & GCS_RESULTSTR) != 0;
    const bool has_composition = (flags & GCS_COMPSTR) != 0;
    const std::wstring result_text = has_result ? ReadCompositionString(context, GCS_RESULTSTR) : std::wstring{};
    const std::wstring composition_text =
        has_composition ? ReadCompositionString(context, GCS_COMPSTR) : std::wstring{};
    LONG cursor = has_composition ? ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0) : 0;
    ImmReleaseContext(window_, context);

    if (!has_result && !has_composition) {
      return true;
    }

    const TextInputContext input_context = runtime_->QueryTextInputContext(text_input_session_id_, 0, 0);
    if (input_context.result_code != TextInputResultCode::Ok) {
      return false;
    }

    std::vector<TextInputCommand> commands;
    TextOffset composition_start = input_context.composition.value_or(input_context.selection.Range()).start;
    if (has_result) {
      TextInputCommand commit;
      commit.kind = TextInputCommandKind::CommitText;
      commit.text = WideToUtf8(result_text);
      commands.push_back(std::move(commit));
      pending_ime_result_ = result_text;
      const std::optional<TextOffset> result_length = Utf16Length(commands.back().text);
      if (!result_length.has_value()) {
        return false;
      }
      composition_start += *result_length;
    }
    if (has_composition) {
      TextInputCommand update;
      update.kind = TextInputCommandKind::UpdateComposition;
      update.text = WideToUtf8(composition_text);
      const std::optional<TextOffset> composition_length = Utf16Length(update.text);
      if (!composition_length.has_value()) {
        return false;
      }
      cursor = std::clamp<LONG>(cursor, 0, static_cast<LONG>(*composition_length));
      update.selection_after = TextSelection{
          composition_start + cursor,
          composition_start + cursor,
      };
      commands.push_back(std::move(update));
    }

    const TextInputApplyResult result = ApplyTextInputCommands(std::move(commands));
    if (result.result_code != TextInputResultCode::Ok) {
      pending_ime_result_.clear();
      return false;
    }
    ime_composing_ = has_composition;
    return true;
  }

  bool EndImeComposition() {
    if (runtime_ == nullptr || text_input_session_id_ == 0) {
      return false;
    }
    ime_composing_ = false;
    pending_high_surrogate_ = 0;

    const TextInputContext context = runtime_->QueryTextInputContext(text_input_session_id_, 0, 0);
    if (context.result_code != TextInputResultCode::Ok || !context.composition.has_value()) {
      return context.result_code == TextInputResultCode::Ok;
    }

    TextInputCommand finish;
    finish.kind = TextInputCommandKind::FinishComposition;
    const TextInputApplyResult result = ApplyTextInputCommands({std::move(finish)});
    return result.result_code == TextInputResultCode::Ok;
  }

  bool SuppressImeCharacter(wchar_t character) {
    if (pending_ime_result_.empty()) {
      return false;
    }
    if (pending_ime_result_.front() != character) {
      pending_ime_result_.clear();
      return false;
    }
    pending_ime_result_.erase(pending_ime_result_.begin());
    return true;
  }

  bool CommitCharacter(wchar_t character) {
    if (text_input_session_id_ == 0) {
      return false;
    }
    if (SuppressImeCharacter(character)) {
      return true;
    }
    if (character < L' ') {
      pending_high_surrogate_ = 0;
      return true;
    }

    std::wstring text;
    if (character >= 0xD800 && character <= 0xDBFF) {
      pending_high_surrogate_ = character;
      return true;
    }
    if (character >= 0xDC00 && character <= 0xDFFF) {
      if (pending_high_surrogate_ == 0) {
        return true;
      }
      text.push_back(pending_high_surrogate_);
      text.push_back(character);
      pending_high_surrogate_ = 0;
    } else {
      pending_high_surrogate_ = 0;
      text.push_back(character);
    }

    TextInputCommand commit;
    commit.kind = TextInputCommandKind::CommitText;
    commit.text = WideToUtf8(text);
    const TextInputApplyResult result = ApplyTextInputCommands({std::move(commit)});
    return result.result_code == TextInputResultCode::Ok;
  }

  Runtime* runtime_ = nullptr;
  HWND window_ = nullptr;
  float dpi_scale_ = 1.0F;
  TextInputSessionId text_input_session_id_ = 0;
  TextInputState text_input_state_;
  bool ime_composing_ = false;
  wchar_t pending_high_surrogate_ = 0;
  std::wstring pending_ime_result_;
};

Win32TextInput::Win32TextInput() : state_(std::make_unique<State>()) {}

Win32TextInput::~Win32TextInput() = default;

void Win32TextInput::SetRuntime(Runtime* runtime) noexcept {
  state_->runtime_ = runtime;
}

void Win32TextInput::SetWindow(HWND window) noexcept {
  state_->window_ = window;
}

void Win32TextInput::SetDpiScale(float scale) noexcept {
  state_->dpi_scale_ = std::max(scale, 0.0F);
}

void Win32TextInput::Reset() noexcept {
  state_->runtime_ = nullptr;
  state_->window_ = nullptr;
  state_->text_input_session_id_ = 0;
  state_->text_input_state_ = {};
  state_->ime_composing_ = false;
  state_->pending_high_surrogate_ = 0;
  state_->pending_ime_result_.clear();
}

bool Win32TextInput::Active() const noexcept {
  return state_->text_input_session_id_ != 0;
}

bool Win32TextInput::Composing() const noexcept {
  return state_->ime_composing_;
}

void Win32TextInput::ClearPendingResult() noexcept {
  state_->pending_ime_result_.clear();
}

bool Win32TextInput::BeginComposition() {
  return state_->BeginImeComposition();
}

bool Win32TextInput::UpdateComposition(LPARAM flags) {
  return state_->UpdateImeComposition(flags);
}

bool Win32TextInput::EndComposition() {
  return state_->EndImeComposition();
}

bool Win32TextInput::CommitCharacter(wchar_t character) {
  return state_->CommitCharacter(character);
}

bool Win32TextInput::SuppressCharacter(wchar_t character) {
  return state_->SuppressImeCharacter(character);
}

void Win32TextInput::Start(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Start(session_id, configuration, state, geometry);
}

void Win32TextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  state_->Update(session_id, state, geometry);
}

void Win32TextInput::Restart(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->Restart(session_id, configuration, state, geometry);
}

void Win32TextInput::Stop(TextInputSessionId session_id) {
  state_->Stop(session_id);
}

} // namespace huxerui::detail
