#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace huxerui::cli {

struct ProcessCommand {
  std::string executable;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
};

struct ProcessResult {
  int exit_code = 0;
  std::string output;
};

[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(std::string_view name);
void SetProcessEnvironmentVariable(std::string_view name, std::string_view value);
[[nodiscard]] std::optional<std::filesystem::path> FindExecutable(std::string_view name);
[[nodiscard]] std::string DescribeProcess(const ProcessCommand& command);
int RunProcess(const ProcessCommand& command);
[[nodiscard]] ProcessResult RunProcessCapture(const ProcessCommand& command);

} // namespace huxerui::cli
