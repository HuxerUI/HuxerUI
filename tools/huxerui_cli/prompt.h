#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace huxerui::cli::prompt {

/// What a piece of prompt text means. The terminal layer decides how a role looks: a color, emphasis, or nothing.
enum class Role {
  Plain,
  Strong,
  /// HuxerUI's own color: the prompt's title.
  Brand,
  /// The current choice and given answers, in HuxerUI's color.
  Accent,
  /// Help, hints, and the rail that joins the questions.
  Muted,
  /// Sources that reach HuxerUI through `#include`.
  Headers,
  /// Sources that reach HuxerUI through `import`, in the C++ logo's color.
  Modules,
  Success,
  Error,
};

/// A run of text with one role.
struct Span {
  std::string text;
  Role role = Role::Plain;
};

/// One output line, as spans.
using Line = std::vector<Span>;

/// One option of a selection prompt.
struct Choice {
  Choice(std::string label_value, std::string hint_value = {});
  Choice(std::string label_value, Line hint_value);

  std::string label;
  Line hint;
};

/// How many colors the terminal shows.
enum class ColorDepth {
  None,
  /// The 16 ANSI colors, which every terminal theme maps.
  Basic,
  /// The 256-color palette.
  Palette,
  /// 24-bit color.
  True,
};

/// What the terminal can show. The default is plain ASCII without colors or a width limit.
struct Style {
  ColorDepth color = ColorDepth::None;
  /// Whether the terminal draws the prompt's symbols, such as `◆` and `│`; ASCII stand-ins are used otherwise.
  bool unicode = false;
  /// Columns available to a line; zero means unbounded.
  std::size_t width = 0;
};

enum class Key {
  Up,
  Down,
  Space,
  Enter,
  Backspace,
  Text,
  Cancel,
};

/// A decoded key press. `text` holds the typed UTF-8 sequence for `Key::Text`.
struct KeyEvent {
  Key key = Key::Text;
  std::string text;
};

enum class Kind {
  Select,
  MultiSelect,
  Text,
  Confirm,
};

enum class Status {
  Active,
  Submitted,
  Cancelled,
};

/// Returns an empty string for an acceptable answer, or the message to show otherwise.
using Validator = std::function<std::string(std::string_view)>;

/// The complete state of one prompt. The terminal layer only feeds key events to `Apply` and draws `Render`.
struct State {
  Kind kind = Kind::Select;
  std::string question;
  std::vector<Choice> choices;
  std::size_t cursor = 0;
  std::vector<bool> checked;
  std::string text;
  std::string default_text;
  Validator validate;
  std::string error;
  /// The fewest choices a multi-select accepts.
  std::size_t minimum = 1;
  /// A confirmation's answer, starting from its default.
  bool yes = true;
  Status status = Status::Active;
};

[[nodiscard]] State MakeSelect(std::string question, std::vector<Choice> choices, std::size_t initial);
[[nodiscard]] State MakeMultiSelect(std::string question, std::vector<Choice> choices,
                                    std::span<const std::size_t> initially_checked, std::size_t minimum = 1);
[[nodiscard]] State MakeText(std::string question, std::string default_text, Validator validate = {});
[[nodiscard]] State MakeConfirm(std::string question, bool default_yes);

/// Advances the prompt by one key event.
void Apply(State& state, const KeyEvent& event);

/// The lines that show a prompt, and where a text prompt's cursor belongs.
struct Frame {
  std::vector<Line> lines;
  /// The line and the column, counted in characters from zero, of the text cursor; empty when none is shown.
  std::optional<std::size_t> cursor_line;
  std::size_t cursor_column = 0;
};

/// The prompt in its current state, with the symbols `style` can show and without terminal control sequences.
[[nodiscard]] Frame Render(const State& state, const Style& style = {});

/// The first line of a series of questions, naming what they are for.
[[nodiscard]] Line IntroLine(std::string_view title, const Style& style);

/// A line beside the questions, such as a reason for a default.
[[nodiscard]] Line NoteLine(Line text, const Style& style);

/// The last line of a series of questions, and the lines under it.
[[nodiscard]] std::vector<Line> OutroLines(Line result, std::span<const std::pair<std::string, Line>> details,
                                           const Style& style);

/// A line's text without roles.
[[nodiscard]] std::string PlainText(const Line& line);

/// A line for the terminal: each role's color where `style` has colors, cut to `style.width` with an ellipsis.
[[nodiscard]] std::string Paint(const Line& line, const Style& style);

/// The CLI's own lines among the output of the tools it runs. With a style, as on a terminal, they carry the
/// prompt's symbols and colors and a blank line sets each tool's output apart; without one they are the plain text
/// that scripts and the conformance suite read, unchanged.
class Console {
public:
  Console() = default;
  explicit Console(Style style) : style_(style) {}

  /// What the CLI is doing, such as `Building linux (debug) with mcpp`.
  void Step(std::ostream& output, std::string_view text) const;
  /// A remark beside a step, such as which build system a default chose.
  void Note(std::ostream& output, std::string_view text) const;
  /// A result, such as a published package.
  void Done(std::ostream& output, std::string_view text) const;
  /// A command about to run; `streams` is true when its output follows on the terminal.
  void Command(std::ostream& output, std::string_view command, bool streams) const;
  /// Closes the output of a command that streamed it.
  void CommandEnded(std::ostream& output) const;

  /// What a listed item reports.
  enum class Mark { Ready, Missing, Optional, Blocked, Action };
  /// One item under a step, such as a requirement and where it was found, with its label padded to `label_width`
  /// so the details of one group line up.
  /// A detail that is a path or a command is shown whole; a summary may be cut to the terminal's width.
  void Item(std::ostream& output, Mark mark, std::string_view label, std::string_view detail,
            std::size_t label_width = 0, bool cut_to_width = false) const;
  /// A blank line between groups, on a terminal only.
  void Gap(std::ostream& output) const;
  /// A line the caller composed from roles: painted whole on a terminal, its plain text elsewhere.
  void Write(std::ostream& output, const Line& line) const;
  /// Whether the lines carry the prompt's symbols and colors; output elsewhere keeps its plain format.
  [[nodiscard]] bool Styled() const noexcept {
    return style_.has_value();
  }

private:
  std::optional<Style> style_;
};

/// The submitted answer as text: the chosen labels separated by ", ", or the entered text.
[[nodiscard]] std::string Answer(const State& state);

/// The checked indices of a multi-select state, in choice order.
[[nodiscard]] std::vector<std::size_t> CheckedIndices(const State& state);

/// Decodes terminal input bytes, including VT escape sequences for the arrow keys, into key events.
class KeyDecoder final {
public:
  [[nodiscard]] std::optional<KeyEvent> Feed(unsigned char byte);

  /// Whether an escape byte waits for the rest of a sequence.
  [[nodiscard]] bool AwaitsSequence() const noexcept;

  /// Ends a pending escape byte that no sequence followed: the Escape key, which cancels.
  [[nodiscard]] std::optional<KeyEvent> Flush();

private:
  std::string pending_;
};

/// Asks questions. A missing answer means the user cancelled.
class Prompter {
public:
  virtual ~Prompter() = default;

  [[nodiscard]] virtual std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                          std::size_t initial) = 0;
  [[nodiscard]] virtual std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked, std::size_t minimum = 1) = 0;
  [[nodiscard]] virtual std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                        const Validator& validate) = 0;
  [[nodiscard]] virtual std::optional<bool> Confirm(std::string_view question, bool default_yes) = 0;

  /// Writes lines beside the questions, such as their introduction or the outcome.
  virtual void Show(std::span<const Line> lines) = 0;

  /// What the prompter's output can show, for lines built with `IntroLine`, `NoteLine`, and `OutroLines`.
  [[nodiscard]] virtual Style OutputStyle() const = 0;
};

/// Runs each prompt through the state machine with keys from `next_key`, writing frames to `output`, which must
/// accept VT control sequences. `style` is read before every frame, so a resized terminal is drawn at its new width.
class KeyPrompter : public Prompter {
public:
  KeyPrompter(std::function<std::optional<KeyEvent>()> next_key, std::ostream& output,
              std::function<Style()> style = {});

  [[nodiscard]] std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                  std::size_t initial) override;
  [[nodiscard]] std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked, std::size_t minimum = 1) override;
  [[nodiscard]] std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                const Validator& validate) override;
  [[nodiscard]] std::optional<bool> Confirm(std::string_view question, bool default_yes) override;
  void Show(std::span<const Line> lines) override;
  [[nodiscard]] Style OutputStyle() const override;

private:
  bool Drive(State& state);

  std::function<std::optional<KeyEvent>()> next_key_;
  std::ostream& output_;
  std::function<Style()> style_;
};

/// Asks with numbered choices, one answer per input line; used where a raw terminal is unavailable.
class LinePrompter final : public Prompter {
public:
  LinePrompter(std::istream& input, std::ostream& output);

  [[nodiscard]] std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                  std::size_t initial) override;
  [[nodiscard]] std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked, std::size_t minimum = 1) override;
  [[nodiscard]] std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                const Validator& validate) override;
  [[nodiscard]] std::optional<bool> Confirm(std::string_view question, bool default_yes) override;
  void Show(std::span<const Line> lines) override;
  [[nodiscard]] Style OutputStyle() const override;

private:
  void PrintChoices(std::span<const Choice> choices);

  std::istream& input_;
  std::ostream& output_;
};

/// A prompter for the process's standard streams, or none when standard input or output is not a terminal.
/// It uses raw key input where the terminal supports it and numbered line input otherwise.
[[nodiscard]] std::unique_ptr<Prompter> MakeTerminalPrompter();

} // namespace huxerui::cli::prompt
