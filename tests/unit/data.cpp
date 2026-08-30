#include <catch2/catch_amalgamated.hpp>

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <huxerui/data.h>

namespace huxerui::test {

static_assert(std::is_same_v<Bytes, std::vector<std::byte>>);
static_assert(std::copy_constructible<Uri>);
static_assert(std::move_constructible<Uri>);
static_assert(!std::default_initializable<Uri>);

TEST_CASE("BytesOwnsMutableContiguousBinaryData") {
  Bytes bytes{std::byte{0}, std::byte{0xFF}};
  bytes.push_back(std::byte{'a'});

  REQUIRE(bytes.size() == 3);
  REQUIRE(bytes.data()[0] == std::byte{0});
  REQUIRE(bytes.data()[1] == std::byte{0xFF});
  REQUIRE(bytes.data()[2] == std::byte{'a'});
}

TEST_CASE("UriPreservesAbsoluteSyntaxAndExposesPresentComponents") {
  const Uri uri("Custom://user@example.test:42/documents/%E6%B5%8B%E8%AF%95?first=1&first=2#part");

  REQUIRE(uri.Scheme() == "Custom");
  REQUIRE(uri.Authority() == "user@example.test:42");
  REQUIRE(uri.Path() == "/documents/%E6%B5%8B%E8%AF%95");
  REQUIRE(uri.Query() == "first=1&first=2");
  REQUIRE(uri.Fragment() == "part");
  REQUIRE(
      uri.ToString() ==
      "Custom://user@example.test:42/documents/%E6%B5%8B%E8%AF%95?first=1&first=2#part"
  );
}

TEST_CASE("UriDistinguishesAbsentAndPresentEmptyComponents") {
  const Uri opaque("custom:value");
  REQUIRE_FALSE(opaque.Authority().has_value());
  REQUIRE(opaque.Path() == "value");
  REQUIRE_FALSE(opaque.Query().has_value());
  REQUIRE_FALSE(opaque.Fragment().has_value());

  const Uri empty("custom://?#");
  REQUIRE(empty.Authority().has_value());
  REQUIRE(empty.Authority()->empty());
  REQUIRE(empty.Path().empty());
  REQUIRE(empty.Query().has_value());
  REQUIRE(empty.Query()->empty());
  REQUIRE(empty.Fragment().has_value());
  REQUIRE(empty.Fragment()->empty());
}

TEST_CASE("UriEqualityIsLexicalAndDoesNotNormalize") {
  const Uri encoded("https://example.test/%41");
  const Uri literal("https://example.test/A");
  const Uri scheme_case("HTTPS://example.test/%41");

  REQUIRE(encoded == Uri("https://example.test/%41"));
  REQUIRE_FALSE(encoded == literal);
  REQUIRE_FALSE(encoded == scheme_case);
}

TEST_CASE("UriRejectsInvalidGenericSyntaxWithoutLeakingInput") {
  REQUIRE_THROWS_AS(Uri(""), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("relative/path"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("1custom:value"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("custom://example.test/a b"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("custom://example.test/%"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("custom://example.test/%GG"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("custom://[invalid"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri("custom:value#first#second"), std::invalid_argument);
  REQUIRE_THROWS_AS(Uri(std::string("custom:") + "\xE6\xB5\x8B\xE8\xAF\x95"), std::invalid_argument);
}

TEST_CASE("UriParseReturnsNoValueOnlyForValidationFailure") {
  const std::optional<Uri> parsed = Uri::Parse("https://[::1]:8443/path?value=1");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->Authority() == "[::1]:8443");
  REQUIRE_FALSE(Uri::Parse("not a URI").has_value());
}

} // namespace huxerui::test
