#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace huxerui::resource_compiler {

struct CompileOptions {
  std::filesystem::path root;
  std::filesystem::path output;
  std::string resource_namespace;
  std::string header_name{};
};

struct MergeOptions {
  std::vector<std::filesystem::path> inputs;
  std::filesystem::path output;
};

void Compile(const CompileOptions& options);
void Merge(const MergeOptions& options);

} // namespace huxerui::resource_compiler
