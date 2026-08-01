#include "generator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

huxerui::resource_codegen::Options ParseArguments(int argc, char** argv) {
  huxerui::resource_codegen::Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--root" && index + 1 < argc) {
      options.root = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      options.output = argv[++index];
    } else if (argument == "--namespace" && index + 1 < argc) {
      options.resource_namespace = argv[++index];
    } else {
      throw std::invalid_argument("usage: huxerui-resource-codegen --root <path> --output <path> --namespace <name>");
    }
  }
  if (options.root.empty() || options.output.empty() || options.resource_namespace.empty()) {
    throw std::invalid_argument("usage: huxerui-resource-codegen --root <path> --output <path> --namespace <name>");
  }
  return options;
}

} // namespace

int main(int argc, char** argv) {
  try {
    huxerui::resource_codegen::Generate(ParseArguments(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "huxerui-resource-codegen: " << error.what() << '\n';
    return 1;
  }
}
