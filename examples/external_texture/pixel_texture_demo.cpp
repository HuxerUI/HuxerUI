#include "texture_demo.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <huxerui/windows/external_texture.h>
namespace platform_texture = huxerui::windows;
#elif defined(__linux__)
#include <huxerui/linux/external_texture.h>
namespace platform_texture = huxerui::linux;
#else
#error "HuxerUI pixel texture demo requires Windows or Linux"
#endif

namespace {

constexpr int texture_width = 320;
constexpr int texture_height = 180;

class PixelTextureDemo final : public huxerui::example::TextureDemo {
public:
  PixelTextureDemo()
      : texture_(std::make_shared<platform_texture::PixelTexture>(huxerui::Size{
            static_cast<float>(texture_width), static_cast<float>(texture_height)})),
        entries_{{
            "PixelTexture",
            "Copied RGBA frames from a platform producer thread.",
            texture_,
        }} {
    PublishFrame();
    worker_ = std::thread([this] { Run(); });
  }

  ~PixelTextureDemo() override {
    stopped_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    texture_->Finish();
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
  void Run() noexcept {
    std::unique_lock lock(mutex_);
    while (!stopped_.load(std::memory_order_acquire)) {
      if (!running_.load(std::memory_order_acquire)) {
        wake_.wait(lock, [this] {
          return stopped_.load(std::memory_order_acquire) || running_.load(std::memory_order_acquire);
        });
        continue;
      }
      lock.unlock();
      try {
        PublishFrame();
      } catch (...) {
        return;
      }
      lock.lock();
      wake_.wait_for(lock, std::chrono::milliseconds(50), [this] {
        return stopped_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire);
      });
    }
  }

  void PublishFrame() {
    std::vector<std::byte> pixels(static_cast<std::size_t>(texture_width * texture_height * 4));
    for (int y = 0; y < texture_height; ++y) {
      for (int x = 0; x < texture_width; ++x) {
        std::byte* pixel = pixels.data() + static_cast<std::size_t>((y * texture_width + x) * 4);
        pixel[0] = static_cast<std::byte>((x + phase_ * 3U) % 256U);
        pixel[1] = static_cast<std::byte>((y * 255) / (texture_height - 1));
        pixel[2] = static_cast<std::byte>((255U + phase_ * 2U - static_cast<std::uint32_t>(x / 2)) % 256U);
        pixel[3] = std::byte{255};
      }
    }
    texture_->Publish({
        .pixel_width = texture_width,
        .pixel_height = texture_height,
        .bytes_per_row = static_cast<std::size_t>(texture_width * 4),
        .format = platform_texture::PixelFormat::Rgba8888,
        .pixels = pixels,
    });
    ++phase_;
  }

  std::shared_ptr<platform_texture::PixelTexture> texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
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
  root.Provide<TextureDemo>(std::make_shared<PixelTextureDemo>());
}

} // namespace huxerui::example
