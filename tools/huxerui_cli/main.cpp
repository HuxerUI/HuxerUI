#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include "cli.h"
#include "sdk.h"

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  try {
    const auto executable = huxerui::cli::ExecutablePath(argc > 0 ? argv[0] : "huxerui");
    const auto sdk = huxerui::cli::LocateHuxerUIHome(executable);
    return huxerui::cli::Run(arguments, std::filesystem::current_path(), sdk, std::cin, std::cout, std::cerr);
  } catch (const std::exception& exception) {
    std::cerr << "huxerui: " << exception.what() << '\n';
    return 1;
  }
}
