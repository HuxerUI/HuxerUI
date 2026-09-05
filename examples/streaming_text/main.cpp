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

enum class BlockKind { User, Reasoning, Paragraph, Image, List, Code, Files, Table, Question };

struct MessageBlock {
  TextBlockId id;
  BlockKind kind;
  std::vector<TextSelectionBlock> text;
  bool expanded = false;
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
};

const auto& MockChanges() {
  static const std::array<FileChange, 2> changes{{
    {"src/message_view.cpp", {
      {' ', "#include <huxerui/huxerui.h>"},
      {' ', ""},
      {' ', "using namespace huxerui;"},
      {' ', ""},
      {' ', "View MessageView(const AttributedText& message) {"},
      {'-', "  return Text(message);"},
      {'+', "  return SelectionArea("},
      {'+', "      Text(message).SelectionBlock(42)"},
      {'+', "  );"},
      {' ', "}"},
    }},
    {"tests/message_view.cpp", {
      {' ', "TEST_CASE(\"Messages retain rich text\") {"},
      {' ', "  const AttributedText message{"},
      {'-', "    TextSpan(\"Hello, Agent\"),"},
      {'+', "    TextSpan(\"Hello, \"),"},
      {'+', "    TextSpan(\"Agent\").Style({.font_weight = FontWeight::Bold}),"},
      {' ', "  };"},
      {' ', "  REQUIRE(message.PlainText() == \"Hello, Agent\");"},
      {'+', "  REQUIRE(message.Length() == 12);"},
      {'+', "  REQUIRE(message.StyleRanges().size() == 1);"},
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

  void Seal(Messages& updated) {
    if (active_id == 0) {
      return;
    }
    auto last = std::make_shared<MessageBlock>(*updated.back());
    last->text[0].text = last->kind == BlockKind::Code ? Highlight(live.Get(), code_color) : live.Get();
    updated.back() = std::move(last);
    active_id = 0;
    ++version;
  }

  void Append(BlockKind kind, std::vector<TextSelectionBlock> text, bool streaming = false) {
    const bool following = Following();
    auto updated = std::make_shared<Messages>(*messages.Get());
    Seal(*updated);
    const auto id = next_id;
    next_id += text.size();
    updated->push_back(std::make_shared<const MessageBlock>(MessageBlock{id, kind, std::move(text)}));
    if (kind == BlockKind::Question) {
      auto values = answers.Get();
      values.emplace(id, Answer{});
      answers = std::move(values);
    }
    if (streaming) {
      active_id = id;
      live = AttributedText{};
    }
    messages = std::move(updated);
    // Structural edits rebuild the index; streaming snapshots only replace the independently observed tail.
    RebuildIndex();
    follow_new_block = follow_new_block || following;
  }

  void Finish() {
    if (active_id != 0) {
      auto updated = std::make_shared<Messages>(*messages.Get());
      Seal(*updated);
      messages = std::move(updated);
      RebuildIndex();
    }
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

  void ToggleReasoning(TextBlockId id) {
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

std::vector<MessageBlock> ResponseContent(const ColorScheme& colors, const std::string& prompt, bool follow_up) {
  std::vector<MessageBlock> response{
    {0, BlockKind::Reasoning, {Paragraph(
        "Scripted reasoning summary: separate readable paragraphs from images and interactive controls, "
        "then keep generation and answers outside virtual rows. This is a local demo, not private model reasoning.")}},
    {0, BlockKind::Paragraph, {{AttributedText{
      TextSpan(follow_up ? "Thanks, I'll use those details. " : "Let's build that conversation. ")
          .Style({.font_weight = FontWeight::Bold}),
      TextSpan("Your message: "),
      TextSpan(prompt).Style({.font_slant = FontSlant::Italic}),
      TextSpan("\n\nThe Agent reply can use the full conversation width, mixing "),
      TextSpan("rich text").Style({.foreground = colors.on_primary_container,
                                   .background = colors.primary_container}),
      TextSpan(" with ordinary components as each result arrives."),
    }, "\n\n"}}},
  };
  if (!follow_up) {
    response.push_back({0, BlockKind::Image, {Paragraph("A concept for the conversation layout.")}});
    response.push_back({0, BlockKind::List, {
      Paragraph("1. ", ""),
      {AttributedText{
        TextSpan("Keep text selectable. ").Style({.font_weight = FontWeight::Bold}),
        TextSpan("Paragraphs, code, and table cells share one logical selection source."),
      }, "\n"},
      Paragraph("2. ", ""),
      {AttributedText{
        TextSpan("Keep state durable. ").Style({.font_weight = FontWeight::Bold}),
        TextSpan("Earlier messages stay in the conversation; scrolling away does not stop the response."),
      }, "\n\n"},
    }});
  }
  response.push_back({0, BlockKind::Code, {{AttributedText{
    TextSpan("auto ").Style({.font = Font::Monospace(15.0F), .font_weight = FontWeight::Bold}),
    TextSpan("message = AttributedText{\n"
             "  TextSpan(\"Hello, \"),\n"
             "  TextSpan(\"Agent\").Style({.font_weight = FontWeight::Bold}),\n"
             "};\nText(message).SelectionBlock(42);").Style({.font = Font::Monospace(15.0F)}),
  }, "\n\n"}}});
  std::vector<TextSelectionBlock> files;
  for (const auto& change : MockChanges()) {
    files.push_back(Paragraph(change.path + "   " + change.Summary(), "\n"));
  }
  files.back().separator = "\n\n";
  response.push_back({0, BlockKind::Files, std::move(files)});
  response.push_back({0, BlockKind::Table, {
    {AttributedText{TextSpan("Target").Style({.font_weight = FontWeight::Bold})}, "\t"},
    {AttributedText{TextSpan("Result").Style({.font_weight = FontWeight::Bold})}, "\t"},
    {AttributedText{TextSpan("Rendering").Style({.font_weight = FontWeight::Bold})}, "\n"},
    Paragraph("Windows", "\t"), Paragraph("Ready", "\t"), Paragraph("Native", "\n"),
    Paragraph("Web", "\t"), Paragraph("Basic shaping", "\t"), Paragraph("Canvas"),
  }});
  response.push_back({0, BlockKind::Paragraph, {{AttributedText{
    TextSpan("The proposed changes are ready to review. ").Style({.font_weight = FontWeight::Bold}),
    TextSpan("Select and copy across blocks, or send another message. Learn more about "),
    TextSpan("HuxerUI").Link(Uri("https://github.com/HuxerUI/HuxerUI")),
    TextSpan(". No files were changed; this response was generated by the local demo script."),
  }, "\n\n"}}});
  if (!follow_up) {
    response.push_back({0, BlockKind::Question, {Paragraph(
        "Before continuing, choose your target platforms, response style, and project name.")}});
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

Task<void>
StreamParagraph(std::shared_ptr<StreamModel> model, TaskScope tasks, BlockKind kind, AttributedText response) {
  model->Append(kind, {{AttributedText{}, "\n\n"}}, true);
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
    co_await Delay(kind == BlockKind::Reasoning ? 50ms : 36ms);
    if (model->stop) {
      break;
    }
    // The mock transport delivers complete scalars; publish every two chunks instead of every character.
    for (unsigned count = 0; count < 3 && byte < response.PlainText().size(); ++count) {
      const auto first = static_cast<unsigned char>(response.PlainText()[byte]);
      byte += first < 0x80U ? 1 : first < 0xE0U ? 2 : first < 0xF0U ? 3 : 4;
      received += first < 0xF0U ? 1 : 2;
    }
    if (++chunks % 2 == 0) {
      flush();
    }
  }
  flush();
}

Task<void> GenerateResponse(std::shared_ptr<StreamModel> model, TaskScope tasks, ColorScheme colors, std::string prompt,
    bool follow_up) {
  model->code_color = colors.primary;
  try {
    for (const auto& block : ResponseContent(colors, prompt, follow_up)) {
      if (model->stop) {
        break;
      }
      const bool paragraph = block.kind == BlockKind::Reasoning || block.kind == BlockKind::Paragraph ||
          block.kind == BlockKind::Code;
      model->status = block.kind == BlockKind::Reasoning ? "Thinking..." : "Responding...";
      if (paragraph) {
        co_await StreamParagraph(model, tasks, block.kind, block.text[0].text);
      } else {
        co_await Delay(350ms);
        if (!model->stop) {
          model->Append(block.kind, block.text);
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
  model->Append(BlockKind::User, {Paragraph(prompt)});
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

const VectorAsset& ConceptImage() {
  static const auto image = VectorAsset::Create({640, 180}, [](VectorBuilder& builder) {
    auto rect = [&](Rect bounds, Color color, float radius = 8.0F) {
      builder.FillPath(Path::RoundedRect(bounds, CornerRadii{radius}), color);
    };
    rect({0, 0, 640, 180}, Color::Rgb(232, 236, 246), 16);
    rect({18, 18, 116, 144}, Color::Rgb(43, 49, 73));
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
  });
  return image;
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
  for (std::size_t row = 0; row < 3; ++row) {
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
      ).OnClick([model, id = row->id] { model->ToggleReasoning(id); }).Key("disclosure"),
      row->expanded ? DocumentText(model, row).With(FontSize(14.0F), Foreground(colors.on_surface_variant),
          Padding(EdgeInsets{.left = 12.0F})).Key("summary") : View{},
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  case BlockKind::Paragraph:
    content = DocumentText(model, row);
    break;
  case BlockKind::Image:
    content = Column {
      Image(ConceptImage()).Fit(ImageFit::Contain).With(
          Frame{.height = 180.0F},
          Semantics{.label = "Conversation concept with a sidebar, message, and action card"}),
      DocumentText(model, row).With(FontSize(13.0F), Foreground(colors.on_surface_variant)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  case BlockKind::List:
    content = Column {
      Row {
        DocumentText(model, row, 0).With(Frame{.width = 26.0F}),
        DocumentText(model, row, 1).With(Grow()),
      },
      Row {
        DocumentText(model, row, 2).With(Frame{.width = 26.0F}),
        DocumentText(model, row, 3).With(Grow()),
      },
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
  case BlockKind::Code:
    content = Column {
      Text("C++ / select text to copy", TextRole::Label),
      ScrollView(DocumentText(model, row)).ScrollAxis(Axis::Horizontal).With(ScrollBar()),
    }.With(Spacing(12.0F), Padding(14.0F), Background(colors.surface_container_low),
        CornerRadius(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
    break;
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
      Text("Send a message below. Watch the Agent build its reply with text, images, code, and questions."),
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
  auto draft = UseState(TextEditingValue::FromText("Help me build an Agent chat UI."));
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
