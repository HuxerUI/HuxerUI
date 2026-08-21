#include "platform.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace huxerui::cli {
namespace {

const std::array<const PlatformDriver*, 6>& PlatformDrivers() {
  static const std::array drivers{
      &detail::AndroidPlatformDriver(),
      &detail::WindowsPlatformDriver(),
      &detail::LinuxPlatformDriver(),
      &detail::MacOSPlatformDriver(),
      &detail::IosPlatformDriver(),
      &detail::WebPlatformDriver(),
  };
  return drivers;
}

} // namespace

namespace detail {

std::vector<Diagnostic>
ValidateRequiredFiles(const std::filesystem::path& root, std::span<const std::string_view> paths) {
  std::vector<Diagnostic> diagnostics;
  for (const std::string_view relative_path : paths) {
    if (!std::filesystem::is_regular_file(root / relative_path)) {
      diagnostics.push_back({true, "missing " + std::string(relative_path)});
    }
  }
  return diagnostics;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string JsonString(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(marker);
  if (key_position == std::string_view::npos) {
    throw std::runtime_error("integration plan is missing " + std::string(key));
  }
  const std::size_t colon = json.find(':', key_position + marker.size());
  const std::size_t quote = colon == std::string_view::npos ? colon : json.find('\"', colon + 1);
  if (quote == std::string_view::npos) {
    throw std::runtime_error("integration plan has an invalid " + std::string(key));
  }

  std::string value;
  bool escaped = false;
  for (std::size_t index = quote + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (escaped) {
      if (character == 'n') {
        value.push_back('\n');
      } else if (character == 'r') {
        value.push_back('\r');
      } else {
        value.push_back(character);
      }
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '\"') {
      return value;
    } else {
      value.push_back(character);
    }
  }
  throw std::runtime_error("integration plan has an unterminated " + std::string(key));
}

std::filesystem::path AppIntegrationPlan(const PlatformCommandContext& context) {
  const std::filesystem::path root = context.build_directory / "huxerui-integration";
  std::vector<std::filesystem::path> plans;
  if (std::filesystem::is_directory(root)) {
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() && entry.path().filename() == "app.json") {
        plans.push_back(entry.path());
      }
    }
  }
  if (plans.size() != 1) {
    throw std::runtime_error(
        plans.empty() ? "application integration plan was not generated"
                      : "build produced more than one application integration plan"
    );
  }
  return plans.front();
}

std::vector<std::string> DeviceArguments(const std::optional<PlatformDevice>& device) {
  if (!device) {
    return {};
  }
  return {"-s", device->id};
}

std::string ProfileConfiguration(std::string_view profile) {
  if (profile == "debug") {
    return "Debug";
  }
  if (profile == "release") {
    return "Release";
  }
  throw std::invalid_argument("unknown build profile: " + std::string(profile));
}

std::vector<ProcessCommand> DesktopBuildCommands(const PlatformCommandContext& context) {
  const std::string configuration = ProfileConfiguration(context.profile);
  std::vector<std::string> configure_arguments{
      "-S",
      context.project_root.string(),
      "-B",
      context.build_directory.string(),
      "-DCMAKE_BUILD_TYPE=" + configuration,
  };
  if (!context.cmake_generator.empty()) {
    configure_arguments.insert(configure_arguments.begin(), {"-G", context.cmake_generator});
  }
  return {
      {"cmake", std::move(configure_arguments), context.project_root},
      {"cmake",
       {"--build", context.build_directory.string(), "--config", configuration, "--parallel"},
       context.project_root},
  };
}

std::vector<ProcessCommand> ModuleGraphConfigureCommands(const PlatformCommandContext& context) {
  return {
      {"cmake",
       {
           "-S",
           context.project_root.string(),
           "-B",
           (context.project_root / ".huxerui/build/module-graph").string(),
           "-DCMAKE_BUILD_TYPE=Debug",
           "-DHUXERUI_MODULE_GRAPH_ONLY=ON",
       },
       context.project_root},
  };
}

} // namespace detail

std::vector<GeneratedFile> PlatformDriver::CreateModulePackage(const ProjectTemplateContext&) const {
  return {};
}

bool PlatformDriver::SupportsDeviceDiscovery() const noexcept {
  return false;
}

std::vector<PlatformDevice> PlatformDriver::DiscoverDevices() const {
  throw std::logic_error("platform does not support device discovery: " + std::string(Id()));
}

std::vector<ProcessCommand> PlatformDriver::ModuleGraphCommands(const PlatformCommandContext&) const {
  return {};
}

void PlatformDriver::UpdateProjectIntegration(const PlatformCommandContext&) const {}

std::vector<ProcessCommand> PlatformDriver::OpenCommands(const PlatformCommandContext&) const {
  throw std::logic_error("platform does not support opening a development project: " + std::string(Id()));
}

std::string_view DeviceStateName(DeviceState state) noexcept {
  switch (state) {
  case DeviceState::Ready:
    return "ready";
  case DeviceState::Offline:
    return "offline";
  case DeviceState::Unauthorized:
    return "unauthorized";
  case DeviceState::Unavailable:
    return "unavailable";
  }
  return "unavailable";
}

std::string_view CurrentHostId() noexcept {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

const PlatformDriver* FindPlatformDriver(std::string_view id) noexcept {
  const std::array<const PlatformDriver*, 6>& drivers = PlatformDrivers();
  const auto iterator =
      std::find_if(drivers.begin(), drivers.end(), [id](const PlatformDriver* driver) { return driver->Id() == id; });
  return iterator == drivers.end() ? nullptr : *iterator;
}

std::vector<std::string_view> PlatformIds() {
  const std::array<const PlatformDriver*, 6>& drivers = PlatformDrivers();
  std::vector<std::string_view> ids;
  ids.reserve(drivers.size());
  for (const PlatformDriver* driver : drivers) {
    ids.push_back(driver->Id());
  }
  return ids;
}

} // namespace huxerui::cli
