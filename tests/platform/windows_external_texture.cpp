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
#include "win32_renderer.h"

namespace huxerui::test {
namespace {

static_assert(!std::is_copy_constructible_v<windows::PixelTexture>);
static_assert(!std::is_copy_assignable_v<windows::PixelTexture>);
static_assert(!std::is_move_constructible_v<windows::PixelTexture>);
static_assert(!std::is_move_assignable_v<windows::PixelTexture>);
static_assert(!std::is_copy_constructible_v<windows::D3D11Texture>);
static_assert(!std::is_copy_assignable_v<windows::D3D11Texture>);
static_assert(!std::is_move_constructible_v<windows::D3D11Texture>);
static_assert(!std::is_move_assignable_v<windows::D3D11Texture>);

using Microsoft::WRL::ComPtr;

#if !defined(HUXERUI_WINDOWS_7_COMPAT)

struct D3D11TestDevice {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
};

D3D11TestDevice CreateD3D11TestDevice() {
  constexpr D3D_FEATURE_LEVEL feature_levels[]{
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D11TestDevice result;
  D3D_FEATURE_LEVEL feature_level{};
  HRESULT create_result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
      static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, result.device.GetAddressOf(), &feature_level,
      result.context.GetAddressOf()
  );
  if (FAILED(create_result)) {
    create_result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
        static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, result.device.GetAddressOf(), &feature_level,
        result.context.GetAddressOf()
    );
  }
  REQUIRE(SUCCEEDED(create_result));
  return result;
}

ComPtr<ID3D11Texture2D> CreateD3D11Source(
    D3D11TestDevice& device, UINT width, UINT height, DXGI_FORMAT format, const void* pixels = nullptr,
    UINT bytes_per_row = 0
) {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = width;
  description.Height = height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = format;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial{};
  initial.pSysMem = pixels;
  initial.SysMemPitch = bytes_per_row;
  ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(
      device.device->CreateTexture2D(&description, pixels == nullptr ? nullptr : &initial, texture.GetAddressOf())
  ));
  return texture;
}

std::uint32_t ReadD3D11SnapshotPixel(D3D11TestDevice& device, const detail::Win32D3D11Frame& frame) {
  ComPtr<IDXGIKeyedMutex> keyed_mutex;
  REQUIRE(SUCCEEDED(frame.Texture()->QueryInterface(IID_PPV_ARGS(keyed_mutex.GetAddressOf()))));
  REQUIRE(keyed_mutex->AcquireSync(1, INFINITE) == S_OK);

  D3D11_TEXTURE2D_DESC description{};
  frame.Texture()->GetDesc(&description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  REQUIRE(SUCCEEDED(device.device->CreateTexture2D(&description, nullptr, staging.GetAddressOf())));
  device.context->CopyResource(staging.Get(), frame.Texture());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  REQUIRE(SUCCEEDED(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
  std::uint32_t pixel = 0;
  std::memcpy(&pixel, mapped.pData, sizeof(pixel));
  device.context->Unmap(staging.Get(), 0);
  REQUIRE(SUCCEEDED(keyed_mutex->ReleaseSync(1)));
  return pixel;
}

#endif

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

#if !defined(HUXERUI_WINDOWS_7_COMPAT)

TEST_CASE("WindowsD3D11TexturePublishesAnImmutableOwnedSnapshot") {
  D3D11TestDevice device = CreateD3D11TestDevice();
  const std::array<std::uint32_t, 2> red_green{0xFFFF0000U, 0xFF00FF00U};
  ComPtr<ID3D11Texture2D> source = CreateD3D11Source(
      device, 2, 1, DXGI_FORMAT_B8G8R8A8_UNORM, red_green.data(),
      static_cast<UINT>(sizeof(red_green))
  );
  const auto texture = std::make_shared<windows::D3D11Texture>(Size{16.0F, 9.0F});

  texture->Publish({source.Get(), windows::D3D11Texture::Alpha::Premultiplied});
  REQUIRE(texture->Revision() == 1);
  const std::shared_ptr<const detail::Win32D3D11Frame> first = detail::GetD3D11Frame(*texture);
  REQUIRE(first != nullptr);
  REQUIRE(first->PixelWidth() == 2);
  REQUIRE(first->PixelHeight() == 1);
  REQUIRE(first->Alpha() == windows::D3D11Texture::Alpha::Premultiplied);
  REQUIRE(first->Texture() != source.Get());

  const std::array<std::uint32_t, 2> blue{0xFF0000FFU, 0xFF0000FFU};
  device.context->UpdateSubresource(source.Get(), 0, nullptr, blue.data(), static_cast<UINT>(sizeof(blue)), 0);
  REQUIRE(ReadD3D11SnapshotPixel(device, *first) == 0xFFFF0000U);

  texture->Publish({source.Get(), windows::D3D11Texture::Alpha::Opaque});
  const std::shared_ptr<const detail::Win32D3D11Frame> second = detail::GetD3D11Frame(*texture);
  REQUIRE(texture->Revision() == 2);
  REQUIRE(second != first);
  REQUIRE(second->Alpha() == windows::D3D11Texture::Alpha::Opaque);
  REQUIRE(ReadD3D11SnapshotPixel(device, *second) == 0xFF0000FFU);
}

TEST_CASE("WindowsD3D11TextureValidatesSourcesAndPreservesTheLastFrameAfterFinish") {
  D3D11TestDevice device = CreateD3D11TestDevice();
  ComPtr<ID3D11Texture2D> valid = CreateD3D11Source(device, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM);
  ComPtr<ID3D11Texture2D> wrong_format = CreateD3D11Source(device, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
  const auto texture = std::make_shared<windows::D3D11Texture>(Size{1.0F, 1.0F});

  REQUIRE_THROWS_AS(texture->Publish({}), std::invalid_argument);
  REQUIRE(texture->Revision() == 0);

  texture->Publish({valid.Get()});
  const std::shared_ptr<const detail::Win32D3D11Frame> published = detail::GetD3D11Frame(*texture);
  REQUIRE(texture->Revision() == 1);
  REQUIRE_THROWS_AS(texture->Publish({wrong_format.Get()}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      texture->Publish({valid.Get(), static_cast<windows::D3D11Texture::Alpha>(99)}), std::invalid_argument
  );
  REQUIRE(texture->Revision() == 1);
  REQUIRE(detail::GetD3D11Frame(*texture) == published);

  texture->Finish();
  REQUIRE(detail::GetD3D11Frame(*texture) == published);
  REQUIRE_THROWS_AS(texture->Publish({valid.Get()}), std::logic_error);
  texture->Finish();
}

TEST_CASE("WindowsD3D11TextureRetriesContentionAndRendersAcrossDevicesAfterReset") {
  D3D11TestDevice device = CreateD3D11TestDevice();
  const std::array<std::uint32_t, 4> pixels{0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU, 0xFFFFFFFFU};
  ComPtr<ID3D11Texture2D> source = CreateD3D11Source(
      device, 2, 2, DXGI_FORMAT_B8G8R8A8_UNORM, pixels.data(),
      2U * static_cast<UINT>(sizeof(std::uint32_t))
  );
  const auto texture = std::make_shared<windows::D3D11Texture>(Size{2.0F, 2.0F});
  scheduled_texture = texture;
  TestPlatform platform;
  Runtime runtime{WindowsExternalTextureApp, platform};
  runtime.SetWindowMetrics({.viewport = {2.0F, 2.0F}});
  static_cast<void>(runtime.BuildRenderFrame());

  const int requests_before_publish = platform.requested_frames;
  texture->Publish({source.Get(), windows::D3D11Texture::Alpha::Opaque});
  REQUIRE(platform.requested_frames == requests_before_publish);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_publish + 1);
  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.damage.rects == std::vector<Rect>{{0.0F, 0.0F, 2.0F, 2.0F}});

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  REQUIRE((SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE));
  HWND first_window =
      CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
  HWND second_window =
      CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
  REQUIRE(first_window != nullptr);
  REQUIRE(second_window != nullptr);

  detail::Win32Renderer first_renderer;
  detail::Win32Renderer second_renderer;
  first_renderer.Initialize();
  second_renderer.Initialize();
  const RECT paint_rect{0, 0, 64, 64};

  const std::shared_ptr<const detail::Win32D3D11Frame> published = detail::GetD3D11Frame(*texture);
  ComPtr<IDXGIKeyedMutex> held_mutex;
  REQUIRE(SUCCEEDED(published->Texture()->QueryInterface(IID_PPV_ARGS(held_mutex.GetAddressOf()))));
  REQUIRE(held_mutex->AcquireSync(1, INFINITE) == S_OK);
  REQUIRE(first_renderer.Render(first_window, 96.0F, frame, paint_rect) == detail::Win32RenderResult::Retry);
  REQUIRE(SUCCEEDED(held_mutex->ReleaseSync(1)));

  REQUIRE(first_renderer.Render(first_window, 96.0F, frame, paint_rect) == detail::Win32RenderResult::Presented);
  REQUIRE(second_renderer.Render(second_window, 96.0F, frame, paint_rect) == detail::Win32RenderResult::Presented);
  first_renderer.ResetDeviceResources();
  REQUIRE(first_renderer.Render(first_window, 96.0F, frame, paint_rect) == detail::Win32RenderResult::Presented);

  first_renderer.Discard();
  second_renderer.Discard();
  DestroyWindow(second_window);
  DestroyWindow(first_window);
  if (SUCCEEDED(com_result)) {
    CoUninitialize();
  }
}

#else

TEST_CASE("WindowsD3D11TextureIsUnavailableInWindows7CompatibilityBuilds") {
  windows::D3D11Texture texture(Size{1.0F, 1.0F});
  REQUIRE_THROWS_AS(texture.Publish({}), std::runtime_error);
}

#endif

} // namespace
} // namespace huxerui::test
