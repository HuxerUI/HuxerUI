#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace huxerui::resource_codegen {

struct Options {
  std::filesystem::path root;
  std::filesystem::path output;
  std::string resource_namespace;
  std::string header_name{};
};

struct MergeOptions {
  std::vector<std::filesystem::path> inputs;
  std::filesystem::path output;
};

void Generate(const Options& options);
void Merge(const MergeOptions& options);

} // namespace huxerui::resource_codegen
