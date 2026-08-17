#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>

#include "appkit_renderer.h"
#include "runtime_test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include <huxerui/macos/external_texture.h>

namespace huxerui::test {
namespace {

ExternalTexture mac_external_texture;

View MacExternalTextureApp() {
  return Column {
    Image(mac_external_texture).Fit(ImageFit::Fill).With(Frame{2.0F, 2.0F}),
  };
}

CVPixelBufferRef CreatePixelBuffer(std::array<std::uint8_t, 4> bgra) {
  CVPixelBufferRef pixel_buffer = nullptr;
  const CVReturn result = CVPixelBufferCreate(
      kCFAllocatorDefault,
      2,
      2,
      kCVPixelFormatType_32BGRA,
      nullptr,
      &pixel_buffer
  );
  if (result != kCVReturnSuccess || pixel_buffer == nullptr) {
    throw std::runtime_error("HuxerUI test could not create a CVPixelBuffer");
  }
  if (CVPixelBufferLockBaseAddress(pixel_buffer, 0) != kCVReturnSuccess) {
    CVPixelBufferRelease(pixel_buffer);
    throw std::runtime_error("HuxerUI test could not lock a CVPixelBuffer");
  }
  auto* bytes = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
  const std::size_t bytes_per_row = CVPixelBufferGetBytesPerRow(pixel_buffer);
  for (std::size_t y = 0; y < 2; ++y) {
    for (std::size_t x = 0; x < 2; ++x) {
      std::copy(bgra.begin(), bgra.end(), bytes + y * bytes_per_row + x * 4);
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
  return pixel_buffer;
}

std::array<std::uint8_t, 4> RenderPixel(detail::AppKitRenderer& renderer, const RenderFrame& frame) {
  std::array<std::uint8_t, 16> pixels{};
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(
      pixels.data(),
      2,
      2,
      8,
      8,
      color_space,
      static_cast<CGBitmapInfo>(
          static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
          static_cast<std::uint32_t>(kCGBitmapByteOrder32Big)
      )
  );
  CGColorSpaceRelease(color_space);
  if (context == nullptr) {
    throw std::runtime_error("HuxerUI test could not create a bitmap context");
  }
  renderer.Draw(context, CGRectMake(0.0, 0.0, 2.0, 2.0), &frame);
  CGContextRelease(context);
  return {pixels[0], pixels[1], pixels[2], pixels[3]};
}

bool Near(std::uint8_t value, std::uint8_t expected) {
  const int difference = static_cast<int>(value) - static_cast<int>(expected);
  return difference >= -2 && difference <= 2;
}

TEST_CASE("MacExternalTexturePublishesLatestFrameThroughAppKitRenderer") {
  @autoreleasepool {
    macos::ExternalTextureSource source({2.0F, 2.0F});
    mac_external_texture = source.Texture();
    REQUIRE(mac_external_texture.HasValue());
    REQUIRE_THROWS_AS(source.Publish(nullptr), std::invalid_argument);

    CVPixelBufferRef red = CreatePixelBuffer({0, 0, 255, 255});
    CVPixelBufferRef green = CreatePixelBuffer({0, 255, 0, 255});
    source.Publish(red);
    source.Publish(green);
    CVPixelBufferRelease(red);
    CVPixelBufferRelease(green);

    TestPlatform platform;
    Runtime runtime{MacExternalTextureApp, platform};
    runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
    const RenderFrame& initial = runtime.BuildRenderFrame();
    detail::AppKitRenderer renderer;
    const std::array<std::uint8_t, 4> initial_pixel = RenderPixel(renderer, initial);
    REQUIRE(Near(initial_pixel[0], 0));
    REQUIRE(Near(initial_pixel[1], 255));
    REQUIRE(Near(initial_pixel[2], 0));
    REQUIRE(Near(initial_pixel[3], 255));

    const int requests_before_publish = platform.requested_frames;
    CVPixelBufferRef blue = CreatePixelBuffer({255, 0, 0, 255});
    source.Publish(blue);
    CVPixelBufferRelease(blue);
    REQUIRE(platform.requested_frames == requests_before_publish);
    platform.RunPlatformModuleTasks();
    REQUIRE(platform.requested_frames == requests_before_publish + 1);

    const RenderFrame& updated = runtime.BuildRenderFrame();
    REQUIRE_FALSE(updated.damage.full);
    const std::array<std::uint8_t, 4> updated_pixel = RenderPixel(renderer, updated);
    REQUIRE(Near(updated_pixel[0], 0));
    REQUIRE(Near(updated_pixel[1], 0));
    REQUIRE(Near(updated_pixel[2], 255));
    REQUIRE(Near(updated_pixel[3], 255));

    CVPixelBufferRef yellow = CreatePixelBuffer({0, 255, 255, 255});
    source.Publish(yellow);
    source.Finish();
    CVPixelBufferRelease(yellow);
    platform.RunPlatformModuleTasks();
    const RenderFrame& finished = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 4> finished_pixel = RenderPixel(renderer, finished);
    REQUIRE(Near(finished_pixel[0], 255));
    REQUIRE(Near(finished_pixel[1], 255));
    REQUIRE(Near(finished_pixel[2], 0));
    REQUIRE(Near(finished_pixel[3], 255));

    CVPixelBufferRef rejected = CreatePixelBuffer({0, 0, 0, 255});
    REQUIRE_THROWS_AS(source.Publish(rejected), std::logic_error);
    CVPixelBufferRelease(rejected);
  }
}

TEST_CASE("MacExternalTextureSourceIsMoveOnlyAndPreservesItsConsumer") {
  macos::ExternalTextureSource source({16.0F, 9.0F});
  const ExternalTexture texture = source.Texture();
  macos::ExternalTextureSource moved(std::move(source));

  REQUIRE_FALSE(source.Texture().HasValue());
  REQUIRE(moved.Texture() == texture);
  REQUIRE_THROWS_AS(source.Publish(nullptr), std::logic_error);
  moved.Finish();
  moved.Finish();
}

} // namespace
} // namespace huxerui::test
