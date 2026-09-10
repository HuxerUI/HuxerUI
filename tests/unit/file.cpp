#include <catch2/catch_amalgamated.hpp>

#include <huxerui/file.h>

#include <concepts>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

std::string Utf8Path(const fs::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

} // namespace

static_assert(std::copy_constructible<File>);
static_assert(std::move_constructible<File>);
static_assert(!std::default_initializable<File>);
static_assert(std::constructible_from<File, const Uri&>);
static_assert(std::copy_constructible<AppDirectories>);
static_assert(std::is_same_v<IoResult<std::string>, Result<std::string, IoError>>);
static_assert(std::is_same_v<IoResult<void>, Result<void, IoError>>);

TEST_CASE("IoResultDistinguishesValuesFromErrors") {
  IoResult<std::string> value(std::string{"value"});
  REQUIRE(value.Succeeded());
  REQUIRE(value.Value() == "value");
  REQUIRE_THROWS_AS(value.Error(), std::logic_error);

  IoResult<std::string> error(IoError{IoErrorCode::NotFound, "missing"});
  REQUIRE_FALSE(error.Succeeded());
  REQUIRE(error.Error().code == IoErrorCode::NotFound);
  REQUIRE_THROWS_AS(error.Value(), std::logic_error);
}

TEST_CASE("FileNormalizesUtf8PathsAndProvidesLexicalOperations") {
  File root(Utf8Path(fs::absolute("huxerui-file-tests")));
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

TEST_CASE("FileUriConversionPreservesAbsoluteUtf8PathsWithoutFilesystemAccess") {
  const File file(Utf8Path(fs::absolute(u8"huxerui-uri-tests/folder with space/100%/报告.txt")));
  const Uri uri = file.ToUri();

  REQUIRE(uri.Scheme() == "file");
  REQUIRE(uri.Authority().has_value());
  REQUIRE(uri.Authority()->empty());
  REQUIRE(uri.Query() == std::nullopt);
  REQUIRE(uri.Fragment() == std::nullopt);
  REQUIRE(uri.ToString().find("folder%20with%20space/100%25/%E6%8A%A5%E5%91%8A.txt") != std::string::npos);
  REQUIRE(File(uri) == file);

  const Uri localhost("file://localhost" + std::string(uri.Path()));
  REQUIRE(File(localhost) == file);
}

TEST_CASE("FileUriConversionRejectsSchemesAndStructureThatDoNotIdentifyLocalFiles") {
  REQUIRE_THROWS_AS(File(Uri("https://example.test/file.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file:///folder/a%2Fb.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file:///folder/a%00b.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file:///folder/file.txt?query")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file:///folder/file.txt#fragment")), std::invalid_argument);
}

} // namespace huxerui::test
