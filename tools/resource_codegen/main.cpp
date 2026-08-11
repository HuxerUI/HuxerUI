#include "generator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view generate_usage =
    "usage: hapt --root <path> --output <path> --namespace <name> [--header-name <filename>]";
constexpr std::string_view merge_usage = "usage: hapt merge --input <package> [--input <package> ...] --output <path>";

huxerui::resource_codegen::Options ParseGenerateArguments(int argc, char** argv) {
  huxerui::resource_codegen::Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--root" && index + 1 < argc) {
      if (!options.root.empty()) {
        throw std::invalid_argument(std::string(generate_usage));
      }
      options.root = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      if (!options.output.empty()) {
        throw std::invalid_argument(std::string(generate_usage));
      }
      options.output = argv[++index];
    } else if (argument == "--namespace" && index + 1 < argc) {
      if (!options.resource_namespace.empty()) {
        throw std::invalid_argument(std::string(generate_usage));
      }
      options.resource_namespace = argv[++index];
    } else if (argument == "--header-name" && index + 1 < argc) {
      if (!options.header_name.empty()) {
        throw std::invalid_argument(std::string(generate_usage));
      }
      options.header_name = argv[++index];
    } else {
      throw std::invalid_argument(std::string(generate_usage));
    }
  }
  if (options.root.empty() || options.output.empty() || options.resource_namespace.empty()) {
    throw std::invalid_argument(std::string(generate_usage));
  }
  return options;
}

huxerui::resource_codegen::MergeOptions ParseMergeArguments(int argc, char** argv) {
  huxerui::resource_codegen::MergeOptions options;
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
      huxerui::resource_codegen::Merge(ParseMergeArguments(argc, argv));
    } else {
      huxerui::resource_codegen::Generate(ParseGenerateArguments(argc, argv));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hapt: " << error.what() << '\n';
    return 1;
  }
}
