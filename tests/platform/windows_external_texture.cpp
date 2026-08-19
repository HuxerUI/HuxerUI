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

#include <huxerui/windows/external_texture.h>

#include "external_texture_internal.h"
#include "runtime_test_support.h"
#include "win32_external_texture_internal.h"

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<windows::ExternalTextureSource>);
static_assert(!std::is_copy_assignable_v<windows::ExternalTextureSource>);
static_assert(std::is_nothrow_move_constructible_v<windows::ExternalTextureSource>);
static_assert(std::is_nothrow_move_assignable_v<windows::ExternalTextureSource>);

std::shared_ptr<detail::Win32ExternalTextureState> StateFor(const ExternalTexture& texture) {
  return std::dynamic_pointer_cast<detail::Win32ExternalTextureState>(detail::ExternalTextureState::From(texture));
}

std::uint32_t PixelAt(const detail::Win32ExternalTextureFrame& frame, int x, int y) {
  std::uint32_t pixel = 0;
  const std::span<const std::byte> pixels = frame.Pixels();
  const std::size_t offset = static_cast<std::size_t>(y) * frame.BytesPerRow() + static_cast<std::size_t>(x) * 4U;
  std::memcpy(&pixel, pixels.data() + offset, sizeof(pixel));
  return pixel;
}

ExternalTexture scheduled_texture;

View WindowsExternalTextureApp() {
  return Image(scheduled_texture).Fit(ImageFit::Fill).With(Frame{2.0F, 2.0F});
}

TEST_CASE("WindowsExternalTextureCopiesAndConvertsTheLatestPixelFrame") {
  windows::ExternalTextureSource source({16.0F, 9.0F});
  const ExternalTexture texture = source.Texture();
  const std::shared_ptr<detail::Win32ExternalTextureState> state = StateFor(texture);
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
      .format = windows::ExternalTexturePixelFormat::Rgba8888,
      .pixels = rgba,
  });
  rgba.fill(std::byte{0});

  const std::optional<detail::Win32ExternalTextureFrame> frame = state->AcquireLatestFrame();
  REQUIRE(frame.has_value());
  REQUIRE(frame->PixelWidth() == 1);
  REQUIRE(frame->PixelHeight() == 2);
  REQUIRE(frame->BytesPerRow() == 4);
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFFFF0000U);
  REQUIRE(PixelAt(*frame, 0, 1) == 0x80008000U);
  REQUIRE_FALSE(state->AcquireLatestFrame().has_value());
}

TEST_CASE("WindowsExternalTextureUsesALatestWinsMailboxAndAcceptsBgra") {
  windows::ExternalTextureSource source({1.0F, 1.0F});
  const std::shared_ptr<detail::Win32ExternalTextureState> state = StateFor(source.Texture());
  const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
  const std::array<std::byte, 4> blue_bgra{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};

  source.Publish({1, 1, 4, windows::ExternalTexturePixelFormat::Rgba8888, red});
  source.Publish({1, 1, 4, windows::ExternalTexturePixelFormat::Bgra8888, blue_bgra});

  const std::optional<detail::Win32ExternalTextureFrame> frame = state->AcquireLatestFrame();
  REQUIRE(frame.has_value());
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFF0000FFU);
  REQUIRE_FALSE(state->AcquireLatestFrame().has_value());
}

TEST_CASE("WindowsExternalTextureValidatesPixelFramesAndFinishedSources") {
  windows::ExternalTextureSource source({1.0F, 1.0F});
  const std::array<std::byte, 4> pixel{};

  REQUIRE_THROWS_AS(
      source.Publish({0, 1, 4, windows::ExternalTexturePixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish({1, 1, 3, windows::ExternalTexturePixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish({2, 1, 8, windows::ExternalTexturePixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish({1, 1, 4, static_cast<windows::ExternalTexturePixelFormat>(99), pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      source.Publish(
          {std::numeric_limits<int>::max(),
           1,
           std::numeric_limits<std::size_t>::max(),
           windows::ExternalTexturePixelFormat::Rgba8888,
           pixel}
      ),
      std::invalid_argument
  );

  const std::shared_ptr<detail::Win32ExternalTextureState> state = StateFor(source.Texture());
  source.Publish({1, 1, 4, windows::ExternalTexturePixelFormat::Rgba8888, pixel});
  source.Finish();
  REQUIRE(state->AcquireLatestFrame().has_value());
  REQUIRE_THROWS_AS(source.Publish({1, 1, 4, windows::ExternalTexturePixelFormat::Rgba8888, pixel}), std::logic_error);
  source.Finish();
}

TEST_CASE("WindowsExternalTextureSourceMovePreservesItsConsumer") {
  windows::ExternalTextureSource source({16.0F, 9.0F});
  const ExternalTexture texture = source.Texture();
  windows::ExternalTextureSource moved(std::move(source));
  const std::array<std::byte, 4> pixel{};

  REQUIRE_FALSE(source.Texture().HasValue());
  REQUIRE(moved.Texture() == texture);
  REQUIRE_THROWS_AS(source.Publish({1, 1, 4, windows::ExternalTexturePixelFormat::Rgba8888, pixel}), std::logic_error);
}

TEST_CASE("WindowsExternalTexturePublicationSchedulesDamageThroughItsBoundRuntime") {
  windows::ExternalTextureSource source({2.0F, 2.0F});
  scheduled_texture = source.Texture();
  TestPlatform platform;
  Runtime runtime{WindowsExternalTextureApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  static_cast<void>(runtime.BuildRenderFrame());

  const int requests_before_publish = platform.requested_frames;
  const std::array<std::byte, 16> pixels{};
  source.Publish({2, 2, 8, windows::ExternalTexturePixelFormat::Rgba8888, pixels});
  REQUIRE(platform.requested_frames == requests_before_publish);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_publish + 1);

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE_FALSE(frame.damage.full);
  REQUIRE(frame.damage.rects == std::vector<Rect>{{0.0F, 0.0F, 2.0F, 2.0F}});
}

} // namespace
} // namespace huxerui::test
