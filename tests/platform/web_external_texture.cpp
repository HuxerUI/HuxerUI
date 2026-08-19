#include <catch2/catch_amalgamated.hpp>

#include <stdexcept>
#include <type_traits>
#include <utility>

#include <emscripten/val.h>

#include <huxerui/web/external_texture.h>

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<web::ExternalTextureSource>);
static_assert(!std::is_copy_assignable_v<web::ExternalTextureSource>);
static_assert(std::is_nothrow_move_constructible_v<web::ExternalTextureSource>);
static_assert(std::is_nothrow_move_assignable_v<web::ExternalTextureSource>);

TEST_CASE("WebExternalTextureSourceOwnsAStableMoveOnlyCapability") {
  web::ExternalTextureSource source({320.0F, 180.0F});
  const ExternalTexture texture = source.Texture();

  REQUIRE(texture.HasValue());
  REQUIRE(texture.IntrinsicSize() == Size{320.0F, 180.0F});

  web::ExternalTextureSource moved(std::move(source));
  REQUIRE_FALSE(source.Texture().HasValue());
  REQUIRE(moved.Texture() == texture);
  REQUIRE_THROWS_AS(source.Publish(emscripten::val::undefined()), std::logic_error);

  moved.Finish();
  moved.Finish();
  REQUIRE_THROWS_AS(moved.Publish(emscripten::val::undefined()), std::logic_error);
}

TEST_CASE("WebExternalTextureSourceRejectsValuesThatAreNotOpenVideoFrames") {
  web::ExternalTextureSource source({16.0F, 9.0F});
  REQUIRE_THROWS_AS(source.Publish(emscripten::val::undefined()), std::invalid_argument);
}

} // namespace
} // namespace huxerui::test
