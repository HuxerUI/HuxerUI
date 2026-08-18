#include "web_text_input.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <emscripten.h>

#include <huxerui/app.h>

namespace huxerui::detail {

namespace {

std::unordered_map<std::uintptr_t, WebTextInput*>& TextInputs() {
  static std::unordered_map<std::uintptr_t, WebTextInput*> inputs;
  return inputs;
}

WebTextInput* FindTextInput(std::uintptr_t web_session_id) noexcept {
  const auto found = TextInputs().find(web_session_id);
  return found == TextInputs().end() ? nullptr : found->second;
}

template <typename Callback>
void DispatchWebTextInput(std::uintptr_t web_session_id, const char* operation, Callback&& callback) noexcept {
  WebTextInput* input = FindTextInput(web_session_id);
  if (input == nullptr) {
    return;
  }
  try {
    callback(*input);
  } catch (const std::exception& error) {
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web text input %s failed: %s", operation, error.what());
  } catch (...) {
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web text input %s failed with an unknown exception", operation);
  }
}

std::int32_t WebOffset(TextOffset offset) noexcept {
  return static_cast<std::int32_t>(
      std::clamp<TextOffset>(offset, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max())
  );
}

// clang-format off
EM_JS(bool, ActivateWebTextInput, (
    std::uintptr_t web_session_id,
    std::uint32_t event_token,
    int input_type,
    int capitalization,
    int action,
    bool multiline,
    bool secure,
    bool autocorrect,
    const char* text,
    std::int32_t anchor,
    std::int32_t active,
    float caret_x,
    float caret_y,
    float caret_height
), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session) {
    return false;
  }

  try {
    const selection = (element) => {
      const start = element.selectionStart || 0;
      const end = element.selectionEnd || start;
      return element.selectionDirection === "backward" ? [end, start] : [start, end];
    };
    const isSurrogateBoundary = (value, offset) => {
      if (offset <= 0 || offset >= value.length) {
        return true;
      }
      const before = value.charCodeAt(offset - 1);
      const after = value.charCodeAt(offset);
      return !(before >= 0xD800 && before <= 0xDBFF && after >= 0xDC00 && after <= 0xDFFF);
    };
    const bind = (element) => {
      const listen = (type, listener) => {
        element.addEventListener(type, listener);
        session.listeners.push(() => element.removeEventListener(type, listener));
      };

      listen("input", () => {
        if (session.activeTextInput !== element || session.textComposing) {
          return;
        }
        const previous = session.textValue || "";
        const current = element.value;
        let prefix = 0;
        while (prefix < previous.length && prefix < current.length && previous[prefix] === current[prefix]) {
          ++prefix;
        }
        while (!isSurrogateBoundary(previous, prefix) || !isSurrogateBoundary(current, prefix)) {
          --prefix;
        }
        let previousEnd = previous.length;
        let currentEnd = current.length;
        while (
            previousEnd > prefix
            && currentEnd > prefix
            && previous[previousEnd - 1] === current[currentEnd - 1]
        ) {
          --previousEnd;
          --currentEnd;
        }
        while (!isSurrogateBoundary(previous, previousEnd)) {
          ++previousEnd;
        }
        while (!isSurrogateBoundary(current, currentEnd)) {
          ++currentEnd;
        }
        const selected = selection(element);
        session.textValue = current;
        const inserted = current.slice(prefix, currentEnd);
        const pointer = Module.stringToNewUTF8(inserted);
        Module._huxerui_web_text_edit(
            web_session_id,
            session.textToken,
            prefix,
            previousEnd,
            pointer,
            selected[0],
            selected[1]
        );
        _free(pointer);
      });

      listen("select", () => {
        if (session.activeTextInput !== element || session.textSynchronizing || session.textComposing) {
          return;
        }
        const selected = selection(element);
        Module._huxerui_web_text_selection(web_session_id, session.textToken, selected[0], selected[1]);
      });

      listen("copy", (event) => {
        if (session.activeTextInput === element && session.textSecure) {
          event.preventDefault();
        }
      });

      listen("cut", (event) => {
        if (session.activeTextInput === element && session.textSecure) {
          event.preventDefault();
        }
      });

      listen("compositionstart", () => {
        if (session.activeTextInput !== element) {
          return;
        }
        const selected = selection(element);
        session.textComposing = true;
        session.textCompositionStart = Math.min(selected[0], selected[1]);
        Module._huxerui_web_text_composition_start(
            web_session_id,
            session.textToken,
            Math.min(selected[0], selected[1]),
            Math.max(selected[0], selected[1])
        );
      });

      listen("compositionupdate", (event) => {
        if (session.activeTextInput !== element || !session.textComposing) {
          return;
        }
        const value = event.data || "";
        const caret = session.textCompositionStart + value.length;
        const pointer = Module.stringToNewUTF8(value);
        Module._huxerui_web_text_composition_update(
            web_session_id,
            session.textToken,
            pointer,
            caret,
            caret
        );
        _free(pointer);
      });

      listen("compositionend", (event) => {
        if (session.activeTextInput !== element || !session.textComposing) {
          return;
        }
        session.textComposing = false;
        const value = event.data || "";
        const caret = session.textCompositionStart + value.length;
        const pointer = Module.stringToNewUTF8(value);
        Module._huxerui_web_text_composition_end(
            web_session_id,
            session.textToken,
            pointer,
            caret,
            caret
        );
        _free(pointer);
      });

      listen("keydown", (event) => {
        if (session.activeTextInput !== element || session.textComposing) {
          return;
        }
        if (event.key === "Tab") {
          event.preventDefault();
          session.sendKey(event, 0);
          return;
        }
        if (event.key !== "Enter") {
          return;
        }
        let requestedAction = session.textAction;
        if (requestedAction === 0) {
          requestedAction = session.textMultiline ? 6 : 1;
        }
        if (requestedAction === 6 && session.textMultiline) {
          return;
        }
        event.preventDefault();
        Module._huxerui_web_text_action(web_session_id, session.textToken, requestedAction);
      });
      listen("keyup", (event) => {
        if (session.activeTextInput === element && event.key === "Tab") {
          event.preventDefault();
          session.sendKey(event, 1);
        }
      });
    };

    if (!session.textInput) {
      const configureElement = (element) => {
        element.setAttribute("aria-hidden", "true");
        element.style.position = "fixed";
        element.style.width = "1px";
        element.style.minWidth = "1px";
        element.style.padding = "0";
        element.style.margin = "0";
        element.style.border = "0";
        element.style.outline = "0";
        element.style.opacity = "0";
        element.style.pointerEvents = "none";
        element.style.zIndex = "-1";
        element.style.fontSize = "16px";
        element.style.resize = "none";
        element.style.overflow = "hidden";
        element.style.display = "none";
        document.body.appendChild(element);
        bind(element);
        return element;
      };
      session.textInput = configureElement(document.createElement("input"));
      session.textArea = configureElement(document.createElement("textarea"));
      session.textElements = [session.textInput, session.textArea];
      session.listeners.push(() => {
        for (const element of session.textElements) {
          element.remove();
        }
      });
    }

    const element = multiline ? session.textArea : session.textInput;
    const previous = session.activeTextInput;
    if (previous && previous !== element) {
      previous.style.display = "none";
    }
    session.activeTextInput = element;
    session.textToken = event_token;
    session.textAction = action;
    session.textMultiline = multiline;
    session.textSecure = secure;
    session.textComposing = false;
    session.textValue = UTF8ToString(text);

    element.style.display = "block";
    if (element === session.textInput) {
      element.type = secure ? "password" : "text";
    }
    element.inputMode = ["text", "email", "numeric", "decimal", "tel", "url"][input_type] || "text";
    element.autocapitalize = ["none", "characters", "words", "sentences"][capitalization] || "none";
    element.enterKeyHint = ["enter", "done", "go", "next", "search", "send", "enter"][action] || "enter";
    element.autocomplete = secure ? "new-password" : autocorrect ? "on" : "off";
    element.spellcheck = autocorrect;
    element.value = session.textValue;

    const rootBounds = session.root.getBoundingClientRect();
    element.style.left = String(rootBounds.left + caret_x) + "px";
    element.style.top = String(rootBounds.top + caret_y) + "px";
    element.style.height = String(Math.max(1, caret_height)) + "px";
    const start = Math.min(anchor, active);
    const end = Math.max(anchor, active);
    session.textSynchronizing = true;
    element.setSelectionRange(start, end, anchor > active ? "backward" : "forward");
    session.textSynchronizing = false;
    element.focus({preventScroll: true});
    queueMicrotask(() => {
      if (
          Module.huxerUIWebSessions.get(web_session_id) === session
          && session.activeTextInput === element
          && session.textToken === event_token
      ) {
        element.focus({preventScroll: true});
      }
    });
    return true;
  } catch (error) {
    session.activeTextInput = null;
    session.textToken = 0;
    session.textComposing = false;
    for (const element of [session.textInput, session.textArea]) {
      if (element) {
        try {
          element.remove();
        } catch (cleanupError) {
          console.error("HuxerUI Web text input element cleanup failed", cleanupError);
        }
      }
    }
    session.textInput = null;
    session.textArea = null;
    session.textElements = [];
    console.error("HuxerUI Web text input activation failed", error);
    return false;
  }
});

EM_JS(void, SynchronizeWebTextInput, (
    std::uintptr_t web_session_id,
    std::uint32_t event_token,
    const char* text,
    std::int32_t anchor,
    std::int32_t active,
    float caret_x,
    float caret_y,
    float caret_height
), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session || !session.activeTextInput || session.textToken !== event_token) {
    return;
  }
  try {
    const element = session.activeTextInput;
    session.textValue = UTF8ToString(text);
    session.textSynchronizing = true;
    if (element.value !== session.textValue) {
      element.value = session.textValue;
    }
    const start = Math.min(anchor, active);
    const end = Math.max(anchor, active);
    element.setSelectionRange(start, end, anchor > active ? "backward" : "forward");
    const rootBounds = session.root.getBoundingClientRect();
    element.style.left = String(rootBounds.left + caret_x) + "px";
    element.style.top = String(rootBounds.top + caret_y) + "px";
    element.style.height = String(Math.max(1, caret_height)) + "px";
  } catch (error) {
    console.error("HuxerUI Web text input synchronization failed", error);
  } finally {
    session.textSynchronizing = false;
  }
});

EM_JS(void, DeactivateWebTextInput, (std::uintptr_t web_session_id, std::uint32_t event_token), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session || !session.activeTextInput || session.textToken !== event_token) {
    return;
  }
  const element = session.activeTextInput;
  session.activeTextInput = null;
  session.textToken = 0;
  session.textComposing = false;
  try {
    element.blur();
    element.style.display = "none";
    queueMicrotask(() => {
      if (Module.huxerUIWebSessions.get(web_session_id) === session && !session.activeTextInput) {
        session.root.focus({preventScroll: true});
      }
    });
  } catch (error) {
    console.error("HuxerUI Web text input deactivation failed", error);
  }
});

EM_JS(void, FocusWebTextInput, (std::uintptr_t web_session_id, std::uint32_t event_token), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session || !session.activeTextInput || session.textToken !== event_token) {
    return;
  }
  try {
    session.activeTextInput.focus({preventScroll: true});
  } catch (error) {
    console.error("HuxerUI Web text input focus failed", error);
  }
});
// clang-format on

} // namespace

WebTextInput::WebTextInput(std::uintptr_t web_session_id) : web_session_id_(web_session_id) {
  TextInputs().emplace(web_session_id_, this);
}

WebTextInput::~WebTextInput() {
  Reset();
  TextInputs().erase(web_session_id_);
}

void WebTextInput::SetRuntime(Runtime* runtime) noexcept {
  runtime_ = runtime;
}

void WebTextInput::Reset() noexcept {
  if (event_token_ != 0) {
    DeactivateWebTextInput(web_session_id_, event_token_);
  }
  runtime_ = nullptr;
  session_id_ = 0;
  event_token_ = 0;
  configuration_ = {};
  state_ = {};
}

void WebTextInput::Start(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  session_id_ = session_id;
  configuration_ = configuration;
  state_ = state;
  event_token_ = NextEventToken();
  Activate(geometry);
}

void WebTextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  if (session_id != session_id_) {
    return;
  }
  state_ = state;
  Synchronize(geometry);
}

void WebTextInput::Restart(
    TextInputSessionId session_id,
    const TextInputConfiguration& configuration,
    const TextInputState& state,
    const TextInputGeometry& geometry
) {
  if (session_id != session_id_) {
    return;
  }
  if (event_token_ != 0) {
    DeactivateWebTextInput(web_session_id_, event_token_);
  }
  configuration_ = configuration;
  state_ = state;
  event_token_ = NextEventToken();
  Activate(geometry);
}

void WebTextInput::Stop(TextInputSessionId session_id) {
  if (session_id != session_id_) {
    return;
  }
  DeactivateWebTextInput(web_session_id_, event_token_);
  session_id_ = 0;
  event_token_ = 0;
  configuration_ = {};
  state_ = {};
}

void WebTextInput::RequestShow(TextInputSessionId session_id) {
  if (session_id == session_id_ && event_token_ != 0) {
    FocusWebTextInput(web_session_id_, event_token_);
  }
}

bool WebTextInput::Accepts(std::uint32_t event_token) const noexcept {
  return runtime_ != nullptr && session_id_ != 0 && event_token != 0 && event_token == event_token_;
}

std::uint32_t WebTextInput::NextEventToken() noexcept {
  static std::uint32_t next_token = 1;
  if (next_token == 0) {
    next_token = 1;
  }
  return next_token++;
}

void WebTextInput::Activate(const TextInputGeometry& geometry) {
  if (runtime_ == nullptr || geometry.result_code != TextInputResultCode::Ok) {
    return;
  }
  const TextInputContext context = runtime_->QueryTextInputContext(session_id_, 0, 0);
  if (context.result_code != TextInputResultCode::Ok) {
    return;
  }
  ActivateWebTextInput(
      web_session_id_,
      event_token_,
      static_cast<int>(configuration_.type),
      static_cast<int>(configuration_.capitalization),
      static_cast<int>(configuration_.action),
      configuration_.multiline,
      configuration_.secure,
      configuration_.autocorrect,
      context.text.c_str(),
      WebOffset(context.selection.anchor),
      WebOffset(context.selection.active),
      geometry.caret.x,
      geometry.caret.y,
      geometry.caret.height
  );
}

void WebTextInput::Synchronize(const TextInputGeometry& geometry) {
  if (runtime_ == nullptr || geometry.result_code != TextInputResultCode::Ok || event_token_ == 0) {
    return;
  }
  const TextInputContext context = runtime_->QueryTextInputContext(session_id_, 0, 0);
  if (context.result_code != TextInputResultCode::Ok) {
    return;
  }
  SynchronizeWebTextInput(
      web_session_id_,
      event_token_,
      context.text.c_str(),
      WebOffset(context.selection.anchor),
      WebOffset(context.selection.active),
      geometry.caret.x,
      geometry.caret.y,
      geometry.caret.height
  );
}

void WebTextInput::Apply(std::vector<TextInputCommand> commands) {
  if (runtime_ == nullptr || commands.empty()) {
    return;
  }
  TextInputCommandBatch batch;
  batch.session_id = session_id_;
  batch.commands = std::move(commands);
  const TextInputApplyResult result = runtime_->HandleTextInputCommands(batch);
  if (result.result_code != TextInputResultCode::Ok && session_id_ != 0) {
    Synchronize(runtime_->QueryTextInputGeometry(session_id_, state_.selection.Range()));
  }
}

void WebTextInput::CommitEdit(std::uint32_t event_token, TextRange target, std::string text, TextSelection selection) {
  if (!Accepts(event_token)) {
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::CommitText;
  command.target = target;
  command.selection_after = selection;
  command.text = std::move(text);
  Apply({std::move(command)});
}

void WebTextInput::SetSelection(std::uint32_t event_token, TextSelection selection) {
  if (!Accepts(event_token) || selection == state_.selection) {
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::SetSelection;
  command.selection_after = selection;
  Apply({std::move(command)});
}

void WebTextInput::BeginComposition(std::uint32_t event_token, TextRange target) {
  if (!Accepts(event_token)) {
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::BeginComposition;
  command.target = target;
  Apply({std::move(command)});
}

void WebTextInput::UpdateComposition(std::uint32_t event_token, std::string text, TextSelection selection) {
  if (!Accepts(event_token)) {
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::UpdateComposition;
  command.selection_after = selection;
  command.text = std::move(text);
  Apply({std::move(command)});
}

void WebTextInput::EndComposition(std::uint32_t event_token, std::string text, TextSelection selection) {
  if (!Accepts(event_token)) {
    return;
  }
  std::vector<TextInputCommand> commands;
  TextInputCommand update;
  update.kind = TextInputCommandKind::UpdateComposition;
  update.selection_after = selection;
  update.text = std::move(text);
  commands.push_back(std::move(update));
  TextInputCommand finish;
  finish.kind = TextInputCommandKind::FinishComposition;
  commands.push_back(std::move(finish));
  Apply(std::move(commands));
}

void WebTextInput::PerformAction(std::uint32_t event_token, TextInputAction action) {
  if (!Accepts(event_token)) {
    return;
  }
  runtime_->PerformTextInputAction(session_id_, action);
}

} // namespace huxerui::detail

extern "C" {

EMSCRIPTEN_KEEPALIVE void huxerui_web_text_edit(
    std::uintptr_t web_session_id,
    std::uint32_t event_token,
    std::int32_t start,
    std::int32_t end,
    const char* text,
    std::int32_t anchor,
    std::int32_t active
) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "edit", [&](auto& input) {
    input.CommitEdit(event_token, {start, end}, text == nullptr ? std::string{} : text, {anchor, active});
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_text_selection(
    std::uintptr_t web_session_id, std::uint32_t event_token, std::int32_t anchor, std::int32_t active
) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "selection", [&](auto& input) {
    input.SetSelection(event_token, {anchor, active});
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_text_composition_start(
    std::uintptr_t web_session_id, std::uint32_t event_token, std::int32_t start, std::int32_t end
) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "composition start", [&](auto& input) {
    input.BeginComposition(event_token, {start, end});
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_text_composition_update(
    std::uintptr_t web_session_id, std::uint32_t event_token, const char* text, std::int32_t anchor, std::int32_t active
) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "composition update", [&](auto& input) {
    input.UpdateComposition(event_token, text == nullptr ? std::string{} : text, {anchor, active});
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_text_composition_end(
    std::uintptr_t web_session_id, std::uint32_t event_token, const char* text, std::int32_t anchor, std::int32_t active
) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "composition end", [&](auto& input) {
    input.EndComposition(event_token, text == nullptr ? std::string{} : text, {anchor, active});
  });
}

EMSCRIPTEN_KEEPALIVE void
huxerui_web_text_action(std::uintptr_t web_session_id, std::uint32_t event_token, int action) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "action", [&](auto& input) {
    input.PerformAction(event_token, static_cast<huxerui::TextInputAction>(std::clamp(action, 0, 6)));
  });
}
}
