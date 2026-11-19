#include "support.h"

#include "prompt.h"

using namespace huxerui::cli::prompt;

namespace {

std::vector<Choice> BuildSystems() {
  return {{"cmake", ""}, {"mcpp", "C++ modules"}};
}

KeyEvent Press(Key key) {
  return {key, {}};
}

KeyEvent Type(std::string text) {
  return {Key::Text, std::move(text)};
}

// The frame's lines as plain text, as a terminal without colors shows them.
std::vector<std::string> Lines(const State& state, const Style& style = {}) {
  std::vector<std::string> lines;
  for (const Line& line : Render(state, style).lines) {
    lines.push_back(PlainText(line));
  }
  return lines;
}

std::vector<KeyEvent> Decode(std::string_view bytes) {
  KeyDecoder decoder;
  std::vector<KeyEvent> events;
  for (const char byte : bytes) {
    if (std::optional<KeyEvent> event = decoder.Feed(static_cast<unsigned char>(byte))) {
      events.push_back(std::move(*event));
    }
  }
  return events;
}

} // namespace

TEST_CASE("HuxerUICliPromptSelectMovesAndWraps") {
  State state = MakeSelect("Build system", BuildSystems(), 0);
  Apply(state, Press(Key::Up));
  REQUIRE(state.cursor == 1);
  Apply(state, Press(Key::Down));
  REQUIRE(state.cursor == 0);
  Apply(state, Press(Key::Down));
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(Answer(state) == "mcpp");
  Apply(state, Press(Key::Up));
  REQUIRE(state.cursor == 1);
}

TEST_CASE("HuxerUICliPromptSelectRendersCursorAndHints") {
  const State state = MakeSelect("Build system", BuildSystems(), 1);
  // Labels pad to one column so the hints line up.
  REQUIRE(Lines(state) == std::vector<std::string>{
                              "*  Build system",
                              "|  ( ) cmake  ",
                              "|  (*) mcpp   C++ modules",
                              "|  up/down move - enter confirm - esc cancel",
                          });
  Style unicode;
  unicode.unicode = true;
  REQUIRE(Lines(state, unicode) == std::vector<std::string>{
                                       "◆  Build system",
                                       "│  ○ cmake  ",
                                       "│  ● mcpp   C++ modules",
                                       "│  ↑/↓ move · enter confirm · esc cancel",
                                   });
}

TEST_CASE("HuxerUICliPromptPaintsRolesAtTheTerminalsDepth") {
  const Line line{{"headers", Role::Headers}, {" & ", Role::Plain}, {"C++ modules", Role::Modules}};
  REQUIRE(Paint(line, {}) == "headers & C++ modules");
  Style basic;
  basic.color = ColorDepth::Basic;
  REQUIRE(Paint(line, basic) == "\x1b[36mheaders\x1b[0m & \x1b[34mC++ modules\x1b[0m");
  Style full;
  full.color = ColorDepth::True;
  // HuxerUI's violet for the prompt, and the C++ logo's blue for modules.
  REQUIRE(Paint({{"x", Role::Accent}}, full) == "\x1b[38;2;91;81;230mx\x1b[0m");
  REQUIRE(Paint({{"x", Role::Modules}}, full) == "\x1b[38;2;101;154;210mx\x1b[0m");
}

TEST_CASE("HuxerUICliPromptCutsLinesToTheTerminalWidth") {
  Style narrow;
  narrow.width = 10;
  REQUIRE(Paint({{"abcdefghijklmnop", Role::Plain}}, narrow) == "abcdef...");
  narrow.unicode = true;
  REQUIRE(Paint({{"ab", Role::Plain}, {"cdefghijkl", Role::Muted}}, narrow) == "abcdefgh…");
  REQUIRE(Paint({{"short", Role::Plain}}, narrow) == "short");
}

TEST_CASE("HuxerUICliPromptTextShowsItsFieldAndCursor") {
  State state = MakeText("Project name", "hello-hui");
  Frame frame = Render(state);
  REQUIRE(PlainText(frame.lines[1]) == "|  > hello-hui");
  REQUIRE(frame.cursor_line == std::optional<std::size_t>(1));
  REQUIRE(frame.cursor_column == 5);
  REQUIRE(PlainText(frame.lines[2]) == "|  type - enter keeps hello-hui - esc cancel");
  Apply(state, Type("ab"));
  frame = Render(state);
  REQUIRE(PlainText(frame.lines[1]) == "|  > ab");
  REQUIRE(frame.cursor_column == 7);
}

TEST_CASE("HuxerUICliPromptConfirmTakesItsDefaultOrALetter") {
  State state = MakeConfirm("Create hello-hui?", true);
  REQUIRE(Lines(state) == std::vector<std::string>{"*  Create hello-hui?  (Y/n)"});
  Apply(state, Press(Key::Enter));
  REQUIRE(state.yes);
  // A question longer than the answer column still keeps its answer apart.
  REQUIRE(Lines(state) == std::vector<std::string>{"+  Create hello-hui?  yes"});
  State declined = MakeConfirm("Create hello-hui?", true);
  Apply(declined, Type("n"));
  REQUIRE(declined.status == Status::Submitted);
  REQUIRE_FALSE(declined.yes);
  REQUIRE(Answer(declined) == "no");
}

TEST_CASE("HuxerUICliPromptMultiSelectTogglesAllAndAcceptsNoneWhenAllowed") {
  State state = MakeMultiSelect("Agent skills", BuildSystems(), std::vector<std::size_t>{0}, 0);
  Apply(state, Type("a"));
  REQUIRE(CheckedIndices(state) == std::vector<std::size_t>{0, 1});
  Apply(state, Type("a"));
  REQUIRE(CheckedIndices(state).empty());
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(Answer(state) == "none");
}

TEST_CASE("HuxerUICliPromptMultiSelectRequiresOneChoice") {
  State state = MakeMultiSelect("Build systems", BuildSystems(), std::vector<std::size_t>{0, 1});
  Apply(state, Press(Key::Space));
  Apply(state, Press(Key::Down));
  Apply(state, Press(Key::Space));
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Active);
  REQUIRE(Lines(state).back() == "|  ! Select at least one option.");
  Apply(state, Press(Key::Space));
  REQUIRE(Lines(state).back().find('!') == std::string::npos);
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(CheckedIndices(state) == std::vector<std::size_t>{1});
  REQUIRE(Lines(state) == std::vector<std::string>{"+  Build systems  mcpp"});
}

TEST_CASE("HuxerUICliPromptTextUsesDefaultAndValidates") {
  State state = MakeText("Project name", "my-app", [](std::string_view value) {
    return value.find(' ') == std::string_view::npos ? std::string{} : std::string("No spaces.");
  });
  Apply(state, Type("a"));
  Apply(state, Press(Key::Space));
  Apply(state, Type("b"));
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Active);
  REQUIRE(state.error == "No spaces.");
  Apply(state, Press(Key::Backspace));
  Apply(state, Press(Key::Backspace));
  Apply(state, Press(Key::Backspace));
  REQUIRE(state.text.empty());
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(Answer(state) == "my-app");
}

TEST_CASE("HuxerUICliPromptTextBackspaceRemovesOneCodePoint") {
  State state = MakeText("Name", "");
  Apply(state, Type("a"));
  Apply(state, Type("\xC3\xA9"));
  Apply(state, Press(Key::Backspace));
  REQUIRE(state.text == "a");
}

TEST_CASE("HuxerUICliPromptCancelEndsEveryKind") {
  State select = MakeSelect("Q", BuildSystems(), 0);
  Apply(select, Press(Key::Cancel));
  REQUIRE(select.status == Status::Cancelled);
  REQUIRE(Lines(select) == std::vector<std::string>{"x  Q             cancelled"});
  State text = MakeText("Q", "x");
  Apply(text, Press(Key::Cancel));
  REQUIRE(text.status == Status::Cancelled);
}

TEST_CASE("HuxerUICliPromptDecoderTreatsALoneEscapeAsCancel") {
  KeyDecoder decoder;
  REQUIRE_FALSE(decoder.Feed(0x1b).has_value());
  REQUIRE(decoder.AwaitsSequence());
  const std::optional<KeyEvent> event = decoder.Flush();
  REQUIRE(event.has_value());
  REQUIRE(event->key == Key::Cancel);
  REQUIRE_FALSE(decoder.AwaitsSequence());
}

TEST_CASE("HuxerUICliPromptDecodesTerminalBytes") {
  const std::vector<KeyEvent> events = Decode("\x1b[A\x1b[B\x1bOA \r\x7fq\xC3\xA9\x03\x1b[1;5C");
  REQUIRE(events.size() == 9);
  REQUIRE(events[0].key == Key::Up);
  REQUIRE(events[1].key == Key::Down);
  REQUIRE(events[2].key == Key::Up);
  REQUIRE(events[3].key == Key::Space);
  REQUIRE(events[4].key == Key::Enter);
  REQUIRE(events[5].key == Key::Backspace);
  REQUIRE(events[6].text == "q");
  REQUIRE(events[7].text == "\xC3\xA9");
  REQUIRE(events[8].key == Key::Cancel);
}

TEST_CASE("HuxerUICliKeyPrompterRedrawsAndReturnsTheChoice") {
  std::vector<KeyEvent> keys{Press(Key::Down), Press(Key::Enter)};
  std::size_t next = 0;
  std::ostringstream output;
  KeyPrompter prompter([&]() -> std::optional<KeyEvent> {
    return next < keys.size() ? std::optional<KeyEvent>(keys[next++]) : std::nullopt;
  }, output);
  const std::vector<Choice> choices = BuildSystems();
  REQUIRE(prompter.Select("Build system", choices, 0) == std::optional<std::size_t>(1));
  // Each frame returns to the previous one's first line and clears below it; a list hides the cursor.
  REQUIRE(output.str().find("\x1b[4F\x1b[J") != std::string::npos);
  REQUIRE(output.str().find("\x1b[?25l") != std::string::npos);
  REQUIRE(output.str().find("+  Build system  mcpp\n") != std::string::npos);
  // The terminal's cursor is restored, shown and in its own shape.
  REQUIRE(output.str().ends_with("\x1b[0 q\x1b[?25h"));
}

TEST_CASE("HuxerUICliKeyPrompterPutsTheCursorInTheTextField") {
  std::vector<KeyEvent> keys{Type("a"), Press(Key::Enter)};
  std::size_t next = 0;
  std::ostringstream output;
  KeyPrompter prompter([&]() -> std::optional<KeyEvent> {
    return next < keys.size() ? std::optional<KeyEvent>(keys[next++]) : std::nullopt;
  }, output);
  REQUIRE(prompter.Text("Name", "x", {}) == std::optional<std::string>("a"));
  // Up two lines to the field, to the column after `|  > `, as a blinking bar; the next frame starts one line up.
  REQUIRE(output.str().find("\x1b[2A\x1b[6G\x1b[5 q\x1b[?25h") != std::string::npos);
  REQUIRE(output.str().find("\x1b[1F\x1b[J") != std::string::npos);
}

TEST_CASE("HuxerUICliKeyPrompterTreatsEndOfInputAsCancel") {
  std::ostringstream output;
  KeyPrompter prompter([]() -> std::optional<KeyEvent> { return std::nullopt; }, output);
  REQUIRE_FALSE(prompter.Text("Name", "x", {}).has_value());
}

TEST_CASE("HuxerUICliLinePrompterReadsNumberedAnswers") {
  std::istringstream input("\n9\n2\n\n1, 2\nbad name\ngood\n");
  std::ostringstream output;
  LinePrompter prompter(input, output);
  const std::vector<Choice> choices = BuildSystems();
  REQUIRE(prompter.Select("Style", choices, 1) == std::optional<std::size_t>(1));
  REQUIRE(prompter.Select("Style", choices, 0) == std::optional<std::size_t>(1));
  REQUIRE(output.str().find("! Enter a number from 1 to 2.") != std::string::npos);
  const std::vector<std::size_t> defaults{0};
  REQUIRE(prompter.MultiSelect("Build systems", choices, defaults) == std::optional<std::vector<std::size_t>>({0}));
  REQUIRE(prompter.MultiSelect("Build systems", choices, defaults) ==
          std::optional<std::vector<std::size_t>>({0, 1}));
  const Validator validate = [](std::string_view value) {
    return value.find(' ') == std::string_view::npos ? std::string{} : std::string("No spaces.");
  };
  REQUIRE(prompter.Text("Name", "", validate) == std::optional<std::string>("good"));
  REQUIRE(output.str().find("! No spaces.") != std::string::npos);
  REQUIRE_FALSE(prompter.Text("Name", "", validate).has_value());
}

TEST_CASE("HuxerUICliLinePrompterConfirmsAndAllowsAnEmptySelection") {
  std::istringstream input("\nmaybe\nn\n\n");
  std::ostringstream output;
  LinePrompter prompter(input, output);
  REQUIRE(prompter.Confirm("Create?", true) == std::optional<bool>(true));
  REQUIRE(prompter.Confirm("Create?", true) == std::optional<bool>(false));
  REQUIRE(output.str().find("! Enter y or n.") != std::string::npos);
  const std::vector<Choice> choices = BuildSystems();
  REQUIRE(prompter.MultiSelect("Agents", choices, {}, 0) == std::optional<std::vector<std::size_t>>(std::vector<std::size_t>{}));
}

TEST_CASE("HuxerUICliConsoleSetsToolOutputApartOnlyOnATerminal") {
  // Without a style the lines are the plain text scripts and the conformance suite read.
  std::ostringstream plain;
  const Console unstyled;
  unstyled.Note(plain, "Using cmake, the default (or --build-system mcpp)");
  unstyled.Step(plain, "Building linux (debug) with cmake");
  unstyled.Command(plain, "cmake --build build", true);
  unstyled.CommandEnded(plain);
  unstyled.Done(plain, "Packaged dist/linux/app");
  REQUIRE(plain.str() == "Using cmake, the default (or --build-system mcpp)\n"
                         "Building linux (debug) with cmake\n"
                         "> cmake --build build\n"
                         "Packaged dist/linux/app\n");

  // On a terminal the prompt's symbols mark the CLI's lines, a blank line encloses a tool's output, and a command
  // is never cut, so that it can be copied.
  std::ostringstream styled;
  const Console terminal(Style{.color = ColorDepth::None, .unicode = false, .width = 20});
  terminal.Note(styled, "Using cmake");
  terminal.Step(styled, "Building linux");
  terminal.Command(styled, "cmake --build a-long-build-directory", true);
  styled << "[tool output]\n";
  terminal.CommandEnded(styled);
  terminal.Command(styled, "mcpp pack", false);
  terminal.Done(styled, "Packaged app");
  REQUIRE(styled.str() == "|  Using cmake\n"
                          "*  Building linux\n"
                          "   > cmake --build a-long-build-directory\n"
                          "\n"
                          "[tool output]\n"
                          "\n"
                          "   > mcpp pack\n"
                          "ok  Packaged app\n");
}

TEST_CASE("HuxerUICliConsoleListsItemsAlignedOnATerminal") {
  using Mark = Console::Mark;
  std::ostringstream plain;
  const Console unstyled;
  unstyled.Item(plain, Mark::Missing, "JDK", "/opt/jdk", 12);
  REQUIRE(plain.str() == "  [missing] JDK: /opt/jdk\n");

  // Each mark takes one column, so labels and details line up; a summary may be cut, a path never is.
  std::ostringstream styled;
  const Console terminal(Style{.color = ColorDepth::None, .unicode = false, .width = 30});
  terminal.Item(styled, Mark::Ready, "cmake", "/usr/local/bin/cmake-with-a-long-path", 7);
  terminal.Item(styled, Mark::Missing, "JDK 17", {}, 7);
  terminal.Item(styled, Mark::Optional, "sdkmanager", "(optional)", 10);
  terminal.Item(styled, Mark::Ready, "provided", "9 checks: pkg-config, C++ compiler", 8, true);
  terminal.Item(styled, Mark::Action, "manual", "Install mcpp: xlings install mcpp -y", 9);
  REQUIRE(styled.str() == "   + cmake    /usr/local/bin/cmake-with-a-long-path\n"
                          "   x JDK 17\n"
                          "   - sdkmanager  (optional)\n"
                          "   + provided  9 checks: p...\n"
                          "   > manual     Install mcpp: xlings install mcpp -y\n");
}
