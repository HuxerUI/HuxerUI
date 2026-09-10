#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace huxerui::codegen {

// The text the transform injects around a [[huxerui::composable]] body.
//
// This is the EXPANSION of HUXERUI_SCOPE_BEGIN / HUXERUI_SCOPE_END rather than
// the macro names, and the difference matters for exactly one reason: macros do
// not cross a module boundary. An application that writes `import huxerui;`
// instead of `#include <huxerui/huxerui.h>` never sees those macros, so
// generated code naming them would not compile at all.
//
// Emitting the expansion makes generated code macro-free and therefore
// identical under both spellings. The macros themselves remain public API for
// hand-written code, and the transform still RECOGNISES them in its input --
// see the explicit-scope diagnostic in transform.cpp.
inline constexpr std::string_view kScopeOpenText =
    "return ::huxerui::Scope([=]() -> ::huxerui::View {";
inline constexpr std::string_view kScopeCloseText = "});";

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
  std::size_t composable_count = 0;
};

[[nodiscard]] SourcePosition PositionAt(std::string_view source, std::size_t offset) noexcept;

[[nodiscard]] TransformResult TransformSource(std::string_view source, std::string_view source_path);

} // namespace huxerui::codegen
