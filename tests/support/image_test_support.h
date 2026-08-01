#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <span>
#include <string_view>
#include <vector>

namespace huxerui::test {

inline void AppendBigEndian32(std::vector<std::byte>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::byte>(value >> 24U));
  bytes.push_back(static_cast<std::byte>(value >> 16U));
  bytes.push_back(static_cast<std::byte>(value >> 8U));
  bytes.push_back(static_cast<std::byte>(value));
}

inline std::uint32_t PngCrc32(std::span<const std::byte> bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::byte byte : bytes) {
    crc ^= std::to_integer<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

inline void AppendPngChunk(std::vector<std::byte>& bytes, std::string_view type, std::span<const std::byte> data) {
  AppendBigEndian32(bytes, static_cast<std::uint32_t>(data.size()));
  const std::size_t crc_begin = bytes.size();
  for (char character : type) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  bytes.insert(bytes.end(), data.begin(), data.end());
  AppendBigEndian32(bytes, PngCrc32(std::span<const std::byte>(bytes).subspan(crc_begin)));
}

inline std::vector<std::byte> MakeTestPng(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 || width > 1024 || height > 1024) {
    throw std::invalid_argument("test PNG dimensions are invalid");
  }
  std::vector<std::byte> scanlines;
  scanlines.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 4U + 1U));
  for (std::uint32_t y = 0; y < height; ++y) {
    scanlines.push_back(std::byte{0});
    for (std::uint32_t x = 0; x < width; ++x) {
      scanlines.insert(scanlines.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xFF}});
    }
  }

  std::vector<std::byte> compressed{std::byte{0x78}, std::byte{0x01}};
  for (std::size_t offset = 0; offset < scanlines.size();) {
    const std::size_t block_size = std::min<std::size_t>(65535, scanlines.size() - offset);
    const bool final = offset + block_size == scanlines.size();
    compressed.push_back(final ? std::byte{0x01} : std::byte{0x00});
    compressed.push_back(static_cast<std::byte>(block_size));
    compressed.push_back(static_cast<std::byte>(block_size >> 8U));
    const std::uint16_t complement = static_cast<std::uint16_t>(~block_size);
    compressed.push_back(static_cast<std::byte>(complement));
    compressed.push_back(static_cast<std::byte>(complement >> 8U));
    compressed.insert(
        compressed.end(),
        scanlines.begin() + static_cast<std::ptrdiff_t>(offset),
        scanlines.begin() + static_cast<std::ptrdiff_t>(offset + block_size)
    );
    offset += block_size;
  }
  std::uint32_t adler_a = 1;
  std::uint32_t adler_b = 0;
  for (std::byte byte : scanlines) {
    adler_a = (adler_a + std::to_integer<std::uint8_t>(byte)) % 65521U;
    adler_b = (adler_b + adler_a) % 65521U;
  }
  AppendBigEndian32(compressed, (adler_b << 16U) | adler_a);

  std::vector<std::byte> result{
      std::byte{0x89},
      std::byte{'P'},
      std::byte{'N'},
      std::byte{'G'},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x0A},
  };
  std::array<std::byte, 13> header{};
  header[0] = static_cast<std::byte>(width >> 24U);
  header[1] = static_cast<std::byte>(width >> 16U);
  header[2] = static_cast<std::byte>(width >> 8U);
  header[3] = static_cast<std::byte>(width);
  header[4] = static_cast<std::byte>(height >> 24U);
  header[5] = static_cast<std::byte>(height >> 16U);
  header[6] = static_cast<std::byte>(height >> 8U);
  header[7] = static_cast<std::byte>(height);
  header[8] = std::byte{8};
  header[9] = std::byte{6};
  AppendPngChunk(result, "IHDR", header);
  AppendPngChunk(result, "IDAT", compressed);
  AppendPngChunk(result, "IEND", std::span<const std::byte>{});
  return result;
}

} // namespace huxerui::test
