#include <catch2/catch_amalgamated.hpp>

#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <huxerui/data.h>

namespace huxerui::test {

static_assert(std::is_same_v<Bytes, std::vector<std::byte>>);
static_assert(std::copy_constructible<Uri>);
static_assert(std::move_constructible<Uri>);
static_assert(!std::default_initializable<Uri>);

using TextResult = Result<std::string, int>;
static_assert(!std::default_initializable<TextResult>);
static_assert(!std::default_initializable<Result<void, int>>);
static_assert(!std::constructible_from<Result<int, int>, int>);
static_assert(std::copy_constructible<TextResult>);
static_assert(std::move_constructible<Result<std::unique_ptr<int>, int>>);
static_assert(!std::copy_constructible<Result<std::unique_ptr<int>, int>>);
static_assert(!std::copy_constructible<Result<int, std::unique_ptr<int>>>);
static_assert(std::is_same_v<decltype(std::declval<TextResult&>().Value()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const TextResult&>().Value()), const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<TextResult&&>().Value()), std::string&&>);
static_assert(std::is_same_v<decltype(std::declval<const TextResult&&>().Value()), const std::string&&>);
static_assert(std::is_same_v<decltype(std::declval<TextResult&>().Error()), int&>);
static_assert(std::is_same_v<decltype(std::declval<const TextResult&>().Error()), const int&>);
static_assert(std::is_same_v<decltype(std::declval<TextResult&&>().Error()), int&&>);
static_assert(std::is_same_v<decltype(std::declval<const TextResult&&>().Error()), const int&&>);

TEST_CASE("BufferReferenceRetainsStorageAndSlicesWithoutCopying") {
  int releases = 0;
  auto storage = std::shared_ptr<Bytes>(new Bytes(8), [&releases](Bytes* bytes) {
    ++releases;
    delete bytes;
  });
  BufferReference reference(*storage, storage);
  const auto* address = storage->data();
  REQUIRE(reference.AsBytes().data() == address);
  auto copy = reference;
  auto slice = reference.Slice(2, 4);
  REQUIRE(slice.AsBytes().data() == address + 2);
  REQUIRE(slice.AsBytes().size() == 4);
  REQUIRE(copy == reference);
  REQUIRE(slice == reference.Slice(1, 6).Slice(1, 4));
  REQUIRE_FALSE(slice == reference);
  REQUIRE_FALSE(BufferReference(*storage, storage) == reference);
  (*storage)[2] = std::byte{42};
  REQUIRE(slice.AsBytes()[0] == std::byte{42});
  storage.reset();
  reference = {};
  copy = {};
  REQUIRE(releases == 0);
  auto moved = std::move(slice);
  REQUIRE(slice.AsBytes().empty());
  REQUIRE(slice == BufferReference{});
  REQUIRE(slice.Slice(0, 0) == BufferReference{});
  REQUIRE(moved.AsBytes().data() == address + 2);
  moved = {};
  REQUIRE(releases == 1);
}

TEST_CASE("BufferReferenceValidatesRangesWithoutOverflow") {
  auto storage = std::make_shared<Bytes>(4);
  BufferReference reference(*storage, storage);
  REQUIRE(reference.Slice(4, 0).AsBytes().empty());
  REQUIRE(reference.Slice(4, 0).AsBytes().data() == storage->data() + 4);
  REQUIRE_THROWS_AS(reference.Slice(5, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(reference.Slice(3, 2), std::invalid_argument);
  REQUIRE_THROWS_AS(reference.Slice(1, std::numeric_limits<std::size_t>::max()), std::invalid_argument);
  REQUIRE_THROWS_AS(BufferReference(*storage, {}), std::invalid_argument);
  REQUIRE(BufferReference{}.AsBytes().empty());
  REQUIRE_NOTHROW(BufferReference({}, {}));
  std::weak_ptr<Bytes> weak = storage;
  BufferReference empty({}, storage);
  reference = {};
  storage.reset();
  REQUIRE_FALSE(weak.expired());
  empty = {};
  REQUIRE(weak.expired());
}

TEST_CASE("ResultPreservesValuesAndRejectsWrongBranchAccess") {
  TextResult value("value");
  value.Value() += "!";
  REQUIRE(value.Succeeded());
  REQUIRE(std::as_const(value).Value() == "value!");
  REQUIRE_THROWS_AS(value.Error(), std::logic_error);
  REQUIRE_THROWS_AS(std::as_const(value).Error(), std::logic_error);
  REQUIRE_THROWS_AS(std::move(value).Error(), std::logic_error);

  TextResult error(7);
  error.Error() = 8;
  REQUIRE_FALSE(error.Succeeded());
  REQUIRE(std::as_const(error).Error() == 8);
  REQUIRE_THROWS_AS(error.Value(), std::logic_error);
  REQUIRE_THROWS_AS(std::as_const(error).Value(), std::logic_error);
  REQUIRE_THROWS_AS(std::move(error).Value(), std::logic_error);
}

TEST_CASE("ResultFactoriesDisambiguateIdenticalValueAndErrorTypes") {
  using SameResult = Result<std::string, std::string>;
  auto success = SameResult::Success("same");
  auto failure = SameResult::Failure("same");
  REQUIRE(success.Succeeded());
  REQUIRE_FALSE(failure.Succeeded());
  REQUIRE(success.Value() == failure.Error());
  REQUIRE_THROWS_AS(success.Error(), std::logic_error);
  REQUIRE_THROWS_AS(failure.Value(), std::logic_error);

  auto copy = success;
  copy.Value() = "copy";
  REQUIRE(success.Value() == "same");
  copy = failure;
  REQUIRE_FALSE(copy.Succeeded());
  REQUIRE(copy.Error() == "same");
  failure = std::move(success);
  REQUIRE(failure.Succeeded());
  REQUIRE(failure.Value() == "same");
}

TEST_CASE("ResultOwnsMoveOnlyValuesAndErrors") {
  using OwnedResult = Result<std::unique_ptr<int>, std::unique_ptr<int>>;
  auto success = OwnedResult::Success(std::make_unique<int>(3));
  auto failure = OwnedResult::Failure(std::make_unique<int>(4));
  auto value = std::move(success).Value();
  auto error = std::move(failure).Error();
  REQUIRE(*value == 3);
  REQUIRE(*error == 4);
  REQUIRE(success.Succeeded());
  REQUIRE(success.Value() == nullptr);
  REQUIRE_FALSE(failure.Succeeded());
  REQUIRE(failure.Error() == nullptr);
}

TEST_CASE("ResultVoidUsesExplicitSuccessAndPreservesErrors") {
  using VoidResult = Result<void, std::string>;
  auto success = VoidResult::Success();
  REQUIRE(success.Succeeded());
  REQUIRE_NOTHROW(std::as_const(success).Value());
  REQUIRE_THROWS_AS(success.Error(), std::logic_error);
  REQUIRE_THROWS_AS(std::as_const(success).Error(), std::logic_error);

  auto failure = VoidResult::Failure("failure");
  REQUIRE_FALSE(failure.Succeeded());
  failure.Error() += "!";
  REQUIRE(std::as_const(failure).Error() == "failure!");
  REQUIRE_THROWS_AS(failure.Value(), std::logic_error);
  success = failure;
  REQUIRE_FALSE(success.Succeeded());
  const std::string error = std::move(success).Error();
  REQUIRE(error == "failure!");
  REQUIRE_FALSE(success.Succeeded());
  failure = VoidResult::Success();
  REQUIRE(failure.Succeeded());
  REQUIRE_THROWS_AS(std::move(failure).Error(), std::logic_error);
}

TEST_CASE("ResultVoidSupportsMoveOnlyAndOptionalErrorValues") {
  auto failure = Result<void, std::unique_ptr<int>>::Failure(std::make_unique<int>(5));
  auto error = std::move(failure).Error();
  REQUIRE(*error == 5);
  REQUIRE_FALSE(failure.Succeeded());
  REQUIRE(failure.Error() == nullptr);

  const auto empty_error = Result<void, std::optional<int>>::Failure(std::nullopt);
  REQUIRE_FALSE(empty_error.Succeeded());
  REQUIRE_FALSE(empty_error.Error().has_value());
}

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
