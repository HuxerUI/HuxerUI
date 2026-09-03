#include "texture_demo.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <huxerui/windows/external_texture.h>

namespace {

using Microsoft::WRL::ComPtr;

constexpr int texture_width = 320;
constexpr int texture_height = 180;
constexpr huxerui::Size texture_size{static_cast<float>(texture_width), static_cast<float>(texture_height)};

void ThrowIfFailed(HRESULT result, const char* message) {
  if (FAILED(result)) {
    throw std::runtime_error(message);
  }
}

class WindowsTextureDemo final : public huxerui::example::TextureDemo {
public:
  WindowsTextureDemo()
      : pixel_texture_(std::make_shared<huxerui::windows::PixelTexture>(texture_size)),
        d3d11_texture_(std::make_shared<huxerui::windows::D3D11Texture>(texture_size)),
        entries_{
            {
                "PixelTexture",
                "Copied BGRA frames from a platform producer thread.",
                pixel_texture_,
            },
            {
                "D3D11Texture",
                "One GPU copy into an immutable shared snapshot drawn directly by the renderer.",
                d3d11_texture_,
            },
        } {
    CreateProducerDevice();
    PublishFrame();
    worker_ = std::thread([this] { Run(); });
  }

  ~WindowsTextureDemo() override {
    stopped_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    d3d11_texture_->Finish();
    pixel_texture_->Finish();
  }

  [[nodiscard]] const std::vector<huxerui::example::TextureDemoEntry>& Entries() const noexcept override {
    return entries_;
  }

  [[nodiscard]] std::string_view Message() const noexcept override {
    return {};
  }

  void SetRunning(bool running) noexcept override {
    running_.store(running, std::memory_order_release);
    wake_.notify_all();
  }

private:
  void CreateProducerDevice() {
    constexpr D3D_FEATURE_LEVEL feature_levels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
        static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, device_.GetAddressOf(), &feature_level,
        context_.GetAddressOf()
    );
    if (FAILED(result)) {
      result = D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
          static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, device_.GetAddressOf(), &feature_level,
          context_.GetAddressOf()
      );
    }
    ThrowIfFailed(result, "HuxerUI external texture example could not create its D3D11 producer device");

    D3D11_TEXTURE2D_DESC description{};
    description.Width = texture_width;
    description.Height = texture_height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(
        device_->CreateTexture2D(&description, nullptr, source_.GetAddressOf()),
        "HuxerUI external texture example could not create its D3D11 source texture"
    );
  }

  void Run() noexcept {
    using Clock = std::chrono::steady_clock;
    constexpr auto frame_interval = std::chrono::nanoseconds(1'000'000'000 / 60);
    std::unique_lock lock(mutex_);
    auto next_frame = Clock::now() + frame_interval;
    while (!stopped_.load(std::memory_order_acquire)) {
      if (!running_.load(std::memory_order_acquire)) {
        wake_.wait(lock, [this] {
          return stopped_.load(std::memory_order_acquire) || running_.load(std::memory_order_acquire);
        });
        next_frame = Clock::now() + frame_interval;
        continue;
      }
      const bool interrupted = wake_.wait_until(lock, next_frame, [this] {
        return stopped_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire);
      });
      if (interrupted) {
        continue;
      }
      lock.unlock();
      try {
        PublishFrame();
      } catch (...) {
        return;
      }
      lock.lock();
      next_frame += frame_interval;
      if (next_frame < Clock::now()) {
        next_frame = Clock::now();
      }
    }
  }

  void PublishFrame() {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(texture_width * texture_height));
    for (int y = 0; y < texture_height; ++y) {
      for (int x = 0; x < texture_width; ++x) {
        const std::uint32_t red = (static_cast<std::uint32_t>(x) + phase_ * 3U) % 256U;
        const std::uint32_t green = static_cast<std::uint32_t>((y * 255) / (texture_height - 1));
        const std::uint32_t blue = (255U + phase_ * 2U - static_cast<std::uint32_t>(x / 2)) % 256U;
        pixels[static_cast<std::size_t>(y * texture_width + x)] = 0xFF000000U | (red << 16U) | (green << 8U) | blue;
      }
    }

    const UINT bytes_per_row = static_cast<UINT>(texture_width * sizeof(std::uint32_t));
    context_->UpdateSubresource(source_.Get(), 0, nullptr, pixels.data(), bytes_per_row, 0);
    d3d11_texture_->Publish({source_.Get(), huxerui::windows::D3D11Texture::Alpha::Opaque});
    pixel_texture_->Publish({
        .pixel_width = texture_width,
        .pixel_height = texture_height,
        .bytes_per_row = bytes_per_row,
        .format = huxerui::windows::PixelFormat::Bgra8888,
        .pixels = std::as_bytes(std::span(pixels)),
    });
    ++phase_;
  }

  std::shared_ptr<huxerui::windows::PixelTexture> pixel_texture_;
  std::shared_ptr<huxerui::windows::D3D11Texture> d3d11_texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> source_;
  std::atomic<bool> running_ = true;
  std::atomic<bool> stopped_ = false;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  std::uint32_t phase_ = 0;
};

} // namespace

namespace huxerui::example {

void InstallTextureDemo(RootContext& root) {
  root.Provide<TextureDemo>(std::make_shared<WindowsTextureDemo>());
}

} // namespace huxerui::example
