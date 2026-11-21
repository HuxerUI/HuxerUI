#include "ohos_bitmap.h"

#include <multimedia/image_framework/image/image_packer_native.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/resource.h>

#include "graphics/bitmap_internal.h"

namespace huxerui::detail {
namespace {

void CheckImageResult(Image_ErrorCode result, const char* message) {
  if (result != IMAGE_SUCCESS) {
    throw std::runtime_error(std::string(message) + " (" + std::to_string(result) + ")");
  }
}

template <class T, auto create, auto release>
auto CreateImageResource(const char* message) {
  T* result = nullptr;
  CheckImageResult(create(&result), message);
  return std::unique_ptr<T, decltype(release)>(result, release);
}

auto PixelmapInfo(OH_PixelmapNative* pixelmap) {
  auto info = CreateImageResource<OH_Pixelmap_ImageInfo, OH_PixelmapImageInfo_Create,
      OH_PixelmapImageInfo_Release>("HuxerUI could not create OHOS image information");
  CheckImageResult(OH_PixelmapNative_GetImageInfo(pixelmap, info.get()),
                   "HuxerUI could not get OHOS image information");
  return info;
}

std::pair<std::uint32_t, std::uint32_t> PixelmapDimensions(OH_Pixelmap_ImageInfo* info) {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  CheckImageResult(OH_PixelmapImageInfo_GetWidth(info, &width), "HuxerUI could not get the OHOS image width");
  CheckImageResult(OH_PixelmapImageInfo_GetHeight(info, &height), "HuxerUI could not get the OHOS image height");
  return {width, height};
}

} // namespace

std::pair<std::uint32_t, std::uint32_t> OhosPixelmapDimensions(OH_PixelmapNative* pixelmap) {
  return PixelmapDimensions(PixelmapInfo(pixelmap).get());
}

Bitmap CopyOhosPixelmap(OH_PixelmapNative* pixelmap, float scale, bool premultiplied) {
  const auto info = PixelmapInfo(pixelmap);
  const auto [width, height] = PixelmapDimensions(info.get());
  std::int32_t format = 0;
  CheckImageResult(OH_PixelmapImageInfo_GetPixelFormat(info.get(), &format),
                   "HuxerUI could not get the OHOS image format");
  if (format != PIXEL_FORMAT_RGBA_8888) {
    throw std::runtime_error("HuxerUI OHOS PixelMap does not contain RGBA8 pixels");
  }

  // ReadPixels copies tightly packed rows even if the PixelMap stores padded rows.
  Bytes pixels(BitmapByteSize(width, height));
  std::size_t size = pixels.size();
  CheckImageResult(OH_PixelmapNative_ReadPixels(pixelmap, reinterpret_cast<std::uint8_t*>(pixels.data()), &size),
                   "HuxerUI could not read the OHOS image pixels");
  if (size != pixels.size()) {
    throw std::runtime_error("HuxerUI OHOS PixelMap returned an unexpected pixel count");
  }
  if (premultiplied) {
    UnpremultiplyRgba(pixels);
  }
  return Bitmap::FromRgba8(width, height, std::move(pixels), scale);
}

OhosPixelmap CreateOhosPixelmap(std::uint32_t width, std::uint32_t height) {
  const auto options = CreateImageResource<OH_Pixelmap_InitializationOptions, OH_PixelmapInitializationOptions_Create,
      OH_PixelmapInitializationOptions_Release>("HuxerUI could not create OHOS pixel map options");
  CheckImageResult(OH_PixelmapInitializationOptions_SetWidth(options.get(), width),
                   "HuxerUI could not set the OHOS pixel map width");
  CheckImageResult(OH_PixelmapInitializationOptions_SetHeight(options.get(), height),
                   "HuxerUI could not set the OHOS pixel map height");
  CheckImageResult(OH_PixelmapInitializationOptions_SetPixelFormat(options.get(), PIXEL_FORMAT_RGBA_8888),
                   "HuxerUI could not set the OHOS pixel map format");
  CheckImageResult(OH_PixelmapInitializationOptions_SetAlphaType(options.get(), PIXELMAP_ALPHA_TYPE_UNPREMULTIPLIED),
                   "HuxerUI could not set the OHOS pixel map alpha type");
  CheckImageResult(OH_PixelmapInitializationOptions_SetEditable(options.get(), true),
                   "HuxerUI could not make the OHOS pixel map writable");
  OH_PixelmapNative* pixelmap = nullptr;
  CheckImageResult(OH_PixelmapNative_CreateEmptyPixelmap(options.get(), &pixelmap),
                   "HuxerUI could not create an OHOS pixel map");
  return {pixelmap, &OH_PixelmapNative_Release};
}

Bytes EncodeOhosPixelmap(OH_PixelmapNative* pixelmap, ImageEncodeOptions settings, std::size_t limit) {
  const auto packer = CreateImageResource<OH_ImagePackerNative, OH_ImagePackerNative_Create,
      OH_ImagePackerNative_Release>("HuxerUI could not create the OHOS image encoder");
  const auto options = CreateImageResource<OH_PackingOptions, OH_PackingOptions_Create,
      OH_PackingOptions_Release>("HuxerUI could not create OHOS image encoder options");
  const bool jpeg = settings.format == ImageFormat::Jpeg;
  std::string type = jpeg ? "image/jpeg" : "image/png";
  Image_MimeType mime{type.data(), type.size()};
  CheckImageResult(OH_PackingOptions_SetMimeType(options.get(), &mime), "HuxerUI could not set the OHOS image type");
  const auto quality = jpeg ? static_cast<std::uint32_t>(settings.jpeg_quality) : 100U;
  CheckImageResult(OH_PackingOptions_SetQuality(options.get(), quality),
                   "HuxerUI could not set the OHOS image quality");

  // The buffer-based encoder requires a capacity up front. An anonymous file lets the encoder determine the size.
  const int descriptor = memfd_create("huxerui-image", MFD_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("HuxerUI could not create an OHOS image output");
  }
  const std::unique_ptr<std::FILE, decltype(&std::fclose)> output(fdopen(descriptor, "rb+"), &std::fclose);
  if (!output) {
    close(descriptor);
    throw std::runtime_error("HuxerUI could not open the OHOS image output");
  }
  CheckImageResult(OH_ImagePackerNative_PackToFileFromPixelmap(packer.get(), options.get(), pixelmap, descriptor),
                   "HuxerUI could not encode the OHOS bitmap");
  const auto size = lseek(descriptor, 0, SEEK_END);
  if (size <= 0 || lseek(descriptor, 0, SEEK_SET) < 0) {
    throw std::runtime_error("HuxerUI could not rewind the OHOS image output");
  }
  if (static_cast<std::uintmax_t>(size) > limit) {
    throw std::length_error("HuxerUI OHOS encoded image exceeds the allowed size");
  }
  Bytes bytes(static_cast<std::size_t>(size));
  if (std::fread(bytes.data(), 1, bytes.size(), output.get()) != bytes.size()) {
    throw std::runtime_error("HuxerUI could not read the OHOS image output");
  }
  return bytes;
}

OhosPixelmap DecodeOhosPixelmap(const ImageAsset& asset) {
  if (!asset.HasValue()) {
    throw std::logic_error("HuxerUI cannot decode a missing image asset");
  }
  const auto bytes = asset.EncodedBytes();
  Bytes buffer(bytes.begin(), bytes.end());
  OH_ImageSourceNative* image = nullptr;
  auto* data = reinterpret_cast<std::uint8_t*>(buffer.data());
  CheckImageResult(OH_ImageSourceNative_CreateFromData(data, buffer.size(), &image),
                   "HuxerUI could not open the OHOS image");
  const std::unique_ptr<OH_ImageSourceNative, decltype(&OH_ImageSourceNative_Release)> source(
      image, &OH_ImageSourceNative_Release);
  const auto options = CreateImageResource<OH_DecodingOptions, OH_DecodingOptions_Create, OH_DecodingOptions_Release>(
      "HuxerUI could not create OHOS image decoder options");
  CheckImageResult(OH_DecodingOptions_SetPixelFormat(options.get(), PIXEL_FORMAT_RGBA_8888),
                   "HuxerUI could not set the OHOS image decoder pixel format");
  CheckImageResult(OH_DecodingOptions_SetDesiredDynamicRange(options.get(), IMAGE_DYNAMIC_RANGE_SDR),
                   "HuxerUI could not set the OHOS image decoder dynamic range");
  OH_PixelmapNative* pixelmap = nullptr;
  CheckImageResult(OH_ImageSourceNative_CreatePixelmap(source.get(), options.get(), &pixelmap),
                   "HuxerUI could not decode the OHOS image");
  return {pixelmap, &OH_PixelmapNative_Release};
}

Bitmap DecodeOhosBitmap(const ImageAsset& asset) {
  // Image Source decodes RGBA8 to premultiplied pixels.
  return CopyOhosPixelmap(DecodeOhosPixelmap(asset).get(), asset.Scale(), true);
}

Task<Bitmap> DecodeBitmap(ImageAsset image, std::uint16_t orientation) {
  return RunWorker([orientation](ImageAsset asset) {
    return ApplyEncodedOrientation(DecodeOhosBitmap(asset), orientation);
  }, std::move(image));
}

Task<Bytes> EncodeBitmap(Bitmap bitmap, ImageEncodeOptions options) {
  return RunWorker([](Bitmap source, ImageEncodeOptions settings) {
    if (settings.format == ImageFormat::Jpeg) {
      source = FlattenBitmap(std::move(source), settings.jpeg_background);
    }
    const auto pixelmap = CreateOhosPixelmap(source.PixelWidth(), source.PixelHeight());
    const auto pixels = source.Pixels();
    Bytes buffer(pixels.begin(), pixels.end());
    CheckImageResult(OH_PixelmapNative_WritePixels(pixelmap.get(), reinterpret_cast<std::uint8_t*>(buffer.data()),
                                                 buffer.size()), "HuxerUI could not write the OHOS pixel map");
    return EncodeOhosPixelmap(pixelmap.get(), settings, std::numeric_limits<std::size_t>::max());
  }, std::move(bitmap), options);
}

} // namespace huxerui::detail
