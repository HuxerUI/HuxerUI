#include <catch2/catch_amalgamated.hpp>

#include <huxerui/file.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
  IoResult<std::string> text_result = text.ReadString();
  REQUIRE(text_result.Succeeded());
  REQUIRE(text_result.Value() == "first\n第二行");

  IoResult<FileInfo> info = text.Stat();
  REQUIRE(info.Succeeded());
  REQUIRE(info.Value().type == FileType::File);
  REQUIRE(info.Value().size == text_result.Value().size());

  IoResult<std::vector<File>> children = directory.ListChildren();
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
  IoResult<Bytes> binary_result = binary.ReadBytes();
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
  REQUIRE(invalid.ReadString().Error().code == IoErrorCode::InvalidEncoding);

  REQUIRE_FALSE(directory.ReadBytes().Succeeded());
  REQUIRE(directory.ReadBytes().Error().code == IoErrorCode::IsDirectory);
  REQUIRE_FALSE(root.Child("missing").Stat().Succeeded());
  REQUIRE(root.Child("missing").Stat().Error().code == IoErrorCode::NotFound);

  REQUIRE(directory.DeleteRecursively());
  REQUIRE_FALSE(directory.Exists());
}

TEST_CASE("FileStreamsReadWriteAppendAndCopyWithCallerOwnedBuffers") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  File source = root.Child("source.bin");
  REQUIRE(source.WriteBytes(Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}}));

  IoResult<InputStream> opened_input = source.OpenRead();
  REQUIRE(opened_input.Succeeded());
  InputStream input = std::move(opened_input).Value();
  Bytes read_buffer(2);
  REQUIRE(input.Read(read_buffer).Value() == 2);
  REQUIRE((read_buffer == Bytes{std::byte{1}, std::byte{2}}));

  File destination = root.Child("destination.bin");
  IoResult<OutputStream> opened_output = destination.OpenWrite();
  REQUIRE(opened_output.Succeeded());
  OutputStream output = std::move(opened_output).Value();
  Bytes copy_buffer(3);
  REQUIRE(input.CopyTo(output, copy_buffer).Value() == 3);
  REQUIRE(output.Close().Succeeded());
  REQUIRE((destination.ReadBytes().Value() == Bytes{std::byte{3}, std::byte{4}, std::byte{5}}));

  IoResult<OutputStream> opened_append = destination.OpenWrite(FileWriteMode::Append);
  REQUIRE(opened_append.Succeeded());
  OutputStream append = std::move(opened_append).Value();
  REQUIRE(append.Write(Bytes{std::byte{6}, std::byte{7}}).Succeeded());
  REQUIRE(append.Close().Succeeded());
  REQUIRE((destination.ReadBytes().Value() ==
           Bytes{std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}}));

  REQUIRE_FALSE(root.Child("missing.bin").OpenRead().Succeeded());
  REQUIRE_FALSE(root.OpenRead().Succeeded());
  REQUIRE(root.OpenRead().Error().code == IoErrorCode::IsDirectory);
  REQUIRE_FALSE(root.OpenWrite().Succeeded());
  REQUIRE(root.OpenWrite().Error().code == IoErrorCode::IsDirectory);
}

TEST_CASE("AppDirectoriesAreCreatedAndProtected") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  const AppDirectories directories = detail::PrepareAppDirectories({
      .executable_directory = root,
      .data_directory = root.Child("data"),
      .cache_directory = root.Child("cache"),
      .temporary_directory = root.Child("temporary"),
  });

  detail::ProtectAppDirectories(directories);
  REQUIRE(directories.executable_directory == root);
  REQUIRE(directories.data_directory.IsDirectory());
  REQUIRE(directories.cache_directory.IsDirectory());
  REQUIRE(directories.temporary_directory.IsDirectory());
  REQUIRE_FALSE(directories.data_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.cache_directory.DeleteRecursively());
  REQUIRE_FALSE(directories.temporary_directory.Delete());
  REQUIRE_FALSE(directories.temporary_directory.DeleteRecursively());
  REQUIRE_FALSE(root.DeleteRecursively());
}

TEST_CASE("AppDirectoriesProtectTheirAncestors") {
  TemporaryDirectory temporary;
  File root(temporary.Path());
  File application_root = root.Child("application");
  const AppDirectories directories = detail::PrepareAppDirectories({
      .data_directory = application_root.Child("data"),
      .cache_directory = application_root.Child("cache"),
      .temporary_directory = application_root.Child("temporary"),
  });

  detail::ProtectAppDirectories(directories);
  REQUIRE(directories.data_directory.IsDirectory());
  REQUIRE_FALSE(application_root.DeleteRecursively());
  REQUIRE(application_root.IsDirectory());
}

} // namespace huxerui::test
