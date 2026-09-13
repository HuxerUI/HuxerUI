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
  // Where to report the files this compilation read, and which output the
  // report is about. Empty means no report, which is the CMake path: it
  // enumerates the resource tree itself. A build program that cannot afford
  // that enumeration asks for both instead -- see Compile().
  std::filesystem::path depfile{};
  std::filesystem::path depfile_target{};
  // The C++ module to write beside the header: `<output>/modules/<namespace>_resources.cppm`
  // exporting the same declarations, so a module-style application writes
  // `import <name>;` instead of including the header. Empty means no module.
  std::string module_name{};
};

struct MergeOptions {
  std::vector<std::filesystem::path> inputs;
  std::filesystem::path output;
};

void Compile(const CompileOptions& options);
void Merge(const MergeOptions& options);

} // namespace huxerui::resource_compiler
