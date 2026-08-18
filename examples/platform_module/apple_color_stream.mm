#include "color_stream.h"

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

struct AppleColorStreamState : std::enable_shared_from_this<AppleColorStreamState> {
  AppleColorStreamState() : source({320.0F, 180.0F}) {}

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
    source.Publish(pixel_buffer);
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
    source.Finish();
  }

  huxerui_apple::ExternalTextureSource source;
  __strong NSTimer* timer = nil;
  std::uint32_t phase = 0;
};

huxerui::PlatformModuleFactory AppleColorStreamFactory() {
  huxerui::PlatformModuleFactory factory;
  factory.create = [](const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events) {
    if (!options.IsNull()) {
      throw std::invalid_argument("HuxerUI example color stream options must be null");
    }
    static_cast<void>(events);
    auto state = std::make_shared<AppleColorStreamState>();
    huxerui::PlatformModuleFactory::Instance instance;
    instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
        -> std::function<void()> {
      if (!NSThread.isMainThread) {
        result(
            ColorStreamError(
                "example/color-stream-thread",
                "The platform color stream must be used from the main thread"
            )
        );
        return {};
      }
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
    instance.dispose = [state] { state->Stop(); };
    return instance;
  };
  return factory;
}

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.Modules().Register(color_stream::type, AppleColorStreamFactory());
  root.Provide(std::make_shared<ColorStreamService>(root.Modules().Open(color_stream::type)));
}

} // namespace huxerui::example
