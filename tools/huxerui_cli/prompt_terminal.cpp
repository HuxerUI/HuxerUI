#include "prompt.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace huxerui::cli::prompt {
namespace {

// An Escape key press is a lone ESC byte; the arrow keys send ESC followed at once by the rest of a sequence.
constexpr int escape_wait_milliseconds = 40;

std::string Environment(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return {};
  }
  std::string text(value);
  std::free(value);
  return text;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

std::string Lowercase(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text;
}

// The colors a terminal shows, by the conventions terminals and tools share: NO_COLOR turns colors off,
// FORCE_COLOR and CLICOLOR_FORCE turn them on, COLORTERM names 24-bit color, and TERM names the 256-color palette.
ColorDepth DetectColorDepth(bool virtual_terminal) {
  if (!Environment("NO_COLOR").empty()) {
    return ColorDepth::None;
  }
  const bool forced = !Environment("FORCE_COLOR").empty() || !Environment("CLICOLOR_FORCE").empty();
  const std::string term = Lowercase(Environment("TERM"));
  if (!forced && (!virtual_terminal || term == "dumb")) {
    return ColorDepth::None;
  }
  const std::string colorterm = Lowercase(Environment("COLORTERM"));
  // Windows Terminal sets WT_SESSION and draws 24-bit color.
  if (colorterm == "truecolor" || colorterm == "24bit" || !Environment("WT_SESSION").empty()) {
    return ColorDepth::True;
  }
  if (term.find("256") != std::string::npos) {
    return ColorDepth::Palette;
  }
  return ColorDepth::Basic;
}

#if defined(_WIN32)

bool StandardStreamsAreTerminals() {
  return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
}

// Raw key input through the console's virtual-terminal input mode, which reports arrow keys as the same escape
// sequences a POSIX terminal sends. The original console modes are restored when the prompter is destroyed.
// While the prompter lives the console takes VT sequences and UTF-8, and both settings are restored after it. The
// prompt's symbols are used only where the console is known to have a font for them, which Windows Terminal does
// and the classic console's raster fonts do not; a console without VT processing gets neither colors nor symbols.
class ConsoleEncoding final {
public:
  ConsoleEncoding() : output_(GetStdHandle(STD_OUTPUT_HANDLE)), original_code_page_(GetConsoleOutputCP()) {
    utf8_ = SetConsoleOutputCP(CP_UTF8) != 0;
    if (output_ != INVALID_HANDLE_VALUE && GetConsoleMode(output_, &original_mode_)) {
      mode_saved_ = true;
      virtual_terminal_ = (original_mode_ & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
                          SetConsoleMode(output_, original_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
  }

  ~ConsoleEncoding() {
    if (mode_saved_) {
      SetConsoleMode(output_, original_mode_);
    }
    if (utf8_ && original_code_page_ != 0) {
      SetConsoleOutputCP(original_code_page_);
    }
  }

  ConsoleEncoding(const ConsoleEncoding&) = delete;
  ConsoleEncoding& operator=(const ConsoleEncoding&) = delete;

  [[nodiscard]] bool VirtualTerminal() const noexcept {
    return virtual_terminal_;
  }

  [[nodiscard]] bool Unicode() const {
    return utf8_ && (!Environment("WT_SESSION").empty() || !Environment("TERM_PROGRAM").empty());
  }

private:
  HANDLE output_ = INVALID_HANDLE_VALUE;
  UINT original_code_page_ = 0;
  DWORD original_mode_ = 0;
  bool mode_saved_ = false;
  bool utf8_ = false;
  bool virtual_terminal_ = false;
};

std::size_t TerminalWidth() {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
    return 0;
  }
  return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
}

class RawTerminal final {
public:
  RawTerminal() {
    input_ = GetStdHandle(STD_INPUT_HANDLE);
    output_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input_ == INVALID_HANDLE_VALUE || output_ == INVALID_HANDLE_VALUE || !GetConsoleMode(input_, &input_mode_) ||
        !GetConsoleMode(output_, &output_mode_)) {
      return;
    }
    const DWORD raw_input =
        (input_mode_ & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(output_, output_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return;
    }
    if (!SetConsoleMode(input_, raw_input)) {
      SetConsoleMode(output_, output_mode_);
      return;
    }
    active_ = true;
  }

  ~RawTerminal() {
    if (active_) {
      SetConsoleMode(input_, input_mode_);
      SetConsoleMode(output_, output_mode_);
    }
  }

  RawTerminal(const RawTerminal&) = delete;
  RawTerminal& operator=(const RawTerminal&) = delete;

  [[nodiscard]] bool Active() const noexcept {
    return active_;
  }

  [[nodiscard]] std::optional<unsigned char> ReadByte() {
    unsigned char byte = 0;
    DWORD read = 0;
    if (!ReadFile(input_, &byte, 1, &read, nullptr) || read != 1) {
      return std::nullopt;
    }
    return byte;
  }

  [[nodiscard]] bool ByteArrivesSoon() {
    return WaitForSingleObject(input_, escape_wait_milliseconds) == WAIT_OBJECT_0;
  }

private:
  HANDLE input_ = INVALID_HANDLE_VALUE;
  HANDLE output_ = INVALID_HANDLE_VALUE;
  DWORD input_mode_ = 0;
  DWORD output_mode_ = 0;
  bool active_ = false;
};

#else

bool StandardStreamsAreTerminals() {
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

// A POSIX terminal's encoding is the locale's: the first of LC_ALL, LC_CTYPE, and LANG that is set.
class ConsoleEncoding final {
public:
  [[nodiscard]] bool VirtualTerminal() const noexcept {
    return true;
  }

  [[nodiscard]] bool Unicode() const {
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
      const std::string value = Lowercase(Environment(name));
      if (!value.empty()) {
        return value.find("utf-8") != std::string::npos || value.find("utf8") != std::string::npos;
      }
    }
    return false;
  }
};


std::size_t TerminalWidth() {
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
    return 0;
  }
  return size.ws_col;
}

// Raw key input without echo or signal generation; Ctrl-C arrives as a byte so the prompt can cancel and the
// terminal settings are restored when the prompter is destroyed.
class RawTerminal final {
public:
  RawTerminal() {
    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
      return;
    }
    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
  }

  ~RawTerminal() {
    if (active_) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }
  }

  RawTerminal(const RawTerminal&) = delete;
  RawTerminal& operator=(const RawTerminal&) = delete;

  [[nodiscard]] bool Active() const noexcept {
    return active_;
  }

  [[nodiscard]] std::optional<unsigned char> ReadByte() {
    unsigned char byte = 0;
    if (read(STDIN_FILENO, &byte, 1) != 1) {
      return std::nullopt;
    }
    return byte;
  }

  [[nodiscard]] bool ByteArrivesSoon() {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    return poll(&input, 1, escape_wait_milliseconds) > 0;
  }

private:
  termios original_{};
  bool active_ = false;
};

#endif

class TerminalPrompter final : public Prompter {
public:
  TerminalPrompter() = default;

  [[nodiscard]] std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                  std::size_t initial) override {
    Session session(*this);
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).Select(question, choices, initial);
    }
    return session.prompter.Select(question, choices, initial);
  }

  [[nodiscard]] std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked, std::size_t minimum) override {
    Session session(*this);
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).MultiSelect(question, choices, initially_checked, minimum);
    }
    return session.prompter.MultiSelect(question, choices, initially_checked, minimum);
  }

  [[nodiscard]] std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                const Validator& validate) override {
    Session session(*this);
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).Text(question, default_text, validate);
    }
    return session.prompter.Text(question, default_text, validate);
  }

  [[nodiscard]] std::optional<bool> Confirm(std::string_view question, bool default_yes) override {
    Session session(*this);
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).Confirm(question, default_yes);
    }
    return session.prompter.Confirm(question, default_yes);
  }

  void Show(std::span<const Line> lines) override {
    const Style style = OutputStyle();
    for (const Line& line : lines) {
      std::cout << Paint(line, style) << '\n';
    }
    std::cout.flush();
  }

  // Read before every frame, so a resized terminal is drawn at its new width.
  [[nodiscard]] Style OutputStyle() const override {
    Style style;
    style.color = DetectColorDepth(encoding_.VirtualTerminal());
    style.unicode = encoding_.VirtualTerminal() && encoding_.Unicode() && Lowercase(Environment("TERM")) != "dumb";
    style.width = TerminalWidth();
    return style;
  }

private:
  // Raw mode lasts for one question so that output between questions, and any failure, sees a normal terminal.
  struct Session {
    explicit Session(const TerminalPrompter& owner)
        : prompter(
              [this]() -> std::optional<KeyEvent> {
                while (const std::optional<unsigned char> byte = terminal.ReadByte()) {
                  if (std::optional<KeyEvent> event = decoder.Feed(*byte)) {
                    return event;
                  }
                  if (decoder.AwaitsSequence() && !terminal.ByteArrivesSoon()) {
                    return decoder.Flush();
                  }
                }
                return std::nullopt;
              },
              std::cout, [&owner] { return owner.OutputStyle(); }) {}

    RawTerminal terminal;
    KeyDecoder decoder;
    KeyPrompter prompter;
  };

  ConsoleEncoding encoding_;
};

} // namespace

std::unique_ptr<Prompter> MakeTerminalPrompter() {
  if (!StandardStreamsAreTerminals()) {
    return nullptr;
  }
  return std::make_unique<TerminalPrompter>();
}

} // namespace huxerui::cli::prompt
