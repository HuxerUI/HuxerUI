#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace huxerui;

enum class BlockKind { User, Reasoning, Paragraph, Image, List, Code, Tool, Files, Table, Question };

struct MessageBlock {
  TextBlockId id;
  BlockKind kind;
  std::vector<TextSelectionBlock> text;
  bool expanded = false;
  std::string label{};
  std::optional<int> exit_code{};
};

using Messages = std::vector<std::shared_ptr<const MessageBlock>>;

struct Answer {
  bool windows = true;
  bool web = false;
  int response_style = 0;
  TextEditingValue project = TextEditingValue::FromText("");
  bool submitted = false;

  bool operator==(const Answer&) const = default;
};

using Answers = std::unordered_map<TextBlockId, Answer>;

TextSelectionBlock Paragraph(std::string text, std::string separator = "\n\n") {
  return {AttributedText(std::move(text)), std::move(separator)};
}

struct DiffLine {
  char marker;
  std::string text;
};

struct FileChange {
  std::string path;
  std::vector<DiffLine> lines;

  std::size_t Count(char marker) const {
    return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [marker](const DiffLine& line) {
      return line.marker == marker;
    }));
  }

  std::string Summary() const {
    return "+" + std::to_string(Count('+')) + " / -" + std::to_string(Count('-'));
  }

  std::string Header() const {
    return "--- a/" + path + "\n+++ b/" + path + "\n@@ -1," + std::to_string(lines.size() - Count('+')) +
        " +1," + std::to_string(lines.size() - Count('-')) + " @@";
  }

  std::string Body(bool after) const {
    std::string result;
    for (const auto& line : lines) {
      if (line.marker != (after ? '-' : '+')) {
        result += line.text + "\n";
      }
    }
    return result;
  }
};

const auto& MockChanges() {
  static const std::array<FileChange, 2> changes{{
    {"src/conversation.cpp", {
      {' ', "#include <huxerui/huxerui.h>"},
      {' ', ""},
      {' ', "using namespace huxerui;"},
      {' ', ""},
      {' ', "View Conversation(std::shared_ptr<const Transcript> snapshot,"},
      {' ', "                  ScrollController scroll) {"},
      {'-', "  return VirtualList(snapshot->Count(), [snapshot](std::size_t index) {"},
      {'-', "    return Text(snapshot->BlockAt(index).text);"},
      {'-', "  }).EstimatedItemExtent(96.0F).Controller(scroll);"},
      {'+', "  return SelectionArea("},
      {'+', "      VirtualList(snapshot->Count(), [snapshot](std::size_t index) {"},
      {'+', "        const auto id = snapshot->IdAt(index);"},
      {'+', "        return Text(snapshot->BlockAt(index).text)"},
      {'+', "            .SelectionBlock(id)"},
      {'+', "            .Key(id);"},
      {'+', "      }).EstimatedItemExtent(96.0F).Controller(scroll)"},
      {'+', "  ).Source(snapshot);"},
      {' ', "}"},
    }},
    {"tests/conversation.cpp", {
      {' ', "TEST_CASE(\"Copy spans unmounted messages\") {"},
      {' ', "  const auto snapshot = MakeTranscript(500);"},
      {' ', "  ConversationHarness app(snapshot);"},
      {' ', "  app.BuildFrame();"},
      {'-', "  REQUIRE(app.VisibleText().contains(\"Message 0\"));"},
      {'+', "  REQUIRE(app.SelectAll());"},
      {'+', "  app.ScrollToItem(400);"},
      {'+', "  app.BuildFrame();"},
      {'+', "  REQUIRE_FALSE(app.IsMounted(snapshot->IdAt(0)));"},
      {'+', "  REQUIRE(app.CopyText() == snapshot->PlainText());"},
      {'+', "  REQUIRE(app.RealizedCount() < 20);"},
      {'+', "}"},
      {'+', ""},
      {'+', "TEST_CASE(\"Streaming does not steal the reading position\") {"},
      {'+', "  ConversationHarness app(MakeTranscript(500));"},
      {'+', "  app.ScrollToItem(120);"},
      {'+', "  const auto anchor = app.VisibleAnchor();"},
      {'+', "  app.AppendToLastMessage(\" additional text\");"},
      {'+', "  app.BuildFrame();"},
      {'+', "  REQUIRE(app.VisibleAnchor() == anchor);"},
      {' ', "}"},
    }},
  }};
  return changes;
}

bool HasText(const std::string& text) {
  return text.find_first_not_of(" \t\r\n") != std::string::npos;
}

struct TranscriptIndex {
  struct Entry {
    TextBlockId id;
    TextSelectionBlock block;
  };
  std::vector<Entry> entries;
  std::unordered_map<TextBlockId, std::size_t> positions;
};

class Transcript final : public TextSelectionSource {
public:
  Transcript(std::shared_ptr<const TranscriptIndex> index, TextBlockId active_id, AttributedText live)
      : index_(std::move(index)), active_id_(active_id), live_(std::move(live)) {}

  std::size_t Count() const noexcept override { return index_->entries.size(); }
  TextBlockId IdAt(std::size_t index) const override { return index_->entries.at(index).id; }
  std::optional<std::size_t> IndexOf(TextBlockId id) const override {
    const auto found = index_->positions.find(id);
    return found == index_->positions.end() ? std::nullopt : std::optional(found->second);
  }
  TextSelectionBlock BlockAt(std::size_t index) const override {
    const auto& entry = index_->entries.at(index);
    return entry.id == active_id_ ? TextSelectionBlock{live_, entry.block.separator} : entry.block;
  }

private:
  std::shared_ptr<const TranscriptIndex> index_;
  TextBlockId active_id_;
  AttributedText live_;
};

AttributedText Highlight(const AttributedText& text, Color color) {
  std::vector<TextStyleRange> styles(text.StyleRanges().begin(), text.StyleRanges().end());
  for (auto& range : styles) {
    if (range.style.font && range.style.font->FamilyKind() == FontFamilyKind::Monospace) {
      range.style.foreground = color;
    }
  }
  return text.WithStyles(std::move(styles));
}

struct StreamModel {
  State<std::shared_ptr<const Messages>> messages;
  State<AttributedText> live;
  State<std::shared_ptr<const TextSelectionSource>> source;
  State<Answers> answers;
  State<TextEditingValue> draft;
  State<std::string> status;
  State<bool> running;
  ScrollController scroll;
  std::shared_ptr<const TranscriptIndex> index = std::make_shared<const TranscriptIndex>();
  TextBlockId next_id = 1;
  TextBlockId active_id = 0;
  Color code_color{};
  std::uint64_t version = 0;
  bool stop = false;
  bool highlighting = false;
  bool follow_new_block = false;

  void PublishSource() {
    source = std::make_shared<const Transcript>(index, active_id, live.Get());
  }

  void RebuildIndex() {
    auto updated = std::make_shared<TranscriptIndex>();
    for (const auto& row : *messages.Get()) {
      if (row->kind == BlockKind::Reasoning && !row->expanded) {
        continue;
      }
      for (std::size_t part = 0; part < row->text.size(); ++part) {
        if (row->kind == BlockKind::Tool && !row->expanded && part != 0) {
          continue;
        }
        const auto id = row->id + part;
        updated->positions.emplace(id, updated->entries.size());
        updated->entries.push_back({id, row->text[part]});
      }
    }
    index = std::move(updated);
    PublishSource();
  }

  bool Following() const {
    const auto metrics = scroll.Metrics();
    return metrics.maximum_offset - metrics.offset <= 24.0F;
  }

  void Latest() const {
    if (!messages.Get()->empty()) {
      static_cast<void>(scroll.ScrollToItem(messages.Get()->size() - 1, ScrollAlignment::End));
    }
  }

  void Seal() {
    if (active_id == 0) {
      return;
    }
    auto updated = std::make_shared<Messages>(*messages.Get());
    auto last = std::make_shared<MessageBlock>(*updated->back());
    last->text.at(active_id - last->id).text = last->kind == BlockKind::Code
        ? Highlight(live.Get(), code_color) : live.Get();
    if (last->kind == BlockKind::Tool && stop) {
      last->exit_code.reset();
    }
    updated->back() = std::move(last);
    active_id = 0;
    ++version;
    messages = std::move(updated);
    RebuildIndex();
  }

  void Append(MessageBlock block, bool streaming = false) {
    const bool following = Following();
    Seal();
    auto updated = std::make_shared<Messages>(*messages.Get());
    block.id = next_id;
    next_id += block.text.size();
    if (block.kind == BlockKind::Question) {
      auto values = answers.Get();
      values.emplace(block.id, Answer{});
      answers = std::move(values);
    }
    if (streaming) {
      // A tool keeps its command stable while only the final output part receives transport deltas.
      active_id = block.id + block.text.size() - 1;
      live = AttributedText{};
      block.text.back().text = AttributedText{};
    }
    updated->push_back(std::make_shared<const MessageBlock>(std::move(block)));
    messages = std::move(updated);
    // Structural edits rebuild the index; streaming snapshots only replace the independently observed tail.
    RebuildIndex();
    follow_new_block = follow_new_block || following;
  }

  void Finish() {
    Seal();
    running = false;
  }

  void Publish(AttributedText value) {
    const bool following = Following();
    ++version;
    live = std::move(value);
    PublishSource();
    if (following) {
      Latest();
    }
  }

  bool PublishHighlight(TextBlockId id, std::uint64_t source_version, AttributedText value) {
    if (id != active_id || source_version != version) {
      return false;
    }
    if (value != live.Get()) {
      Publish(std::move(value));
    }
    return true;
  }

  void ToggleDetails(TextBlockId id) {
    auto updated = std::make_shared<Messages>(*messages.Get());
    for (auto& row : *updated) {
      if (row->id == id) {
        auto replacement = std::make_shared<MessageBlock>(*row);
        replacement->expanded = !replacement->expanded;
        row = std::move(replacement);
        break;
      }
    }
    messages = std::move(updated);
    RebuildIndex();
  }

  template <class Edit> void EditAnswer(TextBlockId id, Edit edit) {
    auto updated = answers.Get();
    if (!running.Get() && !updated.at(id).submitted) {
      edit(updated.at(id));
      answers = std::move(updated);
    }
  }

  bool CanSubmit(TextBlockId id) const {
    const auto& value = answers.Get().at(id);
    return !running.Get() && !value.submitted && (value.windows || value.web) && HasText(value.project.text);
  }
};

AttributedText AppendResponse(const AttributedText& current, const AttributedText& response, TextOffset end) {
  const TextOffset start = current.Length();
  std::string body = current.PlainText() + response.TextInRange({start, end});
  std::vector<TextStyleRange> styles(current.StyleRanges().begin(), current.StyleRanges().end());
  // Keep the committed prefix, including asynchronous colors, and style only the newly received suffix.
  for (auto range : response.StyleRanges()) {
    range.range.start = std::max(range.range.start, start);
    range.range.end = std::min(range.range.end, end);
    if (range.range.start < range.range.end) {
      styles.push_back(std::move(range));
    }
  }
  std::vector<TextLinkRange> links;
  for (auto link : response.LinkRanges()) {
    if (link.range.start >= end) {
      break;
    }
    link.range.end = std::min(link.range.end, end);
    links.push_back(std::move(link));
  }
  return AttributedText::FromRanges(std::move(body), std::move(styles), std::move(links));
}

AttributedText CodeText(std::string text) {
  return AttributedText{TextSpan(std::move(text)).Style({.font = Font::Monospace(14.0F)})};
}

MessageBlock ToolCall(std::string label, std::string command, std::string output, int exit_code = 0) {
  return {0, BlockKind::Tool, {{CodeText("$ " + command), "\n"}, {CodeText(std::move(output)), "\n\n"}},
          true, std::move(label), exit_code};
}

std::vector<MessageBlock> ResponseContent(const ColorScheme& colors, const std::string& prompt, bool follow_up) {
  std::vector<MessageBlock> response{
    {0, BlockKind::Reasoning, {Paragraph(
        "Scripted analysis summary: first separate a document-lifetime problem from a rendering problem. "
        "A message leaving the viewport should release its View, not erase the text that Copy depends on. "
        "Inspect the conversation boundary, reproduce the failure with a long fixture, and change only the "
        "ownership and binding points supported by that evidence. This is a fictional work summary, not "
        "private model reasoning.")}},
    {0, BlockKind::Paragraph, {{AttributedText{
      TextSpan(follow_up ? "I'll replay the repair with your new constraints. "
                        : "I'll investigate selection in the Agent conversation. ")
          .Style({.font_weight = FontWeight::Bold}),
      TextSpan("Your request: "),
      TextSpan(prompt).Style({.font_slant = FontSlant::Italic}),
      TextSpan("\n\nThis run uses a fresh, simulated Atlas chat project. The reported failure appears after a "
               "conversation grows beyond the viewport: selecting visible text works, but copying an entire "
               "answer loses earlier paragraphs. Meanwhile, new output can pull a reader away from a message "
               "they were inspecting. I'll trace both paths, reproduce the selection failure, and show the "
               "proposed patch together with its verification evidence. The transcript below mixes "),
      TextSpan("analysis, tool results, and a final handoff")
          .Style({.foreground = colors.on_primary_container, .background = colors.primary_container}),
      TextSpan("; none of the displayed commands will actually run on your machine."),
    }, "\n\n"}}},
    {0, BlockKind::List, {
      Paragraph("1. ", ""), Paragraph("Locate the conversation View, the immutable transcript, and the code that "
          "publishes incoming text. Check which objects survive virtual-row eviction.", "\n"),
      Paragraph("2. ", ""), Paragraph("Reproduce Copy with 500 messages and a selected endpoint outside the viewport. "
          "Keep the fixture deterministic so a visual symptom becomes a repeatable assertion.", "\n"),
      Paragraph("3. ", ""), Paragraph("Bind visible text to stable logical block identities and keep one selection "
          "owner around the virtual list. Preserve the existing streaming publication path.", "\n"),
      Paragraph("4. ", ""), Paragraph("Rebuild, rerun the focused tests, and review scrolling, cancellation, "
          "and narrow-screen behavior before handing the change back."),
    }},
    ToolCall("Search the conversation boundary",
        "rg -n 'VirtualList|SelectionArea|SelectionBlock|ScrollToItem' src tests",
        "src/conversation.cpp:7:  return VirtualList(snapshot->Count(), [snapshot](std::size_t index) {\n"
        "src/stream.cpp:41:  controller.ScrollToItem(last, ScrollAlignment::End);\n"
        "src/stream.cpp:58:  if (following) PublishLatest();\n"
        "tests/conversation.cpp:1: TEST_CASE(\"Copy spans unmounted messages\") {\n"
        "tests/stream.cpp:24: TEST_CASE(\"Streaming batches text deltas\") {\n"
        "\nSearch finished: 5 matches in 4 files.\n"
        "No SelectionArea or SelectionBlock binding in src/conversation.cpp.\n"),
    {0, BlockKind::Paragraph, {Paragraph(
        "The search points to a small boundary rather than a missing text renderer. The conversation already uses "
        "a virtual list, and the stream publisher already checks whether the reader is following the bottom. "
        "There is no selection owner around that list, however, and the visible Text nodes have no binding to "
        "the document's stable block IDs. I will inspect that construction before adding any new state or "
        "changing the scrolling policy. A renderer workaround would leave Copy dependent on whatever rows "
        "happen to be mounted at the time of the request.")}},
    ToolCall("Read the current implementation", "sed -n '1,80p' src/conversation.cpp",
        MockChanges()[0].Body(false)),
    {0, BlockKind::Reasoning, {Paragraph(
        "Scripted analysis summary: the transcript is already immutable and exposes Count, IdAt, IndexOf, and "
        "BlockAt. That is the existing logical source needed by selection; another text cache would create a "
        "second authority. The next check should distinguish a missing selection capability from malformed "
        "UTF-16 ranges or an incomplete platform clipboard service.")}},
    {0, BlockKind::Paragraph, {{AttributedText{
      TextSpan("What the current code tells us").Style({.font_weight = FontWeight::Bold}),
      TextSpan("\n\nEach row is reconstructed from an immutable snapshot, which is a useful foundation: generation "
               "is not owned by a row and does not stop when that row disappears. The missing link is between "
               "the visible paragraph and its logical document entry. A row index is not that identity; inserting "
               "an earlier message shifts indices while the user's selected text should still refer to the same "
               "message.\n\nThe selection source should describe the committed document, including unmounted "
               "blocks. Mounted Text nodes should contribute only the geometry currently available for painting "
               "and hit testing. That separation lets Copy read a long answer without creating hundreds of Views, "
               "and lets handles disappear offscreen without silently deleting the logical range."),
    }, "\n\n"}}},
    {0, BlockKind::List, {
      Paragraph("\xE2\x80\xA2 ", ""), Paragraph("One authoritative transcript: text and selection entries must come from "
          "the same immutable snapshot, including while the current answer is still growing.", "\n"),
      Paragraph("    \xE2\x97\xA6 ", ""), Paragraph("Give every logical paragraph a stable TextBlockId; use that identity "
          "for both SelectionBlock and the virtual row's Key.", "\n"),
      Paragraph("    \xE2\x97\xA6 ", ""), Paragraph("Keep completed blocks shared. Publish a new value only for the active "
          "tail, rather than rewriting the entire conversation for each transport chunk.", "\n"),
      Paragraph("\xE2\x80\xA2 ", ""), Paragraph("One persistent selection owner: put SelectionArea outside VirtualList, "
          "not inside each row factory.", "\n"),
      Paragraph("\xE2\x80\xA2 ", ""), Paragraph("No extra scrolling authority: keep the existing conditional bottom-follow "
          "behavior, and verify that appending below the viewport does not steal the reading position."),
    }},
    ToolCall("Reproduce the missing selection", "./build/atlas_tests '[conversation]' --reporter compact",
        "Run: conversation regression fixture / 500 messages\n"
        "PASS  Streaming batches text deltas\n"
        "PASS  Reading position survives an append below the viewport\n"
        "\nFAIL  Copy spans unmounted messages\n"
        "  tests/conversation.cpp:6\n"
        "  REQUIRE(app.SelectAll())\n"
        "  actual: false\n"
        "  expected: a logical range covering 500 messages\n"
        "\nMounted rows: 9 / 500\n"
        "Clipboard service: available\n"
        "UTF-16 range validation: no invalid offsets reported\n"
        "Selection capability at the conversation boundary: missing\n"
        "\n2 passed, 1 failed. The failure is retained in this transcript.\n", 1),
    {0, BlockKind::Paragraph, {Paragraph(
        "The failing run now gives us a precise condition: Select All cannot establish a logical range at the "
        "conversation boundary. The clipboard is available and the fixture reports no invalid text offsets, "
        "so this is not an encoding conversion failure. The nine realized rows are expected for the viewport; "
        "increasing the cache would merely hide the problem until the next sufficiently long conversation.\n\n"
        "The patch below wraps the existing virtual list in SelectionArea, provides the same snapshot as its "
        "Source, and binds each Text to the snapshot's stable block ID. I am leaving the stream publisher alone "
        "because its conditional-follow behavior already passed the reproduction. The new regression also "
        "checks that Copy does not accidentally realize the rest of the document.")}},
    {0, BlockKind::Code, {{CodeText(MockChanges()[0].Body(true)), "\n\n"}}},
    {0, BlockKind::Paragraph, {Paragraph(
        "The important change is ownership, not the amount of retained UI. SelectionArea remains mounted while "
        "individual paragraphs enter and leave the viewport. Its source can retrieve the selected text even when "
        "there is no corresponding Text node. A mounted paragraph contributes layout geometry through "
        "SelectionBlock(id), and Key(id) keeps row identity independent of insertion or reordering.\n\n"
        "The source snapshot and row factory capture the same value. This avoids a subtle mismatch where the "
        "painted paragraph belongs to one revision but Copy reads another. Streaming can still replace the last "
        "block's body as new text arrives; already committed paragraphs remain shared and unchanged.")}},
  };
  std::vector<TextSelectionBlock> files;
  for (const auto& change : MockChanges()) {
    files.push_back(Paragraph(change.path + "   " + change.Summary(), "\n"));
  }
  files.back().separator = "\n\n";
  response.push_back({0, BlockKind::Files, std::move(files)});
  response.push_back({0, BlockKind::Reasoning, {Paragraph(
      "Scripted validation summary: the patch must satisfy both document behavior and virtualization limits. "
      "A passing Copy assertion alone could conceal eager realization of every row, so the test also checks "
      "the realized count. Keep the earlier failed run visible, then append the build and rerun results as "
      "separate tool calls rather than replacing that evidence.")}});
  response.push_back({0, BlockKind::Paragraph, {Paragraph(
      "I will now rebuild the simulated project and rerun the conversation tests. The acceptance condition is "
      "not simply that a highlight appears: Copy must include offscreen blocks in document order, the list must "
      "remain virtualized, and the reader's anchor must survive text arriving below it. These are independent "
      "checks, so the final report will distinguish the selection fix from scrolling behavior that was already "
      "correct. The file previews above are derived from the same mock diff data as the displayed code.")}});
  response.push_back(ToolCall("Build the proposed changes", "cmake --build build --target atlas_tests -j 4",
      "[1/8] Checking generated composable sources\n"
      "[2/8] Transforming src/conversation.cpp\n"
      "[3/8] Compiling src/conversation.cpp\n"
      "[4/8] Compiling tests/conversation.cpp\n"
      "[5/8] Compiling tests/stream.cpp\n"
      "[6/8] Linking atlas_core\n"
      "[7/8] Linking atlas_tests\n"
      "[8/8] Build complete\n"
      "\nTarget: atlas_tests\n"
      "Configuration: Debug / simulated sandbox\n"
      "Compiler diagnostics: none in this scripted run\n"));
  response.push_back(ToolCall("Rerun the conversation checks", "./build/atlas_tests '[conversation]' --reporter compact",
      "Run: conversation regression fixture / 500 messages\n"
      "PASS  Copy spans unmounted messages\n"
      "      Selected range: first block through last block\n"
      "      Scroll position: item 400\n"
      "      First selected endpoint mounted: no\n"
      "      Copied text matches the complete logical snapshot\n"
      "      Realized rows: 9 / 500\n"
      "PASS  Streaming does not steal the reading position\n"
      "      Reader anchor: message 120, unchanged after tail append\n"
      "PASS  Streaming batches text deltas\n"
      "      Completed blocks remain shared\n"
      "PASS  Copy preserves paragraph separators\n"
      "PASS  Cancel preserves received output\n"
      "PASS  Stable identities survive an earlier insertion\n"
      "\n6 test cases passed; 32 assertions passed.\n"
      "This is simulated test output, not a test of your project.\n"));
  response.push_back({0, BlockKind::Paragraph, {{AttributedText{
    TextSpan("Verification summary").Style({.font_weight = FontWeight::Bold}),
    TextSpan("\n\nThe scripted rerun now copies the complete 500-message snapshot while retaining only nine "
             "realized rows. It also confirms that appending to the last answer does not change the reader's "
             "anchor at message 120. The failed test has not been removed from the history: it remains above "
             "the patch, with exit code 1, followed by this successful rerun with exit code 0.\n\n"
             "Those results cover the logical behavior of the example scenario. They do not establish the "
             "quality of every platform's shaping, selection-handle placement, or clipboard integration. "
             "Those still need host-specific validation, especially for mixed-direction paragraphs, soft "
             "line breaks, emoji, and IME editing. The table separates this simulated evidence from work "
             "that a real project would still need to run."),
  }, "\n\n"}}});
  response.push_back({0, BlockKind::Table, {
    {AttributedText{TextSpan("Check").Style({.font_weight = FontWeight::Bold})}, "\t"},
    {AttributedText{TextSpan("Outcome").Style({.font_weight = FontWeight::Bold})}, "\t"},
    {AttributedText{TextSpan("Evidence").Style({.font_weight = FontWeight::Bold})}, "\n"},
    Paragraph("Offscreen Copy", "\t"), Paragraph("Passed in fixture", "\t"), Paragraph("500 blocks", "\n"),
    Paragraph("Virtualization", "\t"), Paragraph("Bounded realization", "\t"), Paragraph("9 rows", "\n"),
    Paragraph("Reader anchor", "\t"), Paragraph("Unchanged", "\t"), Paragraph("Item 120", "\n"),
    Paragraph("Platform behavior", "\t"), Paragraph("Not executed", "\t"), Paragraph("Manual checks"),
  }});
  if (!follow_up) {
    response.push_back({0, BlockKind::Image, {
      Paragraph("Conversation overview: navigation, transcript, and action cards."),
      Paragraph("Tool execution: a stable command with incremental output."),
      Paragraph("Patch review: additions and deletions beside the original context."),
    }});
  }
  response.push_back({0, BlockKind::Paragraph, {Paragraph(
      "For the interaction review, scroll back to the failed command while this answer continues to grow. "
      "The conversation should stay at your reading position until you choose Latest. Expand an earlier tool "
      "call, select several output lines, and continue the selection into the explanation below it. Collapse "
      "the call again and only its visible command should remain in Select All; reopening the output restores "
      "its place in document order without rerunning the simulated command.\n\n"
      "On a narrow viewport, prose and list items should wrap naturally, with continuation lines aligned under "
      "their own text rather than under the bullet. Commands, code, and the result table keep horizontal "
      "scrolling so long paths and source lines remain readable. Stopping a response preserves everything "
      "already received, including partial tool output, and a later message starts a new turn instead of "
      "rewriting this one.")}});
  response.push_back({0, BlockKind::List, {
    Paragraph("\xE2\x80\xA2 ", ""), Paragraph("Review the two proposed files. Their addition/deletion counts and the "
        "previewed lines come from one immutable diff, not separately maintained labels.", "\n"),
    Paragraph("\xE2\x80\xA2 ", ""), Paragraph("Exercise selection across a paragraph, a list item, and a command result; "
        "copied text should retain bullets, indentation, and paragraph separators.", "\n"),
    Paragraph("    \xE2\x97\xA6 ", ""), Paragraph("Scroll selected text completely offscreen, then return. The logical "
        "range should survive even though there was temporarily no geometry to display.", "\n"),
    Paragraph("    \xE2\x97\xA6 ", ""), Paragraph("Stop during a tool call. Its status should become Stopped, retain the "
        "received log prefix, and never claim a successful exit.", "\n"),
    Paragraph("\xE2\x80\xA2 ", ""), Paragraph("Choose the next target below, or send a revision request. Earlier messages, "
        "tool results, expansion choices, and submitted answers remain part of the conversation."),
  }});
  response.push_back({0, BlockKind::Paragraph, {{AttributedText{
    TextSpan("Ready for review. ").Style({.font_weight = FontWeight::Bold}),
    TextSpan("The proposal connects visible paragraphs to a persistent logical document without changing the "
             "existing transport or adding another scrolling controller. The reproduced failure, proposed "
             "source changes, simulated build, and test rerun are all retained in the transcript. You can "
             "inspect them independently instead of relying on a final success label.\n\n"
             "For the actual SDK contract, see "),
    TextSpan("HuxerUI").Link(Uri("https://github.com/HuxerUI/HuxerUI")),
    TextSpan(". All analysis summaries, commands, results, and file changes in this conversation are local "
             "fixtures. No model was contacted, no terminal command was executed, and no project file was changed."),
  }, "\n\n"}}});
  if (!follow_up) {
    response.push_back({0, BlockKind::Question, {Paragraph(
        "Where should the next simulated review focus? Choose the target platforms, response style, and "
        "project name. Submitting this form appends a new user turn; it does not replace the report above.")}});
  }
  return response;
}

Task<void> HighlightLatest(std::shared_ptr<StreamModel> model) {
  while (model->active_id != 0 && model->messages.Get()->back()->kind == BlockKind::Code) {
    const auto id = model->active_id;
    const auto version = model->version;
    const auto text = model->live.Get();
    const Color color = model->code_color;
    AttributedText highlighted;
    try {
      highlighted = co_await RunWorker(Highlight, text, color);
    } catch (const std::runtime_error&) {
      // Worker execution is optional on Web; this example's small tail can be colored synchronously.
      highlighted = Highlight(text, color);
    }
    if (model->PublishHighlight(id, version, std::move(highlighted))) {
      break;
    }
  }
  model->highlighting = false;
}

Task<void> StreamBlock(std::shared_ptr<StreamModel> model, TaskScope tasks, MessageBlock block) {
  const auto kind = block.kind;
  const auto response = block.text.back().text;
  model->Append(std::move(block), true);
  TextOffset received = 0;
  std::size_t byte = 0;
  unsigned chunks = 0;
  auto flush = [&] {
    if (received == model->live.Get().Length()) {
      return;
    }
    model->Publish(AppendResponse(model->live.Get(), response, received));
    if (kind == BlockKind::Code && !model->highlighting) {
      model->highlighting = true;
      tasks.Launch(HighlightLatest(model));
    }
  };
  while (byte < response.PlainText().size() && !model->stop) {
    co_await Delay(kind == BlockKind::Tool ? 140ms : kind == BlockKind::Reasoning ? 50ms : 36ms);
    if (model->stop) {
      break;
    }
    // Logs arrive in line batches; prose arrives in complete scalar batches, independent of UTF-8 byte width.
    const unsigned scalars = kind == BlockKind::Reasoning ? 6 : 12 + (chunks % 4) * 4;
    unsigned lines = 0;
    for (unsigned count = 0; byte < response.PlainText().size() &&
         (kind == BlockKind::Tool ? lines < 2 : count < scalars); ++count) {
      const auto first = static_cast<unsigned char>(response.PlainText()[byte]);
      lines += first == '\n';
      byte += first < 0x80U ? 1 : first < 0xE0U ? 2 : first < 0xF0U ? 3 : 4;
      received += first < 0xF0U ? 1 : 2;
    }
    if (++chunks % 2 == 0 || kind == BlockKind::Tool) {
      flush();
    }
  }
  flush();
  model->Seal();
}

Task<void> GenerateResponse(std::shared_ptr<StreamModel> model, TaskScope tasks, ColorScheme colors, std::string prompt,
    bool follow_up) {
  model->code_color = colors.primary;
  try {
    for (const auto& block : ResponseContent(colors, prompt, follow_up)) {
      if (model->stop) {
        break;
      }
      const bool streaming = block.kind == BlockKind::Reasoning || block.kind == BlockKind::Paragraph ||
          block.kind == BlockKind::Code || block.kind == BlockKind::Tool;
      model->status = block.kind == BlockKind::Tool ? "Running simulated tool: " + block.label
          : block.kind == BlockKind::Reasoning ? "Thinking..." : "Responding...";
      if (streaming) {
        co_await StreamBlock(model, tasks, block);
      } else {
        co_await Delay(350ms);
        if (!model->stop) {
          model->Append(block);
        }
      }
    }
    model->status = model->stop ? "Stopped. Received content is preserved." : "Ready for your next message.";
  } catch (const std::exception& error) {
    model->status = "Generation failed: " + std::string(error.what());
  }
  model->Finish();
}

void Send(std::shared_ptr<StreamModel> model, TaskScope tasks, ColorScheme colors, std::string prompt,
    bool follow_up = false) {
  if (model->running.Get() || !HasText(prompt)) {
    return;
  }
  model->running = true;
  model->stop = false;
  model->follow_new_block = true;
  model->Append({0, BlockKind::User, {Paragraph(prompt)}});
  tasks.Launch(GenerateResponse(model, tasks, colors, std::move(prompt), follow_up));
}

void SendDraft(std::shared_ptr<StreamModel> model, TaskScope tasks, ColorScheme colors) {
  if (model->running.Get() || !HasText(model->draft.Get().text) || model->draft.Get().composition) {
    return;
  }
  const auto prompt = model->draft.Get().text;
  model->draft = TextEditingValue::FromText("");
  Send(model, tasks, colors, prompt);
}

void SubmitAnswer(std::shared_ptr<StreamModel> model, TaskScope tasks, ColorScheme colors, TextBlockId id) {
  if (!model->CanSubmit(id)) {
    return;
  }
  auto values = model->answers.Get();
  auto& value = values.at(id);
  value.submitted = true;
  std::string targets = value.windows ? "Windows" : "";
  if (value.web) {
    targets += targets.empty() ? "Web" : ", Web";
  }
  const std::string prompt = "Project: " + value.project.text + "\nTargets: " + targets +
      "\nResponse: " + (value.response_style == 0 ? "Concise" : "Detailed");
  model->answers = std::move(values);
  Send(model, tasks, colors, prompt, true);
}

const VectorAsset& DocumentIcon() {
  static const auto icon = VectorAsset::Create({24.0F, 24.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(Path{}.MoveTo({5, 3}).LineTo({14, 3}).LineTo({19, 8}).LineTo({19, 21})
                          .LineTo({5, 21}).Close(), Color::White(), StrokeStyle{.width = 1.6F});
    builder.StrokePath(Path{}.MoveTo({9, 12}).LineTo({15, 12}).MoveTo({9, 16}).LineTo({15, 16}),
                       Color::White(), StrokeStyle{.width = 1.6F});
  });
  return icon;
}

enum class ChatIcon { Expand, Collapse, Latest, Send, Stop, Close };

const VectorAsset& ActionIcon(ChatIcon icon) {
  static const auto icons = [] {
    auto stroke = [](Path path) {
      return VectorAsset::Create({24, 24}, [path = std::move(path)](VectorBuilder& builder) {
        builder.StrokePath(path, Color::White(),
            StrokeStyle{.width = 1.6F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
      });
    };
    return std::array{
      stroke(Path{}.MoveTo({9, 6}).LineTo({15, 12}).LineTo({9, 18})),
      stroke(Path{}.MoveTo({6, 9}).LineTo({12, 15}).LineTo({18, 9})),
      stroke(Path{}.MoveTo({12, 4}).LineTo({12, 17}).MoveTo({7, 12}).LineTo({12, 17})
                   .LineTo({17, 12}).MoveTo({5, 21}).LineTo({19, 21})),
      stroke(Path{}.MoveTo({12, 19}).LineTo({12, 5}).MoveTo({7, 10}).LineTo({12, 5}).LineTo({17, 10})),
      VectorAsset::Create({24, 24}, [](VectorBuilder& builder) {
        builder.FillPath(Path::RoundedRect({7, 7, 10, 10}, CornerRadii{1.5F}), Color::White());
      }),
      stroke(Path{}.MoveTo({6, 6}).LineTo({18, 18}).MoveTo({18, 6}).LineTo({6, 18})),
    };
  }();
  return icons.at(static_cast<std::size_t>(icon));
}

const auto& ConceptImages() {
  static const std::array images{
    VectorAsset::Create({640, 360}, [](VectorBuilder& builder) {
      auto rect = [&](Rect bounds, Color color, float radius = 8.0F) {
        builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
      };
      rect({0, 0, 640, 360}, Color::Rgb(232, 236, 246), 16);
      rect({18, 18, 116, 324}, Color::Rgb(43, 49, 73));
      rect({34, 38, 78, 10}, Color::Rgb(182, 194, 247));
      rect({34, 62, 59, 6}, Color::Rgb(123, 134, 172));
      rect({34, 80, 70, 6}, Color::Rgb(123, 134, 172));
      rect({152, 18, 470, 144}, Color::White());
      rect({174, 38, 28, 28}, Color::Rgb(99, 79, 170));
      rect({216, 40, 166, 8}, Color::Rgb(71, 77, 105));
      rect({216, 57, 360, 6}, Color::Rgb(198, 205, 222));
      rect({216, 75, 320, 6}, Color::Rgb(198, 205, 222));
      rect({216, 98, 190, 43}, Color::Rgb(238, 235, 247));
      rect({420, 98, 90, 43}, Color::Rgb(99, 79, 170));
      rect({152, 180, 470, 98}, Color::White());
      rect({174, 200, 330, 8}, Color::Rgb(71, 77, 105));
      rect({174, 222, 410, 6}, Color::Rgb(198, 205, 222));
      rect({174, 242, 370, 6}, Color::Rgb(198, 205, 222));
      rect({152, 296, 470, 46}, Color::White());
      rect({570, 306, 30, 26}, Color::Rgb(99, 79, 170));
    }),
    VectorAsset::Create({480, 300}, [](VectorBuilder& builder) {
      auto rect = [&](Rect bounds, Color color, float radius = 6.0F) {
        builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
      };
      rect({0, 0, 480, 300}, Color::Rgb(28, 33, 49), 16);
      rect({20, 20, 440, 38}, Color::Rgb(43, 49, 73));
      rect({34, 34, 180, 8}, Color::Rgb(198, 205, 222));
      rect({378, 30, 66, 18}, Color::Rgb(73, 144, 105));
      rect({24, 82, 12, 10}, Color::Rgb(182, 194, 247));
      rect({48, 82, 342, 10}, Color::Rgb(182, 194, 247));
      for (int line = 0; line < 6; ++line) {
        rect({24, 114.0F + line * 22.0F, 340.0F - (line % 3) * 46.0F, 7}, Color::Rgb(155, 169, 192));
      }
      rect({24, 262, 200, 8}, Color::Rgb(130, 210, 156));
    }),
    VectorAsset::Create({400, 320}, [](VectorBuilder& builder) {
      auto rect = [&](Rect bounds, Color color, float radius = 4.0F) {
        builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
      };
      rect({0, 0, 400, 320}, Color::Rgb(244, 246, 250), 16);
      rect({16, 16, 368, 40}, Color::Rgb(224, 230, 240));
      rect({30, 32, 202, 8}, Color::Rgb(71, 77, 105));
      for (int line = 0; line < 9; ++line) {
        const float y = 72.0F + line * 25.0F;
        const bool removed = line == 2 || line == 3;
        const bool added = line >= 4 && line <= 6;
        if (removed || added) {
          rect({16, y - 4, 368, 23}, removed ? Color::Rgb(255, 222, 225) : Color::Rgb(211, 241, 221));
        }
        rect({28, y + 3, 12, 6}, Color::Rgb(145, 157, 176));
        rect({56, y + 3, 274.0F - (line % 3) * 38.0F, 6}, Color::Rgb(83, 95, 116));
      }
    }),
  };
  return images;
}

[[huxerui::composable]]
View ImagePreview(DialogContext dialog, VectorAsset image, std::string caption) {
  const auto& colors = UseTheme().colors;
  return Column {
    Row {
      Text("Image preview", TextRole::Title).With(Grow()),
      IconButton(ActionIcon(ChatIcon::Close), "Close image").With(Tooltip("Close image (Escape)"))
          .OnClick([dialog] { dialog.Dismiss(); }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    Image(std::move(image)).Fit(ImageFit::Contain).With(
        Grow(), Semantics{.role = SemanticRole::Image, .label = caption}),
    Text(std::move(caption)).With(FontSize(13.0F), Foreground(colors.on_surface_variant)),
  }.With(
      Frame{.width = 900.0F, .height = 640.0F}, Padding(16.0F), Spacing(12.0F),
      Background(colors.surface), CornerRadius(16.0F), ClipChildren(), CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View ChangePreview(DialogContext dialog, std::optional<std::size_t> selected) {
  const auto& colors = UseTheme().colors;
  const TextStyle code_style{Font::Monospace(13.0F), colors.on_surface};
  auto index = std::make_shared<TranscriptIndex>();
  auto selectable = [&](std::string text) {
    const TextBlockId id = index->entries.size() + 1;
    AttributedText paragraph(std::move(text));
    index->positions.emplace(id, index->entries.size());
    index->entries.push_back({id, {paragraph, "\n"}});
    return Text(paragraph).SelectionBlock(id).Style(code_style);
  };
  std::vector<View> lines;
  for (std::size_t file = 0; file < MockChanges().size(); ++file) {
    if (selected && *selected != file) {
      continue;
    }
    const auto& change = MockChanges().at(file);
    lines.push_back(selectable(change.Header()).With(
        Padding(8.0F), Foreground(colors.on_surface_variant), Background(colors.surface_container_high)));
    std::size_t old_line = 1;
    std::size_t new_line = 1;
    for (const auto& line : change.lines) {
      const std::string old_number = line.marker == '+' ? "" : std::to_string(old_line++);
      const std::string new_number = line.marker == '-' ? "" : std::to_string(new_line++);
      const Color background = line.marker == '+' ? Color::Rgb(36, 160, 75, 0.14F)
          : line.marker == '-' ? Color::Rgb(210, 55, 65, 0.14F) : Color::Transparent();
      lines.push_back(Row {
        Text(old_number).Style(code_style).Align(TextAlign::Center).With(
            Frame{.width = 36.0F}, Foreground(colors.on_surface_variant), Semantics{.hidden = true}),
        Text(new_number).Style(code_style).Align(TextAlign::Center).With(
            Frame{.width = 36.0F}, Foreground(colors.on_surface_variant), Semantics{.hidden = true}),
        selectable(std::string(1, line.marker) + line.text).With(
            Grow(), Padding(EdgeInsets::Symmetric(6.0F, 2.0F))),
      }.With(Frame{.min_height = 22.0F}, Background(background), CrossAlign(CrossAxisAlignment::Center)));
    }
  }
  const auto source = std::make_shared<const Transcript>(std::move(index), 0, AttributedText{});
  View code = ScrollView(Column(std::move(lines)).With(CrossAlign(CrossAxisAlignment::Stretch)))
      .ScrollAxis(Axis::Horizontal).With(ScrollBar());
  return Column {
    Row {
      Text("File changes", TextRole::Title).With(Grow()),
      IconButton(ActionIcon(ChatIcon::Close), "Close changes").With(Tooltip("Close changes (Escape)"))
          .OnClick([dialog] { dialog.Dismiss(); }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
    Text("Mock preview only. Old / new line numbers are excluded when copying.", TextRole::Label)
        .With(Foreground(colors.on_surface_variant)),
    SelectionArea(ScrollView(std::move(code)).With(ScrollBar(), Grow())).Source(source).With(Grow()),
  }.With(
      Frame{.width = 800.0F, .height = 560.0F}, Padding(16.0F), Spacing(10.0F),
      Background(colors.surface), CornerRadius(16.0F), ClipChildren(), CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View DocumentText(std::shared_ptr<StreamModel> model, std::shared_ptr<const MessageBlock> row, std::size_t part = 0) {
  const auto id = row->id + part;
  const auto text = id == model->active_id ? model->live.Get() : row->text.at(part).text;
  return Text(text).SelectionBlock(id).With(FontSize(16.0F))
      .On<TextEvents::LinkActivated>([status = model->status](const TextLinkActivation& link) {
        status = "Link requested: " + link.target.ToString();
      });
}

View MessageAction(std::string label, Color foreground) {
  return Text(std::move(label), TextRole::Label).With(
      FontSize(14.0F), Foreground(foreground),
      Padding(EdgeInsets::Symmetric(6.0F, 8.0F)), Frame{.min_height = 36.0F}, CornerRadius(6.0F),
      Focusable{}, PointerCursor(PointerCursorKind::Hand), Semantics{.role = SemanticRole::Button});
}

[[huxerui::composable]]
View QuestionForm(std::shared_ptr<StreamModel> model, TaskScope tasks, std::shared_ptr<const MessageBlock> row) {
  const auto colors = UseTheme().colors;
  const auto& answer = model->answers.Get().at(row->id);
  const auto id = row->id;
  const bool enabled = !answer.submitted && !model->running.Get();
  return Column {
    DocumentText(model, row),
    Flow {
      Checkbox("Windows", answer.windows).OnChanged([model, id](bool value) {
        model->EditAnswer(id, [value](Answer& answer) { answer.windows = value; });
      }),
      Checkbox("Web", answer.web).OnChanged([model, id](bool value) {
        model->EditAnswer(id, [value](Answer& answer) { answer.web = value; });
      }),
    }.With(Spacing(16.0F), Enabled{enabled}),
    Flow {
      RadioButton("Concise", answer.response_style == 0).OnChanged([model, id](bool selected) {
        if (selected) { model->EditAnswer(id, [](Answer& answer) { answer.response_style = 0; }); }
      }),
      RadioButton("Detailed", answer.response_style == 1).OnChanged([model, id](bool selected) {
        if (selected) { model->EditAnswer(id, [](Answer& answer) { answer.response_style = 1; }); }
      }),
    }.With(Spacing(16.0F), Enabled{enabled}),
    TextField(answer.project).Label("Project name").Placeholder("For example, Atlas").MaxLength(80)
        .OnChanged([model, id](const TextEditingValue& value) {
          model->EditAnswer(id, [&value](Answer& answer) { answer.project = value; });
        }).With(Enabled{enabled}),
    answer.submitted
        ? Text("Answered", TextRole::Label).With(Foreground(colors.on_surface_variant)).Key("answered")
        : Button("Submit answer").With(Enabled{model->CanSubmit(id)})
              .OnClick([=] { SubmitAnswer(model, tasks, colors, id); }).Key("submit"),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View ResultTable(std::shared_ptr<StreamModel> model, std::shared_ptr<const MessageBlock> block) {
  const auto& colors = UseTheme().colors;
  constexpr std::array widths{144.0F, 180.0F, 112.0F};
  std::vector<View> rows;
  for (std::size_t row = 0; row < block->text.size() / widths.size(); ++row) {
    std::vector<View> cells;
    for (std::size_t column = 0; column < 3; ++column) {
      const auto part = row * 3 + column;
      cells.push_back(DocumentText(model, block, part).With(Frame{.width = widths[column]}, Padding(10.0F))
          .Key(block->id + part));
    }
    rows.push_back(Row(std::move(cells)).With(
        Background(row == 0 ? colors.surface_container_high : colors.surface_container_low),
        CrossAlign(CrossAxisAlignment::Stretch)
    ).Key(row));
  }
  return ScrollView(Column(std::move(rows))).ScrollAxis(Axis::Horizontal).With(ScrollBar());
}

[[huxerui::composable]]
View MessageRow(std::shared_ptr<StreamModel> model, TaskScope tasks, std::shared_ptr<const MessageBlock> row) {
  const auto colors = UseTheme().colors;
  View content;
  switch (row->kind) {
  case BlockKind::User:
    return Row {
      DocumentText(model, row).With(
          Frame{.max_width = 580.0F}, Padding(14.0F), CornerRadius(18.0F),
          Background(colors.secondary_container), Foreground(colors.on_secondary_container)),
    }.With(MainAlign(MainAxisAlignment::End),
        Padding(EdgeInsets{.top = 20.0F, .right = 4.0F, .bottom = 12.0F, .left = 32.0F}));
  case BlockKind::Reasoning:
    content = Column {
      Row {
        Image(DocumentIcon()).Tint(colors.primary).With(
            Frame{.width = 24.0F, .height = 24.0F}, Semantics{.hidden = true}),
        Text("AGENT", TextRole::Label),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
      Row {
        Text(model->active_id == row->id ? "Thinking..." : "Reasoning summary", TextRole::Label)
            .With(FontSize(14.0F)),
        Image(ActionIcon(row->expanded ? ChatIcon::Collapse : ChatIcon::Expand)).Tint(colors.on_surface_variant)
            .With(Frame{.width = 16.0F, .height = 16.0F}),
      }.With(
          Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center), Foreground(colors.on_surface_variant),
          Padding(EdgeInsets::Symmetric(4.0F, 6.0F)), Frame{.min_height = 36.0F}, CornerRadius(6.0F),
          Focusable{}, PointerCursor(PointerCursorKind::Hand),
          Semantics{.role = SemanticRole::Button, .label = "Reasoning summary", .expanded = row->expanded,
                    .busy = model->active_id == row->id, .descendants = SemanticDescendantPolicy::Exclude}
      ).OnClick([model, id = row->id] { model->ToggleDetails(id); }).Key("disclosure"),
      row->expanded ? DocumentText(model, row).With(FontSize(14.0F), Foreground(colors.on_surface_variant),
          Padding(EdgeInsets{.left = 12.0F})).Key("summary") : View{},
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  case BlockKind::Paragraph:
    content = DocumentText(model, row);
    break;
  case BlockKind::Image: {
    const auto dialog = UseDialog();
    std::vector<View> images;
    for (std::size_t part = 0; part < row->text.size(); ++part) {
      const auto image = ConceptImages().at(part);
      const auto caption = row->text[part].text.PlainText();
      images.push_back(Column {
        Image(image).Fit(ImageFit::Contain).With(
            Frame{.height = 180.0F}, CornerRadius(10.0F), ClipChildren(),
            Background(colors.surface_container_low), Focusable{}, PointerCursor(PointerCursorKind::Hand),
            Semantics{.role = SemanticRole::Button, .label = caption, .hint = "Open larger image"}
        ).OnClick([dialog, image, caption] { dialog.Show(ImagePreview, image, caption); }),
        DocumentText(model, row, part).With(FontSize(13.0F), Foreground(colors.on_surface_variant)),
      }.With(Frame{.width = 300.0F}, Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key(row->id + part));
    }
    content = Column {
      Text("Layout concepts / scroll horizontally, click to enlarge", TextRole::Label)
          .With(Foreground(colors.on_surface_variant)),
      ScrollView(Row(std::move(images)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Start)))
          .ScrollAxis(Axis::Horizontal).With(ScrollBar()),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  }
  case BlockKind::List: {
    std::vector<View> items;
    for (std::size_t part = 0; part + 1 < row->text.size(); part += 2) {
      // Prefixes include nested indentation and belong to the copied document, not a decorative overlay.
      items.push_back(Row {
        DocumentText(model, row, part),
        DocumentText(model, row, part + 1).With(Grow()),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Start)).Key(row->id + part));
    }
    content = Column(std::move(items)).With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  }
  case BlockKind::Code:
    content = Column {
      Text("C++ / select text to copy", TextRole::Label),
      ScrollView(DocumentText(model, row)).ScrollAxis(Axis::Horizontal).With(ScrollBar()),
    }.With(Spacing(12.0F), Padding(14.0F), Background(colors.surface_container_low),
        CornerRadius(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  case BlockKind::Tool: {
    const bool active = model->active_id == row->id + row->text.size() - 1;
    const std::string result = active ? "Running..." : !row->exit_code ? "Stopped"
        : (*row->exit_code == 0 ? "Completed / exit " : "Failed / exit ") + std::to_string(*row->exit_code);
    const Color result_color = row->exit_code && *row->exit_code != 0 && !active
        ? colors.error : colors.on_surface_variant;
    content = Column {
      Row {
        Text(row->label, TextRole::Label).With(Grow()),
        Text(result, TextRole::Label).With(FontSize(12.0F), Foreground(result_color)),
        Image(ActionIcon(row->expanded ? ChatIcon::Collapse : ChatIcon::Expand)).Tint(colors.on_surface_variant)
            .With(Frame{.width = 16.0F, .height = 16.0F}, Semantics{.hidden = true}),
      }.With(
          Spacing(8.0F), Frame{.min_height = 40.0F}, CrossAlign(CrossAxisAlignment::Center),
          Focusable{}, PointerCursor(PointerCursorKind::Hand),
          Semantics{.role = SemanticRole::Button, .label = row->label + ": " + result,
                    .expanded = row->expanded, .busy = active, .descendants = SemanticDescendantPolicy::Exclude}
      ).OnClick([model, id = row->id] { model->ToggleDetails(id); }).Key("tool-disclosure"),
      Text("Terminal / simulated sandbox", TextRole::Label).With(Foreground(colors.on_surface_variant)),
      ScrollView(Column {
        DocumentText(model, row, 0).With(Foreground(colors.primary)),
        row->expanded ? DocumentText(model, row, 1).Key("output") : View{},
      }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)))
          .ScrollAxis(Axis::Horizontal).With(ScrollBar()),
    }.With(Spacing(8.0F), Padding(14.0F), Background(colors.surface_container_low),
        CornerRadius(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  }
  case BlockKind::Files: {
    const auto dialog = UseDialog();
    std::vector<View> files;
    for (std::size_t file = 0; file < MockChanges().size(); ++file) {
      files.push_back(Row {
        Image(DocumentIcon()).Tint(colors.primary).With(
            Frame{.width = 22.0F, .height = 22.0F}, Semantics{.hidden = true}),
        DocumentText(model, row, file).With(Grow()),
        Image(ActionIcon(ChatIcon::Expand)).Tint(colors.on_surface_variant).With(
            Frame{.width = 16.0F, .height = 16.0F}, Semantics{.hidden = true}),
      }.With(
          Spacing(8.0F), Padding(6.0F), Frame{.min_height = 40.0F}, CornerRadius(6.0F),
          CrossAlign(CrossAxisAlignment::Center), Focusable{}, PointerCursor(PointerCursorKind::Hand),
          Semantics{.role = SemanticRole::Button, .label = MockChanges()[file].path,
                    .hint = "Preview mock text changes", .descendants = SemanticDescendantPolicy::Exclude}
      ).OnClick([dialog, file] { dialog.Show(ChangePreview, std::optional<std::size_t>(file)); }).Key(file));
    }
    content = Column {
      Text("Proposed files / simulated", TextRole::Label),
      Column(std::move(files)).With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Stretch)),
      Flow {
        MessageAction("Review proposal", colors.on_surface_variant).OnClick([dialog] {
          dialog.Show(ChangePreview, std::optional<std::size_t>{});
        }),
        MessageAction("Request revision", colors.on_surface_variant).With(Enabled{!model->running.Get()})
            .OnClick([=] { Send(model, tasks, colors, "Revise the proposed layout.", true); }),
      }.With(Spacing(8.0F)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  }
  case BlockKind::Table:
    content = ResultTable(model, row);
    break;
  case BlockKind::Question:
    content = QuestionForm(model, tasks, row).With(
        Padding(14.0F), Background(colors.surface_container_low), CornerRadius(12.0F));
    break;
  }
  return Column {
    std::move(content),
  }.With(Padding(EdgeInsets::Symmetric(4.0F, 8.0F)), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View TranscriptBlocks(std::shared_ptr<StreamModel> model, TaskScope tasks) {
  const auto messages = model->messages.Get();
  Lifecycle([model] {
    if (std::exchange(model->follow_new_block, false)) {
      model->Latest();
    }
  }, messages->size());
  if (messages->empty()) {
    return Column {
      Text("Start a conversation", TextRole::Title),
      Text("Follow a scripted repair: analysis, source inspection, a failing test, a proposed patch, and a rerun."),
    }.With(Grow(), Spacing(12.0F), Padding(20.0F), MainAlign(MainAxisAlignment::Center),
        CrossAlign(CrossAxisAlignment::Stretch));
  }
  return VirtualList(messages->size(), [model, tasks, messages](std::size_t index) {
    return MessageRow(model, tasks, messages->at(index)).Key(messages->at(index)->id);
  }).EstimatedItemExtent(100.0F).Controller(model->scroll).With(ScrollBar(), Grow());
}

[[huxerui::composable]]
View SelectableTranscript(std::shared_ptr<StreamModel> model, TaskScope tasks) {
  return SelectionArea(TranscriptBlocks(model, tasks)).Source(model->source.Get()).With(Grow());
}

[[huxerui::composable]]
View Composer(std::shared_ptr<StreamModel> model, TaskScope tasks) {
  const auto colors = UseTheme().colors;
  const bool running = model->running.Get();
  return Column {
    TextField(model->draft).Label("Message").Placeholder("Ask the Agent...").MaxLength(2000)
        .OnChanged([model](const TextEditingValue& value) { model->draft = value; })
        .OnSubmitted([=] { SendDraft(model, tasks, colors); }),
    Row {
      Text(model->status, TextRole::Label).With(Grow(), Foreground(colors.on_surface_variant)),
      IconButton(ActionIcon(ChatIcon::Latest), "Latest").With(Tooltip("Back to latest"))
          .OnClick([model] { model->Latest(); }),
      running ? IconButton(ActionIcon(ChatIcon::Stop), "Stop").With(Tooltip("Stop response"))
                    .OnClick([model] { model->stop = true; }).Key("stop")
              : IconButton(ActionIcon(ChatIcon::Send), "Send").With(Tooltip("Send message"),
                    Enabled{HasText(model->draft.Get().text) && !model->draft.Get().composition})
                    .OnClick([=] { SendDraft(model, tasks, colors); }).Key("send"),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View Conversation() {
  auto tasks = UseTaskScope();
  auto messages = UseState(std::shared_ptr<const Messages>(std::make_shared<const Messages>()));
  auto live = UseState(AttributedText{});
  auto source = UseState<std::shared_ptr<const TextSelectionSource>>(
      std::make_shared<const Transcript>(std::make_shared<const TranscriptIndex>(), 0, AttributedText{}));
  auto answers = UseState(Answers{});
  auto draft = UseState(TextEditingValue::FromText(
      "Investigate why Copy loses earlier messages in a long Agent conversation, and propose a tested fix."));
  auto status = UseState(std::string{"Enter to send. Responses are scripted locally."});
  auto running = UseState(false);
  auto scroll = UseScrollController();
  auto model = UseState(std::make_shared<StreamModel>(StreamModel{
      .messages = messages, .live = live, .source = source, .answers = answers, .draft = draft,
      .status = status, .running = running, .scroll = scroll,
  })).Get();
  return Column {
    Text("Agent chat", TextRole::Title),
    Text("Local scripted demo / no model connection", TextRole::Label),
    SelectableTranscript(model, tasks),
    Composer(model, tasks),
  }.With(Padding(16.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View App() {
  return MaterialTheme {Conversation()};
}

const Application application{
    App,
    {.window = {.title = "HuxerUI Streaming Text", .initial_size = {900.0F, 760.0F}}}
};
