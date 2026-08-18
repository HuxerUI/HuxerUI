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

#include <huxerui/linux/external_texture.h>

namespace {

huxerui::PlatformError ColorStreamError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

struct LinuxColorStreamState {
  LinuxColorStreamState() : source({320.0F, 180.0F}) {}

  ~LinuxColorStreamState() {
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
    source.Finish();
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
    source.Publish({
        .pixel_width = 320,
        .pixel_height = 180,
        .bytes_per_row = 320U * 4U,
        .format = huxerui::linux::ExternalTexturePixelFormat::Rgba8888,
        .pixels = pixels,
    });
    ++phase;
  }

  huxerui::linux::ExternalTextureSource source;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool stopped = false;
  std::uint32_t phase = 0;
};

huxerui::PlatformModuleFactory LinuxColorStreamFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    if (!options.IsNull()) {
      throw std::invalid_argument("HuxerUI example color stream options must be null");
    }
    static_cast<void>(events);
    auto state = std::make_shared<LinuxColorStreamState>();
    huxerui::PlatformModuleFactory::Instance instance;
    instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
        -> std::function<void()> {
      if (method == huxerui::example::color_stream::texture_method && arguments.IsNull()) {
        state->Start();
        result(huxerui::PlatformPayload(state->source.Texture()));
        return {};
      }
      result(
          ColorStreamError(
              "example/color-stream-method",
              "The platform color stream method or payload is not supported"
          )
      );
      return {};
    };
    instance.dispose = [state] { state->Dispose(); };
    return instance;
  };
  return factory;
}

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.Modules().Register(color_stream::type, LinuxColorStreamFactory());
  root.Provide(std::make_shared<ColorStreamService>(root.Modules().Open(color_stream::type)));
}

} // namespace huxerui::example
