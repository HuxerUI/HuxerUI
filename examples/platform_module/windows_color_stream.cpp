#include "color_stream.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/windows/external_texture.h>

namespace {

huxerui::PlatformError ColorStreamError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

struct WindowsColorStreamState : huxerui::example::ColorStreamService {
  explicit WindowsColorStreamState(huxerui::PlatformAdapter& adapter_value)
      : adapter(&adapter_value),
        texture(std::make_shared<huxerui::windows::PixelTexture>(huxerui::Size{320.0F, 180.0F})) {}

  ~WindowsColorStreamState() {
    Dispose();
  }

  void Start() {
    std::lock_guard lock(mutex);
    if (worker.joinable()) {
      return;
    }
    PublishFrame();
    worker = std::thread([this] { Run(); });
  }

  huxerui::PlatformRequestId
  Texture(std::function<void(huxerui::PlatformResult<std::shared_ptr<huxerui::ExternalTexture>>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example color stream completion must not be empty");
    }
    Start();
    adapter->DispatchToUIThread([completion = std::move(completion), texture = texture]() mutable {
      completion(std::move(texture));
    });
    return ++request_id;
  }

  void Dispose() noexcept {
    {
      std::lock_guard lock(mutex);
      if (stopped) {
        return;
      }
      stopped = true;
    }
    wake.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
    texture->Finish();
  }

  void Run() noexcept {
    std::unique_lock lock(mutex);
    while (!wake.wait_for(lock, std::chrono::milliseconds(33), [this] { return stopped; })) {
      lock.unlock();
      try {
        PublishFrame();
      } catch (...) {
        lock.lock();
        break;
      }
      lock.lock();
    }
  }

  void PublishFrame() {
    std::vector<std::byte> pixels(320U * 180U * 4U);
    for (std::size_t y = 0; y < 180U; ++y) {
      for (std::size_t x = 0; x < 320U; ++x) {
        std::byte* pixel = pixels.data() + (y * 320U + x) * 4U;
        pixel[0] = static_cast<std::byte>((x + phase * 3U) % 256U);
        pixel[1] = static_cast<std::byte>((y * 255U) / 179U);
        pixel[2] = static_cast<std::byte>((255U + phase * 2U - x / 2U) % 256U);
        pixel[3] = std::byte{255};
      }
    }
    texture->Publish({
        .pixel_width = 320,
        .pixel_height = 180,
        .bytes_per_row = 320U * 4U,
        .format = huxerui::windows::PixelFormat::Rgba8888,
        .pixels = pixels,
    });
    ++phase;
  }

  huxerui::PlatformAdapter* adapter;
  std::shared_ptr<huxerui::windows::PixelTexture> texture;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool stopped = false;
  std::uint32_t phase = 0;
  huxerui::PlatformRequestId request_id = 0;
};

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<ColorStreamService>(std::make_shared<WindowsColorStreamState>(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type));
}

} // namespace huxerui::example
