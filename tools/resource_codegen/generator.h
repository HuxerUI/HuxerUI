#pragma once

#include <filesystem>
#include <string>

namespace huxerui::resource_codegen {

struct Options {
  std::filesystem::path root;
  std::filesystem::path output;
  std::string resource_namespace;
};

void Generate(const Options& options);

} // namespace huxerui::resource_codegen
