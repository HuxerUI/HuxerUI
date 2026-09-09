#include <catch2/catch_amalgamated.hpp>
#include <huxerui/file.h>

namespace huxerui::test {

TEST_CASE("WindowsFileUriConversionRejectsNonNativePathForms") {
  REQUIRE_THROWS_AS(File(Uri("file:/folder/file.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file:////server/share/file.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file:///C:/folder/a%5Cb.txt")), std::invalid_argument);
}

TEST_CASE("WindowsFileUriConversionMapsDriveAndUncPathsWithoutLegacyForms") {
  const File drive(Uri("file:///C:/folder/item%20one.txt"));
  REQUIRE(drive.Path() == "C:/folder/item one.txt");
  REQUIRE(drive.ToUri().ToString() == "file:///C:/folder/item%20one.txt");
  REQUIRE(File("C:/").ToUri().ToString() == "file:///C:/");

  const File unc(Uri("file://server.example/Share/folder/item%20one.txt"));
  REQUIRE(unc.Path() == "//server.example/Share/folder/item one.txt");
  REQUIRE(unc.ToUri().ToString() == "file://server.example/Share/folder/item%20one.txt");

  const File localhost_unc("//localhost/Share/folder/item.txt");
  const Uri localhost_unc_uri = localhost_unc.ToUri();
  REQUIRE(localhost_unc_uri.ToString() == "file://localhost/Share/folder/item.txt");
  REQUIRE(File(localhost_unc_uri) == localhost_unc);

  const File punctuation_unc("//server.example/Share@team[1]/item.txt");
  const Uri punctuation_unc_uri = punctuation_unc.ToUri();
  REQUIRE(punctuation_unc_uri.ToString() == "file://server.example/Share@team%5B1%5D/item.txt");
  REQUIRE(File(punctuation_unc_uri) == punctuation_unc);

  REQUIRE_THROWS_AS(File(Uri("file:///C|/folder/item.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file://server.example/C:/item.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file://user@server.example/Share/item.txt")), std::invalid_argument);
  REQUIRE_THROWS_AS(File(Uri("file://server.example:445/Share/item.txt")), std::invalid_argument);
}

} // namespace huxerui::test
