#include "platform.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

void AppendFiles(std::vector<GeneratedFile>& destination, std::vector<GeneratedFile> source) {
  destination.insert(destination.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

std::vector<GeneratedFile> AndroidWrapperFiles() {
  std::vector<GeneratedFile> files = CopyTemplateTree("platform/android/wrapper");
  const auto script =
      std::find_if(files.begin(), files.end(), [](const GeneratedFile& file) { return file.path == "gradlew"; });
  if (script == files.end()) {
    throw std::logic_error("HuxerUI CLI Android wrapper template is missing gradlew");
  }
  script->executable = true;
  return files;
}

class AndroidDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "android";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"cmake"},
        std::string_view{"java"},
        std::string_view{"adb"},
    };
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/android/app", context);
    AppendFiles(files, AndroidWrapperFiles());
    files.push_back({"app/proguard-rules.pro", {}});
    return files;
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/android/module", context);
    AppendFiles(files, AndroidWrapperFiles());
    files.push_back({"consumer-rules.pro", {}});
    return files;
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"settings.gradle"},
        std::string_view{"build.gradle"},
        std::string_view{"gradle.properties"},
        std::string_view{"gradlew"},
        std::string_view{"gradlew.bat"},
        std::string_view{"gradle/libs.versions.toml"},
        std::string_view{"gradle/wrapper/gradle-wrapper.jar"},
        std::string_view{"gradle/wrapper/gradle-wrapper.properties"},
        std::string_view{"app/build.gradle"},
        std::string_view{"app/src/main/AndroidManifest.xml"},
    };
    std::vector<Diagnostic> diagnostics = detail::ValidateRequiredFiles(shell_root, required);
    bool has_activity = false;
    const std::filesystem::path java_root = shell_root / "app/src/main/java";
    if (std::filesystem::is_directory(java_root)) {
      for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(java_root)) {
        if (entry.is_regular_file() && entry.path().filename() == "MainActivity.java") {
          has_activity = true;
          break;
        }
      }
    }
    if (!has_activity) {
      diagnostics.push_back({true, "missing app/src/main/java/.../MainActivity.java"});
    }
    return diagnostics;
  }

  bool SupportsDeviceDiscovery() const noexcept override {
    return true;
  }

  std::vector<PlatformDevice> DiscoverDevices() const override {
    const ProcessCommand command{"adb", {"devices", "-l"}, {}};
    const ProcessResult result = RunProcessCapture(command);
    if (result.exit_code != 0) {
      throw std::runtime_error(
          "command failed with exit code " + std::to_string(result.exit_code) + ": " + DescribeProcess(command)
      );
    }
    return ParseAdbDevices(result.output);
  }

  std::vector<ProcessCommand> ModuleGraphCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("Android native builds do not use a CMake generator option");
    }
    return detail::ModuleGraphConfigureCommands(context);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::filesystem::path shell = context.project_root / "platform/android";
    const std::string configuration = detail::ProfileConfiguration(context.profile);
    const std::filesystem::path wrapper = shell / (CurrentHostId() == "windows" ? "gradlew.bat" : "gradlew");
    return {{wrapper.string(), {":app:assemble" + configuration}, shell}};
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::filesystem::path output_directory =
        context.project_root / "platform/android/app/build/outputs/apk" / context.profile;
    const std::string metadata = detail::ReadFile(output_directory / "output-metadata.json");
    const std::string application_id = detail::JsonString(metadata, "applicationId");
    const std::filesystem::path output_file = detail::JsonString(metadata, "outputFile");
    if (application_id.empty()) {
      throw std::runtime_error("Android APK metadata contains an empty applicationId");
    }
    if (output_file.empty() || output_file.is_absolute() ||
        std::find(output_file.begin(), output_file.end(), std::filesystem::path{".."}) != output_file.end()) {
      throw std::runtime_error("Android APK metadata contains an invalid outputFile");
    }
    const std::filesystem::path apk = output_directory / output_file;
    if (!std::filesystem::is_regular_file(apk)) {
      throw std::runtime_error("Android build artifact is missing: " + apk.string());
    }

    std::vector<std::string> install_arguments = detail::DeviceArguments(context.device);
    install_arguments.insert(install_arguments.end(), {"install", "-r", apk.string()});
    std::vector<std::string> launch_arguments = detail::DeviceArguments(context.device);
    launch_arguments.insert(launch_arguments.end(), {"shell", "am", "start", "-n", application_id + "/.MainActivity"});
    return {
        {"adb", std::move(install_arguments), context.project_root},
        {"adb", std::move(launch_arguments), context.project_root},
    };
  }
};

} // namespace

namespace detail {

const PlatformDriver& AndroidPlatformDriver() noexcept {
  static const AndroidDriver driver;
  return driver;
}

} // namespace detail

std::vector<PlatformDevice> ParseAdbDevices(std::string_view output) {
  std::vector<PlatformDevice> devices;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.starts_with("List of devices attached") || line.starts_with('*')) {
      continue;
    }

    std::istringstream fields(line);
    std::string id;
    std::string raw_state;
    fields >> id >> raw_state;
    if (id.empty() || raw_state.empty()) {
      continue;
    }

    DeviceState state = DeviceState::Unavailable;
    if (raw_state == "device") {
      state = DeviceState::Ready;
    } else if (raw_state == "offline") {
      state = DeviceState::Offline;
    } else if (raw_state == "unauthorized") {
      state = DeviceState::Unauthorized;
    }

    std::string name;
    std::string field;
    while (fields >> field) {
      if (field.starts_with("model:")) {
        name = field.substr(6);
        break;
      }
    }
    devices.push_back({std::move(id), std::move(name), state, DeviceKind::Unspecified, {}});
  }
  return devices;
}

} // namespace huxerui::cli
