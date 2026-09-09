#include <catch2/catch_amalgamated.hpp>

#include <stdexcept>
#include <type_traits>
#include <utility>

#include <emscripten/val.h>

#include <huxerui/web/external_texture.h>

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<web::VideoFrameTexture>);
static_assert(!std::is_copy_assignable_v<web::VideoFrameTexture>);
static_assert(!std::is_move_constructible_v<web::VideoFrameTexture>);
static_assert(!std::is_move_assignable_v<web::VideoFrameTexture>);

TEST_CASE("WebVideoFrameTextureIsTheSharedExternalTextureIdentity") {
  const std::shared_ptr<ExternalTexture> texture =
      std::make_shared<web::VideoFrameTexture>(Size{320.0F, 180.0F});
  const auto video_frame_texture = std::dynamic_pointer_cast<web::VideoFrameTexture>(texture);

  REQUIRE(texture->IntrinsicSize() == Size{320.0F, 180.0F});
  REQUIRE(video_frame_texture != nullptr);
  video_frame_texture->Finish();
  video_frame_texture->Finish();
  REQUIRE_THROWS_AS(video_frame_texture->Publish(emscripten::val::undefined()), std::logic_error);
}

TEST_CASE("WebVideoFrameTextureRejectsValuesThatAreNotOpenVideoFrames") {
  const auto texture = std::make_shared<web::VideoFrameTexture>(Size{16.0F, 9.0F});
  REQUIRE_THROWS_AS(texture->Publish(emscripten::val::undefined()), std::invalid_argument);
}

} // namespace
} // namespace huxerui::test
