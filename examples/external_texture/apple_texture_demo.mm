#include "texture_demo.h"

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#if TARGET_OS_IOS
#include <huxerui/ios/external_texture.h>
namespace platform_texture = huxerui::ios;
#elif TARGET_OS_OSX
#include <huxerui/macos/external_texture.h>
namespace platform_texture = huxerui::macos;
#else
#error "HuxerUI Apple texture demo requires iOS or macOS"
#endif

namespace {

constexpr int texture_width = 320;
constexpr int texture_height = 180;

class AppleTextureDemo final : public huxerui::example::TextureDemo,
                               public std::enable_shared_from_this<AppleTextureDemo> {
public:
  AppleTextureDemo()
      : texture_(std::make_shared<platform_texture::PixelBufferTexture>(huxerui::Size{
            static_cast<float>(texture_width), static_cast<float>(texture_height)})),
        entries_{{
            "PixelBufferTexture",
            "Retained CVPixelBuffer frames produced on the platform main thread.",
            texture_,
        }} {}

  ~AppleTextureDemo() override {
    Stop();
    texture_->Finish();
  }

  [[nodiscard]] const std::vector<huxerui::example::TextureDemoEntry>& Entries() const noexcept override {
    return entries_;
  }

  [[nodiscard]] std::string_view Message() const noexcept override {
    return {};
  }

  void SetRunning(bool running) noexcept override {
    if (running) {
      Start();
    } else {
      Stop();
    }
  }

private:
  void Start() noexcept {
    if (timer_ != nil) {
      return;
    }
    PublishFrame();
    std::weak_ptr<AppleTextureDemo> weak = shared_from_this();
    timer_ = [NSTimer timerWithTimeInterval:1.0 / 20.0
                                    repeats:YES
                                      block:^(NSTimer*) {
                                        if (const std::shared_ptr<AppleTextureDemo> state = weak.lock()) {
                                          state->PublishFrame();
                                        }
                                      }];
    [NSRunLoop.mainRunLoop addTimer:timer_ forMode:NSRunLoopCommonModes];
  }

  void Stop() noexcept {
    [timer_ invalidate];
    timer_ = nil;
  }

  void PublishFrame() noexcept {
    CVPixelBufferRef pixel_buffer = nullptr;
    if (CVPixelBufferCreate(
            kCFAllocatorDefault, texture_width, texture_height, kCVPixelFormatType_32BGRA, nullptr, &pixel_buffer
        ) != kCVReturnSuccess ||
        pixel_buffer == nullptr) {
      return;
    }
    if (CVPixelBufferLockBaseAddress(pixel_buffer, 0) != kCVReturnSuccess) {
      CVPixelBufferRelease(pixel_buffer);
      return;
    }
    auto* bytes = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const std::size_t bytes_per_row = CVPixelBufferGetBytesPerRow(pixel_buffer);
    for (int y = 0; y < texture_height; ++y) {
      for (int x = 0; x < texture_width; ++x) {
        std::uint8_t* pixel = bytes + static_cast<std::size_t>(y) * bytes_per_row + x * 4;
        pixel[0] = static_cast<std::uint8_t>((x + phase_ * 3U) % 256U);
        pixel[1] = static_cast<std::uint8_t>((y * 255) / (texture_height - 1));
        pixel[2] = static_cast<std::uint8_t>((255U + phase_ * 2U - static_cast<std::uint32_t>(x / 2)) % 256U);
        pixel[3] = 255;
      }
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
    try {
      texture_->Publish(pixel_buffer);
    } catch (...) {
    }
    CVPixelBufferRelease(pixel_buffer);
    ++phase_;
  }

  std::shared_ptr<platform_texture::PixelBufferTexture> texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
  __strong NSTimer* timer_ = nil;
  std::uint32_t phase_ = 0;
};

} // namespace

namespace huxerui::example {

void InstallTextureDemo(RootContext& root) {
  auto demo = std::make_shared<AppleTextureDemo>();
  demo->SetRunning(true);
  root.Provide<TextureDemo>(std::move(demo));
}

} // namespace huxerui::example
