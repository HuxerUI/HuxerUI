#include "compiler.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view compile_usage =
    "usage: hrc --root <path> --output <path> --namespace <name> [--header-name <filename>] "
    "[--module-name <module>] [--depfile <path> --depfile-target <path>]";
constexpr std::string_view merge_usage = "usage: hrc merge --input <package> [--input <package> ...] --output <path>";

huxerui::resource_compiler::CompileOptions ParseCompileArguments(int argc, char** argv) {
  huxerui::resource_compiler::CompileOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--root" && index + 1 < argc) {
      if (!options.root.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.root = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      if (!options.output.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.output = argv[++index];
    } else if (argument == "--namespace" && index + 1 < argc) {
      if (!options.resource_namespace.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.resource_namespace = argv[++index];
    } else if (argument == "--header-name" && index + 1 < argc) {
      if (!options.header_name.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.header_name = argv[++index];
    } else if (argument == "--module-name" && index + 1 < argc) {
      if (!options.module_name.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.module_name = argv[++index];
    } else if (argument == "--depfile" && index + 1 < argc) {
      if (!options.depfile.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.depfile = argv[++index];
    } else if (argument == "--depfile-target" && index + 1 < argc) {
      if (!options.depfile_target.empty()) {
        throw std::invalid_argument(std::string(compile_usage));
      }
      options.depfile_target = argv[++index];
    } else {
      throw std::invalid_argument(std::string(compile_usage));
    }
  }
  if (options.root.empty() || options.output.empty() || options.resource_namespace.empty()) {
    throw std::invalid_argument(std::string(compile_usage));
  }
  // ninja reads a depfile for exactly one target, so the two arrive together
  // or not at all; a depfile naming nothing would be discarded silently.
  if (options.depfile.empty() != options.depfile_target.empty()) {
    throw std::invalid_argument("hrc: --depfile and --depfile-target are used together");
  }
  return options;
}

huxerui::resource_compiler::MergeOptions ParseMergeArguments(int argc, char** argv) {
  huxerui::resource_compiler::MergeOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--input" && index + 1 < argc) {
      options.inputs.emplace_back(argv[++index]);
    } else if (argument == "--output" && index + 1 < argc) {
      options.output = argv[++index];
    } else {
      throw std::invalid_argument(std::string(merge_usage));
    }
  }
  if (options.inputs.empty() || options.output.empty()) {
    throw std::invalid_argument(std::string(merge_usage));
  }
  return options;
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "merge") {
      huxerui::resource_compiler::Merge(ParseMergeArguments(argc, argv));
    } else {
      huxerui::resource_compiler::Compile(ParseCompileArguments(argc, argv));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hrc: " << error.what() << '\n';
    return 1;
  }
}
