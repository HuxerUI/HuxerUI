#include "transform.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Arguments {
  std::filesystem::path input;
  std::filesystem::path output;
};

[[nodiscard]] Arguments ParseArguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--input" && index + 1 < argc) {
      arguments.input = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      arguments.output = argv[++index];
    } else {
      throw std::invalid_argument(
          "usage: hcg --input <path> --output <path>");
    }
  }
  if (arguments.input.empty() || arguments.output.empty()) {
    throw std::invalid_argument(
        "usage: hcg --input <path> --output <path>");
  }
  return arguments;
}

[[nodiscard]] std::string ReadFile(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error(
        "unable to open input file: " + path.string());
  }
  return {
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>(),
  };
}

void WriteIfChanged(
    const std::filesystem::path& path,
    std::string_view content) {
  if (std::filesystem::exists(path) &&
      ReadFile(path) == content) {
    return;
  }

  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream stream(
      path,
      std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error(
        "unable to open output file: " + path.string());
  }
  stream.write(
      content.data(),
      static_cast<std::streamsize>(content.size()));
  if (!stream) {
    throw std::runtime_error(
        "unable to write output file: " + path.string());
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Arguments arguments = ParseArguments(argc, argv);
    const std::string source = ReadFile(arguments.input);

    try {
      const huxerui::codegen::TransformResult result =
          huxerui::codegen::TransformSource(
              source,
              arguments.input.string());
      WriteIfChanged(arguments.output, result.source);
    } catch (const huxerui::codegen::TransformError& error) {
      const huxerui::codegen::SourcePosition position =
          huxerui::codegen::PositionAt(
              source,
              error.Offset());
      std::cerr
          << arguments.input.string()
          << ':' << position.line
          << ':' << position.column
          << ": error: " << error.what()
          << '\n';
      return 1;
    }
  } catch (const std::exception& error) {
    std::cerr << "hcg: error: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
