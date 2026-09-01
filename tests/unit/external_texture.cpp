#include <catch2/catch_amalgamated.hpp>

#include <limits>
#include <stdexcept>
#include <type_traits>

#include <huxerui/external_texture.h>

#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<ExternalTexture>);
static_assert(!std::is_copy_assignable_v<ExternalTexture>);

TEST_CASE("ExternalTextureHasStableSharedIdentity") {
  const auto texture = std::make_shared<ExternalTextureTestTexture>(Size{640.0F, 480.0F});
  const std::shared_ptr<ExternalTexture> copy = texture;
  const std::shared_ptr<ExternalTexture> other = MakeTestExternalTexture({640.0F, 480.0F});

  REQUIRE(texture->IntrinsicSize() == Size{640.0F, 480.0F});
  REQUIRE(copy == texture);
  REQUIRE(other != texture);
}

TEST_CASE("ExternalTextureRequiresPositiveFiniteIntrinsicSize") {
  REQUIRE_THROWS_AS(std::make_shared<ExternalTextureTestTexture>(Size{0.0F, 480.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(std::make_shared<ExternalTextureTestTexture>(Size{640.0F, -1.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      std::make_shared<ExternalTextureTestTexture>(Size{std::numeric_limits<float>::infinity(), 480.0F}),
      std::invalid_argument
  );
}

} // namespace
} // namespace huxerui::test
