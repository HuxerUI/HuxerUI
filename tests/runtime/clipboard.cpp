#include "runtime_test_support.h"

#include "application/application_internal.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace huxerui::test {
namespace {

class TestClipboard final : public PlatformClipboard {
public:
  bool SupportsSynchronousText() const noexcept override { return synchronous_text; }

  std::optional<std::string> ReadText() override {
    ++read_count;
    return text;
  }

  bool WriteText(std::string_view value) override {
    ++write_count;
    text = std::string(value);
    return write_succeeds;
  }

  bool WriteItem(ClipboardItem item) override {
    written_item = std::move(item);
    return write_succeeds;
  }

  Task<IoResult<ClipboardItem>> ReadItemAsync(ClipboardReadOptions options) override {
    ++item_reads;
    if (changed) return detail::ClipboardResult(IoResult<ClipboardItem>::Failure(detail::ClipboardChangedError()));
    detail::ClipboardItemRead read;
    read.RecordError(detail::ClipboardError(read_error));
    if (options.text && item.text) read.SetText(&ClipboardItem::text, IoResult<std::string>::Success(*item.text));
    if (options.html && item.html) read.SetText(&ClipboardItem::html, IoResult<std::string>::Success(*item.html));
    for (const auto& mime : options.custom_mime_types) {
      if (const auto found = item.custom_formats.find(mime); found != item.custom_formats.end()) {
        read.SetCustom(mime, IoResult<Bytes>::Success(found->second));
      }
    }
    return detail::ClipboardResult(std::move(read).Finish());
  }

  bool WriteFiles(detail::ClipboardFiles files) override {
    written_files = std::move(files);
    return write_succeeds;
  }

  Task<IoResult<std::vector<FileReference>>> ReadFilesAsync() override {
    return detail::ClipboardResult(detail::ClipboardFailure<std::vector<FileReference>>(read_error));
  }

  bool MayHaveText() const noexcept override { return text.has_value(); }

  std::optional<std::string> text;
  ClipboardItem item;
  std::optional<ClipboardItem> written_item;
  std::optional<detail::ClipboardFiles> written_files;
  int item_reads = 0;
  bool changed = false;
  IoErrorCode read_error = IoErrorCode::NotFound;
  int read_count = 0;
  int write_count = 0;
  bool write_succeeds = true;
  bool synchronous_text = true;
};

std::shared_ptr<Clipboard> clipboard_service;
std::optional<ApplicationHandle> application_handle;
std::optional<ViewHandle> clipboard_field;
TaskScope clipboard_tasks;

View ClipboardApp() {
  application_handle = UseApplication();
  clipboard_service = application_handle->Clipboard();
  clipboard_tasks = UseTaskScope();
  return Text("Clipboard");
}

View ClipboardFieldApp() {
  clipboard_field = UseViewHandle();
  return TextField(TextEditingValue::FromText("")).With(*clipboard_field);
}

void ResetClipboardService() {
  application_handle.reset();
  clipboard_service.reset();
  clipboard_field.reset();
  clipboard_tasks = {};
}

template <class T> Task<void> Receive(Task<T> pending, std::optional<T>& result) {
  result.emplace(co_await std::move(pending));
}

} // namespace

static_assert(!std::is_copy_constructible_v<Clipboard>);
static_assert(!std::is_move_constructible_v<Clipboard>);

TEST_CASE("ApplicationProvidesStableClipboardOutsideComposition") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  platform_clipboard.text = "initial";
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  REQUIRE(clipboard_service);
  REQUIRE(application_handle->Clipboard() == clipboard_service);
  REQUIRE(clipboard_service->SupportsSynchronousText());
  REQUIRE(clipboard_service->ReadText() == std::optional<std::string>{"initial"});
  REQUIRE(platform_clipboard.read_count == 1);

  REQUIRE(clipboard_service->WriteText("updated"));
  REQUIRE(platform_clipboard.text == std::optional<std::string>{"updated"});
  REQUIRE(platform_clipboard.write_count == 1);
}

TEST_CASE("UnsupportedClipboardServiceReturnsUnavailableResults") {
  ResetClipboardService();
  TestPlatform platform;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  REQUIRE(clipboard_service);
  REQUIRE_FALSE(clipboard_service->SupportsSynchronousText());
  REQUIRE_FALSE(clipboard_service->ReadText().has_value());
  REQUIRE_FALSE(clipboard_service->WriteText("text"));
  std::optional<bool> result;
  clipboard_tasks.Launch(Receive(
      clipboard_service->WriteItemAsync({.text = "text", .html = {}, .image = {}, .custom_formats = {}}), result));
  platform.RunPlatformModuleTasks();
  REQUIRE(result.has_value());
  CHECK_FALSE(*result);
}

TEST_CASE("ClipboardRejectsInvalidUtf8BeforeCallingThePlatform") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  const std::string invalid_utf8{"\xC3\x28", 2};
  REQUIRE_THROWS_AS(clipboard_service->WriteText(invalid_utf8), std::invalid_argument);
  REQUIRE(platform_clipboard.write_count == 0);

  platform_clipboard.text = invalid_utf8;
  REQUIRE_FALSE(clipboard_service->ReadText().has_value());
  REQUIRE(platform_clipboard.read_count == 1);
}

TEST_CASE("ClipboardServiceDisconnectsWhenRuntimeIsDestroyed") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  {
    UiWindow runtime(ClipboardApp, platform);
    runtime.BuildFrame();
    REQUIRE(clipboard_service->SupportsSynchronousText());
  }

  REQUIRE_FALSE(clipboard_service->SupportsSynchronousText());
  REQUIRE_FALSE(clipboard_service->ReadText().has_value());
  REQUIRE_FALSE(clipboard_service->WriteText("ignored"));
  REQUIRE(application_handle->Clipboard() == clipboard_service);
  REQUIRE(platform_clipboard.read_count == 0);
  REQUIRE(platform_clipboard.write_count == 0);
}

TEST_CASE("ApplicationClipboardUsesItsOwningPlatformAndLifetime") {
  ResetClipboardService();
  TestClipboard first_clipboard;
  TestClipboard second_clipboard;
  TestPlatform first_platform;
  TestPlatform second_platform;
  first_platform.platform_clipboard = &first_clipboard;
  second_platform.platform_clipboard = &second_clipboard;
  UiWindow first_runtime(ClipboardApp, first_platform);
  first_runtime.BuildFrame();
  const auto first_service = clipboard_service;
  {
    UiWindow second_runtime(ClipboardApp, second_platform);
    second_runtime.BuildFrame();
    REQUIRE(clipboard_service != first_service);
    REQUIRE(first_service->WriteText("first"));
    REQUIRE(clipboard_service->WriteText("second"));
    REQUIRE(first_clipboard.text == std::optional<std::string>{"first"});
    REQUIRE(second_clipboard.text == std::optional<std::string>{"second"});
  }
  REQUIRE_FALSE(clipboard_service->SupportsSynchronousText());
  REQUIRE(first_service->SupportsSynchronousText());
  REQUIRE(first_service->ReadText() == std::optional<std::string>{"first"});
}

TEST_CASE("Typed clipboard reads distinguish absent content and denied access") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  std::optional<IoResult<std::string>> text;
  platform_clipboard.item.text = std::string{};
  clipboard_tasks.Launch(Receive(clipboard_service->ReadTextAsync(), text));
  platform.RunPlatformModuleTasks();
  REQUIRE(text);
  REQUIRE(text->Succeeded());
  CHECK(text->Value().empty());

  std::optional<IoResult<ImageAsset>> image;
  platform_clipboard.item.text.reset();
  clipboard_tasks.Launch(Receive(clipboard_service->ReadImageAsync(), image));
  platform.RunPlatformModuleTasks();
  REQUIRE(image);
  REQUIRE_FALSE(image->Succeeded());
  CHECK(image->Error().code == IoErrorCode::NotFound);

  std::optional<IoResult<std::vector<FileReference>>> files;
  platform_clipboard.read_error = IoErrorCode::PermissionDenied;
  clipboard_tasks.Launch(Receive(clipboard_service->ReadFilesAsync(), files));
  platform.RunPlatformModuleTasks();
  REQUIRE(files);
  REQUIRE_FALSE(files->Succeeded());
  CHECK(files->Error().code == IoErrorCode::PermissionDenied);
}

TEST_CASE("Clipboard rich text preserves character ranges and falls back to plain text") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  const auto formatted = AttributedText::FromRanges(
      "\xF0\x9F\x98\x80 Link\n", {{{0, 2}, {.font_size = 12.5F, .font_weight = FontWeight::Bold}}},
      {{{3, 7}, Uri("https://example.test/?a=1&b=2")}});
  auto item = ClipboardItem::FromAttributedText(formatted);
  REQUIRE(clipboard_service->WriteItem(item));
  REQUIRE(platform_clipboard.written_item);
  CHECK(platform_clipboard.written_item->text == formatted.PlainText());
  CHECK(platform_clipboard.written_item->html == item.html);

  std::optional<IoResult<ClipboardItem>> read;
  platform_clipboard.item = std::move(item);
  clipboard_tasks.Launch(Receive(clipboard_service->ReadItemAsync(), read));
  platform.RunPlatformModuleTasks();
  REQUIRE(read);
  REQUIRE(read->Succeeded());
  auto rich = read->Value().ToAttributedText();
  REQUIRE(rich.Succeeded());
  CHECK(rich.Value() == formatted);

  const ClipboardItem plain{.text = "plain", .html = "<span", .image = {}, .custom_formats = {}};
  rich = plain.ToAttributedText();
  REQUIRE(rich.Succeeded());
  CHECK(rich.Value().PlainText() == "plain");
  CHECK(rich.Value().StyleRanges().empty());
}

TEST_CASE("Clipboard HTML preserves supported font sizes and ignores invalid sizes") {
  const auto content = detail::ReadClipboardHtml(
      "<span style=\"font-size:9pt\">A</span><span style=\"font-size:1e-40px\">B</span>"
      "<span style=\"font-size:1000.00001px\">C</span><span style=\"font-size:12,5px\">D</span>"
      "<span style=\"font-size:12 px\">E</span><span style=\"font-size:0x1p3px\">F</span>"
      "<span style=\"font-size:+12px\">G</span><span style=\"font-size:1001px\">H</span>"
      "<span style=\"font-size:1e-46px\">I</span><span style=\"font-size:1e100px\">J</span>");
  REQUIRE(content.Succeeded());
  const auto expected = AttributedText::FromRanges(
      "ABCDEFGHIJ",
      {{{0, 1}, {.font_size = 12.0F}}, {{1, 2}, {.font_size = 1.0e-40F}}, {{2, 3}, {.font_size = 1000.0F}}});
  CHECK(content.Value() == expected);
}

TEST_CASE("Clipboard validates typed publication before calling the platform") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  CHECK_THROWS_AS(clipboard_service->WriteFiles(std::vector<File>{}), std::invalid_argument);
  CHECK_THROWS_AS(clipboard_service->WriteItem({}), std::invalid_argument);
  ClipboardItem item;
  item.image.emplace();
  CHECK_THROWS_AS(clipboard_service->WriteItemAsync(item), std::invalid_argument);
  item.image.reset();
  item.custom_formats.emplace("TEXT/HTML", Bytes{});
  CHECK_THROWS_AS(clipboard_service->WriteItem(item), std::invalid_argument);
  item.custom_formats.clear();
  item.custom_formats.emplace("Application/Vnd.Example.Selection", Bytes{});
  item.custom_formats.emplace("application/vnd.example.selection", Bytes{});
  CHECK_THROWS_AS(clipboard_service->WriteItem(item), std::invalid_argument);
  CHECK_FALSE(platform_clipboard.written_item);
  CHECK_FALSE(platform_clipboard.written_files);
}

TEST_CASE("Clipboard asynchronous publication starts immediately and reports platform acceptance") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  const ClipboardItem item{
      .text = "copy",
      .html = "<b>copy</b>",
      .image = {},
      .custom_formats = {{"Application/Vnd.Example.Selection", {std::byte{0}, std::byte{255}}},
                         {"application/vnd.example.empty", {}}},
  };
  auto pending = clipboard_service->WriteItemAsync(item);
  REQUIRE(platform_clipboard.written_item);
  CHECK(platform_clipboard.written_item->text == item.text);
  CHECK(platform_clipboard.written_item->html == item.html);
  CHECK(platform_clipboard.written_item->custom_formats.at("application/vnd.example.selection") ==
        item.custom_formats.at("Application/Vnd.Example.Selection"));
  CHECK(platform_clipboard.written_item->custom_formats.at("application/vnd.example.empty").empty());

  std::optional<bool> accepted;
  clipboard_tasks.Launch(Receive(std::move(pending), accepted));
  platform.RunPlatformModuleTasks();
  REQUIRE(accepted.has_value());
  CHECK(*accepted);

  platform_clipboard.write_succeeds = false;
  std::optional<bool> rejected;
  clipboard_tasks.Launch(Receive(clipboard_service->WriteItemAsync(item), rejected));
  platform.RunPlatformModuleTasks();
  REQUIRE(rejected.has_value());
  CHECK_FALSE(*rejected);
}

TEST_CASE("Clipboard returns readable representations from a single requested item", "[clipboard-item]") {
  ResetClipboardService();
  TestClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  const Bytes custom{std::byte{255}, std::byte{0}, std::byte{65}};
  clipboard.item.text = "";
  clipboard.item.html = "<table><tr><td>a</td><td>b</td></tr></table>";
  clipboard.item.custom_formats.emplace("application/vnd.example.selection", custom);
  clipboard.item.custom_formats.emplace("application/vnd.example.empty", Bytes{});
  const ClipboardReadOptions options{
      .custom_mime_types = {
          "Application/Vnd.Example.Selection", "application/vnd.example.empty", "application/vnd.example.missing"},
  };
  std::optional<IoResult<ClipboardItem>> read;
  clipboard_tasks.Launch(Receive(clipboard_service->ReadItemAsync(options), read));
  platform.RunPlatformModuleTasks();
  REQUIRE(clipboard.item_reads == 1);
  REQUIRE(read);
  REQUIRE(read->Succeeded());
  CHECK(read->Value().text == "");
  CHECK(read->Value().html == clipboard.item.html);
  CHECK(read->Value().custom_formats == clipboard.item.custom_formats);

  clipboard.item.html = std::string("bad\0html", 8);
  clipboard.item.custom_formats.clear();
  clipboard.read_error = IoErrorCode::TooLarge;
  clipboard_tasks.Launch(Receive(clipboard_service->ReadItemAsync(options), read));
  platform.RunPlatformModuleTasks();
  REQUIRE(read);
  REQUIRE(read->Succeeded());
  CHECK(read->Value().text == "");
  CHECK_FALSE(read->Value().html);
  CHECK(read->Value().custom_formats.empty());

  clipboard.changed = true;
  clipboard_tasks.Launch(Receive(clipboard_service->ReadItemAsync(options), read));
  platform.RunPlatformModuleTasks();
  REQUIRE(read);
  REQUIRE_FALSE(read->Succeeded());
  CHECK(read->Error() == detail::ClipboardChangedError());
}

TEST_CASE("A queued clipboard read retires with its original Runtime") {
  ResetClipboardService();
  TestClipboard first_clipboard;
  first_clipboard.item.text = "original";
  TestPlatform first_platform;
  first_platform.platform_clipboard = &first_clipboard;
  Task<IoResult<std::string>> pending;
  {
    UiWindow runtime(ClipboardApp, first_platform);
    runtime.BuildFrame();
    pending = clipboard_service->ReadTextAsync();
  }

  TestPlatform next_platform;
  UiWindow next(ClipboardApp, next_platform);
  next.BuildFrame();
  std::optional<IoResult<std::string>> result;
  clipboard_tasks.Launch(Receive(std::move(pending), result));
  next_platform.RunPlatformModuleTasks();
  REQUIRE(result);
  REQUIRE_FALSE(result->Succeeded());
  CHECK(result->Error().code == IoErrorCode::Unsupported);
}

TEST_CASE("An asynchronous clipboard remains readable without synchronous text support") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  platform_clipboard.synchronous_text = false;
  platform_clipboard.item.text = "asynchronous";
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  CHECK_FALSE(clipboard_service->SupportsSynchronousText());
  CHECK_FALSE(clipboard_service->ReadText());
  CHECK_FALSE(clipboard_service->WriteText("text"));
  CHECK(platform_clipboard.read_count == 0);
  CHECK(platform_clipboard.write_count == 0);
  std::optional<IoResult<std::string>> result;
  clipboard_tasks.Launch(Receive(clipboard_service->ReadTextAsync(), result));
  platform.RunPlatformModuleTasks();
  REQUIRE(result);
  REQUIRE(result->Succeeded());
  CHECK(result->Value() == "asynchronous");
}

TEST_CASE("Paste availability checks formats without reading the clipboard payload") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardFieldApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(clipboard_field);
  REQUIRE(clipboard_field->RequestFocus());

  CHECK_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Paste));
  platform_clipboard.text = "paste";
  CHECK(runtime.CanPerformTextEditingAction(TextEditingAction::Paste));
  platform_clipboard.synchronous_text = false;
  CHECK_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Paste));
  CHECK_FALSE(runtime.PerformTextEditingAction(TextEditingAction::Paste));
  for (const Key key : {Key::C, Key::X, Key::V}) {
    CHECK_FALSE(runtime.HandleKeyEvent({.key = key, .modifiers = {.control = true}}));
  }
  CHECK(platform_clipboard.read_count == 0);
}

} // namespace huxerui::test
