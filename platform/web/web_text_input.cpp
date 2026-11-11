#include "web_text_input.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <emscripten.h>

#include <huxerui/app.h>

#include "text/text_input_internal.h"

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

bool Contains(TextRange outer, TextRange inner) noexcept {
  return inner.IsValid() && inner.start >= outer.start && inner.end <= outer.end;
}

TextRange RequiredContextRange(const TextInputContext& context) noexcept {
  auto required = context.selection.Range();
  if (context.composition) {
    required.start = std::min(required.start, context.composition->start);
    required.end = std::max(required.end, context.composition->end);
  }
  return required;
}

struct WebTextSnapshot {
  std::string text;
  TextRange range;
  TextSelection selection;
};

std::optional<WebTextSnapshot> MakeWebTextSnapshot(TextInputContext context) {
  if (context.result_code != TextInputResultCode::Ok) return std::nullopt;
  const auto length = Utf16Length(context.text);
  if (!length || *length > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
  const TextRange range{context.slice_start, context.slice_start + *length};
  if (!Contains(range, RequiredContextRange(context))) return std::nullopt;
  return WebTextSnapshot{std::move(context.text), range, context.selection};
}

std::optional<WebTextSnapshot> ReadWebTextSnapshot(UiWindow& window, TextInputSessionId session) {
  auto context = window.QueryTextInputContext(session, 0, 0);
  if (context.result_code != TextInputResultCode::Ok) return std::nullopt;
  const auto required = RequiredContextRange(context);
  constexpr TextOffset surrounding_units = 2048;
  const TextRange surrounding{
      required.start - std::min(required.start, surrounding_units),
      required.end + std::min(context.total_length - required.end, surrounding_units),
  };
  auto snapshot = MakeWebTextSnapshot(std::move(context));
  if (!snapshot || !Contains(snapshot->range, surrounding)) {
    auto expanded = MakeWebTextSnapshot(window.QueryTextInputContext(session, surrounding.start, surrounding.Length()));
    if (expanded) return expanded;
  }
  return snapshot;
}

std::optional<TextSelection> ToDocumentSelection(TextSelection selection, TextRange snapshot) noexcept {
  if (!Contains({0, snapshot.Length()}, selection.Range())) return std::nullopt;
  selection.anchor += snapshot.start;
  selection.active += snapshot.start;
  return selection;
}

enum class SnapshotSync { Unavailable, Published, Composing };

// clang-format off
EM_JS(bool, PrepareWebTextInput, (
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
    const toSliceOffset = (offset) => {
      for (const omitted of session.textOmittedOffsets) {
        if (omitted > offset) break;
        ++offset;
      }
      return offset;
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
        if (session.activeTextInput !== element || element.readOnly || session.textSynchronizing ||
            session.textComposing) {
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
        const targetStart = toSliceOffset(prefix);
        const targetEnd = toSliceOffset(previousEnd);
        const inserted = current.slice(prefix, currentEnd);
        const toEditedSliceOffset = (offset) => {
          if (offset <= prefix) return toSliceOffset(offset);
          if (offset <= currentEnd) return targetStart + offset - prefix;
          return toSliceOffset(previousEnd + offset - currentEnd) + inserted.length - (targetEnd - targetStart);
        };
        const [anchor, active] = selection(element);
        const pointer = Module.stringToNewUTF8(inserted);
        Module._huxerui_web_text_edit(
            web_session_id,
            session.textToken,
            targetStart,
            targetEnd,
            pointer,
            toEditedSliceOffset(anchor),
            toEditedSliceOffset(active)
        );
        _free(pointer);
      });

      listen("select", () => {
        if (session.activeTextInput !== element || session.textSynchronizing || session.textComposing ||
            element.value !== session.textValue) {
          return;
        }
        const [anchor, active] = selection(element).map(toSliceOffset);
        Module._huxerui_web_text_selection(web_session_id, session.textToken, anchor, active);
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
        if (session.activeTextInput !== element || element.readOnly) {
          return;
        }
        const [anchor, active] = selection(element).map(toSliceOffset);
        session.textComposing = true;
        Module._huxerui_web_text_composition_start(
            web_session_id,
            session.textToken,
            Math.min(anchor, active),
            Math.max(anchor, active)
        );
      });

      listen("compositionupdate", (event) => {
        if (session.activeTextInput !== element || !session.textComposing) {
          return;
        }
        const value = event.data || "";
        const pointer = Module.stringToNewUTF8(value);
        Module._huxerui_web_text_composition_update(web_session_id, session.textToken, pointer);
        _free(pointer);
      });

      listen("compositionend", (event) => {
        if (session.activeTextInput !== element || !session.textComposing) {
          return;
        }
        session.textComposing = false;
        const value = event.data || "";
        const pointer = Module.stringToNewUTF8(value);
        Module._huxerui_web_text_composition_end(web_session_id, session.textToken, pointer);
        _free(pointer);
      });

      listen("keydown", (event) => {
        if (session.activeTextInput !== element || session.textComposing || event.isComposing) {
          return;
        }
        if (session.sendKey(event, 0, false)) {
          event.preventDefault();
          event.stopPropagation();
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
        if (session.activeTextInput !== element || session.textComposing || event.isComposing) {
          return;
        }
        if (session.sendKey(event, 1, false)) {
          event.preventDefault();
          event.stopPropagation();
        }
      });
    };

    if (!session.textInput) {
      const multilineBreaks = new RegExp("\\r\\n?", "g");
      const singlelineBreaks = new RegExp("[\\r\\n]", "g");
      session.publishTextSnapshot = (element, textPointer, anchor, active) => {
        const source = textPointer ? UTF8ToString(textPointer) : "";
        const multiline = element === session.textArea;
        const omittedOffsets = [];
        // The DOM normalizes line breaks before exposing value and selection offsets.
        const value = source.replace(multiline ? multilineBreaks : singlelineBreaks, (lineBreak, offset) => {
          if (!multiline || lineBreak.length === 2) omittedOffsets.push(offset + lineBreak.length - 1);
          return multiline ? "\n" : "";
        });
        const toDomOffset = (offset) => {
          let omitted = 0;
          while (omitted < omittedOffsets.length && omittedOffsets[omitted] < offset) ++omitted;
          return offset - omitted;
        };
        session.textSynchronizing = true;
        try {
          session.textValue = value;
          session.textOmittedOffsets = omittedOffsets;
          element.readOnly = !textPointer;
          if (element.value !== value) element.value = value;
          const start = toDomOffset(Math.min(anchor, active));
          const end = toDomOffset(Math.max(anchor, active));
          element.setSelectionRange(start, end, anchor > active ? "backward" : "forward");
        } finally {
          session.textSynchronizing = false;
        }
      };
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

    element.style.display = "block";
    if (element === session.textInput) {
      element.type = secure ? "password" : "text";
    }
    element.inputMode = ["text", "email", "numeric", "decimal", "tel", "url"][input_type] || "text";
    element.autocapitalize = ["none", "characters", "words", "sentences"][capitalization] || "none";
    element.enterKeyHint = ["enter", "done", "go", "next", "search", "send", "enter"][action] || "enter";
    element.autocomplete = secure ? "new-password" : autocorrect ? "on" : "off";
    element.spellcheck = autocorrect;

    const rootBounds = session.root.getBoundingClientRect();
    element.style.left = String(rootBounds.left + caret_x) + "px";
    element.style.top = String(rootBounds.top + caret_y) + "px";
    element.style.height = String(Math.max(1, caret_height)) + "px";
    session.publishTextSnapshot(element, text, anchor, active);
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

EM_JS(int, SynchronizeWebTextInput, (
    std::uintptr_t web_session_id,
    std::uint32_t event_token,
    const char* text,
    std::int32_t anchor,
    std::int32_t active,
    bool has_caret,
    float caret_x,
    float caret_y,
    float caret_height
), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session || !session.activeTextInput || session.textToken !== event_token) {
    return 0;
  }
  const element = session.activeTextInput;
  try {
    if (has_caret) {
      const rootBounds = session.root.getBoundingClientRect();
      element.style.left = String(rootBounds.left + caret_x) + "px";
      element.style.top = String(rootBounds.top + caret_y) + "px";
      element.style.height = String(Math.max(1, caret_height)) + "px";
    }
    // Replacing the DOM value during composition can cancel the browser's IME session.
    if (session.textComposing) return 2;

    session.publishTextSnapshot(element, text, anchor, active);
    return 1;
  } catch (error) {
    element.readOnly = true;
    element.value = "";
    session.textValue = "";
    session.textOmittedOffsets = [];
    console.error("HuxerUI Web text input synchronization failed", error);
    return 0;
  }
});

EM_JS(bool, DeactivateWebTextInput, (std::uintptr_t web_session_id, std::uint32_t event_token), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session || !session.activeTextInput || session.textToken !== event_token) {
    return false;
  }
  const element = session.activeTextInput;
  const wasFocused = document.activeElement === element;
  session.activeTextInput = null;
  session.textToken = 0;
  session.textComposing = false;
  try {
    element.blur();
    element.style.display = "none";
    if (wasFocused) {
      queueMicrotask(() => {
        if (Module.huxerUIWebSessions.get(web_session_id) === session && document.activeElement === document.body) {
          session.root.focus({preventScroll: true});
        }
      });
    }
  } catch (error) {
    console.error("HuxerUI Web text input deactivation failed", error);
  }
  return wasFocused;
});

EM_JS(void, SetWebTextInputFocus, (std::uintptr_t web_session_id, std::uint32_t event_token, bool focused), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(web_session_id);
  if (!session || !session.activeTextInput || session.textToken !== event_token) {
    return;
  }
  try {
    if (focused) {
      session.activeTextInput.focus({preventScroll: true});
    } else if (document.activeElement === session.activeTextInput) {
      session.root.focus({preventScroll: true});
    }
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

void WebTextInput::SetUiWindow(UiWindow* ui_window) noexcept {
  ui_window_ = ui_window;
}

void WebTextInput::Reset() noexcept {
  if (event_token_ != 0) {
    DeactivateWebTextInput(web_session_id_, event_token_);
  }
  ui_window_ = nullptr;
  session_id_ = 0;
  event_token_ = 0;
  configuration_ = {};
  state_ = {};
  snapshot_range_.reset();
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
  Prepare(geometry);
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
  const bool restore_focus = event_token_ != 0 && DeactivateWebTextInput(web_session_id_, event_token_);
  configuration_ = configuration;
  state_ = state;
  event_token_ = NextEventToken();
  Prepare(geometry);
  if (restore_focus) {
    SetWebTextInputFocus(web_session_id_, event_token_, true);
  }
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
  snapshot_range_.reset();
}

void WebTextInput::RequestShowKeyboard(TextInputSessionId session_id) {
  if (session_id == session_id_ && event_token_ != 0) {
    SetWebTextInputFocus(web_session_id_, event_token_, true);
  }
}

void WebTextInput::RequestHideKeyboard(TextInputSessionId session_id) {
  if (session_id == session_id_ && event_token_ != 0) {
    SetWebTextInputFocus(web_session_id_, event_token_, false);
  }
}

bool WebTextInput::Accepts(std::uint32_t event_token) const noexcept {
  return ui_window_ != nullptr && snapshot_range_ && session_id_ != 0 && event_token != 0 && event_token == event_token_;
}

std::uint32_t WebTextInput::NextEventToken() noexcept {
  static std::uint32_t next_token = 1;
  if (next_token == 0) {
    next_token = 1;
  }
  return next_token++;
}

void WebTextInput::Prepare(const TextInputGeometry& geometry) {
  snapshot_range_.reset();
  if (!ui_window_) return;
  const auto snapshot = ReadWebTextSnapshot(*ui_window_, session_id_);
  const Rect caret = geometry.result_code == TextInputResultCode::Ok ? geometry.caret : Rect{};
  const bool prepared = PrepareWebTextInput(
      web_session_id_,
      event_token_,
      static_cast<int>(configuration_.type),
      static_cast<int>(configuration_.capitalization),
      static_cast<int>(configuration_.action),
      configuration_.multiline,
      configuration_.secure,
      configuration_.autocorrect,
      snapshot ? snapshot->text.c_str() : nullptr,
      snapshot ? static_cast<std::int32_t>(snapshot->selection.anchor - snapshot->range.start) : 0,
      snapshot ? static_cast<std::int32_t>(snapshot->selection.active - snapshot->range.start) : 0,
      caret.x, caret.y, caret.height
  );
  if (prepared && snapshot) snapshot_range_ = snapshot->range;
}

void WebTextInput::Synchronize(const TextInputGeometry& geometry) {
  if (!ui_window_ || event_token_ == 0) return;
  const auto snapshot = ReadWebTextSnapshot(*ui_window_, session_id_);
  const bool has_caret = geometry.result_code == TextInputResultCode::Ok;
  const Rect caret = has_caret ? geometry.caret : Rect{};
  const auto result = static_cast<SnapshotSync>(SynchronizeWebTextInput(
      web_session_id_,
      event_token_,
      snapshot ? snapshot->text.c_str() : nullptr,
      snapshot ? static_cast<std::int32_t>(snapshot->selection.anchor - snapshot->range.start) : 0,
      snapshot ? static_cast<std::int32_t>(snapshot->selection.active - snapshot->range.start) : 0,
      has_caret, caret.x, caret.y, caret.height));
  if (result != SnapshotSync::Composing) {
    snapshot_range_ = result == SnapshotSync::Published && snapshot ? std::optional(snapshot->range) : std::nullopt;
  }
}

void WebTextInput::Resynchronize() {
  if (ui_window_ && session_id_ != 0) {
    Synchronize(ui_window_->QueryTextInputGeometry(session_id_, state_.selection.Range()));
  }
}

void WebTextInput::Apply(std::vector<TextInputCommand> commands) {
  if (ui_window_ == nullptr || commands.empty()) {
    return;
  }
  TextInputCommandBatch batch;
  batch.session_id = session_id_;
  batch.commands = std::move(commands);
  const TextInputApplyResult result = ui_window_->HandleTextInputCommands(batch);
  if (result.result_code != TextInputResultCode::Ok) Resynchronize();
}

void WebTextInput::CommitEdit(std::uint32_t event_token, TextRange target, std::string text, TextSelection selection) {
  if (!Accepts(event_token)) return;
  const auto length = Utf16Length(text);
  if (!length || !Contains({0, snapshot_range_->Length()}, target)) {
    Resynchronize();
    return;
  }
  const TextOffset unchanged_end = snapshot_range_->end - target.Length();
  if (*length > std::numeric_limits<TextOffset>::max() - unchanged_end) {
    Resynchronize();
    return;
  }
  const auto selected = ToDocumentSelection(selection, {snapshot_range_->start, unchanged_end + *length});
  if (!selected) {
    Resynchronize();
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::CommitText;
  command.target = TextRange{snapshot_range_->start + target.start, snapshot_range_->start + target.end};
  command.selection_after = *selected;
  command.text = std::move(text);
  Apply({std::move(command)});
}

void WebTextInput::SetSelection(std::uint32_t event_token, TextSelection selection) {
  if (!Accepts(event_token)) return;
  const auto selected = ToDocumentSelection(selection, *snapshot_range_);
  if (!selected || *selected == state_.selection) return;
  TextInputCommand command;
  command.kind = TextInputCommandKind::SetSelection;
  command.selection_after = *selected;
  Apply({std::move(command)});
}

void WebTextInput::BeginComposition(std::uint32_t event_token, TextRange target) {
  if (!Accepts(event_token) || !Contains({0, snapshot_range_->Length()}, target)) {
    return;
  }
  TextInputCommand command;
  command.kind = TextInputCommandKind::BeginComposition;
  command.target = TextRange{snapshot_range_->start + target.start, snapshot_range_->start + target.end};
  Apply({std::move(command)});
}

void WebTextInput::ApplyCompositionText(std::uint32_t event_token, std::string text, bool finish) {
  if (!Accepts(event_token)) return;
  if (!state_.composition) {
    if (finish) Resynchronize();
    return;
  }
  std::vector<TextInputCommand> commands;
  auto& update = commands.emplace_back();
  update.kind = TextInputCommandKind::UpdateComposition;
  update.text = std::move(text);
  if (finish) commands.emplace_back().kind = TextInputCommandKind::FinishComposition;
  Apply(std::move(commands));
}

void WebTextInput::PerformAction(std::uint32_t event_token, TextInputAction action) {
  if (!Accepts(event_token)) {
    return;
  }
  ui_window_->PerformTextInputAction(session_id_, action);
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

EMSCRIPTEN_KEEPALIVE void
huxerui_web_text_composition_update(std::uintptr_t web_session_id, std::uint32_t event_token, const char* text) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "composition update", [&](auto& input) {
    input.ApplyCompositionText(event_token, text == nullptr ? std::string{} : text, false);
  });
}

EMSCRIPTEN_KEEPALIVE void
huxerui_web_text_composition_end(std::uintptr_t web_session_id, std::uint32_t event_token, const char* text) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "composition end", [&](auto& input) {
    input.ApplyCompositionText(event_token, text == nullptr ? std::string{} : text, true);
  });
}

EMSCRIPTEN_KEEPALIVE void
huxerui_web_text_action(std::uintptr_t web_session_id, std::uint32_t event_token, int action) {
  huxerui::detail::DispatchWebTextInput(web_session_id, "action", [&](auto& input) {
    input.PerformAction(event_token, static_cast<huxerui::TextInputAction>(std::clamp(action, 0, 6)));
  });
}
}
