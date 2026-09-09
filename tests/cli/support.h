#pragma once

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "platform.h"
#include "process_runner.h"
#include "project.h"
#include "sdk.h"
#include "template.h"

namespace huxerui::cli::test {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("huxerui-cli-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

struct Invocation {
  int result = 0;
  std::string output;
  std::string error;
};

inline Invocation Invoke(const std::filesystem::path& directory, std::initializer_list<std::string_view> arguments) {
  const std::vector<std::string_view> values(arguments);
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input("n\n");
  huxerui::cli::SdkLocation sdk;
  if (!values.empty() && values.front() == "create") {
    sdk = {HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable};
  }
  const int result = huxerui::cli::Run(values, directory, sdk, input, output, error);
  return {result, output.str(), error.str()};
}

inline std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  std::erase(content, '\r');
  return content;
}

inline std::string ReadBinary(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

inline std::vector<std::string> ExpectedLibraryGraphArguments(const huxerui::cli::PlatformCommandContext& context) {
  std::vector<std::string> arguments{
      "-S",
      context.project_root.string(),
      "-B",
      (context.project_root / ".huxerui/build" /
       huxerui::cli::BuildHomeKey(context.huxerui_home) / "library-graph").string(),
      "-DHUXERUI_LIBRARY_GRAPH_ONLY=ON",
      "-DHUXERUI_LIBRARY_GRAPH_OUTPUT=" + (context.project_root / ".huxerui/generated/libraries.json").string(),
      "-DHUXERUI_HOME=" + context.huxerui_home.string(),
  };
  if (!huxerui::cli::ReadEnvironmentVariable("CMAKE_GENERATOR") && huxerui::cli::FindExecutable("ninja")) {
    arguments.insert(arguments.begin(), {"-G", "Ninja"});
  }
  return arguments;
}

#if !defined(_WIN32)
inline bool IsExecutable(const std::filesystem::path& path) {
  const std::filesystem::perms permissions = std::filesystem::status(path).permissions();
  return (permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                         std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
}
#endif

} // namespace huxerui::cli::test
