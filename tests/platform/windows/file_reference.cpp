#include "file_reference_test_support.h"

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#undef CreateDirectory
#undef CopyFile
#undef DeleteFile
#undef MoveFile

namespace huxerui::test {
namespace {

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

} // namespace

TEST_CASE("WindowsDirectoryReferencesRejectPathOnlyProviderImports") {
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
  // The native Windows destination cannot delegate its retained authority to a path-only provider.
  file_tasks.Launch([&]() -> Task<void> {
    result = co_await source.CopyDirectoryContentsToAsync(destination);
    file_task_complete = true;
  });
  platform.RunUntil([] { return file_task_complete; });
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->Succeeded());
  REQUIRE(result->Error().code == IoErrorCode::Unsupported);
  REQUIRE_FALSE(input->imported_to.has_value());
  REQUIRE_FALSE(destination.Child("value.bin").Exists());
}

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
  const auto path = reference->AsFile();
  REQUIRE(path.has_value());
  CAPTURE(path->Path(), directory.Path());
  REQUIRE(fs::equivalent(fs::u8path(path->Path()), fs::u8path(directory.Path())));
  const auto entries = directory.ListChildren();
  REQUIRE(entries.Succeeded());
  REQUIRE(entries.Value().empty());
  std::optional<IoResult<std::vector<FileReference>>> children;
  std::optional<IoResult<FileReference>> created;
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
    REQUIRE(created->Error().code == IoErrorCode::PermissionDenied);
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
    REQUIRE(path.has_value());
    CAPTURE(path->Path(), selected.Path());
    REQUIRE(fs::equivalent(fs::u8path(path->Path()), fs::u8path(selected.Path())));
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
  std::optional<IoResult<FileReference>> result;
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
  const auto path = result->Value().AsFile();
  REQUIRE(path.has_value());
  CAPTURE(path->Path(), target.Path());
  REQUIRE(fs::equivalent(fs::u8path(path->Path()), fs::u8path(target.Path())));
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

} // namespace huxerui::test
