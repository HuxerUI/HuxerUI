#include "linux_text_input.h"
#include "linux_text_input_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/app.h>

namespace huxerui::detail {

std::optional<TextOffset> LinuxUtf8ByteToUtf16(std::string_view text, int byte_offset) noexcept {
  if (byte_offset < 0 || static_cast<std::size_t>(byte_offset) > text.size() ||
      !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  TextOffset result = 0;
  const char* current = text.data();
  const char* end = text.data() + byte_offset;
  while (current < end) {
    const gunichar code_point = g_utf8_get_char(current);
    current = g_utf8_next_char(current);
    if (current > end) {
      return std::nullopt;
    }
    result += code_point > 0xFFFFU ? 2 : 1;
  }
  return result;
}

std::optional<int> LinuxUtf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept {
  if (offset < 0 || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    return std::nullopt;
  }
  TextOffset utf16 = 0;
  const char* current = text.data();
  const char* end = text.data() + text.size();
  while (current < end && utf16 < offset) {
    const gunichar code_point = g_utf8_get_char(current);
    const TextOffset units = code_point > 0xFFFFU ? 2 : 1;
    if (utf16 + units > offset) {
      return std::nullopt;
    }
    utf16 += units;
    current = g_utf8_next_char(current);
  }
  return utf16 == offset ? std::optional<int>(static_cast<int>(current - text.data())) : std::nullopt;
}

std::optional<LinuxDeleteSurroundingPlan> ResolveLinuxDeleteSurrounding(int offset, int characters) noexcept {
  if (characters < 0) {
    return std::nullopt;
  }
  const std::int64_t start = offset;
  const std::int64_t end = start + characters;
  if (start > 0 || end < 0 || -start > std::numeric_limits<TextOffset>::max() ||
      end > std::numeric_limits<TextOffset>::max()) {
    return std::nullopt;
  }
  return LinuxDeleteSurroundingPlan{
      .before = static_cast<TextOffset>(-start),
      .after = static_cast<TextOffset>(end),
  };
}

namespace {

GtkInputPurpose InputPurpose(const TextInputConfiguration& configuration) noexcept {
  if (configuration.secure) {
    return GTK_INPUT_PURPOSE_PASSWORD;
  }
  switch (configuration.type) {
  case TextInputType::Text:
    return GTK_INPUT_PURPOSE_FREE_FORM;
  case TextInputType::Email:
    return GTK_INPUT_PURPOSE_EMAIL;
  case TextInputType::Number:
    return GTK_INPUT_PURPOSE_DIGITS;
  case TextInputType::Decimal:
    return GTK_INPUT_PURPOSE_NUMBER;
  case TextInputType::Phone:
    return GTK_INPUT_PURPOSE_PHONE;
  case TextInputType::Url:
    return GTK_INPUT_PURPOSE_URL;
  }
  return GTK_INPUT_PURPOSE_FREE_FORM;
}

GtkInputHints InputHints(const TextInputConfiguration& configuration) noexcept {
  GtkInputHints hints = GTK_INPUT_HINT_NONE;
  if (!configuration.autocorrect) {
    hints = static_cast<GtkInputHints>(hints | GTK_INPUT_HINT_NO_SPELLCHECK);
  }
  if (configuration.secure) {
    hints = static_cast<GtkInputHints>(hints | GTK_INPUT_HINT_PRIVATE | GTK_INPUT_HINT_NO_EMOJI);
  }
  switch (configuration.capitalization) {
  case TextCapitalization::None:
    break;
  case TextCapitalization::Characters:
    hints = static_cast<GtkInputHints>(hints | GTK_INPUT_HINT_UPPERCASE_CHARS);
    break;
  case TextCapitalization::Words:
    hints = static_cast<GtkInputHints>(hints | GTK_INPUT_HINT_UPPERCASE_WORDS);
    break;
  case TextCapitalization::Sentences:
    hints = static_cast<GtkInputHints>(hints | GTK_INPUT_HINT_UPPERCASE_SENTENCES);
    break;
  }
  return hints;
}

} // namespace

struct LinuxTextInput::State {
  State() : context(gtk_im_multicontext_new()) {
    g_signal_connect(context, "preedit-start", G_CALLBACK(PreeditStart), this);
    g_signal_connect(context, "preedit-changed", G_CALLBACK(PreeditChanged), this);
    g_signal_connect(context, "preedit-end", G_CALLBACK(PreeditEnd), this);
    g_signal_connect(context, "commit", G_CALLBACK(Commit), this);
    g_signal_connect(context, "retrieve-surrounding", G_CALLBACK(RetrieveSurrounding), this);
    g_signal_connect(context, "delete-surrounding", G_CALLBACK(DeleteSurrounding), this);
  }

  ~State() {
    g_object_unref(context);
  }

  bool IsActive() const noexcept {
    return runtime != nullptr && session_id != 0 && !configuration.read_only;
  }

  TextInputApplyResult Apply(std::vector<TextInputCommand> commands) {
    if (!IsActive() || commands.empty()) {
      return {};
    }
    TextInputCommandBatch batch{.session_id = session_id, .commands = std::move(commands)};
    return runtime->HandleTextInputCommands(batch);
  }

  void UpdateContext() {
    if (!IsActive()) {
      return;
    }
    GdkRectangle cursor{
        static_cast<int>(std::floor(geometry.caret.x)),
        static_cast<int>(std::floor(geometry.caret.y)),
        std::max(1, static_cast<int>(std::ceil(geometry.caret.width))),
        std::max(1, static_cast<int>(std::ceil(geometry.caret.height))),
    };
    gtk_im_context_set_cursor_location(context, &cursor);
    g_object_set(
        context,
        "input-purpose",
        InputPurpose(configuration),
        "input-hints",
        InputHints(configuration),
        nullptr
    );
    gtk_im_context_set_use_preedit(context, !configuration.secure);
    if (configuration.secure) {
      return;
    }
    const TextOffset selection_start = state.selection.Range().start;
    const TextOffset context_start = std::max<TextOffset>(0, selection_start - 2048);
    const TextInputContext surrounding = runtime->QueryTextInputContext(session_id, context_start, 4096);
    if (surrounding.result_code != TextInputResultCode::Ok) {
      return;
    }
    const std::optional<int> cursor_byte =
        LinuxUtf16ToUtf8Byte(surrounding.text, surrounding.selection.active - surrounding.slice_start);
    const std::optional<int> anchor_byte =
        LinuxUtf16ToUtf8Byte(surrounding.text, surrounding.selection.anchor - surrounding.slice_start);
    if (cursor_byte.has_value() && anchor_byte.has_value()) {
      gtk_im_context_set_surrounding_with_selection(
          context,
          surrounding.text.data(),
          static_cast<int>(surrounding.text.size()),
          *cursor_byte,
          *anchor_byte
      );
    }
  }

  static void PreeditStart(GtkIMContext*, gpointer user_data) {
    auto& self = *static_cast<State*>(user_data);
    if (!self.IsActive() || self.composing || self.configuration.secure) {
      return;
    }
    TextInputCommand begin;
    begin.kind = TextInputCommandKind::BeginComposition;
    begin.target = self.state.selection.Range();
    self.composing = self.Apply({std::move(begin)}).result_code == TextInputResultCode::Ok;
  }

  static void PreeditChanged(GtkIMContext* context, gpointer user_data) {
    auto& self = *static_cast<State*>(user_data);
    if (!self.IsActive() || self.configuration.secure) {
      return;
    }
    char* text = nullptr;
    PangoAttrList* attributes = nullptr;
    int cursor_chars = 0;
    gtk_im_context_get_preedit_string(context, &text, &attributes, &cursor_chars);
    const std::string preedit = text != nullptr ? text : "";
    if (attributes != nullptr) {
      pango_attr_list_unref(attributes);
    }
    g_free(text);
    if (!self.composing) {
      PreeditStart(context, user_data);
    }
    const char* cursor = g_utf8_offset_to_pointer(preedit.c_str(), cursor_chars);
    const TextOffset cursor_utf16 =
        LinuxUtf8ByteToUtf16(preedit, static_cast<int>(cursor - preedit.c_str())).value_or(0);
    const TextOffset composition_start = self.state.composition.value_or(self.state.selection.Range()).start;
    TextInputCommand update;
    update.kind = TextInputCommandKind::UpdateComposition;
    update.text = preedit;
    update.selection_after = TextSelection{
        composition_start + cursor_utf16,
        composition_start + cursor_utf16,
        TextAffinity::Downstream,
    };
    self.composing = self.Apply({std::move(update)}).result_code == TextInputResultCode::Ok;
  }

  static void PreeditEnd(GtkIMContext*, gpointer user_data) {
    auto& self = *static_cast<State*>(user_data);
    if (!self.IsActive() || !self.composing) {
      return;
    }
    TextInputCommand finish;
    finish.kind = TextInputCommandKind::FinishComposition;
    static_cast<void>(self.Apply({std::move(finish)}));
    self.composing = false;
  }

  static void Commit(GtkIMContext*, const char* text, gpointer user_data) {
    auto& self = *static_cast<State*>(user_data);
    if (!self.IsActive() || text == nullptr) {
      return;
    }
    TextInputCommand commit;
    commit.kind = TextInputCommandKind::CommitText;
    commit.text = text;
    static_cast<void>(self.Apply({std::move(commit)}));
    self.composing = false;
  }

  static gboolean RetrieveSurrounding(GtkIMContext*, gpointer user_data) {
    auto& self = *static_cast<State*>(user_data);
    if (!self.IsActive() || self.configuration.secure) {
      return FALSE;
    }
    self.UpdateContext();
    return TRUE;
  }

  static gboolean DeleteSurrounding(GtkIMContext*, int offset, int characters, gpointer user_data) {
    auto& self = *static_cast<State*>(user_data);
    const std::optional<LinuxDeleteSurroundingPlan> plan = ResolveLinuxDeleteSurrounding(offset, characters);
    if (!self.IsActive() || !plan.has_value()) {
      return FALSE;
    }
    TextInputCommand deletion;
    deletion.kind = TextInputCommandKind::DeleteSurrounding;
    deletion.delete_before = plan->before;
    deletion.delete_after = plan->after;
    deletion.delete_unit = TextInputUnit::UnicodeCodePoint;
    return self.Apply({std::move(deletion)}).result_code == TextInputResultCode::Ok;
  }

  Runtime* runtime = nullptr;
  GtkIMContext* context = nullptr;
  GtkWidget* widget = nullptr;
  TextInputSessionId session_id = 0;
  TextInputConfiguration configuration;
  TextInputState state;
  TextInputGeometry geometry;
  bool focused = false;
  bool composing = false;
};

LinuxTextInput::LinuxTextInput() : state_(std::make_unique<State>()) {}

LinuxTextInput::~LinuxTextInput() = default;

void LinuxTextInput::SetRuntime(Runtime* runtime) noexcept {
  state_->runtime = runtime;
}

void LinuxTextInput::SetClientWidget(GtkWidget* widget) {
  state_->widget = widget;
  gtk_im_context_set_client_widget(state_->context, widget);
}

void LinuxTextInput::SetFocus(bool focused) {
  if (state_->focused == focused) {
    return;
  }
  state_->focused = focused;
  if (focused && state_->IsActive()) {
    gtk_im_context_focus_in(state_->context);
  } else {
    gtk_im_context_focus_out(state_->context);
  }
}

void LinuxTextInput::Reset() noexcept {
  if (state_->context != nullptr) {
    gtk_im_context_reset(state_->context);
    gtk_im_context_focus_out(state_->context);
    gtk_im_context_set_client_widget(state_->context, nullptr);
  }
  state_->widget = nullptr;
  state_->runtime = nullptr;
  state_->session_id = 0;
  state_->composing = false;
}

bool LinuxTextInput::Active() const noexcept {
  return state_->IsActive();
}

bool LinuxTextInput::Composing() const noexcept {
  return state_->composing;
}

bool LinuxTextInput::FilterKeyEvent(GdkEvent* event) {
  return state_->IsActive() && event != nullptr && gtk_im_context_filter_keypress(state_->context, event) != FALSE;
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
  state_->composing = state.composition.has_value();
  state_->UpdateContext();
  if (state_->focused && state_->IsActive()) {
    gtk_im_context_focus_in(state_->context);
  }
}

void LinuxTextInput::Update(
    TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry
) {
  if (session_id != state_->session_id) {
    return;
  }
  state_->state = state;
  state_->geometry = geometry;
  state_->composing = state.composition.has_value();
  state_->UpdateContext();
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
  gtk_im_context_reset(state_->context);
  Start(session_id, configuration, state, geometry);
}

void LinuxTextInput::Stop(TextInputSessionId session_id) {
  if (session_id != state_->session_id) {
    return;
  }
  gtk_im_context_reset(state_->context);
  gtk_im_context_focus_out(state_->context);
  state_->session_id = 0;
  state_->composing = false;
  state_->configuration = {};
}

} // namespace huxerui::detail
