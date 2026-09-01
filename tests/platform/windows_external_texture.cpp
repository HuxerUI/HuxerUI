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

static_assert(!std::is_copy_constructible_v<windows::PixelTexture>);
static_assert(!std::is_copy_assignable_v<windows::PixelTexture>);
static_assert(!std::is_move_constructible_v<windows::PixelTexture>);
static_assert(!std::is_move_assignable_v<windows::PixelTexture>);

std::uint32_t PixelAt(const detail::Win32PixelFrame& frame, int x, int y) {
  std::uint32_t pixel = 0;
  const std::span<const std::byte> pixels = frame.Pixels();
  const std::size_t offset = static_cast<std::size_t>(y) * frame.BytesPerRow() + static_cast<std::size_t>(x) * 4U;
  std::memcpy(&pixel, pixels.data() + offset, sizeof(pixel));
  return pixel;
}

std::shared_ptr<ExternalTexture> scheduled_texture;

View WindowsExternalTextureApp() {
  return Image(scheduled_texture).Fit(ImageFit::Fill).With(Frame{2.0F, 2.0F});
}

TEST_CASE("WindowsExternalTextureCopiesAndConvertsTheLatestPixelFrame") {
  const auto texture = std::make_shared<windows::PixelTexture>(Size{16.0F, 9.0F});
  REQUIRE(texture->IntrinsicSize() == Size{16.0F, 9.0F});

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
  texture->Publish({
      .pixel_width = 1,
      .pixel_height = 2,
      .bytes_per_row = 8,
      .format = windows::PixelFormat::Rgba8888,
      .pixels = rgba,
  });
  rgba.fill(std::byte{0});

  const std::shared_ptr<const detail::Win32PixelFrame> frame = detail::GetPixelFrame(*texture);
  REQUIRE(frame != nullptr);
  REQUIRE(frame->PixelWidth() == 1);
  REQUIRE(frame->PixelHeight() == 2);
  REQUIRE(frame->BytesPerRow() == 4);
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFFFF0000U);
  REQUIRE(PixelAt(*frame, 0, 1) == 0x80008000U);
  REQUIRE(detail::GetPixelFrame(*texture) == frame);
}

TEST_CASE("WindowsExternalTextureUsesALatestWinsMailboxAndAcceptsBgra") {
  const auto texture = std::make_shared<windows::PixelTexture>(Size{1.0F, 1.0F});
  const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
  const std::array<std::byte, 4> blue_bgra{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};

  texture->Publish({1, 1, 4, windows::PixelFormat::Rgba8888, red});
  texture->Publish({1, 1, 4, windows::PixelFormat::Bgra8888, blue_bgra});

  const std::shared_ptr<const detail::Win32PixelFrame> frame = detail::GetPixelFrame(*texture);
  REQUIRE(frame != nullptr);
  REQUIRE(PixelAt(*frame, 0, 0) == 0xFF0000FFU);
  REQUIRE(detail::GetPixelFrame(*texture) == frame);
}

TEST_CASE("WindowsExternalTextureValidatesPixelFramesAndFinishedTextures") {
  const auto texture = std::make_shared<windows::PixelTexture>(Size{1.0F, 1.0F});
  const std::array<std::byte, 4> pixel{};

  REQUIRE_THROWS_AS(
      texture->Publish({0, 1, 4, windows::PixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish({1, 1, 3, windows::PixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish({2, 1, 8, windows::PixelFormat::Rgba8888, pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish({1, 1, 4, static_cast<windows::PixelFormat>(99), pixel}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      texture->Publish(
          {std::numeric_limits<int>::max(),
           1,
           std::numeric_limits<std::size_t>::max(),
           windows::PixelFormat::Rgba8888,
           pixel}
      ),
      std::invalid_argument
  );

  texture->Publish({1, 1, 4, windows::PixelFormat::Rgba8888, pixel});
  texture->Finish();
  REQUIRE(detail::GetPixelFrame(*texture) != nullptr);
  REQUIRE_THROWS_AS(
      texture->Publish({1, 1, 4, windows::PixelFormat::Rgba8888, pixel}), std::logic_error
  );
  texture->Finish();
}

TEST_CASE("WindowsPixelTextureIsTheSharedExternalTextureIdentity") {
  const std::shared_ptr<ExternalTexture> texture = std::make_shared<windows::PixelTexture>(Size{16.0F, 9.0F});

  REQUIRE(texture->IntrinsicSize() == Size{16.0F, 9.0F});
  REQUIRE(std::dynamic_pointer_cast<windows::PixelTexture>(texture) != nullptr);
}

TEST_CASE("WindowsExternalTexturePublicationSchedulesDamageThroughItsBoundRuntime") {
  const auto texture = std::make_shared<windows::PixelTexture>(Size{2.0F, 2.0F});
  scheduled_texture = texture;
  TestPlatform platform;
  Runtime runtime{WindowsExternalTextureApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  static_cast<void>(runtime.BuildRenderFrame());

  const int requests_before_publish = platform.requested_frames;
  const std::array<std::byte, 16> pixels{};
  texture->Publish({2, 2, 8, windows::PixelFormat::Rgba8888, pixels});
  REQUIRE(platform.requested_frames == requests_before_publish);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_publish + 1);

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE_FALSE(frame.damage.full);
  REQUIRE(frame.damage.rects == std::vector<Rect>{{0.0F, 0.0F, 2.0F, 2.0F}});
}

} // namespace
} // namespace huxerui::test
