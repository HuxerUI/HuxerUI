#include <catch2/catch_amalgamated.hpp>

#include <limits>
#include <stdexcept>
#include <type_traits>

#include <huxerui/external_texture.h>

#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

static_assert(std::is_copy_constructible_v<ExternalTexture>);
static_assert(std::is_copy_assignable_v<ExternalTexture>);
static_assert(std::is_nothrow_move_constructible_v<ExternalTexture>);
static_assert(std::is_nothrow_move_assignable_v<ExternalTexture>);

TEST_CASE("ExternalTextureDistinguishesEmptyAndLiveValues") {
  const ExternalTexture empty;
  const ExternalTexture texture = MakeTestExternalTexture({640.0F, 480.0F});
  const ExternalTexture copy = texture;
  const ExternalTexture other = MakeTestExternalTexture({640.0F, 480.0F});

  REQUIRE_FALSE(empty.HasValue());
  REQUIRE(empty.IntrinsicSize() == Size{});
  REQUIRE(texture.HasValue());
  REQUIRE(texture.IntrinsicSize() == Size{640.0F, 480.0F});
  REQUIRE(copy == texture);
  REQUIRE(other != texture);
}

TEST_CASE("ExternalTextureRequiresPositiveFiniteIntrinsicSize") {
  REQUIRE_THROWS_AS(std::make_shared<ExternalTextureTestState>(Size{0.0F, 480.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(std::make_shared<ExternalTextureTestState>(Size{640.0F, -1.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      std::make_shared<ExternalTextureTestState>(Size{std::numeric_limits<float>::infinity(), 480.0F}),
      std::invalid_argument
  );
}

} // namespace
} // namespace huxerui::test
