#include "texture_demo.h"

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
  AppleTextureDemo() {
    const huxerui::Size intrinsic_size{static_cast<float>(texture_width), static_cast<float>(texture_height)};
    pixel_buffer_texture_ = std::make_shared<platform_texture::PixelBufferTexture>(intrinsic_size);
    entries_.push_back({
        "PixelBufferTexture",
        "Retained CVPixelBuffer frames produced on the platform main thread.",
        pixel_buffer_texture_,
    });

    metal_device_ = MTLCreateSystemDefaultDevice();
    metal_command_queue_ = [metal_device_ newCommandQueue];
    if (metal_device_ == nil || metal_command_queue_ == nil) {
      message_ = "Metal is unavailable on this device.";
      return;
    }
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:texture_width height:texture_height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget;
    metal_source_ = [metal_device_ newTextureWithDescriptor:descriptor];
    if (metal_source_ == nil) {
      message_ = "The Metal render-target texture could not be created.";
      return;
    }
    metal_texture_ = std::make_shared<platform_texture::MetalTexture>(intrinsic_size);
    entries_.push_back({
        "MetalTexture",
        "GPU-rendered Metal frames copied into an immutable latest-frame snapshot.",
        metal_texture_,
    });
  }

  ~AppleTextureDemo() override {
    Stop();
    pixel_buffer_texture_->Finish();
    if (metal_texture_) {
      metal_texture_->Finish();
    }
  }

  [[nodiscard]] const std::vector<huxerui::example::TextureDemoEntry>& Entries() const noexcept override {
    return entries_;
  }

  [[nodiscard]] std::string_view Message() const noexcept override {
    return message_;
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
    PublishPixelBuffer();
    PublishMetal();
    ++phase_;
  }

  void PublishPixelBuffer() noexcept {
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
      pixel_buffer_texture_->Publish(pixel_buffer);
    } catch (...) {
    }
    CVPixelBufferRelease(pixel_buffer);
  }

  void PublishMetal() noexcept {
    if (!metal_texture_ || metal_source_ == nil || metal_command_queue_ == nil) {
      return;
    }
    id<MTLCommandBuffer> command_buffer = [metal_command_queue_ commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = metal_source_;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    const double red = static_cast<double>((phase_ * 7U) % 256U) / 255.0;
    const double green = static_cast<double>((96U + phase_ * 3U) % 256U) / 255.0;
    const double blue = static_cast<double>((224U + phase_ * 5U) % 256U) / 255.0;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, 1.0);
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (command_buffer == nil || encoder == nil) {
      return;
    }
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      return;
    }
    try {
      const platform_texture::MetalTexture::Frame frame{
          metal_source_, platform_texture::MetalTexture::Origin::TopLeft,
          platform_texture::MetalTexture::Alpha::Premultiplied};
      metal_texture_->Publish(frame);
    } catch (...) {
    }
  }

  std::shared_ptr<platform_texture::PixelBufferTexture> pixel_buffer_texture_;
  std::shared_ptr<platform_texture::MetalTexture> metal_texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
  std::string message_;
  __strong NSTimer* timer_ = nil;
  __strong id<MTLDevice> metal_device_ = nil;
  __strong id<MTLCommandQueue> metal_command_queue_ = nil;
  __strong id<MTLTexture> metal_source_ = nil;
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
