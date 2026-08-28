#include "linux_internal.h"

#include <catch2/catch_amalgamated.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <huxerui/linux/external_texture.h>

#include "external_texture_internal.h"
#include "linux_external_texture_internal.h"
#include "linux_renderer.h"
#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

constexpr std::uint32_t kDefaultBackgroundPixel = 0xFFF7F8FAU;

static_assert(!std::is_copy_constructible_v<linux::ExternalTextureSource>);
static_assert(!std::is_copy_assignable_v<linux::ExternalTextureSource>);
static_assert(std::is_nothrow_move_constructible_v<linux::ExternalTextureSource>);
static_assert(std::is_nothrow_move_assignable_v<linux::ExternalTextureSource>);

std::shared_ptr<detail::LinuxExternalTextureState> StateFor(const ExternalTexture& texture) {
  return std::dynamic_pointer_cast<detail::LinuxExternalTextureState>(detail::ExternalTextureState::From(texture));
}

std::uint32_t PixelAt(const detail::LinuxExternalTextureFrame& frame, int x, int y) {
  std::uint32_t pixel = 0;
  const std::span<const std::byte> pixels = frame.Pixels();
  const std::size_t offset = static_cast<std::size_t>(y) * frame.BytesPerRow() + static_cast<std::size_t>(x) * 4U;
  std::memcpy(&pixel, pixels.data() + offset, sizeof(pixel));
  return pixel;
}

ExternalTexture scheduled_texture;
ExternalTexture rendered_texture;

View LinuxExternalTextureApp() {
  return Image(scheduled_texture).Fit(ImageFit::Fill).With(Frame{2.0F, 2.0F});
}

View LinuxExternalTextureRenderApp() {
  return Canvas(
             [](PaintContext& paint, Size) {
               paint.DrawImageRect(
                   rendered_texture,
                   {1.0F, 0.0F, 1.0F, 1.0F},
                   {2.0F, 1.0F, 2.0F, 2.0F},
                   ImageSampling::Nearest,
                   1.0F
               );
               paint.DrawImageRect(
                   rendered_texture,
                   {0.0F, 0.0F, 1.0F, 1.0F},
                   {0.0F, 3.0F, 1.0F, 1.0F},
                   ImageSampling::Nearest,
                   0.5F
               );
             }
  ).With(Frame{5.0F, 4.0F});
}

std::array<std::uint32_t, 20> RenderPixels(detail::LinuxRenderer& renderer, const RenderFrame& frame) {
  std::array<std::uint32_t, 20> pixels{};
  SDL_Surface* surface = SDL_CreateSurfaceFrom(5, 4, SDL_PIXELFORMAT_ARGB8888, pixels.data(), 5 * 4);
  REQUIRE(surface != nullptr);
  renderer.Draw(surface, frame);
  SDL_DestroySurface(surface);
  return pixels;
}

TEST_CASE("LinuxExternalTextureCopiesAndConvertsTheLatestPixelFrame") {
  linux::ExternalTextureSource source({16.0F, 9.0F});
  const ExternalTexture texture = source.Texture();
  const std::shared_ptr<detail::LinuxExternalTextureState> state = StateFor(texture);
  REQUIRE(state != nullptr);
  REQUIRE(texture.IntrinsicSize() == Size{16.0F, 9.0F});

  std::array<std::byte, 12> rgba{
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{0},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{128},
  };
  source.Publish({
      .pixel_width = 1,
      .pixel_height = 2,
      .bytes_per_row = 8,
      .format = linux::ExternalTexturePixelFormat::Rgba8888,
      .pixels = rgba,
  });
  rgba.fill(std::byte{0});

  const std::optional<detail::LinuxExternalTextureFrame> frame = state->AcquireLatestFrame();
  REQUIRE(frame.has_value());
  REQUIRE(frame->PixelWidth() == 1);
  REQUIRE(frame->PixelHeight() == 2);
  REQUIRE(frame->BytesPerRow() == 4);
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFFFF0000U);
  REQUIRE(PixelAt(*frame, 0, 1) == 0x80008000U);
  REQUIRE_FALSE(state->AcquireLatestFrame().has_value());
}

TEST_CASE("LinuxExternalTextureUsesALatestWinsMailboxAndAcceptsBgra") {
  linux::ExternalTextureSource source({1.0F, 1.0F});
  const std::shared_ptr<detail::LinuxExternalTextureState> state = StateFor(source.Texture());
  const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
  const std::array<std::byte, 4> blue_bgra{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};

  source.Publish({1, 1, 4, linux::ExternalTexturePixelFormat::Rgba8888, red});
  source.Publish({1, 1, 4, linux::ExternalTexturePixelFormat::Bgra8888, blue_bgra});

  const std::optional<detail::LinuxExternalTextureFrame> frame = state->AcquireLatestFrame();
  REQUIRE(frame.has_value());
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFF0000FFU);
  REQUIRE_FALSE(state->AcquireLatestFrame().has_value());
}

TEST_CASE("LinuxExternalTextureRendersCropDestinationOpacityAndRetainedFramesIntoCpuBackbuffer") {
  linux::ExternalTextureSource source({2.0F, 1.0F});
  rendered_texture = source.Texture();
  const std::array<std::byte, 8> red_green{
      std::byte{255},
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  source.Publish({2, 1, 8, linux::ExternalTexturePixelFormat::Rgba8888, red_green});

  TestPlatform platform;
  Runtime runtime{LinuxExternalTextureRenderApp, platform};
  runtime.SetWindowMetrics({.viewport = {5.0F, 4.0F}});
  const RenderFrame& frame = runtime.BuildRenderFrame();
  detail::LinuxRenderer renderer;

  const std::array<std::uint32_t, 20> first = RenderPixels(renderer, frame);
  REQUIRE(first[0] == kDefaultBackgroundPixel);
  REQUIRE(first[1U * 5U + 1U] == kDefaultBackgroundPixel);
  REQUIRE(first[1U * 5U + 2U] == 0xFF00FF00U);
  REQUIRE(first[2U * 5U + 3U] == 0xFF00FF00U);
  REQUIRE(first[3U * 5U] == 0xFFFB7C7DU);
  REQUIRE(first[3U * 5U + 1U] == kDefaultBackgroundPixel);

  const std::array<std::byte, 8> blue_yellow{
      std::byte{0},
      std::byte{0},
      std::byte{255},
      std::byte{255},
      std::byte{255},
      std::byte{255},
      std::byte{0},
      std::byte{255},
  };
  source.Publish({2, 1, 8, linux::ExternalTexturePixelFormat::Rgba8888, blue_yellow});
  const std::array<std::uint32_t, 20> updated = RenderPixels(renderer, frame);
  REQUIRE(updated[1U * 5U + 2U] == 0xFFFFFF00U);
  REQUIRE(updated[3U * 5U] == 0xFF7B7CFDU);

  const std::array<std::uint32_t, 20> retained = RenderPixels(renderer, frame);
  REQUIRE(retained == updated);
  renderer.Discard();
}

TEST_CASE("LinuxExternalTextureValidatesPixelFramesAndFinishedSources") {
  linux::ExternalTextureSource source({1.0F, 1.0F});
  const std::array<std::byte, 4> pixel{};

  REQUIRE_THROWS_AS(
      source.Publish({0, 1, 4, linux::ExternalTexturePixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish({1, 1, 3, linux::ExternalTexturePixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish({2, 1, 8, linux::ExternalTexturePixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish({1, 1, 4, static_cast<linux::ExternalTexturePixelFormat>(99), pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish(
          {std::numeric_limits<int>::max(),
           1,
           std::numeric_limits<std::size_t>::max(),
           linux::ExternalTexturePixelFormat::Rgba8888,
           pixel}
      ),
      std::invalid_argument
  );

  const std::shared_ptr<detail::LinuxExternalTextureState> state = StateFor(source.Texture());
  source.Publish({1, 1, 4, linux::ExternalTexturePixelFormat::Rgba8888, pixel});
  source.Finish();
  REQUIRE(state->AcquireLatestFrame().has_value());
  REQUIRE_THROWS_AS(source.Publish({1, 1, 4, linux::ExternalTexturePixelFormat::Rgba8888, pixel}), std::logic_error);
  source.Finish();
}

TEST_CASE("LinuxExternalTextureSourceMovePreservesItsConsumer") {
  linux::ExternalTextureSource source({16.0F, 9.0F});
  const ExternalTexture texture = source.Texture();
  linux::ExternalTextureSource moved(std::move(source));
  const std::array<std::byte, 4> pixel{};

  REQUIRE_FALSE(source.Texture().HasValue());
  REQUIRE(moved.Texture() == texture);
  REQUIRE_THROWS_AS(source.Publish({1, 1, 4, linux::ExternalTexturePixelFormat::Rgba8888, pixel}), std::logic_error);
}

TEST_CASE("LinuxExternalTexturePublicationSchedulesDamageThroughItsBoundRuntime") {
  linux::ExternalTextureSource source({2.0F, 2.0F});
  scheduled_texture = source.Texture();
  TestPlatform platform;
  Runtime runtime{LinuxExternalTextureApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  static_cast<void>(runtime.BuildRenderFrame());

  const int requests_before_publish = platform.requested_frames;
  const std::array<std::byte, 16> pixels{};
  source.Publish({2, 2, 8, linux::ExternalTexturePixelFormat::Rgba8888, pixels});
  REQUIRE(platform.requested_frames == requests_before_publish);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_publish + 1);

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE_FALSE(frame.damage.full);
  REQUIRE(frame.damage.rects == std::vector<Rect>{{0.0F, 0.0F, 2.0F, 2.0F}});
}

} // namespace
} // namespace huxerui::test
