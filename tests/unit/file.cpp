#include <catch2/catch_amalgamated.hpp>

#include <huxerui/file.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_internal.h"

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
    path_ = fs::temp_directory_path() /
            ("huxerui-file-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] std::string Path() const {
    return Utf8Path(path_);
  }

private:
  fs::path path_;
};

} // namespace

static_assert(std::copy_constructible<File>);
static_assert(std::move_constructible<File>);
static_assert(!std::default_initializable<File>);
static_assert(!std::copy_constructible<FileSystem>);

TEST_CASE("FileResultDistinguishesValuesFromErrors") {
  FileResult<std::string> value(std::string{"value"});
  REQUIRE(value.Succeeded());
  REQUIRE(value.Value() == "value");
  REQUIRE_THROWS_AS(value.Error(), std::logic_error);

  FileResult<std::string> error(FileError{FileErrorCode::NotFound, "missing"});
  REQUIRE_FALSE(error.Succeeded());
  REQUIRE(error.Error().code == FileErrorCode::NotFound);
  REQUIRE_THROWS_AS(error.Value(), std::logic_error);
}

TEST_CASE("FileNormalizesUtf8PathsAndProvidesLexicalOperations") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  File file = root.Resolve("folder/../folder/报告.txt");

  REQUIRE(file.Name() == "报告.txt");
  REQUIRE(file.Stem() == "报告");
  REQUIRE(file.Extension() == ".txt");
  REQUIRE(file.Parent().has_value());
  REQUIRE(file.Parent()->Name() == "folder");
  REQUIRE(file.ParentPath() == file.Parent()->Path());
  REQUIRE(File(root, "child") == root.Child("child"));
  REQUIRE(root.Resolve("folder/./item") == root.Resolve("folder/item"));
  REQUIRE_FALSE(File("/").Parent().has_value());

  REQUIRE_THROWS_AS(File(""), std::invalid_argument);
  REQUIRE_THROWS_AS(File(std::string_view{"\xC3\x28", 2}), std::invalid_argument);
  REQUIRE_THROWS_AS(root.Child(""), std::invalid_argument);
  REQUIRE_THROWS_AS(root.Child(".."), std::invalid_argument);
  REQUIRE_THROWS_AS(root.Child("nested/item"), std::invalid_argument);
  REQUIRE_THROWS_AS(root.Resolve("/absolute"), std::invalid_argument);
}

TEST_CASE("FilePerformsSynchronousLocalFileAndDirectoryOperations") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  File directory = root.Resolve("nested/content");
  REQUIRE(directory.CreateDirectories());
  REQUIRE(directory.CreateDirectories());
  REQUIRE(directory.IsDirectory());

  File text = directory.Child("内容.txt");
  REQUIRE(text.WriteString("first"));
  REQUIRE(text.AppendString("\n第二行"));
  FileResult<std::string> text_result = text.ReadString();
  REQUIRE(text_result.Succeeded());
  REQUIRE(text_result.Value() == "first\n第二行");

  FileResult<FileInfo> info = text.Stat();
  REQUIRE(info.Succeeded());
  REQUIRE(info.Value().type == FileType::File);
  REQUIRE(info.Value().size == text_result.Value().size());

  FileResult<std::vector<File>> children = directory.ListChildren();
  REQUIRE(children.Succeeded());
  REQUIRE(children.Value().size() == 1);
  REQUIRE(children.Value().front() == text);

  File copy = directory.Child("copy.txt");
  REQUIRE(text.CopyTo(copy));
  REQUIRE_FALSE(text.CopyTo(copy));
  REQUIRE(text.WriteString("replacement"));
  REQUIRE(text.CopyTo(copy, true));
  REQUIRE(copy.ReadString().Value() == "replacement");

  File moved = directory.Child("moved.txt");
  REQUIRE(copy.MoveTo(moved));
  REQUIRE_FALSE(copy.Exists());
  REQUIRE(moved.IsFile());
  REQUIRE(moved.Delete());
  REQUIRE(moved.Delete());

  File binary = directory.Child("binary.bin");
  const Bytes prefix{std::byte{0}, std::byte{0xFF}};
  const Bytes suffix{std::byte{'a'}, std::byte{0}};
  REQUIRE(binary.WriteBytes(prefix));
  REQUIRE(binary.AppendBytes(suffix));
  FileResult<Bytes> binary_result = binary.ReadBytes();
  REQUIRE(binary_result.Succeeded());
  REQUIRE((binary_result.Value() == Bytes{std::byte{0}, std::byte{0xFF}, std::byte{'a'}, std::byte{0}}));

  File bom = directory.Child("bom.txt");
  const Bytes bom_bytes{
      std::byte{0xEF},
      std::byte{0xBB},
      std::byte{0xBF},
      std::byte{'o'},
      std::byte{'k'},
  };
  REQUIRE(bom.WriteBytes(bom_bytes));
  REQUIRE(bom.ReadString().Value() == "ok");

  File invalid = directory.Child("invalid.txt");
  const Bytes invalid_bytes{std::byte{0xFF}};
  REQUIRE(invalid.WriteBytes(invalid_bytes));
  REQUIRE_FALSE(invalid.ReadString().Succeeded());
  REQUIRE(invalid.ReadString().Error().code == FileErrorCode::InvalidEncoding);

  REQUIRE_FALSE(directory.ReadBytes().Succeeded());
  REQUIRE(directory.ReadBytes().Error().code == FileErrorCode::IsDirectory);
  REQUIRE_FALSE(root.Child("missing").Stat().Succeeded());
  REQUIRE(root.Child("missing").Stat().Error().code == FileErrorCode::NotFound);

  REQUIRE(directory.DeleteRecursively());
  REQUIRE_FALSE(directory.Exists());
}

TEST_CASE("FileSystemCreatesAndProtectsApplicationDirectories") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  std::shared_ptr<FileSystem> file_system = detail::MakeFileSystem({
      .executable_directory = root.Path(),
      .data_directory = root.Child("data").Path(),
      .cache_directory = root.Child("cache").Path(),
      .temporary_directory = root.Child("temporary").Path(),
  });

  const AppDirectories& directories = file_system->Directories();
  REQUIRE(directories.executable_directory == root);
  REQUIRE(directories.data_directory.IsDirectory());
  REQUIRE(directories.cache_directory.IsDirectory());
  REQUIRE(directories.temporary_directory.IsDirectory());
  REQUIRE_FALSE(directories.data_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.cache_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.temporary_directory.Delete());
  REQUIRE_FALSE(directories.temporary_directory.DeleteRecursively());
  REQUIRE_FALSE(root.DeleteRecursively());
  REQUIRE(file_system->CurrentDirectory() == File(Utf8Path(fs::current_path())));
}

TEST_CASE("FileSystemProtectsAncestorsOfApplicationDirectories") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  File application_root = root.Child("application");
  std::shared_ptr<FileSystem> file_system = detail::MakeFileSystem({
      .data_directory = application_root.Child("data").Path(),
      .cache_directory = application_root.Child("cache").Path(),
      .temporary_directory = application_root.Child("temporary").Path(),
  });

  REQUIRE(file_system->Directories().data_directory.IsDirectory());
  REQUIRE_FALSE(application_root.DeleteRecursively());
  REQUIRE(application_root.IsDirectory());
}

} // namespace huxerui::test
