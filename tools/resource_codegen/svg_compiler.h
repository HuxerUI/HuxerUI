#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace huxerui::resource_codegen {

struct CompiledSvg {
  std::vector<std::byte> payload;
  float intrinsic_width = 0.0F;
  float intrinsic_height = 0.0F;
};

CompiledSvg CompileSvg(const std::filesystem::path& path);

} // namespace huxerui::resource_codegen
