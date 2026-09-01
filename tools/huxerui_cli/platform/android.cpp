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

constexpr std::string_view android_gradle_plugin_version = "8.13.2";
constexpr std::string_view android_compile_sdk = "36";
constexpr std::string_view android_min_compile_sdk = "23";
constexpr std::string_view android_min_sdk = "23";
constexpr std::string_view android_target_sdk = "36";
constexpr std::string_view android_ndk_version = "29.0.14206865";

constexpr std::array android_template_replacements{
    TemplateReplacement{"@ANDROID_AGP_VERSION@", android_gradle_plugin_version},
    TemplateReplacement{"@ANDROID_COMPILE_SDK@", android_compile_sdk},
    TemplateReplacement{"@ANDROID_MIN_COMPILE_SDK@", android_min_compile_sdk},
    TemplateReplacement{"@ANDROID_MIN_SDK@", android_min_sdk},
    TemplateReplacement{"@ANDROID_TARGET_SDK@", android_target_sdk},
    TemplateReplacement{"@ANDROID_NDK_VERSION@", android_ndk_version},
};

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

std::filesystem::path AndroidSdkRoot() {
  for (const std::string_view variable : {std::string_view{"ANDROID_HOME"}, std::string_view{"ANDROID_SDK_ROOT"}}) {
    if (const std::optional<std::string> value = ReadEnvironmentVariable(variable);
        value && std::filesystem::is_directory(*value)) {
      return *value;
    }
  }
  return {};
}

std::optional<std::filesystem::path> AndroidSdkManager(const std::filesystem::path& sdk_root) {
  if (const std::optional<std::filesystem::path> executable = FindExecutable("sdkmanager")) {
    return executable;
  }
  if (sdk_root.empty()) {
    return std::nullopt;
  }
  const std::string filename = CurrentHostId() == "windows" ? "sdkmanager.bat" : "sdkmanager";
  const std::array candidates{
      sdk_root / "cmdline-tools/latest/bin" / filename,
      sdk_root / "tools/bin" / filename,
  };
  const auto candidate = std::find_if(candidates.begin(), candidates.end(), [](const std::filesystem::path& path) {
    return std::filesystem::is_regular_file(path);
  });
  return candidate == candidates.end() ? std::nullopt : std::optional<std::filesystem::path>(*candidate);
}

std::filesystem::path AndroidAdb(const std::filesystem::path& sdk_root = AndroidSdkRoot()) {
  if (const std::optional<std::filesystem::path> executable = FindExecutable("adb")) {
    return *executable;
  }
  if (sdk_root.empty()) {
    return {};
  }
  const std::filesystem::path candidate =
      sdk_root / "platform-tools" / (CurrentHostId() == "windows" ? "adb.exe" : "adb");
  return std::filesystem::is_regular_file(candidate) ? candidate : std::filesystem::path{};
}

std::filesystem::path RequireAndroidAdb() {
  const std::filesystem::path executable = AndroidAdb();
  if (executable.empty()) {
    throw std::runtime_error("Android SDK platform-tools is unavailable; run 'huxerui doctor android'");
  }
  return executable;
}

std::string AndroidAdbCommand() {
  const std::filesystem::path executable = AndroidAdb();
  return executable.empty() ? "adb" : executable.string();
}

std::filesystem::path AndroidApk(const PlatformCommandContext& context) {
  const std::filesystem::path output_directory =
      context.project_root / "platform/android/app/build/outputs/apk" / context.profile;
  const std::string metadata = detail::ReadFile(output_directory / "output-metadata.json");
  const std::filesystem::path output_file = detail::JsonString(metadata, "outputFile");
  if (output_file.empty() || output_file.is_absolute() ||
      std::find(output_file.begin(), output_file.end(), std::filesystem::path{".."}) != output_file.end()) {
    throw std::runtime_error("Android APK metadata contains an invalid outputFile");
  }
  const std::filesystem::path apk = output_directory / output_file;
  if (!std::filesystem::is_regular_file(apk)) {
    throw std::runtime_error("Android build artifact is missing: " + apk.string());
  }
  return apk;
}

class AndroidDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "android";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux" ||
           CurrentHostId() == "android";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array desktop_tools{std::string_view{"java"}};
    static constexpr std::array android_tools{
        std::string_view{"java"},
        std::string_view{"aapt2"},
        std::string_view{"termux-open"},
    };
    return CurrentHostId() == "android" ? std::span<const std::string_view>(android_tools)
                                        : std::span<const std::string_view>(desktop_tools);
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const override {
    std::vector<EnvironmentDiagnostic> diagnostics = PlatformDriver::DiagnoseEnvironment();
    if (!SupportsCurrentHost()) {
      return diagnostics;
    }

    const std::filesystem::path sdk_root = AndroidSdkRoot();
    diagnostics.push_back({
        sdk_root.empty() ? EnvironmentDiagnosticStatus::Missing : EnvironmentDiagnosticStatus::Ready,
        "android_sdk",
        "Android SDK",
        sdk_root.string(),
    });
    if (CurrentHostId() != "android") {
      const std::optional<std::filesystem::path> sdk_manager = AndroidSdkManager(sdk_root);
      diagnostics.push_back({
          sdk_manager ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
          "sdkmanager",
          "Android SDK command-line tools",
          sdk_manager ? sdk_manager->string() : std::string{},
      });

      const std::filesystem::path adb_path = AndroidAdb(sdk_root);
      diagnostics.push_back({
          adb_path.empty() ? EnvironmentDiagnosticStatus::Missing : EnvironmentDiagnosticStatus::Ready,
          "platform_tools",
          "Android SDK platform-tools",
          adb_path.string(),
      });
    }

    const std::array packages{
        std::pair{
            std::string_view{"android_platform"}, sdk_root / ("platforms/android-" + std::string(android_compile_sdk))
        },
        std::pair{std::string_view{"android_ndk"}, sdk_root / "ndk" / android_ndk_version},
    };
    for (const auto& [id, path] : packages) {
      const std::string label = id == "android_platform"
                                    ? "Android platform " + std::string(android_compile_sdk)
                                    : (CurrentHostId() == "android" ? "Termux-compatible Android NDK "
                                                                    : "Android NDK ") +
                                          std::string(android_ndk_version);
      diagnostics.push_back({
          !sdk_root.empty() && std::filesystem::is_directory(path) ? EnvironmentDiagnosticStatus::Ready
                                                                   : EnvironmentDiagnosticStatus::Missing,
          std::string(id),
          label,
          std::filesystem::is_directory(path) ? path.string() : std::string{},
      });
    }
    return diagnostics;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const override {
    std::vector<SetupAction> actions;
    if (CurrentHostId() == "android") {
      bool needs_android_sdk = false;
      for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
        if (diagnostic.status != EnvironmentDiagnosticStatus::Missing) {
          continue;
        }
        if (diagnostic.id == "java") {
          actions.push_back({
              "Install OpenJDK for Termux",
              ProcessCommand{"pkg", {"install", "-y", "openjdk-21"}, {}},
          });
        } else if (diagnostic.id == "aapt2") {
          actions.push_back({
              "Install aapt2 for Termux",
              ProcessCommand{"pkg", {"install", "-y", "aapt2"}, {}},
          });
        } else if (diagnostic.id == "termux-open") {
          actions.push_back({
              "Install Termux system integration tools",
              ProcessCommand{"pkg", {"install", "-y", "termux-tools"}, {}},
          });
        } else if (
            diagnostic.id == "android_sdk" || diagnostic.id == "android_platform" ||
            diagnostic.id == "android_ndk"
        ) {
          needs_android_sdk = true;
        }
      }
      if (needs_android_sdk) {
        actions.push_back({
            "Install a Termux-compatible Android SDK with platform " + std::string(android_compile_sdk) +
                " and NDK " + std::string(android_ndk_version) + ", then set ANDROID_HOME",
            std::nullopt,
        });
      }
      return actions;
    }

    const std::filesystem::path sdk_root = AndroidSdkRoot();
    const std::optional<std::filesystem::path> sdk_manager = AndroidSdkManager(sdk_root);
    for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
      if (diagnostic.status != EnvironmentDiagnosticStatus::Missing) {
        continue;
      }
      if (diagnostic.id == "java") {
        actions.push_back({"Install JDK 17 or later and add java to PATH", std::nullopt});
      } else if (diagnostic.id == "android_sdk" || diagnostic.id == "sdkmanager") {
        actions.push_back({
            "Install the Android SDK command-line tools and set ANDROID_HOME",
            std::nullopt,
        });
      } else if (sdk_manager && diagnostic.id == "platform_tools") {
        actions.push_back({
            "Install Android SDK platform-tools",
            ProcessCommand{sdk_manager->string(), {"platform-tools"}, sdk_root},
        });
      } else if (sdk_manager && diagnostic.id == "android_platform") {
        actions.push_back({
            "Install Android platform " + std::string(android_compile_sdk),
            ProcessCommand{
                sdk_manager->string(), {"platforms;android-" + std::string(android_compile_sdk)}, sdk_root
            },
        });
      } else if (sdk_manager && diagnostic.id == "android_ndk") {
        actions.push_back({
            "Install Android NDK " + std::string(android_ndk_version),
            ProcessCommand{sdk_manager->string(), {"ndk;" + std::string(android_ndk_version)}, sdk_root},
        });
      }
    }
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files =
        RenderTemplateTree("platform/android/app", context, android_template_replacements);
    AppendFiles(files, AndroidWrapperFiles());
    files.push_back({"app/proguard-rules.pro", {}});
    return files;
  }

  std::vector<GeneratedFile> CreateLibraryPackage(const LibraryTemplateContext& context) const override {
    std::vector<GeneratedFile> files =
        RenderTemplateTree("platform/android/library", context.project, android_template_replacements);
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
    return CurrentHostId() != "android";
  }

  std::vector<PlatformDevice> DiscoverDevices() const override {
    const ProcessCommand command{RequireAndroidAdb().string(), {"devices", "-l"}, {}};
    const ProcessResult result = RunProcessCapture(command);
    if (result.exit_code != 0) {
      throw std::runtime_error(
          "command failed with exit code " + std::to_string(result.exit_code) + ": " + DescribeProcess(command)
      );
    }
    return ParseAdbDevices(result.output);
  }

  std::vector<ProcessCommand> LibraryGraphCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("Android native builds do not use a CMake generator option");
    }
    return detail::AndroidLibraryGraphCommands(context, CurrentHostId());
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::optional<std::filesystem::path> aapt2 =
        CurrentHostId() == "android" ? FindExecutable("aapt2") : std::optional<std::filesystem::path>{};
    return detail::AndroidBuildCommands(context, CurrentHostId(), aapt2.value_or(std::filesystem::path{}));
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    return detail::AndroidRunCommands(context, CurrentHostId());
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const std::filesystem::path apk = AndroidApk(context);
    return {{apk, apk.filename()}};
  }
};

} // namespace

namespace detail {

std::vector<ProcessCommand>
AndroidLibraryGraphCommands(const PlatformCommandContext& context, std::string_view host_id) {
  std::vector<ProcessCommand> commands = LibraryGraphConfigureCommands(context);
  if (host_id == "android") {
    commands.front().arguments.push_back("-DANDROID_ABI=arm64-v8a");
  }
  return commands;
}

std::vector<ProcessCommand>
AndroidBuildCommands(const PlatformCommandContext& context, std::string_view host_id,
                     const std::filesystem::path& aapt2) {
  const std::filesystem::path shell = context.project_root / "platform/android";
  const std::string configuration = ProfileConfiguration(context.profile);
  const std::filesystem::path wrapper = shell / (host_id == "windows" ? "gradlew.bat" : "gradlew");
  std::vector<std::string> arguments;
  if (host_id == "android") {
    if (aapt2.empty()) {
      throw std::runtime_error("HuxerUI Termux aapt2 is unavailable; run 'huxerui doctor android'");
    }
    arguments = {
        "-PhuxeruiAbis=arm64-v8a",
        "-Pandroid.aapt2FromMavenOverride=" + aapt2.string(),
    };
  }
  arguments.push_back(":app:assemble" + configuration);
  return {{wrapper.string(), std::move(arguments), shell}};
}

std::vector<ProcessCommand> AndroidRunCommands(const PlatformCommandContext& context, std::string_view host_id) {
  if (host_id == "android") {
    if (context.device) {
      throw std::invalid_argument("HuxerUI Termux Android run does not accept a device");
    }
    const std::filesystem::path apk = AndroidApk(context);
    return {{
        "termux-open",
        {"--view", "--content-type", "application/vnd.android.package-archive", apk.string()},
        context.project_root,
    }};
  }

  const std::filesystem::path output_directory =
      context.project_root / "platform/android/app/build/outputs/apk" / context.profile;
  const std::string metadata = ReadFile(output_directory / "output-metadata.json");
  const std::string application_id = JsonString(metadata, "applicationId");
  if (application_id.empty()) {
    throw std::runtime_error("Android APK metadata contains an empty applicationId");
  }
  const std::filesystem::path apk = AndroidApk(context);

  std::vector<std::string> install_arguments = DeviceArguments(context.device);
  install_arguments.insert(install_arguments.end(), {"install", "-r", apk.string()});
  std::vector<std::string> launch_arguments = DeviceArguments(context.device);
  launch_arguments.insert(launch_arguments.end(), {"shell", "am", "start", "-n", application_id + "/.MainActivity"});
  const std::string adb = AndroidAdbCommand();
  return {
      {adb, std::move(install_arguments), context.project_root},
      {adb, std::move(launch_arguments), context.project_root},
  };
}

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
