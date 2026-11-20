#include "prompt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <istream>
#include <ostream>
#include <utility>

namespace huxerui::cli::prompt {
namespace {

// The prompt's symbols, and the ASCII stand-ins for a terminal without them.
struct Glyphs {
  std::string_view active;
  std::string_view done;
  std::string_view cancelled;
  std::string_view rail;
  std::string_view top;
  std::string_view bottom;
  std::string_view selected;
  std::string_view unselected;
  std::string_view checked;
  std::string_view unchecked;
  std::string_view field;
  std::string_view problem;
  std::string_view success;
  std::string_view move;
  std::string_view separator;
  std::string_view ellipsis;
};

constexpr Glyphs unicode_glyphs{"◆", "◇", "■", "│", "┌", "└", "●", "○", "◼", "◻", "›", "▲", "✔", "↑/↓", " · ", "…"};
constexpr Glyphs ascii_glyphs{"*", "+", "x", "|", "+", "+", "(*)", "( )", "[x]", "[ ]", ">", "!", "ok", "up/down",
                              " - ", "..."};

const Glyphs& GlyphsFor(const Style& style) {
  return style.unicode ? unicode_glyphs : ascii_glyphs;
}

// Answers line up after the longest question the CLI asks.
constexpr std::size_t question_width = 14;

std::size_t DisplayWidth(std::string_view text) {
  // Every character the prompt draws takes one column; UTF-8 continuation bytes add none.
  return static_cast<std::size_t>(std::ranges::count_if(text, [](char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
  }));
}

std::string Padded(std::string_view text, std::size_t width) {
  std::string padded(text);
  const std::size_t used = DisplayWidth(text);
  if (used < width) {
    padded.append(width - used, ' ');
  }
  return padded;
}

Span Rail(const Glyphs& glyphs) {
  return {std::string(glyphs.rail) + "  ", Role::Muted};
}

void PopCodePoint(std::string& text) {
  // UTF-8 continuation bytes have the form 10xxxxxx; remove them with their lead byte.
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
    text.pop_back();
  }
  if (!text.empty()) {
    text.pop_back();
  }
}

std::string Trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = value.find_last_not_of(" \t\r");
  return std::string(value.substr(first, last - first + 1));
}

std::optional<std::size_t> ParseChoiceNumber(std::string_view value, std::size_t count) {
  std::size_t number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size() || number == 0 || number > count) {
    return std::nullopt;
  }
  return number - 1;
}

std::string ChoiceLine(const Choice& choice) {
  const std::string hint = PlainText(choice.hint);
  return hint.empty() ? choice.label : choice.label + "  (" + hint + ')';
}

// The SGR parameters of a role at a color depth. The two brand colors are HuxerUI's violet (#5B51E6) and the C++
// logo's blue (#659AD2); with 16 colors the violet is magenta, so the two never become one blue.
std::string_view RoleColor(Role role, ColorDepth depth) {
  const auto pick = [depth](std::string_view basic, std::string_view palette, std::string_view full) {
    return depth == ColorDepth::True ? full : depth == ColorDepth::Palette ? palette : basic;
  };
  switch (role) {
  case Role::Plain:
    return {};
  case Role::Strong:
    return "1";
  case Role::Brand:
    return pick("1;95", "1;38;5;62", "1;38;2;91;81;230");
  case Role::Accent:
    return pick("95", "38;5;62", "38;2;91;81;230");
  case Role::Muted:
    return pick("90", "38;5;244", "38;2;128;128;128");
  case Role::Headers:
    return pick("36", "38;5;36", "38;2;42;161;152");
  case Role::Modules:
    return pick("34", "38;5;68", "38;2;101;154;210");
  case Role::Success:
    return "32";
  case Role::Error:
    return "31";
  }
  return {};
}

} // namespace

Choice::Choice(std::string label_value, std::string hint_value) : label(std::move(label_value)) {
  if (!hint_value.empty()) {
    hint.push_back({std::move(hint_value), Role::Muted});
  }
}

Choice::Choice(std::string label_value, Line hint_value)
    : label(std::move(label_value)), hint(std::move(hint_value)) {}

State MakeSelect(std::string question, std::vector<Choice> choices, std::size_t initial) {
  State state;
  state.kind = Kind::Select;
  state.question = std::move(question);
  state.choices = std::move(choices);
  state.cursor = initial < state.choices.size() ? initial : 0;
  return state;
}

State MakeMultiSelect(std::string question, std::vector<Choice> choices,
                      std::span<const std::size_t> initially_checked, std::size_t minimum) {
  State state;
  state.kind = Kind::MultiSelect;
  state.question = std::move(question);
  state.choices = std::move(choices);
  state.minimum = minimum;
  state.checked.assign(state.choices.size(), false);
  for (const std::size_t index : initially_checked) {
    if (index < state.checked.size()) {
      state.checked[index] = true;
    }
  }
  return state;
}

State MakeText(std::string question, std::string default_text, Validator validate) {
  State state;
  state.kind = Kind::Text;
  state.question = std::move(question);
  state.default_text = std::move(default_text);
  state.validate = std::move(validate);
  return state;
}

State MakeConfirm(std::string question, bool default_yes) {
  State state;
  state.kind = Kind::Confirm;
  state.question = std::move(question);
  state.yes = default_yes;
  return state;
}

void Apply(State& state, const KeyEvent& event) {
  if (state.status != Status::Active) {
    return;
  }
  if (event.key == Key::Cancel) {
    state.status = Status::Cancelled;
    return;
  }
  state.error.clear();
  const std::size_t count = state.choices.size();
  const bool up = event.key == Key::Up || (event.key == Key::Text && event.text == "k");
  const bool down = event.key == Key::Down || (event.key == Key::Text && event.text == "j");
  switch (state.kind) {
  case Kind::Select:
  case Kind::MultiSelect:
    if (up && count > 0) {
      state.cursor = (state.cursor + count - 1) % count;
    } else if (down && count > 0) {
      state.cursor = (state.cursor + 1) % count;
    } else if (event.key == Key::Space && state.kind == Kind::MultiSelect && count > 0) {
      state.checked[state.cursor] = !state.checked[state.cursor];
    } else if (event.key == Key::Text && event.text == "a" && state.kind == Kind::MultiSelect) {
      const bool all = std::ranges::all_of(state.checked, [](bool checked) { return checked; });
      state.checked.assign(state.checked.size(), !all);
    } else if (event.key == Key::Enter) {
      if (state.kind == Kind::MultiSelect && CheckedIndices(state).size() < state.minimum) {
        state.error = "Select at least one option.";
      } else if (count > 0) {
        state.status = Status::Submitted;
      }
    }
    break;
  case Kind::Text:
    if (event.key == Key::Text) {
      state.text += event.text;
    } else if (event.key == Key::Space) {
      state.text += ' ';
    } else if (event.key == Key::Backspace) {
      PopCodePoint(state.text);
    } else if (event.key == Key::Enter) {
      std::string value = state.text.empty() ? state.default_text : state.text;
      const std::string problem = state.validate ? state.validate(value) : std::string{};
      if (!problem.empty()) {
        state.error = problem;
      } else {
        state.text = std::move(value);
        state.status = Status::Submitted;
      }
    }
    break;
  case Kind::Confirm:
    if (event.key == Key::Text && (event.text == "y" || event.text == "Y")) {
      state.yes = true;
      state.status = Status::Submitted;
    } else if (event.key == Key::Text && (event.text == "n" || event.text == "N")) {
      state.yes = false;
      state.status = Status::Submitted;
    } else if (event.key == Key::Enter) {
      state.status = Status::Submitted;
    }
    break;
  }
}

Frame Render(const State& state, const Style& style) {
  const Glyphs& glyphs = GlyphsFor(style);
  Frame frame;
  if (state.status != Status::Active) {
    const bool cancelled = state.status == Status::Cancelled;
    frame.lines.push_back({
        {std::string(cancelled ? glyphs.cancelled : glyphs.done) + "  ", cancelled ? Role::Error : Role::Success},
        {Padded(state.question, std::max(question_width, DisplayWidth(state.question) + 2)), Role::Plain},
        {cancelled ? std::string("cancelled") : Answer(state), cancelled ? Role::Muted : Role::Accent},
    });
    return frame;
  }

  const std::string separator(glyphs.separator);
  Line title{{std::string(glyphs.active) + "  ", Role::Accent}, {state.question, Role::Strong}};
  std::string help;
  switch (state.kind) {
  case Kind::Select:
  case Kind::MultiSelect: {
    frame.lines.push_back(std::move(title));
    std::size_t label_width = 0;
    for (const Choice& choice : state.choices) {
      label_width = std::max(label_width, DisplayWidth(choice.label));
    }
    for (std::size_t index = 0; index < state.choices.size(); ++index) {
      const bool current = index == state.cursor;
      std::string_view mark;
      Role mark_role = Role::Muted;
      if (state.kind == Kind::MultiSelect) {
        mark = state.checked[index] ? glyphs.checked : glyphs.unchecked;
        mark_role = state.checked[index] ? Role::Accent : Role::Muted;
      } else {
        mark = current ? glyphs.selected : glyphs.unselected;
        mark_role = current ? Role::Accent : Role::Muted;
      }
      Line line{Rail(glyphs), {std::string(mark) + ' ', mark_role},
                {Padded(state.choices[index].label, label_width + 2), current ? Role::Accent : Role::Plain}};
      line.insert(line.end(), state.choices[index].hint.begin(), state.choices[index].hint.end());
      frame.lines.push_back(std::move(line));
    }
    help = std::string(glyphs.move) + " move" + separator;
    if (state.kind == Kind::MultiSelect) {
      help += "space toggle" + separator + "a all" + separator;
    }
    help += "enter confirm" + separator + "esc cancel";
    if (state.kind == Kind::MultiSelect && state.minimum == 0) {
      help += separator + "none is fine";
    }
    break;
  }
  case Kind::Text: {
    frame.lines.push_back(std::move(title));
    Line field{Rail(glyphs), {std::string(glyphs.field) + ' ', Role::Accent}};
    if (state.text.empty()) {
      field.push_back({state.default_text, Role::Muted});
    } else {
      field.push_back({state.text, Role::Plain});
    }
    frame.cursor_line = frame.lines.size();
    frame.cursor_column = DisplayWidth(PlainText({field[0], field[1]})) + DisplayWidth(state.text);
    frame.lines.push_back(std::move(field));
    help = state.text.empty() && !state.default_text.empty()
               ? "type" + separator + "enter keeps " + state.default_text + separator + "esc cancel"
               : "enter confirm" + separator + "esc cancel";
    break;
  }
  case Kind::Confirm:
    title.push_back({state.yes ? "  (Y/n)" : "  (y/N)", Role::Muted});
    frame.lines.push_back(std::move(title));
    return frame;
  }
  if (!state.error.empty()) {
    frame.lines.push_back({Rail(glyphs), {std::string(glyphs.problem) + ' ' + state.error, Role::Error}});
  } else {
    frame.lines.push_back({Rail(glyphs), {help, Role::Muted}});
  }
  return frame;
}

Line IntroLine(std::string_view title, const Style& style) {
  return {{std::string(GlyphsFor(style).top) + "  ", Role::Muted},
          {"HuxerUI", Role::Brand},
          {std::string(GlyphsFor(style).separator) + std::string(title), Role::Muted}};
}

Line NoteLine(Line text, const Style& style) {
  Line line{Rail(GlyphsFor(style))};
  line.insert(line.end(), text.begin(), text.end());
  return line;
}

std::vector<Line> OutroLines(Line result, std::span<const std::pair<std::string, Line>> details, const Style& style) {
  const Glyphs& glyphs = GlyphsFor(style);
  std::vector<Line> lines;
  Line first{{std::string(glyphs.bottom) + "  ", Role::Muted}, {std::string(glyphs.success) + ' ', Role::Success}};
  first.insert(first.end(), result.begin(), result.end());
  lines.push_back(std::move(first));
  std::size_t label_width = 0;
  for (const auto& detail : details) {
    label_width = std::max(label_width, DisplayWidth(detail.first));
  }
  for (const auto& [label, value] : details) {
    Line line{{"   " + Padded(label, label_width + 2), Role::Muted}};
    line.insert(line.end(), value.begin(), value.end());
    lines.push_back(std::move(line));
  }
  return lines;
}

std::string PlainText(const Line& line) {
  std::string text;
  for (const Span& span : line) {
    text += span.text;
  }
  return text;
}

std::string Paint(const Line& line, const Style& style) {
  std::string painted;
  std::size_t used = 0;
  const std::string_view ellipsis = GlyphsFor(style).ellipsis;
  // One column stays free: a line that fills the last column makes some terminals wrap, which breaks the redraw.
  const std::size_t limit = style.width > 1 ? style.width - 1 : 0;
  bool cut = false;
  for (const Span& span : line) {
    std::string text = span.text;
    if (limit > 0 && used + DisplayWidth(text) > limit) {
      const std::size_t room = limit > used + DisplayWidth(ellipsis) ? limit - used - DisplayWidth(ellipsis) : 0;
      std::string kept;
      std::size_t width = 0;
      for (std::size_t index = 0; index < text.size();) {
        std::size_t length = 1;
        while (index + length < text.size() && (static_cast<unsigned char>(text[index + length]) & 0xC0U) == 0x80U) {
          ++length;
        }
        if (width + 1 > room) {
          break;
        }
        kept.append(text, index, length);
        ++width;
        index += length;
      }
      text = kept + std::string(ellipsis);
      cut = true;
    }
    used += DisplayWidth(text);
    const std::string_view color = style.color == ColorDepth::None ? std::string_view{} : RoleColor(span.role, style.color);
    if (color.empty() || text.empty()) {
      painted += text;
    } else {
      painted += "\x1b[" + std::string(color) + 'm' + text + "\x1b[0m";
    }
    if (cut) {
      break;
    }
  }
  return painted;
}

std::string Answer(const State& state) {
  switch (state.kind) {
  case Kind::Select:
    return state.cursor < state.choices.size() ? state.choices[state.cursor].label : std::string{};
  case Kind::MultiSelect: {
    std::string answer;
    for (const std::size_t index : CheckedIndices(state)) {
      answer += (answer.empty() ? "" : ", ") + state.choices[index].label;
    }
    return answer.empty() ? std::string("none") : answer;
  }
  case Kind::Text:
    return state.text;
  case Kind::Confirm:
    return state.yes ? "yes" : "no";
  }
  return {};
}

std::vector<std::size_t> CheckedIndices(const State& state) {
  std::vector<std::size_t> indices;
  for (std::size_t index = 0; index < state.checked.size(); ++index) {
    if (state.checked[index]) {
      indices.push_back(index);
    }
  }
  return indices;
}

std::optional<KeyEvent> KeyDecoder::Feed(unsigned char byte) {
  if (!pending_.empty() && pending_.front() == '\x1b') {
    pending_.push_back(static_cast<char>(byte));
    if (pending_.size() == 2) {
      if (byte == '[' || byte == 'O') {
        return std::nullopt;
      }
      pending_.clear();
      return std::nullopt;
    }
    // CSI parameters are digits and semicolons; the final byte ends the sequence.
    if ((byte >= '0' && byte <= '9') || byte == ';') {
      return std::nullopt;
    }
    pending_.clear();
    if (byte == 'A') {
      return KeyEvent{Key::Up, {}};
    }
    if (byte == 'B') {
      return KeyEvent{Key::Down, {}};
    }
    return std::nullopt;
  }
  if (!pending_.empty()) {
    pending_.push_back(static_cast<char>(byte));
    const auto lead = static_cast<unsigned char>(pending_.front());
    const std::size_t length = lead >= 0xF0U ? 4 : lead >= 0xE0U ? 3 : 2;
    if ((byte & 0xC0U) != 0x80U) {
      pending_.clear();
      return std::nullopt;
    }
    if (pending_.size() < length) {
      return std::nullopt;
    }
    return KeyEvent{Key::Text, std::exchange(pending_, {})};
  }
  switch (byte) {
  case 0x03:
  case 0x04:
    return KeyEvent{Key::Cancel, {}};
  case '\r':
  case '\n':
    return KeyEvent{Key::Enter, {}};
  case 0x08:
  case 0x7F:
    return KeyEvent{Key::Backspace, {}};
  case ' ':
    return KeyEvent{Key::Space, {}};
  case 0x1B:
    pending_.assign(1, '\x1b');
    return std::nullopt;
  default:
    break;
  }
  if (byte >= 0xC0U) {
    pending_.assign(1, static_cast<char>(byte));
    return std::nullopt;
  }
  if (byte >= 0x20U && byte < 0x7FU) {
    return KeyEvent{Key::Text, std::string(1, static_cast<char>(byte))};
  }
  return std::nullopt;
}

bool KeyDecoder::AwaitsSequence() const noexcept {
  return pending_ == "\x1b";
}

std::optional<KeyEvent> KeyDecoder::Flush() {
  if (AwaitsSequence()) {
    pending_.clear();
    return KeyEvent{Key::Cancel, {}};
  }
  return std::nullopt;
}

KeyPrompter::KeyPrompter(std::function<std::optional<KeyEvent>()> next_key, std::ostream& output,
                         std::function<Style()> style)
    : next_key_(std::move(next_key)), output_(output), style_(std::move(style)) {}

Style KeyPrompter::OutputStyle() const {
  return style_ ? style_() : Style{};
}

bool KeyPrompter::Drive(State& state) {
  std::size_t drawn = 0;
  // The row of the previous frame the terminal's cursor is on, counted from the frame's first line.
  std::size_t row = 0;
  const auto draw = [&] {
    if (drawn > 0) {
      // Back to the first line of the previous frame, then clear everything below it.
      if (row > 0) {
        output_ << "\x1b[" << row << 'F';
      } else {
        output_ << '\r';
      }
      output_ << "\x1b[J";
    }
    const Style style = OutputStyle();
    const Frame frame = Render(state, style);
    for (const Line& line : frame.lines) {
      output_ << Paint(line, style) << '\n';
    }
    drawn = frame.lines.size();
    row = drawn;
    if (frame.cursor_line && state.status == Status::Active) {
      // A text prompt shows the terminal's own cursor in its field, blinking as a bar.
      output_ << "\x1b[" << drawn - *frame.cursor_line << "A\x1b[" << frame.cursor_column + 1 << "G\x1b[5 q\x1b[?25h";
      row = *frame.cursor_line;
    } else {
      output_ << "\x1b[?25l";
    }
    output_.flush();
  };
  draw();
  while (state.status == Status::Active) {
    const std::optional<KeyEvent> event = next_key_();
    Apply(state, event.value_or(KeyEvent{Key::Cancel, {}}));
    draw();
  }
  // The terminal's cursor as it was: shown, in its own shape.
  output_ << "\x1b[0 q\x1b[?25h";
  output_.flush();
  return state.status == Status::Submitted;
}

std::optional<std::size_t> KeyPrompter::Select(std::string_view question, std::span<const Choice> choices,
                                               std::size_t initial) {
  State state = MakeSelect(std::string(question), {choices.begin(), choices.end()}, initial);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return state.cursor;
}

std::optional<std::vector<std::size_t>> KeyPrompter::MultiSelect(std::string_view question,
                                                                 std::span<const Choice> choices,
                                                                 std::span<const std::size_t> initially_checked,
                                                                 std::size_t minimum) {
  State state =
      MakeMultiSelect(std::string(question), {choices.begin(), choices.end()}, initially_checked, minimum);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return CheckedIndices(state);
}

std::optional<std::string> KeyPrompter::Text(std::string_view question, std::string_view default_text,
                                             const Validator& validate) {
  State state = MakeText(std::string(question), std::string(default_text), validate);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return state.text;
}

std::optional<bool> KeyPrompter::Confirm(std::string_view question, bool default_yes) {
  State state = MakeConfirm(std::string(question), default_yes);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return state.yes;
}

void KeyPrompter::Show(std::span<const Line> lines) {
  const Style style = OutputStyle();
  for (const Line& line : lines) {
    output_ << Paint(line, style) << '\n';
  }
  output_.flush();
}

LinePrompter::LinePrompter(std::istream& input, std::ostream& output) : input_(input), output_(output) {}

void LinePrompter::PrintChoices(std::span<const Choice> choices) {
  for (std::size_t index = 0; index < choices.size(); ++index) {
    output_ << "  " << index + 1 << ") " << ChoiceLine(choices[index]) << '\n';
  }
}

std::optional<std::size_t> LinePrompter::Select(std::string_view question, std::span<const Choice> choices,
                                                std::size_t initial) {
  output_ << "? " << question << '\n';
  PrintChoices(choices);
  while (true) {
    output_ << "Choose 1-" << choices.size() << " [" << initial + 1 << "]: ";
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    const std::string answer = Trim(line);
    if (answer.empty()) {
      return initial;
    }
    if (const auto index = ParseChoiceNumber(answer, choices.size())) {
      return index;
    }
    output_ << "  ! Enter a number from 1 to " << choices.size() << ".\n";
  }
}

std::optional<std::vector<std::size_t>> LinePrompter::MultiSelect(std::string_view question,
                                                                  std::span<const Choice> choices,
                                                                  std::span<const std::size_t> initially_checked,
                                                                  std::size_t minimum) {
  output_ << "? " << question << '\n';
  PrintChoices(choices);
  std::string defaults;
  for (const std::size_t index : initially_checked) {
    defaults += (defaults.empty() ? "" : ",") + std::to_string(index + 1);
  }
  while (true) {
    output_ << (minimum == 0 ? "Choose any, separated by commas [" : "Choose one or more, separated by commas [")
            << (defaults.empty() ? std::string("none") : defaults) << "]: ";
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    const std::string answer = Trim(line);
    if (answer.empty() && (!initially_checked.empty() || minimum == 0)) {
      return std::vector<std::size_t>(initially_checked.begin(), initially_checked.end());
    }
    std::vector<bool> checked(choices.size(), false);
    bool valid = !answer.empty();
    std::size_t start = 0;
    while (valid && start <= answer.size()) {
      const std::size_t separator = answer.find(',', start);
      const std::size_t end = separator == std::string::npos ? answer.size() : separator;
      const auto index = ParseChoiceNumber(Trim(std::string_view(answer).substr(start, end - start)), choices.size());
      valid = index.has_value();
      if (valid) {
        checked[*index] = true;
      }
      if (separator == std::string::npos) {
        break;
      }
      start = separator + 1;
    }
    if (valid) {
      std::vector<std::size_t> indices;
      for (std::size_t index = 0; index < checked.size(); ++index) {
        if (checked[index]) {
          indices.push_back(index);
        }
      }
      return indices;
    }
    output_ << "  ! Enter numbers from 1 to " << choices.size() << ", separated by commas.\n";
  }
}

std::optional<std::string> LinePrompter::Text(std::string_view question, std::string_view default_text,
                                              const Validator& validate) {
  while (true) {
    output_ << "? " << question;
    if (!default_text.empty()) {
      output_ << " [" << default_text << ']';
    }
    output_ << ": ";
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    std::string answer = Trim(line);
    if (answer.empty()) {
      answer = std::string(default_text);
    }
    const std::string problem = validate ? validate(answer) : std::string{};
    if (problem.empty()) {
      return answer;
    }
    output_ << "  ! " << problem << '\n';
  }
}

std::optional<bool> LinePrompter::Confirm(std::string_view question, bool default_yes) {
  while (true) {
    output_ << "? " << question << (default_yes ? " [Y/n]: " : " [y/N]: ");
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    const std::string answer = Trim(line);
    if (answer.empty()) {
      return default_yes;
    }
    if (answer == "y" || answer == "Y" || answer == "yes") {
      return true;
    }
    if (answer == "n" || answer == "N" || answer == "no") {
      return false;
    }
    output_ << "  ! Enter y or n.\n";
  }
}

void LinePrompter::Show(std::span<const Line> lines) {
  for (const Line& line : lines) {
    output_ << PlainText(line) << '\n';
  }
  output_.flush();
}

Style LinePrompter::OutputStyle() const {
  return {};
}

void Console::Step(std::ostream& output, std::string_view text) const {
  if (!style_) {
    output << text << '\n';
    return;
  }
  output << Paint({{std::string(GlyphsFor(*style_).active) + "  ", Role::Brand}, {std::string(text), Role::Strong}},
                  *style_)
         << '\n';
}

void Console::Note(std::ostream& output, std::string_view text) const {
  if (!style_) {
    output << text << '\n';
    return;
  }
  output << Paint({Rail(GlyphsFor(*style_)), {std::string(text), Role::Muted}}, *style_) << '\n';
}

void Console::Done(std::ostream& output, std::string_view text) const {
  if (!style_) {
    output << text << '\n';
    return;
  }
  output << Paint({{std::string(GlyphsFor(*style_).success) + "  ", Role::Success}, {std::string(text), Role::Plain}},
                  *style_)
         << '\n';
}

void Console::Command(std::ostream& output, std::string_view command, bool streams) const {
  if (!style_) {
    output << "> " << command << '\n';
    output.flush();
    return;
  }
  // A command is shown whole, however wide, so that it can be copied and run again.
  Style whole = *style_;
  whole.width = 0;
  output << Paint({{"   " + std::string(GlyphsFor(whole).field) + ' ', Role::Muted}, {std::string(command), Role::Muted}},
                  whole)
         << (streams ? "\n\n" : "\n");
  output.flush();
}

void Console::Item(std::ostream& output, Mark mark, std::string_view label, std::string_view detail,
                   std::size_t label_width, bool cut_to_width) const {
  struct Shape {
    std::string_view unicode;
    std::string_view ascii;
    std::string_view word;
    Role role;
  };
  // One column each, so that labels line up whichever symbols the terminal draws.
  static constexpr std::array<Shape, 5> shapes{{
      {"✔", "+", "ok", Role::Success},
      {"✗", "x", "missing", Role::Error},
      {"○", "-", "optional", Role::Muted},
      {"■", "#", "blocked", Role::Error},
      {"›", ">", "action", Role::Accent},
  }};
  const Shape& shape = shapes[static_cast<std::size_t>(mark)];
  if (!style_) {
    output << "  [" << shape.word << "] " << label << (detail.empty() ? "" : ": ") << detail << '\n';
    return;
  }
  Style whole = *style_;
  if (!cut_to_width) {
    whole.width = 0;
  }
  output << Paint({{"   " + std::string(whole.unicode ? shape.unicode : shape.ascii) + ' ', shape.role},
                   {Padded(label, detail.empty() ? 0 : label_width + 2), Role::Plain},
                   {std::string(detail), Role::Muted}},
                  whole)
         << '\n';
}

void Console::Write(std::ostream& output, const Line& line) const {
  if (!style_) {
    output << PlainText(line) << '\n';
    return;
  }
  Style whole = *style_;
  whole.width = 0;
  output << Paint(line, whole) << '\n';
}

void Console::Gap(std::ostream& output) const {
  if (style_) {
    output << '\n';
  }
}

void Console::CommandEnded(std::ostream& output) const {
  if (style_) {
    output << '\n';
  }
}

} // namespace huxerui::cli::prompt
