#include "file_reference_test_support.h"

namespace huxerui::test {
namespace {

State<int> directory_revision;
int directory_compositions = 0;

View DirectoryApp() {
  file_application = UseApplication();
  directory_revision = UseState(0);
  static_cast<void>(directory_revision.Get());
  ++directory_compositions;
  return {};
}

} // namespace

TEST_CASE("FileReferencesExposePathValuesWithoutTransferringTheirAccessLifetime") {
  TemporaryDirectory temporary;
  const File root(temporary.Directories().temporary_directory);
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

TEST_CASE("ApplicationProtectsDirectoriesReturnedDirectlyByACustomAdapter") {
  ResetFileState();
  TemporaryDirectory temporary;
  AppDirectories directories = temporary.Directories();
  const File root = *directories.executable_directory;
  directories.executable_directory = root.Child("bin");
  REQUIRE(directories.executable_directory->CreateDirectory());
  REQUIRE(directories.data_directory.CreateDirectory());
  REQUIRE(directories.cache_directory.CreateDirectory());
  REQUIRE(directories.temporary_directory.CreateDirectory());

  class PreparedDirectoriesPlatform final : public TestPlatform {
  public:
    explicit PreparedDirectoriesPlatform(AppDirectories directories) : directories_(std::move(directories)) {}

  protected:
    std::optional<AppDirectories> CreateAppDirectories() override {
      return directories_;
    }

  private:
    AppDirectories directories_;
  } platform(directories);

  {
    Runtime runtime(FileApp, platform);
    runtime.BuildFrame();
    REQUIRE(file_application->Directories().data_directory == directories.data_directory);
    REQUIRE_FALSE(directories.data_directory.Delete());
    REQUIRE_FALSE(directories.cache_directory.DeleteRecursively());
    REQUIRE_FALSE(directories.temporary_directory.DeleteRecursively());
    REQUIRE_FALSE(directories.executable_directory->DeleteRecursively());
    REQUIRE_FALSE(root.DeleteRecursively());
    const File child = directories.data_directory.Child("ordinary.txt");
    REQUIRE(child.WriteString("content"));
    REQUIRE(child.Delete());
  }
  file_application.reset();
  REQUIRE_FALSE(root.DeleteRecursively());
}

TEST_CASE("ApplicationDirectoriesRemainStableAcrossRecompositionAndRuntimeDestruction") {
  ResetFileState();
  directory_compositions = 0;
  TemporaryDirectory first_temporary;
  TemporaryDirectory second_temporary;
  FileTestPlatform first_platform(first_temporary.Directories());
  FileTestPlatform second_platform(second_temporary.Directories());
  std::optional<ApplicationHandle> first_application;
  std::optional<AppDirectories> retained_directories;
  {
    Runtime first_runtime(DirectoryApp, first_platform);
    first_runtime.BuildFrame();
    first_application = file_application;
    REQUIRE(first_application.has_value());
    const AppDirectories* initial = &first_application->Directories();
    retained_directories = *initial;

    directory_revision = 1;
    first_runtime.BuildFrame();
    REQUIRE(directory_compositions == 2);
    REQUIRE(&first_application->Directories() == initial);
    REQUIRE(first_application->Directories().data_directory == first_temporary.Directories().data_directory);

    std::optional<ApplicationHandle> second_application;
    {
      Runtime second_runtime(DirectoryApp, second_platform);
      second_runtime.BuildFrame();
      second_application = file_application;
      REQUIRE(second_application->Directories().data_directory == second_temporary.Directories().data_directory);
      REQUIRE(second_application->Directories().data_directory != initial->data_directory);
      REQUIRE(second_application->CurrentDirectory() == first_application->CurrentDirectory());
    }
    REQUIRE(second_application->Directories().cache_directory == second_temporary.Directories().cache_directory);
    REQUIRE(&first_application->Directories() == initial);
  }
  REQUIRE(first_application->Directories().data_directory == retained_directories->data_directory);
  first_application.reset();
  const File file = retained_directories->data_directory.Child("after-runtime.txt");
  REQUIRE(file.WriteString("retained path"));
  REQUIRE(file.ReadString().Value() == "retained path");
  REQUIRE_FALSE(retained_directories->data_directory.DeleteRecursively());
}

TEST_CASE("ApplicationCurrentDirectoryIsQueriedAtEachCall") {
  ResetFileState();
  TemporaryDirectory temporary;
  TestPlatform platform;
  {
    Runtime runtime(FileApp, platform);
    runtime.BuildFrame();
  }
  struct RestoreWorkingDirectory {
    fs::path original = fs::current_path();
    ~RestoreWorkingDirectory() {
      std::error_code error;
      fs::current_path(original, error);
    }
  } restore;
  REQUIRE(file_application->CurrentDirectory() == File(Utf8Path(restore.original)));
  const std::string directory = temporary.Directories().data_directory.Path();
  fs::current_path(fs::path(std::u8string(directory.begin(), directory.end())).parent_path());
  REQUIRE(file_application->CurrentDirectory() == File(Utf8Path(fs::current_path())));
  REQUIRE(file_application->CurrentDirectory() != File(Utf8Path(restore.original)));
}

TEST_CASE("ApplicationProvidesDirectoriesAndFileAsyncOperationsResumeOnTheUIThread") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  REQUIRE(file_application);
  REQUIRE(file_application->Directories().data_directory.IsDirectory());
  REQUIRE(file_application->Directories().cache_directory.IsDirectory());
  REQUIRE(file_application->Directories().temporary_directory.IsDirectory());

  const std::thread::id ui_thread = std::this_thread::get_id();
  File file = file_application->Directories().data_directory.Child("async.txt");
  file_tasks.Launch([file]() -> Task<void> {
    if (!co_await file.WriteStringAsync("async value")) {
      file_task_complete = true;
      co_return;
    }
    IoResult<std::string> result = co_await file.ReadStringAsync();
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
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  File file = file_application->Directories().data_directory.Child("async.bin");
  file_tasks.Launch([file]() -> Task<void> {
    if (!co_await file.WriteBytesAsync(Bytes{std::byte{0}, std::byte{0xFF}}) ||
        !co_await file.AppendBytesAsync(Bytes{std::byte{'a'}, std::byte{0}})) {
      file_task_complete = true;
      co_return;
    }
    IoResult<Bytes> result = co_await file.ReadBytesAsync();
    if (result.Succeeded()) {
      async_bytes = std::move(result).Value();
    }
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE((async_bytes == Bytes{std::byte{0}, std::byte{0xFF}, std::byte{'a'}, std::byte{0}}));
}

TEST_CASE("FileAsyncStreamsUseRequestedReadSizesAndExplicitClose") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  const File source = file_application->Directories().data_directory.Child("stream-source.bin");
  const File destination = file_application->Directories().data_directory.Child("stream-destination.bin");
  std::vector<std::size_t> read_sizes;
  std::uint64_t copied = 0;
  file_tasks.Launch([&]() -> Task<void> {
    IoResult<AsyncOutputStream> opened_output = co_await source.OpenWriteAsync();
    REQUIRE(opened_output.Succeeded());
    AsyncOutputStream output = std::move(opened_output).Value();
    // Keep suspension outside assertion macros, which expand their expression in multiple contexts.
    const auto first_write = co_await output.WriteAsync(Bytes{std::byte{1}, std::byte{2}});
    REQUIRE(first_write.Succeeded());
    const auto second_write = co_await output.WriteAsync(Bytes{std::byte{3}, std::byte{4}, std::byte{5}});
    REQUIRE(second_write.Succeeded());
    const auto closed = co_await output.CloseAsync();
    REQUIRE(closed.Succeeded());

    IoResult<AsyncInputStream> opened_input = co_await source.OpenReadAsync();
    REQUIRE(opened_input.Succeeded());
    AsyncInputStream input = std::move(opened_input).Value();
    while (true) {
      Bytes data = (co_await input.ReadAsync(2)).Value();
      read_sizes.push_back(data.size());
      if (data.empty()) {
        break;
      }
      async_bytes.insert(async_bytes.end(), data.begin(), data.end());
    }

    IoResult<AsyncInputStream> opened_copy_input = co_await source.OpenReadAsync();
    IoResult<AsyncOutputStream> opened_copy_output = co_await destination.OpenWriteAsync();
    REQUIRE(opened_copy_input.Succeeded());
    REQUIRE(opened_copy_output.Succeeded());
    AsyncInputStream copy_input = std::move(opened_copy_input).Value();
    AsyncOutputStream copy_output = std::move(opened_copy_output).Value();
    copied = (co_await copy_input.CopyToAsync(copy_output, 3)).Value();
    const auto copy_closed = co_await copy_output.CloseAsync();
    REQUIRE(copy_closed.Succeeded());
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE((async_bytes == Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}}));
  REQUIRE(read_sizes == std::vector<std::size_t>{2, 2, 1, 0});
  REQUIRE(copied == 5);
  REQUIRE(destination.ReadBytes().Value() == async_bytes);
}

TEST_CASE("FilePendingOutputRetainsTheFileAfterItsOwnerIsReleased") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File file = file_application->Directories().data_directory.Child("retained-output.bin");
  file_tasks.Launch([&]() -> Task<void> {
    auto opened = co_await file.OpenWriteAsync();
    REQUIRE(opened.Succeeded());
    auto pending = [&] {
      auto output = std::move(opened).Value();
      return output.WriteAsync(Bytes{std::byte{1}, std::byte{2}});
    }();
    const auto written = co_await std::move(pending);
    REQUIRE(written.Succeeded());
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE((file.ReadBytes().Value() == Bytes{std::byte{1}, std::byte{2}}));
}

TEST_CASE("LocalReferenceStreamsPreserveOperationalReadWriteCloseAndCopyErrors") {
  enum class Operation { Read, Write, Close, CopyRead, CopyWrite };
  const auto operation =
      GENERATE(Operation::Read, Operation::Write, Operation::Close, Operation::CopyRead, Operation::CopyWrite);
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File source = file_application->Directories().data_directory.Child("error-source.bin");
  const File destination = file_application->Directories().data_directory.Child("error-destination.bin");
  REQUIRE(source.WriteBytes(Bytes{std::byte{1}, std::byte{2}}));
  REQUIRE(destination.WriteBytes({}));
  std::atomic<bool> fail_read = false;
  std::atomic<bool> fail_write = false;
  const auto coordinate = [&](const File* reading, const File* writing, const std::function<void()>& action) {
    if ((reading && fail_read) || (writing && fail_write)) {
      throw std::system_error(std::make_error_code(std::errc::permission_denied));
    }
    action();
  };
  auto input_reference = detail::MakeLocalFileReference(source, false, {}, coordinate);
  auto output_reference = detail::MakeLocalFileReference(destination, true, {}, coordinate);
  file_tasks.Launch([&]() -> Task<void> {
    auto opened_input = co_await input_reference.OpenReadAsync();
    auto opened_output = co_await output_reference.OpenWriteAsync();
    REQUIRE(opened_input.Succeeded());
    REQUIRE(opened_output.Succeeded());
    auto input = std::move(opened_input).Value();
    auto output = std::move(opened_output).Value();
    fail_read = operation == Operation::Read || operation == Operation::CopyRead;
    fail_write = !fail_read.load();
    if (operation == Operation::Read) {
      auto result = co_await input.ReadAsync(1);
      REQUIRE_FALSE(result.Succeeded());
      REQUIRE(result.Error().code == IoErrorCode::PermissionDenied);
    } else if (operation == Operation::Write || operation == Operation::Close) {
      auto pending = operation == Operation::Write ? output.WriteAsync(Bytes{std::byte{3}}) : output.CloseAsync();
      auto result = co_await std::move(pending);
      REQUIRE_FALSE(result.Succeeded());
      REQUIRE(result.Error().code == IoErrorCode::PermissionDenied);
    } else {
      auto result = co_await input.CopyToAsync(output, 1);
      REQUIRE_FALSE(result.Succeeded());
      REQUIRE(result.Error().code == IoErrorCode::PermissionDenied);
    }
    if (fail_read) {
      REQUIRE_THROWS_AS(input.ReadAsync(1), std::logic_error);
    } else {
      REQUIRE_THROWS_AS(output.CloseAsync(), std::logic_error);
    }
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
}

TEST_CASE("LocalFileReferencesProvideIncrementalAsyncStreams") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  const File file = file_application->Directories().data_directory.Child("reference-stream.bin");
  REQUIRE(file.WriteBytes(Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}}));
  FileReference reference = detail::MakeLocalFileReference(file, true);
  std::vector<std::size_t> read_sizes;
  file_tasks.Launch([&]() -> Task<void> {
    IoResult<AsyncInputStream> opened_input = co_await reference.OpenReadAsync();
    REQUIRE(opened_input.Succeeded());
    AsyncInputStream input = std::move(opened_input).Value();
    while (true) {
      Bytes data = (co_await input.ReadAsync(2)).Value();
      read_sizes.push_back(data.size());
      if (data.empty()) {
        break;
      }
      async_bytes.insert(async_bytes.end(), data.begin(), data.end());
    }

    IoResult<AsyncOutputStream> opened_output = co_await reference.OpenWriteAsync();
    REQUIRE(opened_output.Succeeded());
    AsyncOutputStream output = std::move(opened_output).Value();
    const auto written = co_await output.WriteAsync(Bytes{std::byte{9}, std::byte{8}});
    REQUIRE(written.Succeeded());
    const auto closed = co_await output.CloseAsync();
    REQUIRE(closed.Succeeded());
    file_task_complete = true;
  });

  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(read_sizes == std::vector<std::size_t>{2, 2, 1, 0});
  REQUIRE((async_bytes == Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}}));
  const auto read_back = file.ReadBytes();
  REQUIRE(read_back.Succeeded());
  const Bytes expected{std::byte{9}, std::byte{8}};
  REQUIRE(read_back.Value() == expected);
}

TEST_CASE("CancelingAFileTaskDropsItsContinuation") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();

  File file = file_application->Directories().temporary_directory.Child("canceled.txt");
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
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_application->Directories().temporary_directory;
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
  std::optional<IoError> failure;
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

TEST_CASE("DirectoryCopiesIndexListingBasedDestinationsOnlyWithinOneCopy") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
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
  std::optional<IoErrorCode> expected_error;
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
    expected_error = IoErrorCode::AlreadyExists;
  }
  SECTION("Enumeration failure is not treated as an empty directory") {
    output->list_error = IoErrorCode::PermissionDenied;
    expected_error = IoErrorCode::PermissionDenied;
  }
  SECTION("An index does not bypass final write authorization") {
    output->write_error = IoErrorCode::PermissionDenied;
    expected_error = IoErrorCode::PermissionDenied;
  }
  std::optional<IoResult<DirectoryCopySummary>> result;
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
  REQUIRE(result->Error().code == IoErrorCode::AlreadyExists);
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
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_application->Directories().temporary_directory;
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
  std::vector<IoErrorCode> errors;
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
  REQUIRE((errors == std::vector<IoErrorCode>{IoErrorCode::PermissionDenied, IoErrorCode::IsDirectory,
      IoErrorCode::AlreadyExists, IoErrorCode::Unsupported, IoErrorCode::Unsupported}));
  REQUIRE(overwritten);
  REQUIRE(destination.Child("file.txt").ReadString().Value() == "source");
  REQUIRE_FALSE(source.Child("denied").Exists());
}

TEST_CASE("DirectoryReferencesKeepRetainedChildrenAndRejectLinksAndRenamedChildren") {
  ResetFileState();
  TemporaryDirectory temporary;
  FileTestPlatform platform(temporary.Directories());
  Runtime runtime(FileApp, platform);
  runtime.BuildFrame();
  const File root = file_application->Directories().temporary_directory;
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
    rejected_alias = !alias.Succeeded() && alias.Error().code == IoErrorCode::Unsupported;
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
}

} // namespace huxerui::test
