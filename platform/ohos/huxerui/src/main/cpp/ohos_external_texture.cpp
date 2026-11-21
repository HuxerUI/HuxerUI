#include <huxerui/ohos/external_texture.h>

#include <dlfcn.h>
#include <native_buffer/native_buffer.h>

#include <stdexcept>
#include <utility>

#include "ohos_bitmap.h"

namespace huxerui::ohos {
namespace {

Task<std::optional<Bitmap>> RetainFrame(std::optional<Bitmap> frame) {
  co_return std::move(frame);
}

auto ImportBuffer() noexcept {
  // The framework itself still loads on API 19, where the PixelMap import symbol is absent.
  static const auto import = reinterpret_cast<decltype(&OH_PixelmapNative_CreatePixelmapFromNativeBuffer)>(
      dlsym(RTLD_DEFAULT, "OH_PixelmapNative_CreatePixelmapFromNativeBuffer"));
  return import;
}

} // namespace

PixelTexture::PixelTexture(Size intrinsic_size) : ExternalTexture(intrinsic_size) {}

void PixelTexture::Publish(const PixelFrame& frame) {
  auto pixels = Bitmap::CopyPixels(frame.pixel_width, frame.pixel_height, frame.pixels, frame.bytes_per_row,
                                   frame.format);
  {
    const std::lock_guard lock(mutex_);
    if (finished_) {
      throw std::logic_error("HuxerUI OHOS external texture is finished");
    }
    frame_ = std::move(pixels);
  }
  NotifyFrameAvailable();
}

void PixelTexture::Finish() noexcept {
  const std::lock_guard lock(mutex_);
  finished_ = true;
}

Task<std::optional<Bitmap>> PixelTexture::SnapshotAsync() const {
  return RetainFrame(AcquireFrame());
}

std::optional<Bitmap> PixelTexture::AcquireFrame() const {
  const std::lock_guard lock(mutex_);
  return frame_;
}

bool NativeBufferTexture::IsSupported() noexcept {
  return ImportBuffer() != nullptr;
}

NativeBufferTexture::NativeBufferTexture(Size intrinsic_size) : ExternalTexture(intrinsic_size) {}

bool NativeBufferTexture::Publish(OH_NativeBuffer* buffer) {
  if (!buffer) {
    throw std::invalid_argument("HuxerUI OHOS texture requires a buffer");
  }
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(buffer, &config);
  if (config.width <= 0 || config.height <= 0 || config.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888) {
    throw std::invalid_argument("HuxerUI OHOS texture requires a nonempty RGBA8 buffer");
  }

  const auto import = ImportBuffer();
  constexpr int cpu_access = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
  if (!import || (config.usage & cpu_access) != cpu_access || OH_NativeBuffer_Reference(buffer) != 0) {
    return false;
  }
  std::shared_ptr<OH_NativeBuffer> retained(buffer, &OH_NativeBuffer_Unreference);
  OH_PixelmapNative* pixelmap = nullptr;
  if (import(buffer, &pixelmap) != IMAGE_SUCCESS || !pixelmap) {
    return false;
  }
  std::shared_ptr<const Frame> frame = std::make_shared<Frame>(Frame{
      std::move(retained), std::shared_ptr<OH_PixelmapNative>(pixelmap, &OH_PixelmapNative_Release),
      {static_cast<float>(config.width), static_cast<float>(config.height)}});
  {
    const std::lock_guard lock(mutex_);
    if (finished_) {
      throw std::logic_error("HuxerUI OHOS external texture is finished");
    }
    frame_.swap(frame);
  }
  NotifyFrameAvailable();
  return true;
}

void NativeBufferTexture::Finish() noexcept {
  const std::lock_guard lock(mutex_);
  finished_ = true;
}

Task<std::optional<Bitmap>> NativeBufferTexture::SnapshotAsync() const {
  return RunWorker([](std::shared_ptr<const Frame> frame) -> std::optional<Bitmap> {
    if (!frame) {
      return std::nullopt;
    }
    return detail::CopyOhosPixelmap(frame->pixelmap.get(), 1.0F, true);
  }, AcquireFrame());
}

std::shared_ptr<const NativeBufferTexture::Frame> NativeBufferTexture::AcquireFrame() const {
  const std::lock_guard lock(mutex_);
  return frame_;
}

} // namespace huxerui::ohos
