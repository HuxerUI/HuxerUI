#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

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

std::shared_ptr<ExternalTexture> mac_external_texture;

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

id<MTLTexture> CreateMetalTexture(id<MTLDevice> device, MTLPixelFormat format) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                         width:2 height:2 mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (texture == nil) {
    throw std::runtime_error("HuxerUI test could not create a Metal texture");
  }
  return texture;
}

void ReplaceMetalPixels(id<MTLTexture> texture, const std::array<std::uint8_t, 16>& pixels) {
  [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:8];
}

std::array<std::uint8_t, 16> SolidMetalPixels(std::array<std::uint8_t, 4> bgra) {
  std::array<std::uint8_t, 16> pixels{};
  for (std::size_t offset = 0; offset < pixels.size(); offset += bgra.size()) {
    std::copy(bgra.begin(), bgra.end(), pixels.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return pixels;
}

std::array<std::uint8_t, 16> RenderPixels(detail::AppKitRenderer& renderer, const RenderFrame& frame) {
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
  return pixels;
}

std::array<std::uint8_t, 4> PixelAt(const std::array<std::uint8_t, 16>& pixels, std::size_t x, std::size_t y) {
  const std::size_t offset = (y * 2 + x) * 4;
  return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

std::array<std::uint8_t, 4> RenderPixel(detail::AppKitRenderer& renderer, const RenderFrame& frame) {
  const std::array<std::uint8_t, 16> pixels = RenderPixels(renderer, frame);
  return {pixels[0], pixels[1], pixels[2], pixels[3]};
}

bool Near(std::uint8_t value, std::uint8_t expected) {
  const int difference = static_cast<int>(value) - static_cast<int>(expected);
  return difference >= -2 && difference <= 2;
}

TEST_CASE("MacExternalTexturePublishesLatestFrameThroughAppKitRenderer") {
  @autoreleasepool {
    const auto texture = std::make_shared<macos::PixelBufferTexture>(Size{2.0F, 2.0F});
    mac_external_texture = texture;
    REQUIRE_THROWS_AS(texture->Publish(nullptr), std::invalid_argument);

    CVPixelBufferRef red = CreatePixelBuffer({0, 0, 255, 255});
    CVPixelBufferRef green = CreatePixelBuffer({0, 255, 0, 255});
    texture->Publish(red);
    texture->Publish(green);
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
    detail::AppKitRenderer other_renderer;
    const std::array<std::uint8_t, 4> other_pixel = RenderPixel(other_renderer, initial);
    REQUIRE(Near(other_pixel[0], 0));
    REQUIRE(Near(other_pixel[1], 255));
    REQUIRE(Near(other_pixel[2], 0));
    REQUIRE(Near(other_pixel[3], 255));

    const int requests_before_publish = platform.requested_frames;
    CVPixelBufferRef blue = CreatePixelBuffer({255, 0, 0, 255});
    texture->Publish(blue);
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
    texture->Publish(yellow);
    texture->Finish();
    CVPixelBufferRelease(yellow);
    platform.RunPlatformModuleTasks();
    const RenderFrame& finished = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 4> finished_pixel = RenderPixel(renderer, finished);
    REQUIRE(Near(finished_pixel[0], 255));
    REQUIRE(Near(finished_pixel[1], 255));
    REQUIRE(Near(finished_pixel[2], 0));
    REQUIRE(Near(finished_pixel[3], 255));

    CVPixelBufferRef rejected = CreatePixelBuffer({0, 0, 0, 255});
    REQUIRE_THROWS_AS(texture->Publish(rejected), std::logic_error);
    CVPixelBufferRelease(rejected);
  }
}

TEST_CASE("MacPixelBufferTextureIsTheSharedExternalTextureIdentity") {
  const std::shared_ptr<ExternalTexture> texture = std::make_shared<macos::PixelBufferTexture>(Size{16.0F, 9.0F});

  REQUIRE(texture->IntrinsicSize() == Size{16.0F, 9.0F});
  const auto pixel_buffer_texture = std::dynamic_pointer_cast<macos::PixelBufferTexture>(texture);
  REQUIRE(pixel_buffer_texture != nullptr);
  pixel_buffer_texture->Finish();
  REQUIRE_THROWS_AS(pixel_buffer_texture->Publish(nullptr), std::invalid_argument);
}

TEST_CASE("MacMetalTextureCopiesPublishedFramesForEveryRenderer") {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLTexture> source = CreateMetalTexture(device, MTLPixelFormatBGRA8Unorm);
    ReplaceMetalPixels(source, SolidMetalPixels({0, 0, 255, 255}));

    const auto texture = std::make_shared<macos::MetalTexture>(Size{2.0F, 2.0F});
    const std::shared_ptr<ExternalTexture> identity = texture;
    REQUIRE(identity->IntrinsicSize() == Size{2.0F, 2.0F});
    REQUIRE(std::dynamic_pointer_cast<macos::MetalTexture>(identity) == texture);
    REQUIRE_THROWS_AS(texture->Publish({nil}), std::invalid_argument);
    const std::uint64_t initial_revision = texture->Revision();
    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque});
    REQUIRE(texture->Revision() == initial_revision + 1);

    ReplaceMetalPixels(source, SolidMetalPixels({0, 255, 0, 255}));
    mac_external_texture = texture;
    TestPlatform platform;
    Runtime runtime{MacExternalTextureApp, platform};
    runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
    const RenderFrame& initial = runtime.BuildRenderFrame();
    detail::AppKitRenderer renderer;
    const std::array<std::uint8_t, 4> snapshot_pixel = RenderPixel(renderer, initial);
    REQUIRE(Near(snapshot_pixel[0], 255));
    REQUIRE(Near(snapshot_pixel[1], 0));
    REQUIRE(Near(snapshot_pixel[2], 0));
    REQUIRE(Near(snapshot_pixel[3], 255));

    detail::AppKitRenderer other_renderer;
    const std::array<std::uint8_t, 4> other_pixel = RenderPixel(other_renderer, initial);
    REQUIRE(Near(other_pixel[0], 255));
    REQUIRE(Near(other_pixel[1], 0));
    REQUIRE(Near(other_pixel[2], 0));
    REQUIRE(Near(other_pixel[3], 255));

    const int requests_before_publish = platform.requested_frames;
    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque});
    REQUIRE(platform.requested_frames == requests_before_publish);
    platform.RunPlatformModuleTasks();
    REQUIRE(platform.requested_frames == requests_before_publish + 1);
    const RenderFrame& updated = runtime.BuildRenderFrame();
    REQUIRE_FALSE(updated.damage.full);
    const std::array<std::uint8_t, 4> updated_pixel = RenderPixel(renderer, updated);
    REQUIRE(Near(updated_pixel[0], 0));
    REQUIRE(Near(updated_pixel[1], 255));
    REQUIRE(Near(updated_pixel[2], 0));
    REQUIRE(Near(updated_pixel[3], 255));

    texture->Finish();
    const macos::MetalTexture::Frame finished_frame{
        source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque};
    REQUIRE_THROWS_AS(texture->Publish(finished_frame), std::logic_error);
    const std::array<std::uint8_t, 4> finished_pixel = RenderPixel(renderer, updated);
    REQUIRE(Near(finished_pixel[0], 0));
    REQUIRE(Near(finished_pixel[1], 255));
    REQUIRE(Near(finished_pixel[2], 0));
    REQUIRE(Near(finished_pixel[3], 255));
    mac_external_texture.reset();
  }
}

TEST_CASE("MacMetalTextureAppliesOriginAndAlphaMetadata") {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLTexture> source = CreateMetalTexture(device, MTLPixelFormatBGRA8Unorm);
    const std::array<std::uint8_t, 16> oriented_pixels{
        0, 0, 255, 255, 0, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    ReplaceMetalPixels(source, oriented_pixels);
    const auto texture = std::make_shared<macos::MetalTexture>(Size{2.0F, 2.0F});
    mac_external_texture = texture;
    TestPlatform platform;
    Runtime runtime{MacExternalTextureApp, platform};
    runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
    detail::AppKitRenderer renderer;

    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque});
    const RenderFrame& top_left_frame = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 16> top_left_pixels = RenderPixels(renderer, top_left_frame);
    const std::array<std::uint8_t, 4> top_left_top = PixelAt(top_left_pixels, 0, 0);
    const std::array<std::uint8_t, 4> top_left_bottom = PixelAt(top_left_pixels, 0, 1);
    REQUIRE(Near(top_left_top[0], 255));
    REQUIRE(Near(top_left_top[1], 0));
    REQUIRE(Near(top_left_bottom[0], 0));
    REQUIRE(Near(top_left_bottom[1], 255));

    texture->Publish({source, macos::MetalTexture::Origin::BottomLeft, macos::MetalTexture::Alpha::Opaque});
    platform.RunPlatformModuleTasks();
    const RenderFrame& bottom_left_frame = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 16> bottom_left_pixels = RenderPixels(renderer, bottom_left_frame);
    const std::array<std::uint8_t, 4> bottom_left_top = PixelAt(bottom_left_pixels, 0, 0);
    const std::array<std::uint8_t, 4> bottom_left_bottom = PixelAt(bottom_left_pixels, 0, 1);
    REQUIRE(Near(bottom_left_top[0], 0));
    REQUIRE(Near(bottom_left_top[1], 255));
    REQUIRE(Near(bottom_left_bottom[0], 255));
    REQUIRE(Near(bottom_left_bottom[1], 0));

    ReplaceMetalPixels(source, SolidMetalPixels({0, 0, 128, 128}));
    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Premultiplied});
    platform.RunPlatformModuleTasks();
    const RenderFrame& premultiplied_frame = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 4> premultiplied = RenderPixel(renderer, premultiplied_frame);

    ReplaceMetalPixels(source, SolidMetalPixels({0, 0, 255, 128}));
    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Straight});
    platform.RunPlatformModuleTasks();
    const RenderFrame& straight_frame = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 4> straight = RenderPixel(renderer, straight_frame);
    REQUIRE(Near(straight[0], premultiplied[0]));
    REQUIRE(Near(straight[1], premultiplied[1]));
    REQUIRE(Near(straight[2], premultiplied[2]));
    REQUIRE(Near(straight[3], premultiplied[3]));

    ReplaceMetalPixels(source, SolidMetalPixels({0, 0, 200, 255}));
    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque});
    platform.RunPlatformModuleTasks();
    const RenderFrame& opaque_reference_frame = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 4> opaque_reference = RenderPixel(renderer, opaque_reference_frame);

    ReplaceMetalPixels(source, SolidMetalPixels({0, 0, 200, 0}));
    texture->Publish({source, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque});
    platform.RunPlatformModuleTasks();
    const RenderFrame& opaque_ignored_alpha_frame = runtime.BuildRenderFrame();
    const std::array<std::uint8_t, 4> opaque_ignored_alpha = RenderPixel(renderer, opaque_ignored_alpha_frame);
    REQUIRE(Near(opaque_ignored_alpha[0], opaque_reference[0]));
    REQUIRE(Near(opaque_ignored_alpha[1], opaque_reference[1]));
    REQUIRE(Near(opaque_ignored_alpha[2], opaque_reference[2]));
    REQUIRE(Near(opaque_ignored_alpha[3], opaque_reference[3]));
    mac_external_texture.reset();
  }
}

TEST_CASE("MacMetalTextureRejectsUnsupportedFramesWithoutAdvancing") {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLTexture> valid = CreateMetalTexture(device, MTLPixelFormatBGRA8Unorm);
    id<MTLTexture> rgba = CreateMetalTexture(device, MTLPixelFormatRGBA8Unorm);
    ReplaceMetalPixels(rgba, SolidMetalPixels({255, 0, 0, 255}));
    id<MTLTexture> unsupported = CreateMetalTexture(device, MTLPixelFormatR8Unorm);
    const auto texture = std::make_shared<macos::MetalTexture>(Size{2.0F, 2.0F});
    texture->Publish({rgba, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Premultiplied});
    const std::uint64_t published_revision = texture->Revision();
    const macos::MetalTexture::Frame unsupported_frame{
        unsupported, macos::MetalTexture::Origin::TopLeft, macos::MetalTexture::Alpha::Opaque};
    REQUIRE_THROWS_AS(texture->Publish(unsupported_frame), std::invalid_argument);
    const macos::MetalTexture::Frame invalid_origin_frame{
        valid, static_cast<macos::MetalTexture::Origin>(99), macos::MetalTexture::Alpha::Premultiplied};
    REQUIRE_THROWS_AS(texture->Publish(invalid_origin_frame), std::invalid_argument);
    const macos::MetalTexture::Frame invalid_alpha_frame{
        valid, macos::MetalTexture::Origin::TopLeft, static_cast<macos::MetalTexture::Alpha>(99)};
    REQUIRE_THROWS_AS(texture->Publish(invalid_alpha_frame), std::invalid_argument);
    REQUIRE(texture->Revision() == published_revision);
  }
}

} // namespace
} // namespace huxerui::test
