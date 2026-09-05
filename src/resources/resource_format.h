#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace huxerui::detail::resource_format {

inline constexpr std::array<std::byte, 8> magic{
    std::byte{'H'},
    std::byte{'U'},
    std::byte{'X'},
    std::byte{'R'},
    std::byte{'E'},
    std::byte{'S'},
    std::byte{0},
    std::byte{0},
};

inline constexpr std::uint32_t current_version = 1;

enum class EntryKind : std::uint8_t {
  Raw = 1,
  Image = 2,
  String = 3,
};

inline std::uint64_t ContentHash(std::span<const std::byte> bytes) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::byte byte : bytes) {
    hash ^= std::to_integer<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

} // namespace huxerui::detail::resource_format
