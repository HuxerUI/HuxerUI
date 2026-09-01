#include "color_stream.h"

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/app.h>

#if TARGET_OS_IOS
#include <huxerui/ios/external_texture.h>
namespace huxerui_apple = huxerui::ios;
#elif TARGET_OS_OSX
#include <huxerui/macos/external_texture.h>
namespace huxerui_apple = huxerui::macos;
#else
#error "HuxerUI Apple color stream requires iOS or macOS"
#endif

namespace {

huxerui::PlatformError ColorStreamError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

struct AppleColorStreamState : huxerui::example::ColorStreamService,
                               std::enable_shared_from_this<AppleColorStreamState> {
  explicit AppleColorStreamState(huxerui::PlatformAdapter& adapter_value)
      : adapter(&adapter_value),
        texture(std::make_shared<huxerui_apple::PixelBufferTexture>(huxerui::Size{320.0F, 180.0F})) {}

  ~AppleColorStreamState() override {
    Stop();
  }

  void PublishFrame() {
    CVPixelBufferRef pixel_buffer = nullptr;
    const CVReturn result =
        CVPixelBufferCreate(kCFAllocatorDefault, 320, 180, kCVPixelFormatType_32BGRA, nullptr, &pixel_buffer);
    if (result != kCVReturnSuccess || pixel_buffer == nullptr) {
      return;
    }
    if (CVPixelBufferLockBaseAddress(pixel_buffer, 0) != kCVReturnSuccess) {
      CVPixelBufferRelease(pixel_buffer);
      return;
    }
    auto* bytes = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const std::size_t bytes_per_row = CVPixelBufferGetBytesPerRow(pixel_buffer);
    for (std::size_t y = 0; y < 180; ++y) {
      for (std::size_t x = 0; x < 320; ++x) {
        std::uint8_t* pixel = bytes + y * bytes_per_row + x * 4;
        pixel[0] = static_cast<std::uint8_t>((x + phase * 3U) % 256U);
        pixel[1] = static_cast<std::uint8_t>((y * 255U) / 179U);
        pixel[2] = static_cast<std::uint8_t>((255U + phase * 2U - x / 2U) % 256U);
        pixel[3] = 255;
      }
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
    texture->Publish(pixel_buffer);
    CVPixelBufferRelease(pixel_buffer);
    ++phase;
  }

  void Start() {
    if (timer != nil) {
      return;
    }
    PublishFrame();
    std::weak_ptr<AppleColorStreamState> weak = shared_from_this();
    timer = [NSTimer timerWithTimeInterval:1.0 / 30.0
                                   repeats:YES
                                     block:^(NSTimer*) {
                                       if (const std::shared_ptr<AppleColorStreamState> state = weak.lock()) {
                                         state->PublishFrame();
                                       }
                                     }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
  }

  void Stop() noexcept {
    [timer invalidate];
    timer = nil;
    texture->Finish();
  }

  huxerui::PlatformRequestId
  Texture(std::function<void(huxerui::PlatformResult<std::shared_ptr<huxerui::ExternalTexture>>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example color stream completion must not be empty");
    }
    if (!NSThread.isMainThread) {
      throw std::logic_error("HuxerUI example Apple color stream must be used on the main thread");
    }
    Start();
    adapter->DispatchToUIThread([completion = std::move(completion), texture = texture]() mutable {
      completion(std::move(texture));
    });
    return ++request_id;
  }

  huxerui::PlatformAdapter* adapter;
  std::shared_ptr<huxerui_apple::PixelBufferTexture> texture;
  __strong NSTimer* timer = nil;
  std::uint32_t phase = 0;
  huxerui::PlatformRequestId request_id = 0;
};

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type, [](PlatformAdapter& adapter) {
    return std::static_pointer_cast<ColorStreamService>(std::make_shared<AppleColorStreamState>(adapter));
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type));
}

} // namespace huxerui::example
