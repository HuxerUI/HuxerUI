#pragma once

#include <multimedia/image_framework/image/pixelmap_native.h>

#include <memory>
#include <utility>

#include <huxerui/bitmap.h>

namespace huxerui {
class ImageAsset;

namespace detail {

using OhosPixelmap = std::unique_ptr<OH_PixelmapNative, decltype(&OH_PixelmapNative_Release)>;

OhosPixelmap CreateOhosPixelmap(std::uint32_t width, std::uint32_t height);
OhosPixelmap DecodeOhosPixelmap(const ImageAsset& asset);
std::pair<std::uint32_t, std::uint32_t> OhosPixelmapDimensions(OH_PixelmapNative* pixelmap);
Bitmap CopyOhosPixelmap(OH_PixelmapNative* pixelmap, float scale, bool premultiplied);
Bytes EncodeOhosPixelmap(OH_PixelmapNative* pixelmap, ImageEncodeOptions options, std::size_t limit);
Bitmap DecodeOhosBitmap(const ImageAsset& asset);

} // namespace detail
} // namespace huxerui
