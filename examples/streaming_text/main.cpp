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

constexpr Color success_color = Color::Rgb(46, 109, 79);

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

const ThemeSpec& ChatThemeSpec() {
  static const ThemeSpec theme = [] {
    auto value = MaterialLightThemeSpec();
    value.colors = {
      .primary = Color::Rgb(35, 104, 91),
      .on_primary = Color::White(),
      .primary_container = Color::Rgb(225, 238, 232),
      .on_primary_container = Color::Rgb(27, 71, 62),
      .secondary = Color::Rgb(73, 88, 85),
      .on_secondary = Color::White(),
      .secondary_container = Color::Rgb(236, 239, 235),
      .on_secondary_container = Color::Rgb(38, 45, 44),
      .tertiary_container = Color::Rgb(237, 235, 230),
      .on_tertiary_container = Color::Rgb(70, 66, 56),
      .background = Color::Rgb(248, 250, 248),
      .surface = Color::White(),
      .surface_container_low = Color::Rgb(244, 246, 243),
      .surface_container = Color::Rgb(239, 242, 238),
      .surface_container_high = Color::Rgb(232, 237, 231),
      .surface_container_highest = Color::Rgb(222, 229, 222),
      .on_surface = Color::Rgb(38, 45, 44),
      .on_surface_variant = Color::Rgb(90, 102, 96),
      .outline = Color::Rgb(128, 141, 132),
      .inverse_surface = Color::Rgb(42, 52, 47),
      .inverse_on_surface = Color::Rgb(243, 246, 242),
      .scrim = Color::Rgb(0, 0, 0, 0.32F),
      .error = Color::Rgb(163, 56, 53),
    };
    // Material's base interaction colors are captured values, not live references to the palette.
    value.interactions.indication.hover->fill = WithAlpha(value.colors.primary, 0.08F);
    value.interactions.indication.ripple->color = WithAlpha(value.colors.primary, 0.12F);
    value.interactions.focus_ring = {value.colors.primary, 2.0F, 2.0F};
    return value;
  }();
  return theme;
}

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
  if (follow_up) {
    return {
      {0, BlockKind::Paragraph, {{AttributedText{
        TextSpan("Follow-up review. ").Style({.font_weight = FontWeight::Bold}),
        TextSpan(prompt).Style({.font_slant = FontSlant::Italic}),
        TextSpan("\n\nI'll keep the earlier investigation in history and use a short interaction-review fixture "
                 "for this follow-up, without repeating the source inspection or patch walkthrough."),
      }, "\n\n"}}},
      ToolCall("Check follow-up interactions", "./build/atlas_tests '[interaction]' --reporter compact",
          "Independent interaction fixture / simulated\n"
          "PASS  Horizontal logs do not move the vertical transcript\n"
          "PASS  Copy retains bullets and paragraph separators\n"
          "PASS  Stop preserves partial output without an exit result\n"
          "PASS  Image preview survives virtual-row eviction\n"
          "\n4 test cases passed. No project files changed.\n"),
      {0, BlockKind::List, {
        Paragraph("\xE2\x80\xA2 ", ""), Paragraph(
            "Keep prose responsive and long commands horizontally scrollable.", "\n"),
        Paragraph("    \xE2\x97\xA6 ", ""), Paragraph(
            "Check the requested layout with both short and wrapped text before accepting it.", "\n"),
        Paragraph("\xE2\x80\xA2 ", ""), Paragraph(
            "Use the earlier diff and image previews to review the proposal against these constraints."),
      }},
      {0, BlockKind::Paragraph, {Paragraph(
          "These are additional review notes, not a new patch. The previous proposal, tool results, and "
          "answers remain unchanged. This local script does not interpret arbitrary requests or run commands.")}},
    };
  }

  std::vector<MessageBlock> response{
    {0, BlockKind::Reasoning, {Paragraph(
        "Scripted analysis summary: check whether Copy depends on mounted rows or the logical transcript. "
        "Reproduce the failure before changing ownership; keep the existing bottom-follow policy if it passes.")}},
    {0, BlockKind::Paragraph, {{AttributedText{
      TextSpan("I'll investigate selection in the Agent conversation. ").Style({.font_weight = FontWeight::Bold}),
      TextSpan("In this simulated Atlas project, Copy loses earlier messages once they leave the viewport. "
               "I'll trace the boundary, reproduce the failure, and verify a focused fix. The key distinction is "),
      TextSpan("document lifetime versus visible geometry")
          .Style({.foreground = colors.on_surface, .background = colors.surface_container_high}),
      TextSpan(": unmounting a row must not erase the text available to Copy."),
    }, "\n\n"}}},
    {0, BlockKind::List, {
      Paragraph("1. ", ""), Paragraph(
          "Inspect the conversation View and its transcript source; check which values survive row eviction.", "\n"),
      Paragraph("2. ", ""), Paragraph(
          "Reproduce with 500 messages, including a selected endpoint outside the viewport.", "\n"),
      Paragraph("3. ", ""), Paragraph(
          "Bind visible paragraphs to stable document IDs, then verify Copy and the reader's scroll position."),
    }},
    ToolCall("Inspect the conversation boundary",
        "rg -n 'VirtualList|SelectionArea|SelectionBlock' src tests && sed -n '1,80p' src/conversation.cpp",
        "src/conversation.cpp:7:  return VirtualList(snapshot->Count(), [snapshot](std::size_t index) {\n"
        "No SelectionArea or SelectionBlock binding found.\n\n" + MockChanges()[0].Body(false)),
    ToolCall("Reproduce the missing selection", "./build/atlas_tests '[conversation]' --reporter compact",
        "Fixture: 500 messages / 9 mounted rows\n"
        "PASS  Streaming batches text deltas\n"
        "PASS  Reading position survives an append\n"
        "FAIL  Copy spans unmounted messages\n"
        "      Select All returned false: no logical selection owner\n"
        "      Clipboard available; no invalid UTF-16 offsets\n"
        "\n2 passed, 1 failed.\n", 1),
    {0, BlockKind::Paragraph, {Paragraph(
        "The failure is at the conversation boundary, not the renderer or clipboard. The transcript already "
        "exposes Count, IdAt, IndexOf, and BlockAt, but the virtual list never supplies it to selection.\n\n"
        "Wrap the list in SelectionArea, provide that same snapshot as its Source, and bind each Text with "
        "SelectionBlock(id). Key(id) keeps row identity stable across insertions. The nine mounted rows are "
        "expected; expanding the cache would only hide the missing ownership. Leave the passing scroll policy alone.")}},
    {0, BlockKind::Code, {{CodeText(MockChanges()[0].Body(true)), "\n\n"}}},
  };
  std::vector<TextSelectionBlock> files;
  for (const auto& change : MockChanges()) {
    files.push_back(Paragraph(change.path + "   " + change.Summary(), "\n"));
  }
  files.back().separator = "\n\n";
  response.push_back({0, BlockKind::Files, std::move(files)});
  response.push_back(ToolCall("Build the proposed changes", "cmake --build build --target atlas_tests -j 4",
      "[1/4] Compiling src/conversation.cpp\n"
      "[2/4] Compiling tests/conversation.cpp\n"
      "[3/4] Linking atlas_tests\n"
      "[4/4] Build complete\n"
      "\nDebug / simulated sandbox / no compiler diagnostics\n"));
  response.push_back(ToolCall("Rerun conversation checks", "./build/atlas_tests '[conversation]' --reporter compact",
      "PASS  Copy spans unmounted messages\n"
      "      Complete 500-message snapshot copied; 9 rows realized\n"
      "PASS  Streaming does not steal the reading position\n"
      "      Anchor at message 120 unchanged after tail append\n"
      "PASS  Streaming batches text deltas\n"
      "PASS  Copy preserves paragraph separators\n"
      "PASS  Cancel preserves received output\n"
      "PASS  Stable identities survive an insertion\n"
      "\n6 test cases passed; 32 assertions passed. Simulated results only.\n"));
  response.push_back({0, BlockKind::Table, {
    {AttributedText{TextSpan("Check").Style({.font_weight = FontWeight::Bold})}, "\t"},
    {AttributedText{TextSpan("Outcome").Style({.font_weight = FontWeight::Bold})}, "\t"},
    {AttributedText{TextSpan("Evidence").Style({.font_weight = FontWeight::Bold})}, "\n"},
    Paragraph("Offscreen Copy", "\t"), Paragraph("Passed in fixture", "\t"), Paragraph("500 blocks", "\n"),
    Paragraph("Virtualization", "\t"), Paragraph("Bounded realization", "\t"), Paragraph("9 rows", "\n"),
    Paragraph("Reader anchor", "\t"), Paragraph("Unchanged", "\t"), Paragraph("Item 120", "\n"),
    Paragraph("Platform behavior", "\t"), Paragraph("Not executed", "\t"), Paragraph("Manual checks"),
  }});
  response.push_back({0, BlockKind::Image, {
    Paragraph("Conversation overview: navigation, transcript, and action cards."),
    Paragraph("Tool execution: a stable command with incremental output."),
    Paragraph("Patch review: additions and deletions beside the original context."),
  }});
  response.push_back({0, BlockKind::List, {
    Paragraph("\xE2\x80\xA2 ", ""), Paragraph(
        "Review the proposed files or enlarge a concept image without interrupting the response.", "\n"),
    Paragraph("\xE2\x80\xA2 ", ""), Paragraph(
        "Select across prose, a list item, and a tool log; copied text should retain document order.", "\n"),
    Paragraph("    \xE2\x97\xA6 ", ""), Paragraph(
        "Scroll the selection offscreen and return. The logical range should survive row eviction.", "\n"),
    Paragraph("\xE2\x80\xA2 ", ""), Paragraph(
        "Try Stop during a tool call, or scroll back while text arrives. Latest returns to the end."),
  }});
  response.push_back({0, BlockKind::Paragraph, {{AttributedText{
    TextSpan("Ready for review. ").Style({.font_weight = FontWeight::Bold}),
    TextSpan("The proposal connects visible rows to one persistent document without changing the stream publisher. "
             "The failure, diff, and rerun remain above; platform shaping and clipboard behavior still need "
             "host-specific checks. See "),
    TextSpan("HuxerUI").Style({.foreground = colors.primary}).Link(Uri("https://github.com/HuxerUI/HuxerUI")),
    TextSpan(" for the SDK contract. All analysis summaries and results here are scripted; no model, command, "
             "or project file was accessed."),
  }, "\n\n"}}});
  response.push_back({0, BlockKind::Question, {Paragraph(
      "What should the next review focus on? Choose your target platforms, response style, and project name.")}});
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
  model->code_color = colors.secondary;
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

void Send(std::shared_ptr<StreamModel> model, TaskScope tasks, ColorScheme colors, std::string prompt) {
  if (model->running.Get() || !HasText(prompt)) {
    return;
  }
  const bool follow_up = !model->messages.Get()->empty();
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
  Send(model, tasks, colors, prompt);
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
  const auto& colors = ChatThemeSpec().colors;
  static const std::array images{
    VectorAsset::Create({640, 360}, [colors](VectorBuilder& builder) {
      auto rect = [&](Rect bounds, Color color, float radius = 8.0F) {
        builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
      };
      rect({0, 0, 640, 360}, colors.surface_container, 16);
      rect({18, 18, 116, 324}, colors.inverse_surface);
      rect({34, 38, 78, 10}, colors.primary_container);
      rect({34, 62, 59, 6}, WithAlpha(colors.inverse_on_surface, 0.45F));
      rect({34, 80, 70, 6}, WithAlpha(colors.inverse_on_surface, 0.45F));
      rect({152, 18, 470, 144}, colors.surface);
      rect({174, 38, 28, 28}, colors.primary);
      rect({216, 40, 166, 8}, colors.on_surface);
      rect({216, 57, 360, 6}, colors.surface_container_highest);
      rect({216, 75, 320, 6}, colors.surface_container_highest);
      rect({216, 98, 190, 43}, colors.secondary_container);
      rect({420, 98, 90, 43}, colors.primary);
      rect({152, 180, 470, 98}, colors.surface);
      rect({174, 200, 330, 8}, colors.on_surface);
      rect({174, 222, 410, 6}, colors.surface_container_highest);
      rect({174, 242, 370, 6}, colors.surface_container_highest);
      rect({152, 296, 470, 46}, colors.surface);
      rect({570, 306, 30, 26}, colors.primary);
    }),
    VectorAsset::Create({480, 300}, [colors](VectorBuilder& builder) {
      auto rect = [&](Rect bounds, Color color, float radius = 6.0F) {
        builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
      };
      rect({0, 0, 480, 300}, colors.inverse_surface, 16);
      rect({20, 20, 440, 38}, WithAlpha(colors.inverse_on_surface, 0.08F));
      rect({34, 34, 180, 8}, colors.inverse_on_surface);
      rect({378, 30, 66, 18}, WithAlpha(colors.primary_container, 0.30F));
      rect({24, 82, 12, 10}, colors.primary_container);
      rect({48, 82, 342, 10}, colors.primary_container);
      for (int line = 0; line < 6; ++line) {
        rect({24, 114.0F + line * 22.0F, 340.0F - (line % 3) * 46.0F, 7}, WithAlpha(colors.inverse_on_surface, 0.65F));
      }
      rect({24, 262, 200, 8}, colors.primary_container);
    }),
    VectorAsset::Create({400, 320}, [colors](VectorBuilder& builder) {
      auto rect = [&](Rect bounds, Color color, float radius = 4.0F) {
        builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
      };
      rect({0, 0, 400, 320}, colors.surface, 16);
      rect({16, 16, 368, 40}, colors.surface_container);
      rect({30, 32, 202, 8}, colors.on_surface);
      for (int line = 0; line < 9; ++line) {
        const float y = 72.0F + line * 25.0F;
        const bool removed = line == 2 || line == 3;
        const bool added = line >= 4 && line <= 6;
        if (removed || added) {
          rect({16, y - 4, 368, 23}, removed ? WithAlpha(colors.error, 0.10F) : WithAlpha(success_color, 0.12F));
        }
        rect({28, y + 3, 12, 6}, colors.outline);
        rect({56, y + 3, 274.0F - (line % 3) * 38.0F, 6}, colors.secondary);
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
      const Color background = line.marker == '+' ? WithAlpha(success_color, 0.12F)
          : line.marker == '-' ? WithAlpha(colors.error, 0.10F) : Color::Transparent();
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
    TextField(answer.project).Variant(TextFieldVariant::Outlined)
        .Label("Project name").Placeholder("For example, Atlas").MaxLength(80)
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
        Background(row == 0 ? colors.surface_container : colors.surface),
        CrossAlign(CrossAxisAlignment::Stretch)
    ).Key(row));
  }
  return ScrollView(Column(std::move(rows)).With(
      Border{WithAlpha(colors.outline, 0.22F)}, CornerRadius(10.0F), ClipChildren()
  )).ScrollAxis(Axis::Horizontal).With(ScrollBar());
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
        Image(DocumentIcon()).Tint(colors.on_surface_variant).With(
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
            Background(colors.surface), Border{WithAlpha(colors.outline, 0.22F)},
            Focusable{}, PointerCursor(PointerCursorKind::Hand),
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
      Text("C++ / select text to copy", TextRole::Label).With(Foreground(colors.on_surface_variant)),
      ScrollView(DocumentText(model, row)).ScrollAxis(Axis::Horizontal).With(ScrollBar()),
    }.With(Spacing(12.0F), Padding(14.0F), Background(colors.surface_container_low),
        Border{WithAlpha(colors.outline, 0.22F)}, CornerRadius(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  case BlockKind::Tool: {
    const bool active = model->active_id == row->id + row->text.size() - 1;
    const std::string result = active ? "Running..." : !row->exit_code ? "Stopped"
        : (*row->exit_code == 0 ? "Completed / exit " : "Failed / exit ") + std::to_string(*row->exit_code);
    const Color result_color = active ? colors.primary : !row->exit_code ? colors.on_surface_variant
        : *row->exit_code == 0 ? success_color : colors.error;
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
        DocumentText(model, row, 0),
        row->expanded ? DocumentText(model, row, 1).Key("output") : View{},
      }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)))
          .ScrollAxis(Axis::Horizontal).With(ScrollBar()),
    }.With(Spacing(8.0F), Padding(14.0F), Background(colors.surface),
        Border{WithAlpha(colors.outline, 0.22F)}, CornerRadius(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  }
  case BlockKind::Files: {
    const auto dialog = UseDialog();
    std::vector<View> files;
    for (std::size_t file = 0; file < MockChanges().size(); ++file) {
      files.push_back(Row {
        Image(DocumentIcon()).Tint(colors.on_surface_variant).With(
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
            .OnClick([=] { Send(model, tasks, colors, "Revise the proposed layout."); }),
      }.With(Spacing(8.0F)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  }
  case BlockKind::Table:
    content = ResultTable(model, row);
    break;
  case BlockKind::Question:
    content = QuestionForm(model, tasks, row).With(
        Padding(14.0F), Background(colors.surface_container_low),
        Border{WithAlpha(colors.outline, 0.22F)}, CornerRadius(12.0F));
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
  const bool can_send = HasText(model->draft.Get().text) && !model->draft.Get().composition;
  return Column {
    TextField(model->draft).Variant(TextFieldVariant::Standard)
        .Label("Message").Placeholder("Ask the Agent...").MaxLength(2000)
        .OnChanged([model](const TextEditingValue& value) { model->draft = value; })
        .OnSubmitted([=] { SendDraft(model, tasks, colors); }),
    Row {
      Text(model->status, TextRole::Label).With(Grow(), Foreground(colors.on_surface_variant)),
      IconButton(ActionIcon(ChatIcon::Latest), "Latest").With(Tooltip("Back to latest"))
          .OnClick([model] { model->Latest(); }),
      running ? IconButton(ActionIcon(ChatIcon::Stop), "Stop").With(
                    Tooltip("Stop response"), Background(colors.secondary_container))
                    .OnClick([model] { model->stop = true; }).Key("stop")
              : IconButton(ActionIcon(ChatIcon::Send), "Send").With(Tooltip("Send message"),
                    Enabled{can_send}, Foreground(colors.on_primary),
                    Background(can_send ? colors.primary : colors.surface_container_high))
                    .OnClick([=] { SendDraft(model, tasks, colors); }).Key("send"),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(8.0F), Padding(12.0F), Background(colors.surface),
      Border{WithAlpha(colors.outline, 0.22F)}, CornerRadius(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View Conversation() {
  const auto& colors = UseTheme().colors;
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
    Text("Local scripted demo / no model connection", TextRole::Label).With(Foreground(colors.on_surface_variant)),
    SelectableTranscript(model, tasks),
    Composer(model, tasks),
  }.With(Padding(16.0F), Spacing(12.0F), Background(colors.background), CrossAlign(CrossAxisAlignment::Stretch));
}

View App() {
  return MaterialTheme {ChatThemeSpec(), Conversation()};
}

const Application application{
    App,
    {.window = {.title = "HuxerUI Streaming Text", .initial_size = {900.0F, 760.0F}}}
};
