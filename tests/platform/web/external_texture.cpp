#include <catch2/catch_amalgamated.hpp>

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <emscripten.h>
#include <emscripten/val.h>

#include <huxerui/web/external_texture.h>

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<web::VideoFrameTexture>);
static_assert(!std::is_copy_assignable_v<web::VideoFrameTexture>);
static_assert(!std::is_move_constructible_v<web::VideoFrameTexture>);
static_assert(!std::is_move_assignable_v<web::VideoFrameTexture>);

// clang-format off
EM_JS(emscripten::EM_VAL, CreateTestVideoFrame, (int timestamp), {
  if (typeof VideoFrame === "undefined") {
    // The Node test runner has no WebCodecs implementation.
    globalThis.VideoFrame = class VideoFrame {
      constructor(_, {timestamp}) {
        this.timestamp = timestamp;
        this.displayWidth = 1;
      }
      clone() {
        if (this.displayWidth === 0) throw new Error("Frame is closed");
        return new VideoFrame(null, {timestamp: this.timestamp});
      }
      close() {
        this.displayWidth = 0;
      }
    };
  }
  return Emval.toHandle(new VideoFrame(new Uint8Array([255, 0, 0, 255]), {
    format: "RGBA", codedWidth: 1, codedHeight: 1, timestamp,
  }));
});
// clang-format on

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

TEST_CASE("WebVideoFrameTextureAcquiresIndependentlyOwnedClones") {
  auto texture = std::make_unique<web::VideoFrameTexture>(Size{1.0F, 1.0F});
  REQUIRE(texture->AcquireFrame().isUndefined());
  auto source = emscripten::val::take_ownership(CreateTestVideoFrame(10));
  texture->Publish(source);
  source.call<void>("close");
  auto first = texture->AcquireFrame();
  auto other = texture->AcquireFrame();
  REQUIRE(first["timestamp"].as<int>() == 10);
  first.call<void>("close");
  REQUIRE(other["displayWidth"].as<int>() == 1);

  source = emscripten::val::take_ownership(CreateTestVideoFrame(20));
  texture->Publish(source);
  source.call<void>("close");
  texture->Finish();
  auto latest = texture->AcquireFrame();
  texture.reset();

  REQUIRE(other["timestamp"].as<int>() == 10);
  REQUIRE(other["displayWidth"].as<int>() == 1);
  REQUIRE(latest["timestamp"].as<int>() == 20);
  REQUIRE(latest["displayWidth"].as<int>() == 1);
  other.call<void>("close");
  latest.call<void>("close");
}

} // namespace
} // namespace huxerui::test
