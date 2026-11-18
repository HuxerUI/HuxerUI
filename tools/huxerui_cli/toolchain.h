#pragma once

#include "platform.h"

namespace huxerui::cli::detail {

struct ToolArchive {
  std::string url;
  std::string checksum;
  std::string directory;
  std::filesystem::path destination;
};

std::string_view HostArchitecture();
std::filesystem::path ManagedToolDirectory();
std::string ParseToolVersion(std::string_view output);
bool VersionAtLeast(std::string_view version, std::string_view minimum);
EnvironmentDiagnostic CheckTool(std::string id, std::string label, const std::filesystem::path& path,
                                std::string_view minimum = {}, const ProcessEnvironment& environment = {},
                                std::string_view version_flag = "--version");
bool NeedsSetup(std::span<const EnvironmentDiagnostic> diagnostics, std::string_view id);
std::string EnvironmentValue(const ProcessEnvironment& environment, std::string_view name);
void PrependPath(ProcessEnvironment& environment, const std::filesystem::path& path);
ProcessEnvironment ParseProcessEnvironment(std::string_view text);
ProcessCommand WithEnvironment(ProcessCommand command, const ProcessEnvironment& environment);

EnvironmentDiagnostic ResolveCMake(const ProcessEnvironment& environment = {},
                                   const std::filesystem::path& preferred = {});
SetupAction PlanCMakeSetup();
SetupAction InstallTool(std::string id, std::string description, ToolArchive archive,
                        std::vector<std::string> prerequisites = {}, std::vector<std::string> provides = {});
std::filesystem::path RequireSetupTool(std::string_view name);
bool IsUbuntuNoble();
int InstallUbuntuPackages(std::span<const std::string> packages);

} // namespace huxerui::cli::detail
