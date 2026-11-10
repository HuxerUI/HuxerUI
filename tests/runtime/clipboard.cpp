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

  bool WriteContent(ClipboardContent content) override {
    written_content = std::move(content);
    return write_succeeds;
  }

  Task<IoResult<ClipboardContent>> ReadContentAsync(detail::ClipboardKind kind) override {
    requested_kind = kind;
    if (html && kind == detail::ClipboardKind::RichText) {
      return detail::ClipboardResult(detail::ReadClipboardHtml(*html));
    }
    if (read_content) {
      return detail::ClipboardResult(IoResult<ClipboardContent>::Success(*read_content));
    }
    return detail::ClipboardResult(detail::ClipboardFailure(read_error));
  }

  bool MayHaveContent(detail::ClipboardKind kind) const noexcept override {
    return kind == detail::ClipboardKind::Text && text.has_value();
  }

  std::optional<std::string> text;
  std::optional<std::string> html;
  std::optional<ClipboardContent> read_content;
  std::optional<ClipboardContent> written_content;
  std::optional<detail::ClipboardKind> requested_kind;
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
  REQUIRE(clipboard_service->IsAvailable());
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
  REQUIRE_FALSE(clipboard_service->IsAvailable());
  REQUIRE_FALSE(clipboard_service->ReadText().has_value());
  REQUIRE_FALSE(clipboard_service->WriteText("text"));
  std::optional<bool> result;
  clipboard_tasks.Launch(Receive(clipboard_service->WriteContentAsync(std::string("text")), result));
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
    REQUIRE(clipboard_service->IsAvailable());
  }

  REQUIRE_FALSE(clipboard_service->IsAvailable());
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
  REQUIRE_FALSE(clipboard_service->IsAvailable());
  REQUIRE(first_service->IsAvailable());
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
  platform_clipboard.read_content = std::string{};
  clipboard_tasks.Launch(Receive(clipboard_service->ReadTextAsync(), text));
  platform.RunPlatformModuleTasks();
  REQUIRE(text);
  REQUIRE(text->Succeeded());
  CHECK(text->Value().empty());

  std::optional<IoResult<ImageAsset>> image;
  platform_clipboard.read_content.reset();
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
      "\xF0\x9F\x98\x80 Link\n", {{{0, 2}, {.font_weight = FontWeight::Bold}}},
      {{{3, 7}, Uri("https://example.test/?a=1&b=2")}});
  REQUIRE(clipboard_service->WriteContent(formatted));
  REQUIRE(platform_clipboard.written_content);
  CHECK(std::get<AttributedText>(*platform_clipboard.written_content) == formatted);

  std::optional<IoResult<AttributedText>> text;
  platform_clipboard.html = detail::ClipboardHtml(formatted);
  clipboard_tasks.Launch(Receive(clipboard_service->ReadRichTextAsync(), text));
  platform.RunPlatformModuleTasks();
  REQUIRE(text);
  REQUIRE(text->Succeeded());
  CHECK(text->Value() == formatted);

  platform_clipboard.html.reset();
  platform_clipboard.read_content = std::string("plain");
  clipboard_tasks.Launch(Receive(clipboard_service->ReadRichTextAsync(), text));
  platform.RunPlatformModuleTasks();
  REQUIRE(text);
  REQUIRE(text->Succeeded());
  CHECK(text->Value().PlainText() == "plain");
  CHECK(text->Value().StyleRanges().empty());
}

TEST_CASE("Clipboard validates typed publication before calling the platform") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  CHECK_THROWS_AS(clipboard_service->WriteContent(std::vector<File>{}), std::invalid_argument);
  CHECK_THROWS_AS(clipboard_service->WriteContent(ImageAsset{}), std::invalid_argument);
  CHECK_THROWS_AS(clipboard_service->WriteContentAsync(ImageAsset{}), std::invalid_argument);
  CHECK_FALSE(platform_clipboard.written_content.has_value());
}

TEST_CASE("Clipboard asynchronous publication starts immediately and reports platform acceptance") {
  ResetClipboardService();
  TestClipboard platform_clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  auto pending = clipboard_service->WriteContentAsync(std::string("copy"));
  REQUIRE(platform_clipboard.written_content);
  CHECK(std::get<std::string>(*platform_clipboard.written_content) == "copy");

  std::optional<bool> accepted;
  clipboard_tasks.Launch(Receive(std::move(pending), accepted));
  platform.RunPlatformModuleTasks();
  REQUIRE(accepted.has_value());
  CHECK(*accepted);

  platform_clipboard.write_succeeds = false;
  std::optional<bool> rejected;
  clipboard_tasks.Launch(Receive(clipboard_service->WriteContentAsync(std::string("copy")), rejected));
  platform.RunPlatformModuleTasks();
  REQUIRE(rejected.has_value());
  CHECK_FALSE(*rejected);
}

TEST_CASE("A queued clipboard read retires with its original Runtime") {
  ResetClipboardService();
  TestClipboard first_clipboard;
  first_clipboard.read_content = std::string("original");
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
  platform_clipboard.read_content = std::string("asynchronous");
  TestPlatform platform;
  platform.platform_clipboard = &platform_clipboard;
  UiWindow runtime(ClipboardApp, platform);
  runtime.BuildFrame();

  CHECK_FALSE(clipboard_service->IsAvailable());
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
