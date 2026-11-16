#pragma once

#include <span>

#include <huxerui/bitmap.h>

namespace huxerui {

class ImageAsset;

namespace detail {

std::uint64_t NextImageIdentity() noexcept;
std::size_t BitmapByteSize(std::uint32_t width, std::uint32_t height);

enum class PixelAlpha { Straight, Premultiplied, Opaque };

Bitmap CopyBitmapRows(std::uint32_t width, std::uint32_t height, const std::byte* pixels,
                      std::size_t stride, Bitmap::PixelFormat format, PixelAlpha alpha, float scale = 1.0F);
void UnpremultiplyRgba(std::span<std::byte> pixels);
Bitmap FlattenBitmap(Bitmap bitmap, Color background);

Bitmap ApplyEncodedOrientation(Bitmap bitmap, std::uint16_t orientation);

// Implemented in the existing platform image backend. Both tasks own their input.
Task<Bitmap> DecodeBitmap(ImageAsset image, std::uint16_t orientation);
Task<Bytes> EncodeBitmap(Bitmap bitmap, ImageEncodeOptions options);

} // namespace detail
} // namespace huxerui
