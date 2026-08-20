#include "runtime_test_support.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "file_internal.h"

namespace huxerui::test {

namespace {

std::vector<std::byte> Bytes(std::string_view value) {
  std::vector<std::byte> bytes;
  bytes.reserve(value.size());
  for (const char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  return bytes;
}

class TestFileReferenceState final : public detail::FileReferenceState {
public:
  explicit TestFileReferenceState(std::vector<std::byte> bytes) : bytes(std::move(bytes)) {}

  std::function<void()> ReadBytes(detail::FileReferenceBytesCompletion completion) override {
    ++read_count;
    completion(FileResult<std::vector<std::byte>>(bytes));
    return {};
  }

  std::function<void()>
  ImportTo(File destination, bool overwrite, detail::FileReferenceBoolCompletion completion) override {
    imported_to = std::move(destination);
    imported_with_overwrite = overwrite;
    completion(import_succeeds);
    return {};
  }

  std::function<void()> ReplaceWith(File source, detail::FileReferenceBoolCompletion completion) override {
    replaced_with = std::move(source);
    completion(replace_succeeds);
    return {};
  }

  std::vector<std::byte> bytes;
  int read_count = 0;
  std::optional<File> imported_to;
  bool imported_with_overwrite = false;
  bool import_succeeds = true;
  std::optional<File> replaced_with;
  bool replace_succeeds = true;
};

FileReference MakeReference(
    std::string name = "document.txt",
    bool can_write = true,
    std::shared_ptr<TestFileReferenceState> state = std::make_shared<TestFileReferenceState>(Bytes("content"))
) {
  return detail::MakeFileReference(
      {
          .name = std::move(name),
          .size = state->bytes.size(),
          .content_type = "text/plain",
          .can_write = can_write,
      },
      std::move(state)
  );
}

class ManualFilePickerTransport final : public detail::FilePickerTransport {
public:
  struct OpenCall {
    FilePickerFilter filter;
    bool multiple = false;
    detail::FilePickerOpenCompletion completion;
    std::shared_ptr<std::atomic<bool>> canceled;
  };

  struct SaveCall {
    File source;
    SaveFileOptions options;
    detail::FilePickerSaveCompletion completion;
    std::shared_ptr<std::atomic<bool>> canceled;
  };

  [[nodiscard]] bool CanOpenFiles() const noexcept override {
    return can_open_files;
  }

  [[nodiscard]] bool CanSaveFiles() const noexcept override {
    return can_save_files;
  }

  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, detail::FilePickerOpenCompletion completion) override {
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    open_calls.push_back({std::move(filter), multiple, std::move(completion), canceled});
    return [canceled] { *canceled = true; };
  }

  std::function<void()>
  SaveFile(File source, SaveFileOptions options, detail::FilePickerSaveCompletion completion) override {
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    save_calls.push_back({std::move(source), std::move(options), std::move(completion), canceled});
    return [canceled] { *canceled = true; };
  }

  void CompleteOpen(std::size_t index, std::vector<FileReference> references) {
    open_calls.at(index).completion(std::move(references));
  }

  void CompleteSave(std::size_t index, bool succeeded) {
    save_calls.at(index).completion(succeeded);
  }

  bool can_open_files = true;
  bool can_save_files = true;
  std::vector<OpenCall> open_calls;
  std::vector<SaveCall> save_calls;
};

class FilePickerTestPlatform final : public TestPlatform {
public:
  std::shared_ptr<ManualFilePickerTransport> transport = std::make_shared<ManualFilePickerTransport>();

protected:
  std::shared_ptr<detail::FilePickerTransport> CreateFilePickerTransport() override {
    return transport;
  }
};

std::shared_ptr<FilePicker> file_picker;
TaskScope file_picker_tasks;
std::optional<FileReference> opened_reference;
std::vector<FileReference> opened_references;
std::optional<FileResult<std::string>> reference_text;
bool imported_reference = false;
bool replaced_reference = false;
bool saved_file = false;
bool picker_task_completed = false;
std::thread::id picker_resume_thread;

View FilePickerApp() {
  file_picker = UseService<FilePicker>();
  file_picker_tasks = UseTaskScope();
  return Text("File picker");
}

void ResetFilePickerState() {
  file_picker.reset();
  file_picker_tasks = {};
  opened_reference.reset();
  opened_references.clear();
  reference_text.reset();
  imported_reference = false;
  replaced_reference = false;
  saved_file = false;
  picker_task_completed = false;
  picker_resume_thread = {};
}

} // namespace

static_assert(!std::is_default_constructible_v<FileReference>);
static_assert(std::is_copy_constructible_v<FileReference>);
static_assert(!std::is_copy_constructible_v<FilePicker>);

TEST_CASE("FileReferenceRetainsItsGrantAndProvidesMetadataAndOperations") {
  ResetFilePickerState();
  TestPlatform platform;
  Runtime runtime(FilePickerApp, platform);
  runtime.BuildFrame();

  auto state = std::make_shared<TestFileReferenceState>(Bytes("\xEF\xBB\xBFhello"));
  std::weak_ptr<TestFileReferenceState> weak_state = state;
  FileReference reference = MakeReference("report.txt", true, state);
  FileReference copy = reference;
  state.reset();

  REQUIRE(reference.Name() == "report.txt");
  REQUIRE(reference.Size() == 8);
  REQUIRE(reference.ContentType() == "text/plain");
  REQUIRE(reference.CanWrite());
  REQUIRE_FALSE(weak_state.expired());

  file_picker_tasks.Launch([reference, copy]() -> Task<void> {
    reference_text = co_await reference.ReadStringAsync();
    imported_reference = co_await copy.ImportToAsync(File("/tmp/imported.txt"), true);
    replaced_reference = co_await reference.ReplaceWithAsync(File("/tmp/replacement.txt"));
  });
  platform.RunPlatformModuleTasks();

  REQUIRE(reference_text.has_value());
  REQUIRE(reference_text->Succeeded());
  REQUIRE(reference_text->Value() == "hello");
  REQUIRE(imported_reference);
  REQUIRE(replaced_reference);
  const std::shared_ptr<TestFileReferenceState> retained_state = weak_state.lock();
  REQUIRE(retained_state);
  REQUIRE(retained_state->read_count == 1);
  REQUIRE(retained_state->imported_to == File("/tmp/imported.txt"));
  REQUIRE(retained_state->imported_with_overwrite);
  REQUIRE(retained_state->replaced_with == File("/tmp/replacement.txt"));
}

TEST_CASE("ReadOnlyFileReferenceRejectsReplacementWithoutCallingThePlatform") {
  ResetFilePickerState();
  TestPlatform platform;
  Runtime runtime(FilePickerApp, platform);
  runtime.BuildFrame();

  auto state = std::make_shared<TestFileReferenceState>(Bytes("read only"));
  FileReference reference = MakeReference("readonly.txt", false, state);
  file_picker_tasks.Launch([reference]() -> Task<void> {
    replaced_reference = co_await reference.ReplaceWithAsync(File("/tmp/replacement.txt"));
    picker_task_completed = true;
  });
  platform.RunPlatformModuleTasks();

  REQUIRE(picker_task_completed);
  REQUIRE_FALSE(replaced_reference);
  REQUIRE_FALSE(state->replaced_with.has_value());
}

TEST_CASE("RuntimeInstallsAnUnsupportedFilePickerService") {
  ResetFilePickerState();
  TestPlatform platform;
  Runtime runtime(FilePickerApp, platform);
  runtime.BuildFrame();

  REQUIRE(file_picker);
  REQUIRE_FALSE(file_picker->CanOpenFiles());
  REQUIRE_FALSE(file_picker->CanSaveFiles());

  file_picker_tasks.Launch([picker = file_picker]() -> Task<void> {
    opened_reference = co_await picker->OpenFileAsync();
    opened_references = co_await picker->OpenFilesAsync();
    saved_file = co_await picker->SaveFileAsync(File("/tmp/source.txt"));
    picker_task_completed = true;
  });
  platform.RunPlatformModuleTasks();

  REQUIRE(picker_task_completed);
  REQUIRE_FALSE(opened_reference.has_value());
  REQUIRE(opened_references.empty());
  REQUIRE_FALSE(saved_file);
}

TEST_CASE("FilePickerValidatesPortableFiltersAndSuggestedNames") {
  ResetFilePickerState();
  FilePickerTestPlatform platform;
  Runtime runtime(FilePickerApp, platform);
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(file_picker->OpenFileAsync({.extensions = {"txt"}}), std::invalid_argument);
  REQUIRE_THROWS_AS(file_picker->OpenFileAsync({.name = "Text", .extensions = {".txt"}}), std::invalid_argument);
  REQUIRE_THROWS_AS(file_picker->OpenFileAsync({.name = "Text", .extensions = {"t;xt"}}), std::invalid_argument);
  REQUIRE_THROWS_AS(file_picker->OpenFileAsync({.name = "Text", .extensions = {"t*xt"}}), std::invalid_argument);
  REQUIRE_THROWS_AS(file_picker->OpenFileAsync({.name = "Text", .extensions = {"t?xt"}}), std::invalid_argument);
  REQUIRE_THROWS_AS(file_picker->OpenFileAsync({.name = "Text", .content_types = {"text"}}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      file_picker->OpenFileAsync({.name = "Text", .content_types = {"te*xt/plain"}}),
      std::invalid_argument
  );
  REQUIRE_NOTHROW(static_cast<void>(
      file_picker->OpenFileAsync({.name = "Text", .content_types = {"application/x.test~json", "text/*"}})
  ));
  REQUIRE_NOTHROW(static_cast<void>(file_picker->OpenFileAsync({.name = "Any", .content_types = {"*/*"}})));
  REQUIRE_THROWS_AS(
      detail::MakeFileReference(
          {.name = "document.txt", .content_type = "text/*"},
          std::make_shared<TestFileReferenceState>(Bytes("content"))
      ),
      std::logic_error
  );
  REQUIRE_THROWS_AS(
      file_picker->SaveFileAsync(File("/tmp/source.txt"), {.suggested_name = "nested/file.txt"}),
      std::invalid_argument
  );
  REQUIRE(platform.transport->open_calls.empty());
  REQUIRE(platform.transport->save_calls.empty());
}

TEST_CASE("FilePickerSerializesPlatformPresentationAndResumesOnTheUIThread") {
  ResetFilePickerState();
  FilePickerTestPlatform platform;
  Runtime runtime(FilePickerApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();

  file_picker_tasks.Launch([picker = file_picker]() -> Task<void> {
    opened_reference = co_await picker->OpenFileAsync({
        .name = "Text",
        .extensions = {"txt"},
        .content_types = {"text/plain"},
    });
    picker_resume_thread = std::this_thread::get_id();
  });
  file_picker_tasks.Launch([picker = file_picker]() -> Task<void> {
    saved_file = co_await picker->SaveFileAsync(
        File("/tmp/source.txt"),
        {
            .suggested_name = "export.txt",
            .filter = {.name = "Text", .extensions = {"txt"}},
        }
    );
  });
  platform.RunPlatformModuleTasks();

  REQUIRE(platform.transport->open_calls.size() == 1);
  REQUIRE_FALSE(platform.transport->open_calls.front().multiple);
  REQUIRE(platform.transport->open_calls.front().filter.extensions == std::vector<std::string>{"txt"});
  REQUIRE(platform.transport->save_calls.empty());

  std::thread completion([&platform] { platform.transport->CompleteOpen(0, {MakeReference("selected.txt")}); });
  completion.join();
  REQUIRE_FALSE(opened_reference.has_value());
  platform.RunPlatformModuleTasks();

  REQUIRE(opened_reference.has_value());
  REQUIRE(opened_reference->Name() == "selected.txt");
  REQUIRE(picker_resume_thread == ui_thread);
  REQUIRE(platform.transport->save_calls.size() == 1);
  REQUIRE(platform.transport->save_calls.front().source == File("/tmp/source.txt"));
  REQUIRE(platform.transport->save_calls.front().options.suggested_name == "export.txt");

  platform.transport->CompleteSave(0, true);
  platform.RunPlatformModuleTasks();
  REQUIRE(saved_file);
}

TEST_CASE("CancelingFilePickerRequestsPreservesPlatformPresentationOrder") {
  ResetFilePickerState();
  FilePickerTestPlatform platform;
  Runtime runtime(FilePickerApp, platform);
  runtime.BuildFrame();

  TaskHandle active = file_picker_tasks.Launch([picker = file_picker]() -> Task<void> {
    opened_reference = co_await picker->OpenFileAsync();
  });
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->open_calls.size() == 1);

  TaskHandle queued = file_picker_tasks.Launch([picker = file_picker]() -> Task<void> {
    saved_file = co_await picker->SaveFileAsync(File("/tmp/skipped.txt"));
  });
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->save_calls.empty());

  active.Cancel();
  queued.Cancel();
  REQUIRE(*platform.transport->open_calls.front().canceled);
  REQUIRE(platform.transport->save_calls.empty());

  file_picker_tasks.Launch([picker = file_picker]() -> Task<void> {
    opened_references = co_await picker->OpenFilesAsync();
    picker_task_completed = true;
  });
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->open_calls.size() == 1);

  platform.transport->CompleteOpen(0, {});
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.transport->open_calls.size() == 2);
  REQUIRE(platform.transport->open_calls.back().multiple);
  REQUIRE_FALSE(picker_task_completed);

  platform.transport->CompleteOpen(1, {MakeReference("first.txt"), MakeReference("second.txt")});
  platform.RunPlatformModuleTasks();
  REQUIRE(picker_task_completed);
  REQUIRE(opened_references.size() == 2);
  REQUIRE(opened_references[0].Name() == "first.txt");
  REQUIRE(opened_references[1].Name() == "second.txt");
}

} // namespace huxerui::test
