#include <catch2/catch_amalgamated.hpp>

#include <cstddef>
#include <type_traits>
#include <vector>

#include <huxerui/data.h>

namespace huxerui::test {

static_assert(std::is_same_v<Bytes, std::vector<std::byte>>);

TEST_CASE("BytesOwnsMutableContiguousBinaryData") {
  Bytes bytes{std::byte{0}, std::byte{0xFF}};
  bytes.push_back(std::byte{'a'});

  REQUIRE(bytes.size() == 3);
  REQUIRE(bytes.data()[0] == std::byte{0});
  REQUIRE(bytes.data()[1] == std::byte{0xFF});
  REQUIRE(bytes.data()[2] == std::byte{'a'});
}

} // namespace huxerui::test
