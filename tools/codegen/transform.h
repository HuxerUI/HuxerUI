#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace huxerui::codegen {

struct SourcePosition {
  std::size_t line = 1;
  std::size_t column = 1;
};

class TransformError final : public std::runtime_error {
public:
  TransformError(std::size_t offset, std::string message);

  [[nodiscard]] std::size_t Offset() const noexcept {
    return offset_;
  }

private:
  std::size_t offset_;
};

struct TransformResult {
  std::string source;
  std::size_t scope_count = 0;
};

[[nodiscard]] SourcePosition PositionAt(
    std::string_view source,
    std::size_t offset) noexcept;

[[nodiscard]] TransformResult TransformSource(
    std::string_view source,
    std::string_view source_path);

}  // namespace huxerui::codegen
