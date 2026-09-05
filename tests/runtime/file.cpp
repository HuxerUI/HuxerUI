#include "runtime_test_support.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#undef CreateDirectory
#undef CopyFile
#undef DeleteFile
#undef MoveFile
#endif

#include "io/file_internal.h"

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

std::string Utf8Path(const fs::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence = 0;
    path_ = fs::temp_directory_path() / ("huxerui-runtime-file-tests-" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         "-" + std::to_string(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] detail::FileSystemPaths Paths() const {
    return {
        .executable_directory = Utf8Path(path_),
        .data_directory = Utf8Path(path_ / "data"),
        .cache_directory = Utf8Path(path_ / "cache"),
        .temporary_directory = Utf8Path(path_ / "temporary"),
    };
  }

private:
  fs::path path_;
};

#if defined(_WIN32)
// Mount-point reparse data does not require the symbolic-link privilege. Keep its original object
// open so teardown removes only this test's tag, never entries reached through the junction.
class TemporaryJunction final {
public:
  TemporaryJunction(const File& directory, const File& target) {
    const HANDLE handle = CreateFileW(fs::u8path(directory.Path()).c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    handle_.reset(handle);
    struct MountPoint {
      DWORD tag;
      WORD data_length;
      WORD reserved;
      WORD substitute_offset;
      WORD substitute_length;
      WORD print_offset;
      WORD print_length;
      wchar_t path[1];
    };
    const std::wstring name = L"\\??\\" + fs::u8path(target.Path()).native();
    const std::size_t name_bytes = name.size() * sizeof(wchar_t);
    const std::size_t size = offsetof(MountPoint, path) + name_bytes + 2 * sizeof(wchar_t);
    REQUIRE(size <= MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    std::vector<std::byte> buffer(size);
    auto* data = reinterpret_cast<MountPoint*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->data_length = static_cast<WORD>(size - offsetof(MountPoint, substitute_offset));
    data->substitute_length = static_cast<WORD>(name_bytes);
    data->print_offset = static_cast<WORD>(name_bytes + sizeof(wchar_t));
    std::memcpy(data->path, name.data(), name_bytes);
    DWORD returned = 0;
    REQUIRE(DeviceIoControl(handle_.get(), FSCTL_SET_REPARSE_POINT, data, static_cast<DWORD>(size),
                            nullptr, 0, &returned, nullptr));
  }

  ~TemporaryJunction() {
    struct {
      DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
      WORD data_length = 0;
      WORD reserved = 0;
    } data;
    DWORD returned = 0;
    DeviceIoControl(handle_.get(), FSCTL_DELETE_REPARSE_POINT, &data, sizeof(data), nullptr, 0, &returned, nullptr);
  }

private:
  std::unique_ptr<void, decltype(&CloseHandle)> handle_{nullptr, CloseHandle};
};
#endif

struct TaskQueue {
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::function<void()>> tasks;
};

class FileTestPlatform final : public TestPlatform {
public:
  explicit FileTestPlatform(detail::FileSystemPaths paths)
      : FileTestPlatform(std::make_shared<TaskQueue>(), std::move(paths)) {}

  void RunUntil(const std::function<bool()>& complete) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!complete()) {
      std::function<void()> task;
      {
        std::unique_lock lock(queue_->mutex);
        const bool ready = queue_->condition.wait_until(lock, deadline, [this] { return !queue_->tasks.empty(); });
        REQUIRE(ready);
        task = std::move(queue_->tasks.front());
        queue_->tasks.pop_front();
      }
      task();
    }
  }

  void RunOne() {
    RunUntil([this] {
      std::scoped_lock lock(queue_->mutex);
      return !queue_->tasks.empty();
    });
    std::function<void()> task;
    {
      std::scoped_lock lock(queue_->mutex);
      task = std::move(queue_->tasks.front());
      queue_->tasks.pop_front();
    }
    task();
  }

protected:
  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return detail::MakeFileSystem(paths_);
  }

private:
  FileTestPlatform(std::shared_ptr<TaskQueue> queue, detail::FileSystemPaths paths)
      : TestPlatform([queue](std::function<void()> task) {
          {
            std::scoped_lock lock(queue->mutex);
            queue->tasks.push_back(std::move(task));
          }
          queue->condition.notify_one();
        }),
        queue_(std::move(queue)), paths_(std::move(paths)) {}

  std::shared_ptr<TaskQueue> queue_;
  detail::FileSystemPaths paths_;
};

class ProviderReferenceState final : public detail::FileReferenceState {
public:
  explicit ProviderReferenceState(std::string identity) : identity_(std::move(identity)) {}

  std::string Identity() const override { return identity_; }

  std::function<void()> ReadBytes(detail::FileReferenceBytesCompletion completion) override {
    completion(FileResult<Bytes>(FileError{FileErrorCode::Unsupported, "HuxerUI test source requires streaming"}));
    return {};
  }

  std::function<void()> ReplaceWith(File, detail::FileReferenceBoolCompletion completion) override {
    completion(false);
    return {};
  }

  std::function<void()> ImportTo(File destination, bool,
                                 detail::FileReferenceCompletion<std::uint64_t> completion) override {
    imported_to = std::move(destination);
    imported = std::move(completion);
    return [this] { canceled = true; };
  }

  std::function<void()> ListChildren(detail::FileReferenceCompletion<std::vector<FileReference>> completion) override {
    ++list_count;
    if (list_error) {
      completion(FileResult<std::vector<FileReference>>(
          FileError{*list_error, "HuxerUI test directory enumeration failed"}));
      return {};
    }
    completion(FileResult<std::vector<FileReference>>(children));
    return {};
  }

  bool NeedsChildListingForLookup() const noexcept override { return listing_lookup; }

  std::function<void()> FindChild(std::string name,
                                  detail::FileReferenceCompletion<std::optional<FileReference>> completion) override {
    ++find_count;
    std::optional<FileReference> found;
    for (const auto& child : children) {
      if (child.Name() == name) {
        if (found) {
          completion(FileResult<std::optional<FileReference>>(
              FileError{FileErrorCode::AlreadyExists, "HuxerUI test lookup is ambiguous"}));
          return {};
        }
        found = child;
      }
    }
    completion(FileResult<std::optional<FileReference>>(std::move(found)));
    return {};
  }

  std::function<void()> CopyFileFrom(detail::FileReferenceSource, std::string name, bool,
                                     std::optional<FileReference> existing,
                                     detail::FileReferenceCompletion<detail::FileReferenceWriteResult> completion) override {
    if (write_error) {
      completion(FileResult<detail::FileReferenceWriteResult>(
          FileError{*write_error, "HuxerUI test provider rejected the write"}));
      return {};
    }
    ++write_count;
    if (existing) {
      CHECK(existing->Name() == name);
      completion(FileResult<detail::FileReferenceWriteResult>({*existing, 1, false}));
    } else {
      auto state = std::make_shared<ProviderReferenceState>(identity_ + "/" + name);
      children.push_back(detail::MakeFileReference({.name = name, .can_write = true}, std::move(state)));
      completion(FileResult<detail::FileReferenceWriteResult>({children.back(), 1, true}));
    }
    return {};
  }

  std::function<void()> CheckCopyDestination(detail::FileReferenceSource,
                                             detail::FileReferenceCompletion<bool> completion) override {
    completion(FileResult<bool>(true));
    return {};
  }

  std::vector<FileReference> children;
  std::optional<File> imported_to;
  detail::FileReferenceCompletion<std::uint64_t> imported;
  bool canceled = false;
  bool listing_lookup = false;
  std::size_t list_count = 0;
  std::size_t find_count = 0;
  std::size_t write_count = 0;
  std::optional<FileErrorCode> list_error;
  std::optional<FileErrorCode> write_error;

private:
  std::string identity_;
};

std::shared_ptr<FileSystem> file_system;
TaskScope file_tasks;
std::string async_text;
Bytes async_bytes;
std::thread::id file_resume_thread;
bool file_task_complete = false;
bool canceled_file_task_continued = false;

View FileApp() {
  file_system = UseService<FileSystem>();
  file_tasks = UseTaskScope();
  return Text("Files");
}

void ResetFileState() {
  file_system.reset();
  file_tasks = {};
  async_text.clear();
  async_bytes.clear();
  file_resume_thread = {};
  file_task_complete = false;
  canceled_file_task_continued = false;
}

} // namespace

TEST_CASE("FileReferencesExposePathValuesWithoutTransferringTheirAccessLifetime") {
  TemporaryDirectory temporary;
  const File root(temporary.Paths().temporary_directory);
  REQUIRE(root.CreateDirectory());
  const File selected = root.Child("工程");
  bool directory = false;
  SECTION("A selected file has a local path") { REQUIRE(selected.WriteString("initial")); }
  SECTION("A selected directory has a local path") {
    directory = true;
    REQUIRE(selected.CreateDirectory());
  }
  std::optional<File> path;
  std::weak_ptr<int> weak_access;
  {
    auto access = std::make_shared<int>(0);
    weak_access = access;
    const FileReference reference = detail::MakeLocalFileReference(selected, false, {},
        [access](const File*, const File*, const std::function<void()>& operation) {
          ++*access;
          operation();
        });
    FileReference copy = reference;
    FileReference moved = std::move(copy);
    REQUIRE_FALSE(copy.AsFile().has_value());
    path = moved.AsFile();
    REQUIRE(path.has_value());
    REQUIRE(reference.AsFile() == path);
    REQUIRE(path->Name() == selected.Name());
    REQUIRE_FALSE(reference.CanWrite());
    const File output = directory ? path->Child("settings.json") : *path;
    REQUIRE(output.WriteString("{}"));
    REQUIRE((directory ? selected.Child("settings.json") : selected).ReadString().Value() == "{}");
    REQUIRE_FALSE(reference.CanWrite());
    REQUIRE(*access == 1);
    REQUIRE_FALSE(weak_access.expired());

    REQUIRE(selected.MoveTo(root.Child("moved")));
    REQUIRE(reference.AsFile() == path);
    REQUIRE_FALSE(path->Exists());
    REQUIRE(*access == 1);
  }
  REQUIRE(weak_access.expired());
  REQUIRE(path.has_value());
}

TEST_CASE("RuntimeInstallsFileSystemAndFileAsyncOperationsResumeOnTheUIThread") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  REQUIRE(file_system);
  REQUIRE(file_system->Directories().data_directory.IsDirectory());
  REQUIRE(file_system->Directories().cache_directory.IsDirectory());
  REQUIRE(file_system->Directories().temporary_directory.IsDirectory());

  const std::thread::id ui_thread = std::this_thread::get_id();
  File file = file_system->Directories().data_directory.Child("async.txt");
  file_tasks.Launch([file]() -> Task<void> {
    if (!co_await file.WriteStringAsync("async value")) {
      file_task_complete = true;
      co_return;
    }
    FileResult<std::string> result = co_await file.ReadStringAsync();
    if (result.Succeeded()) {
      async_text = std::move(result).Value();
    }
    file_resume_thread = std::this_thread::get_id();
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(async_text == "async value");
  REQUIRE(file_resume_thread == ui_thread);
}

TEST_CASE("FileAsyncByteOperationsRetainOwnedBinaryDataUntilCompletion") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  File file = file_system->Directories().data_directory.Child("async.bin");
  file_tasks.Launch([file]() -> Task<void> {
    if (!co_await file.WriteBytesAsync(Bytes{std::byte{0}, std::byte{0xFF}}) ||
        !co_await file.AppendBytesAsync(Bytes{std::byte{'a'}, std::byte{0}})) {
      file_task_complete = true;
      co_return;
    }
    FileResult<Bytes> result = co_await file.ReadBytesAsync();
    if (result.Succeeded()) {
      async_bytes = std::move(result).Value();
    }
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE((async_bytes == Bytes{std::byte{0}, std::byte{0xFF}, std::byte{'a'}, std::byte{0}}));
}

TEST_CASE("CancelingAFileTaskDropsItsContinuation") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  File file = file_system->Directories().temporary_directory.Child("canceled.txt");
  TaskHandle handle = file_tasks.Launch([file]() -> Task<void> {
    static_cast<void>(co_await file.WriteStringAsync(std::string(1024 * 1024, 'x')));
    canceled_file_task_continued = true;
  });
  platform.RunOne();
  handle.Cancel();

  REQUIRE_FALSE(canceled_file_task_continued);
}

TEST_CASE("DirectoryReferencesEnumerateRepeatedlyAndCopyBothDestinationKinds") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File source = root.Child("source");
  const File local = root.Child("local");
  const File external = root.Child("external");
  REQUIRE(source.Child("nested").Child("empty").CreateDirectories());
  REQUIRE(local.CreateDirectory());
  REQUIRE(external.CreateDirectory());
  REQUIRE(source.Child(".hidden").WriteString("hidden"));
  REQUIRE(source.Child("nested").Child("文本.bin").WriteBytes(Bytes{std::byte{0}, std::byte{255}}));
  REQUIRE(local.Child("untouched").WriteString("keep"));
  auto reference = detail::MakeLocalFileReference(source, false);
  auto target = detail::MakeLocalFileReference(external, true);
  REQUIRE(reference.Type() == FileType::Directory);
  REQUIRE_FALSE(reference.CanWrite());
  REQUIRE_FALSE(reference.Size().has_value());
  REQUIRE_FALSE(reference.ContentType().has_value());
  std::vector<std::size_t> enumerations;
  std::vector<DirectoryCopySummary> summaries;
  std::optional<FileError> failure;
  file_tasks.Launch([&]() -> Task<void> {
    for (int index = 0; index < 2; ++index) {
      auto children = co_await reference.ListChildrenAsync();
      if (children.Succeeded()) { enumerations.push_back(children.Value().size()); }
      else { failure = children.Error(); }
    }
    auto first = co_await reference.CopyDirectoryContentsToAsync(local);
    if (first.Succeeded()) { summaries.push_back(first.Value()); } else { failure = first.Error(); }
    auto second = co_await reference.CopyDirectoryContentsToAsync(target);
    if (second.Succeeded()) { summaries.push_back(second.Value()); } else { failure = second.Error(); }
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE_FALSE(failure.has_value());
  REQUIRE((enumerations == std::vector<std::size_t>{2, 2}));
  REQUIRE(summaries.size() == 2);
  REQUIRE(summaries[0] == summaries[1]);
  REQUIRE(summaries[0].files_copied == 2);
  REQUIRE(summaries[0].directories_created == 2);
  REQUIRE(summaries[0].bytes_copied == 8);
  REQUIRE(local.Child("untouched").ReadString().Value() == "keep");
  for (const File& output : {local, external}) {
    REQUIRE(output.Child("nested").Child("empty").IsDirectory());
    REQUIRE(output.Child(".hidden").ReadString().Value() == "hidden");
    REQUIRE(output.Child("nested").Child("文本.bin").ReadBytes().Value() ==
            source.Child("nested").Child("文本.bin").ReadBytes().Value());
  }
}

TEST_CASE("DirectoryReferencesFinalizeProviderImportsAndPreserveFailureAndCancellation") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File destination = file_system->Directories().temporary_directory.Child("destination");
  REQUIRE(destination.CreateDirectory());
  auto input = std::make_shared<ProviderReferenceState>("provider:file");
  auto directory = std::make_shared<ProviderReferenceState>("provider:directory");
  directory->children.push_back(detail::MakeFileReference({.name = "value.bin", .size = 1000}, input));
  auto source = detail::MakeFileReference({.name = "source", .type = FileType::Directory}, directory);
  std::optional<FileResult<DirectoryCopySummary>> result;
#if defined(_WIN32)
  // The native Windows destination cannot delegate its retained authority to a path-only provider.
  file_tasks.Launch([&]() -> Task<void> {
    result = co_await source.CopyDirectoryContentsToAsync(destination);
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->Succeeded());
  REQUIRE(result->Error().code == FileErrorCode::Unsupported);
  REQUIRE_FALSE(input->imported_to.has_value());
  REQUIRE_FALSE(destination.Child("value.bin").Exists());
#else
  bool write_output = true;
  bool cancel = false;
  std::optional<FileErrorCode> expected_error;
  SECTION("Actual transferred bytes are counted instead of source metadata") {}
  SECTION("The provider error is preserved") {
    expected_error = FileErrorCode::PermissionDenied;
  }
  SECTION("A missing finalized output preserves the metadata error") {
    write_output = false;
    expected_error = FileErrorCode::NotFound;
  }
  SECTION("Cancellation suppresses late provider completion") {
    cancel = true;
    write_output = false;
  }
  auto task = file_tasks.Launch([&]() -> Task<void> {
    result = co_await source.CopyDirectoryContentsToAsync(destination);
    file_task_complete = true;
  });
  platform.RunUntil([&] { return static_cast<bool>(input->imported); });
  REQUIRE(input->imported_to.has_value());
  REQUIRE(input->imported_to->Name() == "value.bin");
  REQUIRE(fs::equivalent(input->imported_to->Parent()->Path(), destination.Path()));
  if (cancel) {
    task.Cancel();
    REQUIRE(input->canceled);
  }
  if (write_output) {
    REQUIRE(input->imported_to->WriteBytes(Bytes{std::byte{0}, std::byte{255}, std::byte{42}}));
  }
  auto complete = std::move(input->imported);
  if (expected_error == FileErrorCode::PermissionDenied) {
    complete(FileResult<std::uint64_t>(FileError{*expected_error, "HuxerUI provider secret URI"}));
  } else {
    complete(FileResult<std::uint64_t>(3));
  }
  if (cancel) {
    bool drained = false;
    file_tasks.Launch([&]() -> Task<void> {
      static_cast<void>(co_await destination.StatAsync());
      drained = true;
    });
    platform.RunUntil([&] { return drained; });
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(file_task_complete);
    REQUIRE_FALSE(destination.Child("value.bin").Exists());
  } else {
    platform.RunUntil([] { return file_task_complete; });
    REQUIRE(result.has_value());
    if (expected_error) {
      REQUIRE_FALSE(result->Succeeded());
      REQUIRE(result->Error().code == *expected_error);
      REQUIRE(result->Error().message == "HuxerUI directory copy entry transfer failed at \"value.bin\"");
    } else {
      REQUIRE(result->Succeeded());
      REQUIRE(result->Value().files_copied == 1);
      REQUIRE(result->Value().bytes_copied == 3);
      REQUIRE(destination.Child("value.bin").ReadBytes().Value() ==
              Bytes{std::byte{0}, std::byte{255}, std::byte{42}});
    }
  }
#endif
}

TEST_CASE("DirectoryCopiesIndexListingBasedDestinationsOnlyWithinOneCopy") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  auto input = std::make_shared<ProviderReferenceState>("provider:source");
  auto output = std::make_shared<ProviderReferenceState>("provider:destination");
  output->listing_lookup = true;
  for (std::size_t index = 0; index < 1000; ++index) {
    const std::string name = std::to_string(index) + ".txt";
    input->children.push_back(detail::MakeFileReference(
        {.name = name}, std::make_shared<ProviderReferenceState>("provider:source/" + name)));
  }
  auto source = detail::MakeFileReference({.name = "source", .type = FileType::Directory}, input);
  auto destination = detail::MakeFileReference(
      {.name = "destination", .can_write = true, .type = FileType::Directory}, output);
  std::optional<FileErrorCode> expected_error;
  SECTION("Listing lookup is indexed and rebuilt for each copy") {}
  SECTION("Native lookup is not replaced by a display-name index") {
    output->listing_lookup = false;
  }
  SECTION("Unrelated duplicate names do not block the copy") {
    auto unrelated = detail::MakeFileReference(
        {.name = "unrelated.txt"}, std::make_shared<ProviderReferenceState>("provider:unrelated"));
    output->children = {unrelated, unrelated};
  }
  SECTION("An addressed duplicate is rejected before writing") {
    auto duplicate = detail::MakeFileReference(
        {.name = "0.txt"}, std::make_shared<ProviderReferenceState>("provider:duplicate"));
    output->children = {duplicate, duplicate};
    expected_error = FileErrorCode::AlreadyExists;
  }
  SECTION("Enumeration failure is not treated as an empty directory") {
    output->list_error = FileErrorCode::PermissionDenied;
    expected_error = FileErrorCode::PermissionDenied;
  }
  SECTION("An index does not bypass final write authorization") {
    output->write_error = FileErrorCode::PermissionDenied;
    expected_error = FileErrorCode::PermissionDenied;
  }
  std::optional<FileResult<DirectoryCopySummary>> result;
  const auto copy = [&](bool overwrite) {
    file_task_complete = false;
    file_tasks.Launch([&, overwrite]() -> Task<void> {
      result = co_await source.CopyDirectoryContentsToAsync(destination, overwrite);
      file_task_complete = true;
    });
    platform.RunUntil([] { return file_task_complete; });
    REQUIRE(result.has_value());
  };
  copy(false);
  REQUIRE(output->list_count == (output->listing_lookup ? 1 : 0));
  REQUIRE(output->find_count == (output->listing_lookup ? 0 : 1000));
  if (expected_error) {
    REQUIRE_FALSE(result->Succeeded());
    REQUIRE(result->Error().code == *expected_error);
    REQUIRE(output->write_count == 0);
    return;
  }
  REQUIRE(result->Succeeded());
  REQUIRE(result->Value() == DirectoryCopySummary{1000, 0, 1000});
  REQUIRE(output->write_count == 1000);
  copy(false);
  REQUIRE_FALSE(result->Succeeded());
  REQUIRE(result->Error().code == FileErrorCode::AlreadyExists);
  REQUIRE(output->write_count == 1000);
  copy(true);
  REQUIRE(result->Succeeded());
  REQUIRE(result->Value() == DirectoryCopySummary{1000, 0, 1000});
  REQUIRE(output->write_count == 2000);
  REQUIRE(output->list_count == (output->listing_lookup ? 3 : 0));
  REQUIRE(output->find_count == (output->listing_lookup ? 0 : 2001));
}

TEST_CASE("DirectoryReferencesRejectReadonlyConflictsOverlapAndInvalidNames") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File source = root.Child("source");
  const File destination = root.Child("destination");
  REQUIRE(source.Child("nested").CreateDirectories());
  REQUIRE(destination.CreateDirectory());
  REQUIRE(source.Child("file.txt").WriteString("source"));
  REQUIRE(destination.Child("file.txt").WriteString("original"));
  auto reference = detail::MakeLocalFileReference(source, false);
  auto target = detail::MakeLocalFileReference(destination, true);
  REQUIRE_THROWS_AS(target.CreateDirectoryAsync("../escape"), std::invalid_argument);
  REQUIRE_THROWS_AS(target.CopyFileFromAsync(source.Child("file.txt"), "a/b"), std::invalid_argument);
  std::vector<FileErrorCode> errors;
  bool overwritten = false;
  bool children_readonly = false;
  file_tasks.Launch([&]() -> Task<void> {
    auto children = co_await reference.ListChildrenAsync();
    if (children.Succeeded()) {
      children_readonly = true;
      for (const auto& child : children.Value()) { children_readonly &= !child.CanWrite(); }
    }
    auto denied = co_await reference.CreateDirectoryAsync("denied");
    if (!denied.Succeeded()) { errors.push_back(denied.Error().code); }
    auto read = co_await reference.ReadBytesAsync();
    if (!read.Succeeded()) { errors.push_back(read.Error().code); }
    auto conflict = co_await reference.CopyDirectoryContentsToAsync(target);
    if (!conflict.Succeeded()) { errors.push_back(conflict.Error().code); }
    auto same = co_await reference.CopyDirectoryContentsToAsync(source);
    if (!same.Succeeded()) { errors.push_back(same.Error().code); }
    auto nested = co_await reference.CopyDirectoryContentsToAsync(source.Child("nested"));
    if (!nested.Succeeded()) { errors.push_back(nested.Error().code); }
    auto replacement = co_await reference.CopyDirectoryContentsToAsync(target, true);
    overwritten = replacement.Succeeded();
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(children_readonly);
  REQUIRE((errors == std::vector<FileErrorCode>{FileErrorCode::PermissionDenied, FileErrorCode::IsDirectory,
      FileErrorCode::AlreadyExists, FileErrorCode::Unsupported, FileErrorCode::Unsupported}));
  REQUIRE(overwritten);
  REQUIRE(destination.Child("file.txt").ReadString().Value() == "source");
  REQUIRE_FALSE(source.Child("denied").Exists());
}

TEST_CASE("DirectoryReferencesKeepRetainedChildrenAndRejectLinksAndRenamedChildren") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File directory = root.Child("source");
  const File destination = root.Child("destination");
  REQUIRE(directory.CreateDirectory());
  REQUIRE(destination.CreateDirectory());
  REQUIRE(directory.Child("value.txt").WriteString("value"));
  REQUIRE(destination.Child("Case").CreateDirectory());
  std::optional<FileReference> parent = detail::MakeLocalFileReference(directory, false);
  std::optional<FileReference> retained;
  auto target = detail::MakeLocalFileReference(destination, true);
  std::string text;
  bool copied = false;
  bool idempotent = false;
  bool rejected_alias = false;
  file_tasks.Launch([&]() -> Task<void> {
    auto children = co_await parent->ListChildrenAsync();
    if (children.Succeeded() && children.Value().size() == 1) { retained = children.Value().front(); }
    parent.reset();
    if (retained) {
      auto read = co_await retained->ReadStringAsync();
      if (read.Succeeded()) { text = read.Value(); }
      auto first = co_await target.CopyFileFromAsync(*retained, "first.txt");
      auto second = co_await target.CopyFileFromAsync(directory.Child("value.txt"), "second.txt");
      copied = first.Succeeded() && second.Succeeded();
    }
    auto existing = co_await target.CreateDirectoryAsync("Case");
    idempotent = existing.Succeeded() && existing.Value().Name() == "Case";
    auto alias = co_await target.CreateDirectoryAsync("case");
    rejected_alias = !alias.Succeeded() && alias.Error().code == FileErrorCode::Unsupported;
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(text == "value");
  REQUIRE(copied);
  REQUIRE(idempotent);
  REQUIRE(retained.has_value());
  REQUIRE(retained->AsFile().has_value());
  REQUIRE(retained->AsFile()->ReadString().Value() == "value");
  if (fs::equivalent(destination.Child("Case").Path(), destination.Child("case").Path())) {
    REQUIRE(rejected_alias);
  }
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  fs::create_directory_symlink(directory.Path(), directory.Child("cycle").Path());
  file_task_complete = false;
  bool rejected_link = false;
  auto source = detail::MakeLocalFileReference(directory, false);
  file_tasks.Launch([&]() -> Task<void> {
    auto result = co_await source.CopyDirectoryContentsToAsync(target, true);
    rejected_link = !result.Succeeded() && result.Error().code == FileErrorCode::Unsupported;
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(rejected_link);
#endif
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
TEST_CASE("FileReferencesAccessTheSelectedFileWithoutParentReadOrWritePermission") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File parent = root.Child("restricted");
  const File selected = parent.Child("selected.txt");
  const File replacement = root.Child("replacement.txt");
  REQUIRE(parent.CreateDirectory());
  REQUIRE(selected.WriteString("original"));
  REQUIRE(replacement.WriteString("new"));
  std::string contents;
  bool replaced = false;
  fs::permissions(parent.Path(), fs::perms::owner_exec);
  try {
    auto reference = detail::MakeLocalFileReference(selected, true);
    file_tasks.Launch([&, reference]() -> Task<void> {
      auto read = co_await reference.ReadStringAsync();
      if (read.Succeeded()) { contents = read.Value(); }
      replaced = co_await reference.ReplaceWithAsync(replacement);
      file_task_complete = true;
    });
    platform.RunUntil([] { return file_task_complete; });
  } catch (...) {
    fs::permissions(parent.Path(), fs::perms::owner_all);
    throw;
  }
  fs::permissions(parent.Path(), fs::perms::owner_all);
  REQUIRE(contents == "original");
  REQUIRE(replaced);
  REQUIRE(selected.ReadString().Value() == "new");
}
#endif

#if defined(_WIN32)
TEST_CASE("WindowsDirectoryReferencesProbeWriteAccessWithoutChangingTheDirectory") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File directory = file_system->Directories().temporary_directory.Child("selected");
  REQUIRE(directory.CreateDirectory());
  bool writable = true;
  bool expected_write = true;
  std::unique_ptr<void, decltype(&CloseHandle)> blocker{nullptr, CloseHandle};
  SECTION("A writable selection reports child creation access") {}
  SECTION("A read-only selection preserves its grant restriction") {
    writable = false;
    expected_write = false;
  }
  SECTION("Unavailable write access does not prevent reading the directory") {
    const HANDLE handle = CreateFileW(fs::u8path(directory.Path()).c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    blocker.reset(handle);
    expected_write = false;
  }
  std::optional<FileReference> reference;
  REQUIRE_NOTHROW(reference = detail::MakeLocalFileReference(directory, writable));
  REQUIRE(reference->Type() == FileType::Directory);
  REQUIRE(reference->CanWrite() == expected_write);
  REQUIRE(reference->AsFile() == directory);
  const auto entries = directory.ListChildren();
  REQUIRE(entries.Succeeded());
  REQUIRE(entries.Value().empty());
  std::optional<FileResult<std::vector<FileReference>>> children;
  std::optional<FileResult<FileReference>> created;
  file_tasks.Launch([&]() -> Task<void> {
    children = co_await reference->ListChildrenAsync();
    created = co_await reference->CreateDirectoryAsync("child");
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(children.has_value());
  REQUIRE(children->Succeeded());
  REQUIRE(children->Value().empty());
  REQUIRE(created.has_value());
  REQUIRE(created->Succeeded() == expected_write);
  REQUIRE(directory.Child("child").Exists() == expected_write);
  if (!expected_write) {
    REQUIRE(created->Error().code == FileErrorCode::PermissionDenied);
  }
}

TEST_CASE("WindowsDirectoryReferencePathsRemainUsableAfterReleasingNativeHandles") {
  TemporaryDirectory temporary;
  const File root(temporary.Paths().temporary_directory);
  const File parent = root.Child("parent");
  const File selected = parent.Child("selected");
  const File moved = root.Child("moved");
  REQUIRE(selected.CreateDirectories());
  REQUIRE(selected.Child("value.txt").WriteString("original"));
  std::optional<File> path;
  {
    auto reference = detail::MakeLocalFileReference(selected, false);
    auto copy = reference;
    path = copy.AsFile();
    REQUIRE(path == selected);
  }
  REQUIRE(parent.MoveTo(moved));
  REQUIRE_FALSE(path->Exists());
  REQUIRE(moved.Child("selected").Child("value.txt").ReadString().Value() == "original");
  REQUIRE(path->CreateDirectories());
  REQUIRE(path->Child("value.txt").WriteString("unrelated"));
  REQUIRE(path->Child("value.txt").ReadString().Value() == "unrelated");
  REQUIRE(moved.Child("selected").Child("value.txt").ReadString().Value() == "original");
}

TEST_CASE("WindowsDirectoryOverwritesPreserveOriginalsAndCleanUpFailedStagingFiles") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File directory = root.Child("selected");
  const File target = directory.Child("value.txt");
  const File source = root.Child("source.txt");
  REQUIRE(directory.CreateDirectory());
  REQUIRE(target.WriteString("original"));
  REQUIRE(source.WriteString("replacement"));
  auto reference = detail::MakeLocalFileReference(directory, true);
  const HANDLE handle = CreateFileW(fs::u8path(target.Path()).c_str(), FILE_READ_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  REQUIRE(handle != INVALID_HANDLE_VALUE);
  std::unique_ptr<void, decltype(&CloseHandle)> blocker{handle, CloseHandle};
  std::optional<FileResult<FileReference>> result;
  const auto copy = [&] {
    file_task_complete = false;
    result.reset();
    file_tasks.Launch([&]() -> Task<void> {
      result = co_await reference.CopyFileFromAsync(source, "value.txt", true);
      file_task_complete = true;
    });
    platform.RunUntil([] { return file_task_complete; });
    REQUIRE(result.has_value());
  };
  copy();
  REQUIRE_FALSE(result->Succeeded());
  REQUIRE(target.ReadString().Value() == "original");
  REQUIRE(directory.ListChildren().Value() == std::vector<File>{target});
  blocker.reset();
  copy();
  REQUIRE(result->Succeeded());
  REQUIRE(result->Value().AsFile() == target);
  REQUIRE(target.ReadString().Value() == "replacement");
  REQUIRE(directory.ListChildren().Value() == std::vector<File>{target});
  REQUIRE(source.ReadString().Value() == "replacement");
}

TEST_CASE("WindowsDirectoryGrantsDoNotFollowReplacedRoots") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File parent = root.Child("parent");
  const File selected = parent.Child("selected");
  const File moved = root.Child("moved");
  const File replacement = root.Child("replacement.txt");
  REQUIRE(selected.CreateDirectories());
  REQUIRE(selected.Child("value.txt").WriteString("original"));
  REQUIRE(replacement.WriteString("replacement"));
  auto reference = detail::MakeLocalFileReference(selected, true);
  std::optional<FileReference> child;
  file_tasks.Launch([&]() -> Task<void> {
    auto children = co_await reference.ListChildrenAsync();
    if (children.Succeeded() && children.Value().size() == 1) { child = children.Value().front(); }
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(child.has_value());
  REQUIRE(selected.MoveTo(moved));
  REQUIRE(selected.CreateDirectories());
  REQUIRE(selected.Child("value.txt").WriteString("unrelated"));
  std::string contents;
  bool list_succeeded = false;
  bool create_succeeded = false;
  bool replace_succeeded = false;
  bool copy_succeeded = false;
  bool overwrite_succeeded = false;
  bool tree_copy_succeeded = false;
  const File output = root.Child("output");
  REQUIRE(output.CreateDirectory());
  file_task_complete = false;
  file_tasks.Launch([&]() -> Task<void> {
    auto read = co_await child->ReadStringAsync();
    if (read.Succeeded()) { contents = read.Value(); }
    list_succeeded = (co_await reference.ListChildrenAsync()).Succeeded();
    create_succeeded = (co_await reference.CreateDirectoryAsync("new")).Succeeded();
    replace_succeeded = co_await child->ReplaceWithAsync(replacement);
    copy_succeeded = (co_await reference.CopyFileFromAsync(replacement, "copy.txt")).Succeeded();
    overwrite_succeeded = (co_await reference.CopyFileFromAsync(replacement, "value.txt", true)).Succeeded();
    tree_copy_succeeded = (co_await reference.CopyDirectoryContentsToAsync(output)).Succeeded();
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(contents == "original");
  REQUIRE(list_succeeded);
  REQUIRE(create_succeeded);
  REQUIRE(replace_succeeded);
  REQUIRE(copy_succeeded);
  REQUIRE(overwrite_succeeded);
  REQUIRE(tree_copy_succeeded);
  REQUIRE(moved.Child("value.txt").ReadString().Value() == "replacement");
  REQUIRE(moved.Child("copy.txt").ReadString().Value() == "replacement");
  REQUIRE(moved.Child("new").IsDirectory());
  REQUIRE(output.Child("value.txt").ReadString().Value() == "replacement");
  REQUIRE(output.Child("copy.txt").ReadString().Value() == "replacement");
  REQUIRE(output.Child("new").IsDirectory());
  REQUIRE(selected.Child("value.txt").ReadString().Value() == "unrelated");
  REQUIRE_FALSE(selected.Child("new").Exists());
  REQUIRE_FALSE(selected.Child("copy.txt").Exists());
}

TEST_CASE("WindowsDirectoryGrantsRejectDirectoriesConvertedToJunctions") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Paths());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_system->Directories().temporary_directory;
  const File selected = root.Child("selected");
  const File outside = root.Child("outside");
  const File input = root.Child("input.txt");
  REQUIRE(selected.CreateDirectory());
  REQUIRE(outside.CreateDirectory());
  REQUIRE(outside.Child("untouched.txt").WriteString("outside"));
  REQUIRE(input.WriteString("input"));
  auto reference = detail::MakeLocalFileReference(selected, true);
  File changed = selected;
  SECTION("The selected directory itself becomes a junction") {}
  SECTION("A derived directory becomes a junction") {
    file_tasks.Launch([&]() -> Task<void> {
      auto child = co_await reference.CreateDirectoryAsync("child");
      if (child.Succeeded()) { reference = child.Value(); }
      file_task_complete = true;
    });
    platform.RunUntil([] { return file_task_complete; });
    REQUIRE(reference.Name() == "child");
    changed = selected.Child("child");
  }
  TemporaryJunction junction(changed, outside);
  bool listed = true;
  bool created = true;
  bool copied = true;
  file_task_complete = false;
  file_tasks.Launch([&]() -> Task<void> {
    listed = (co_await reference.ListChildrenAsync()).Succeeded();
    created = (co_await reference.CreateDirectoryAsync("escaped")).Succeeded();
    copied = (co_await reference.CopyFileFromAsync(input, "escaped.txt")).Succeeded();
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE_FALSE(listed);
  REQUIRE_FALSE(created);
  REQUIRE_FALSE(copied);
  REQUIRE(outside.Child("untouched.txt").ReadString().Value() == "outside");
  REQUIRE_FALSE(outside.Child("escaped").Exists());
  REQUIRE_FALSE(outside.Child("escaped.txt").Exists());
}
#endif

} // namespace huxerui::test
