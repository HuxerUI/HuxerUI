#include "linux_internal.h"
#include "linux_text_input.h"
#include "linux_text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/app.h>

#include "text_input_internal.h"

namespace huxerui::detail {

std::optional<TextOffset> LinuxUtf8ByteToUtf16(std::string_view text, int byte_offset) noexcept {
  if (byte_offset < 0 || static_cast<std::size_t>(byte_offset) > text.size()) {
    return std::nullopt;
  }
  return Utf16Length(text.substr(0, static_cast<std::size_t>(byte_offset)));
}

std::optional<int> LinuxUtf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept {
  if (offset < 0) {
    return std::nullopt;
  }
  TextOffset utf16 = 0;
  std::size_t byte = 0;
  while (byte < text.size() && utf16 < offset) {
    const unsigned char lead = static_cast<unsigned char>(text[byte]);
    const std::size_t length = lead < 0x80U ? 1U : lead < 0xE0U ? 2U : lead < 0xF0U ? 3U : 4U;
    if (byte + length > text.size()) {
      return std::nullopt;
    }
    const std::optional<TextOffset> units = Utf16Length(text.substr(byte, length));
    if (!units.has_value() || utf16 + *units > offset) {
      return std::nullopt;
    }
    utf16 += *units;
    byte += length;
  }
  return utf16 == offset && Utf16Length(text.substr(byte)).has_value() ? std::optional<int>(static_cast<int>(byte))
                                                                       : std::nullopt;
}

namespace {

SDL_TextInputType InputType(const TextInputConfiguration& configuration) noexcept {
  if (configuration.secure) {
    return configuration.type == TextInputType::Number ? SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN
                                                       : SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN;
  }
  switch (configuration.type) {
  case TextInputType::Text:
  case TextInputType::Phone:
  case TextInputType::Url:
    return SDL_TEXTINPUT_TYPE_TEXT;
  case TextInputType::Email:
    return SDL_TEXTINPUT_TYPE_TEXT_EMAIL;
  case TextInputType::Number:
  case TextInputType::Decimal:
    return SDL_TEXTINPUT_TYPE_NUMBER;
  }
  return SDL_TEXTINPUT_TYPE_TEXT;
}

SDL_Capitalization Capitalization(TextCapitalization capitalization) noexcept {
  switch (capitalization) {
  case TextCapitalization::None:
    return SDL_CAPITALIZE_NONE;
  case TextCapitalization::Characters:
    return SDL_CAPITALIZE_LETTERS;
  case TextCapitalization::Words:
    return SDL_CAPITALIZE_WORDS;
  case TextCapitalization::Sentences:
    return SDL_CAPITALIZE_SENTENCES;
  }
  return SDL_CAPITALIZE_NONE;
}

std::optional<int> Utf8CodePointToByte(std::string_view text, int code_point_offset) noexcept {
  if (code_point_offset < 0 || !Utf16Length(text).has_value()) {
    return std::nullopt;
  }
  int code_points = 0;
  std::size_t byte = 0;
  while (byte < text.size() && code_points < code_point_offset) {
    const unsigned char lead = static_cast<unsigned char>(text[byte]);
    byte += lead < 0x80U ? 1U : lead < 0xE0U ? 2U : lead < 0xF0U ? 3U : 4U;
    ++code_points;
  }
  return code_points == code_point_offset ? std::optional<int>(static_cast<int>(byte)) : std::nullopt;
}

} // namespace

std::optional<TextRange> LinuxTextEditingRangeToUtf16(std::string_view text, int start, int length) noexcept {
  if (start < 0 || length < 0 || length > std::numeric_limits<int>::max() - start) {
    return std::nullopt;
  }
  const std::optional<int> start_byte = Utf8CodePointToByte(text, start);
  const std::optional<int> end_byte = Utf8CodePointToByte(text, start + length);
  if (!start_byte.has_value() || !end_byte.has_value()) {
    return std::nullopt;
  }
  const std::optional<TextOffset> utf16_start = LinuxUtf8ByteToUtf16(text, *start_byte);
  const std::optional<TextOffset> utf16_end = LinuxUtf8ByteToUtf16(text, *end_byte);
  if (!utf16_start.has_value() || !utf16_end.has_value()) {
    return std::nullopt;
  }
  return TextRange{*utf16_start, *utf16_end};
}

void LinuxTextInputCommandHandler::Start(
    const TextInputConfiguration& configuration, const TextInputState& state
) noexcept {
  configuration_ = configuration;
  state_ = state;
  active_ = !configuration.read_only;
  composing_ = state.composition.has_value();
  ++synchronization_revision_;
}

void LinuxTextInputCommandHandler::Update(const TextInputState& state) noexcept {
  state_ = state;
  composing_ = state.composition.has_value();
  ++synchronization_revision_;
}

void LinuxTextInputCommandHandler::Reset() noexcept {
  configuration_ = {};
  state_ = {};
  active_ = false;
  composing_ = false;
  ++synchronization_revision_;
}

bool LinuxTextInputCommandHandler::AcceptsInput() const noexcept {
  return active_;
}

bool LinuxTextInputCommandHandler::Composing() const noexcept {
  return composing_;
}

void LinuxTextInputCommandHandler::HandleTextEditing(
    std::string_view text, int start, int length, const ApplyCommands& apply
) {
  if (!active_ || configuration_.secure) {
    return;
  }
  if (text.empty()) {
    if (composing_) {
      TextInputCommand finish;
      finish.kind = TextInputCommandKind::FinishComposition;
      const std::uint64_t revision = synchronization_revision_;
      const TextInputApplyResult result = apply({std::move(finish)});
      if (revision == synchronization_revision_ && result.result_code == TextInputResultCode::Ok) {
        composing_ = false;
      }
    }
    return;
  }
  if (!composing_) {
    TextInputCommand begin;
    begin.kind = TextInputCommandKind::BeginComposition;
    begin.target = state_.selection.Range();
    const std::uint64_t revision = synchronization_revision_;
    const TextInputApplyResult result = apply({std::move(begin)});
    if (revision == synchronization_revision_) {
      composing_ = result.result_code == TextInputResultCode::Ok;
    }
  }
  if (!composing_) {
    return;
  }
  const TextOffset composition_start = state_.composition.value_or(state_.selection.Range()).start;
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = std::string(text);
  if (const std::optional<TextRange> editing_range = LinuxTextEditingRangeToUtf16(text, start, length)) {
    update.selection_after = TextSelection{
        composition_start + editing_range->start,
        composition_start + editing_range->end,
        TextAffinity::Downstream,
    };
  }
  static_cast<void>(apply({std::move(update)}));
}

void LinuxTextInputCommandHandler::HandleTextInput(std::string_view text, const ApplyCommands& apply) {
  if (!active_ || text.empty()) {
    return;
  }
  TextInputCommand commit;
  commit.kind = TextInputCommandKind::CommitText;
  commit.text = std::string(text);
  const std::uint64_t revision = synchronization_revision_;
  const TextInputApplyResult result = apply({std::move(commit)});
  if (revision == synchronization_revision_ && result.result_code == TextInputResultCode::Ok) {
    composing_ = false;
  }
}

struct LinuxTextInput::State {
  [[nodiscard]] bool IsActive() const noexcept {
    return runtime != nullptr && window != nullptr && session_id != 0 && command_handler.AcceptsInput();
  }

  TextInputApplyResult Apply(std::vector<TextInputCommand> commands) {
    if (!IsActive() || commands.empty()) {
      return {};
    }
    return runtime->HandleTextInputCommands({.session_id = session_id, .commands = std::move(commands)});
  }

  void UpdateInputArea() const {
    if (!IsActive()) {
      return;
    }
    const SDL_Rect area{
        static_cast<int>(std::floor(geometry.caret.x)),
        static_cast<int>(std::floor(geometry.caret.y)),
        std::max(1, static_cast<int>(std::ceil(geometry.caret.width))),
        std::max(1, static_cast<int>(std::ceil(geometry.caret.height))),
    };
    static_cast<void>(SDL_SetTextInputArea(window, &area, 0));
  }

  void Activate() {
    if (!IsActive() || !focused) {
      if (window != nullptr && SDL_TextInputActive(window)) {
        static_cast<void>(SDL_StopTextInput(window));
      }
      return;
    }
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties != 0) {
      static_cast<void>(SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER, InputType(configuration)));
      static_cast<void>(SDL_SetNumberProperty(
          properties,
          SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER,
          Capitalization(configuration.capitalization)
      ));
      static_cast<void>(SDL_SetBooleanProperty(
          properties,
          SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN,
          configuration.autocorrect && !configuration.secure
      ));
      static_cast<void>(
          SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, configuration.multiline)
      );
      static_cast<void>(SDL_StartTextInputWithProperties(window, properties));
      SDL_DestroyProperties(properties);
    } else {
      static_cast<void>(SDL_StartTextInput(window));
    }
    UpdateInputArea();
  }

  Runtime* runtime = nullptr;
  SDL_Window* window = nullptr;
  TextInputSessionId session_id = 0;
  TextInputConfiguration configuration;
  TextInputState state;
  TextInputGeometry geometry;
  bool focused = false;
  LinuxTextInputCommandHandler command_handler;
};

LinuxTextInput::LinuxTextInput() : state_(std::make_unique<State>()) {}

LinuxTextInput::~LinuxTextInput() = default;

void LinuxTextInput::SetRuntime(Runtime* runtime) noexcept {
  state_->runtime = runtime;
}

void LinuxTextInput::SetWindow(SDL_Window* window) noexcept {
  if (state_->window == window) {
    return;
  }
  if (state_->window != nullptr && SDL_TextInputActive(state_->window)) {
    static_cast<void>(SDL_StopTextInput(state_->window));
  }
  state_->window = window;
  state_->Activate();
}

void LinuxTextInput::SetFocus(bool focused) {
  if (state_->focused == focused) {
    return;
  }
  state_->focused = focused;
  state_->Activate();
}

void LinuxTextInput::Reset() noexcept {
  if (state_->window != nullptr && SDL_TextInputActive(state_->window)) {
    static_cast<void>(SDL_StopTextInput(state_->window));
  }
  state_->window = nullptr;
  state_->runtime = nullptr;
  state_->session_id = 0;
  state_->focused = false;
  state_->command_handler.Reset();
}

bool LinuxTextInput::Active() const noexcept {
  return state_->IsActive();
}

bool LinuxTextInput::Composing() const noexcept {
  return state_->command_handler.Composing();
}

void LinuxTextInput::HandleTextEditing(std::string_view text, int start, int length) {
  if (!state_->IsActive()) {
    return;
  }
  state_->command_handler.HandleTextEditing(
      text, start, length, [this](std::vector<TextInputCommand> commands) { return state_->Apply(std::move(commands)); }
  );
}

void LinuxTextInput::HandleTextInput(std::string_view text) {
  if (!state_->IsActive()) {
    return;
  }
  state_->command_handler.HandleTextInput(
      text, [this](std::vector<TextInputCommand> commands) { return state_->Apply(std::move(commands)); }
  );
}

void LinuxTextInput::Start(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  state_->session_id = session_id;
  state_->configuration = configuration;
  state_->state = state;
  state_->geometry = geometry;
  state_->command_handler.Start(configuration, state);
  state_->Activate();
}

void LinuxTextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  if (session_id != state_->session_id) {
    return;
  }
  state_->state = state;
  state_->geometry = geometry;
  state_->command_handler.Update(state);
  state_->UpdateInputArea();
}

void LinuxTextInput::Restart(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  if (session_id != state_->session_id) {
    return;
  }
  if (state_->window != nullptr) {
    static_cast<void>(SDL_ClearComposition(state_->window));
  }
  Start(session_id, configuration, state, geometry);
}

void LinuxTextInput::Stop(TextInputSessionId session_id) {
  if (session_id != state_->session_id) {
    return;
  }
  if (state_->window != nullptr) {
    static_cast<void>(SDL_ClearComposition(state_->window));
    static_cast<void>(SDL_StopTextInput(state_->window));
  }
  state_->session_id = 0;
  state_->command_handler.Reset();
  state_->configuration = {};
}

} // namespace huxerui::detail
