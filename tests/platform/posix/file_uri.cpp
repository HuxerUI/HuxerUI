#include <catch2/catch_amalgamated.hpp>
#include <huxerui/file.h>

namespace huxerui::test {

TEST_CASE("PosixFileUriConversionUsesLocalAbsolutePaths") {
  REQUIRE(File(Uri("file:/tmp/item.txt")) == File("/tmp/item.txt"));
  REQUIRE(File("/").ToUri().ToString() == "file:///");
  REQUIRE_THROWS_AS(File(Uri("file://server/share/file.txt")), std::invalid_argument);
}

} // namespace huxerui::test
