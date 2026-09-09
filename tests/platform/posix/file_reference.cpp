#include "file_reference_test_support.h"

namespace huxerui::test {

TEST_CASE("DirectoryReferencesRejectSymbolicLinkCycles") {
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
  auto target = detail::MakeLocalFileReference(destination, true);
  fs::create_directory_symlink(directory.Path(), directory.Child("cycle").Path());
  file_task_complete = false;
  bool rejected_link = false;
  auto source = detail::MakeLocalFileReference(directory, false);
  file_tasks.Launch([&]() -> Task<void> {
    auto result = co_await source.CopyDirectoryContentsToAsync(target, true);
    rejected_link = !result.Succeeded() && result.Error().code == IoErrorCode::Unsupported;
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(rejected_link);
}

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

} // namespace huxerui::test
