#include "file_reference_test_support.h"

namespace huxerui::test {

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
  std::optional<IoResult<DirectoryCopySummary>> result;
  bool write_output = true;
  bool cancel = false;
  std::optional<IoErrorCode> expected_error;
  SECTION("Actual transferred bytes are counted instead of source metadata") {}
  SECTION("The provider error is preserved") {
    expected_error = IoErrorCode::PermissionDenied;
  }
  SECTION("A missing finalized output preserves the metadata error") {
    write_output = false;
    expected_error = IoErrorCode::NotFound;
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
  if (expected_error == IoErrorCode::PermissionDenied) {
    complete(IoResult<std::uint64_t>(IoError{*expected_error, "HuxerUI provider secret URI"}));
  } else {
    complete(IoResult<std::uint64_t>(3));
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
}

} // namespace huxerui::test
