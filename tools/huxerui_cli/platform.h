#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process_runner.h"
#include "template.h"

namespace huxerui::cli {

struct Diagnostic {
  bool error = false;
  std::string message;
};

enum class DeviceState {
  Ready,
  Offline,
  Unauthorized,
  Unavailable,
};

enum class DeviceKind {
  Unspecified,
  Physical,
  Simulator,
};

struct PlatformDevice {
  std::string id;
  std::string name;
  DeviceState state = DeviceState::Unavailable;
  DeviceKind kind = DeviceKind::Unspecified;
  std::string destination_id;

  bool operator==(const PlatformDevice&) const = default;
};

struct PlatformCommandContext {
  std::filesystem::path project_root;
  std::filesystem::path huxerui_home;
  std::filesystem::path build_directory;
  std::string cmake_generator;
  std::string profile;
  std::optional<PlatformDevice> device;
};

class PlatformDriver {
public:
  virtual ~PlatformDriver() = default;

  [[nodiscard]] virtual std::string_view Id() const noexcept = 0;
  [[nodiscard]] virtual bool SupportsCurrentHost() const noexcept = 0;
  [[nodiscard]] virtual std::span<const std::string_view> RequiredTools() const noexcept = 0;
  [[nodiscard]] virtual std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const = 0;
  [[nodiscard]] virtual std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext& context) const;
  [[nodiscard]] virtual std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const = 0;
  [[nodiscard]] virtual bool SupportsDeviceDiscovery() const noexcept;
  [[nodiscard]] virtual std::vector<PlatformDevice> DiscoverDevices() const;
  [[nodiscard]] virtual std::vector<ProcessCommand> ModuleGraphCommands(const PlatformCommandContext& context) const;
  virtual void UpdateProjectIntegration(const PlatformCommandContext& context) const;
  [[nodiscard]] virtual std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const = 0;
  [[nodiscard]] virtual std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const = 0;
  [[nodiscard]] virtual std::vector<ProcessCommand> OpenCommands(const PlatformCommandContext& context) const;
};

[[nodiscard]] std::string_view DeviceStateName(DeviceState state) noexcept;
[[nodiscard]] std::vector<PlatformDevice> ParseAdbDevices(std::string_view output);
[[nodiscard]] std::vector<PlatformDevice> ParseIosPhysicalDevices(std::string_view output);
[[nodiscard]] std::vector<PlatformDevice> ParseIosSimulatorDevices(std::string_view output);
[[nodiscard]] std::string_view CurrentHostId() noexcept;
[[nodiscard]] const PlatformDriver* FindPlatformDriver(std::string_view id) noexcept;
[[nodiscard]] std::vector<std::string_view> PlatformIds();

} // namespace huxerui::cli

namespace huxerui::cli::detail {

[[nodiscard]] std::vector<Diagnostic>
ValidateRequiredFiles(const std::filesystem::path& root, std::span<const std::string_view> paths);
[[nodiscard]] std::string ReadFile(const std::filesystem::path& path);
[[nodiscard]] std::string JsonString(std::string_view json, std::string_view key);
[[nodiscard]] std::filesystem::path AppIntegrationPlan(const PlatformCommandContext& context);
[[nodiscard]] std::vector<std::string> DeviceArguments(const std::optional<PlatformDevice>& device);
[[nodiscard]] std::string ProfileConfiguration(std::string_view profile);
[[nodiscard]] std::vector<ProcessCommand> DesktopBuildCommands(const PlatformCommandContext& context);
[[nodiscard]] std::vector<ProcessCommand> ModuleGraphConfigureCommands(const PlatformCommandContext& context);

[[nodiscard]] const PlatformDriver& AndroidPlatformDriver() noexcept;
[[nodiscard]] const PlatformDriver& WindowsPlatformDriver() noexcept;
[[nodiscard]] const PlatformDriver& LinuxPlatformDriver() noexcept;
[[nodiscard]] const PlatformDriver& MacOSPlatformDriver() noexcept;
[[nodiscard]] const PlatformDriver& IosPlatformDriver() noexcept;
[[nodiscard]] const PlatformDriver& WebPlatformDriver() noexcept;

} // namespace huxerui::cli::detail
