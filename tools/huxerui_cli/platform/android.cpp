#include "platform.h"

#include "toolchain.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "mcpp/mcpp.h"
#include "template.h"

namespace huxerui::cli {
namespace {

constexpr std::string_view android_gradle_plugin_version = "8.13.2";
constexpr std::string_view android_compile_sdk = "36";
constexpr std::string_view android_min_compile_sdk = "23";
constexpr std::string_view android_min_sdk = "23";
constexpr std::string_view android_target_sdk = "36";
constexpr std::string_view android_build_tools = "35.0.0";
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

std::string AndroidProperty(const EnvironmentRequest& request, std::string_view file, std::string_view key) {
  if (request.project_root.empty())
    return {};
  std::ifstream properties(request.project_root / "platform/android" / file);
  for (std::string line; std::getline(properties, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.starts_with(std::string(key) + "="))
      continue;
    std::string value;
    for (std::size_t index = key.size() + 1; index < line.size(); ++index) {
      if (line[index] == '\\' && index + 1 < line.size())
        ++index;
      value.push_back(line[index]);
    }
    return value;
  }
  return {};
}

std::filesystem::path AndroidSdkRoot(const EnvironmentRequest& request = {}) {
  if (const auto value = AndroidProperty(request, "local.properties", "sdk.dir"); !value.empty()) {
    return std::filesystem::absolute(request.project_root / "platform/android" / value);
  }
  for (const auto variable : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
    if (const auto value = ReadEnvironmentVariable(variable); value && !value->empty())
      return *value;
  }
  const auto home = ReadEnvironmentVariable("HOME").value_or("");
  const auto local = ReadEnvironmentVariable("LOCALAPPDATA").value_or("");
  std::filesystem::path standard;
  if (CurrentHostId() == "windows" && !local.empty())
    standard = std::filesystem::path(local) / "Android/Sdk";
  if (CurrentHostId() == "macos" && !home.empty())
    standard = std::filesystem::path(home) / "Library/Android/sdk";
  if (CurrentHostId() == "linux" && !home.empty())
    standard = std::filesystem::path(home) / "Android/Sdk";
  if (!standard.empty() && std::filesystem::is_directory(standard))
    return standard;
  const auto managed = detail::ManagedToolDirectory();
  return managed.empty() || CurrentHostId() == "android" ? std::filesystem::path{} : managed / "android-sdk";
}

std::filesystem::path AndroidSdkManager(const std::filesystem::path& sdk_root) {
  if (sdk_root.empty())
    return {};
  const auto filename = CurrentHostId() == "windows" ? "sdkmanager.bat" : "sdkmanager";
  const auto root = sdk_root / "cmdline-tools";
  const auto pinned = root / "19.0/bin" / filename;
  if (std::filesystem::is_regular_file(pinned))
    return pinned;
  const auto latest = root / "latest/bin" / filename;
  if (std::filesystem::is_regular_file(latest))
    return latest;
  std::filesystem::path versioned;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    const auto candidate = entry.path() / "bin" / filename;
    if (std::filesystem::is_regular_file(candidate) &&
        entry.path().filename() > versioned.parent_path().parent_path().filename()) {
      versioned = candidate;
    }
  }
  return versioned;
}

std::filesystem::path AndroidAdb(const EnvironmentRequest& request = {}) {
  const auto root = AndroidSdkRoot(request);
  if (!root.empty()) {
    const auto candidate = root / "platform-tools" / (CurrentHostId() == "windows" ? "adb.exe" : "adb");
    if (std::filesystem::is_regular_file(candidate))
      return candidate;
  }
  const bool explicit_root = !AndroidProperty(request, "local.properties", "sdk.dir").empty() ||
                             !ReadEnvironmentVariable("ANDROID_HOME").value_or("").empty() ||
                             !ReadEnvironmentVariable("ANDROID_SDK_ROOT").value_or("").empty();
  // A standalone adb on PATH is only a fallback when no SDK was selected.
  if (!explicit_root && (root.empty() || !std::filesystem::is_directory(root))) {
    return FindExecutable("adb").value_or(std::filesystem::path{});
  }
  return {};
}

std::filesystem::path ManagedJdk() {
  auto path = detail::ManagedToolDirectory();
  if (path.empty())
    return {};
  path /= std::filesystem::path("jdk") / "17.0.18" / detail::HostArchitecture();
  if (CurrentHostId() == "macos")
    path /= "Contents/Home";
  return path;
}

EnvironmentDiagnostic CheckJava(const std::filesystem::path& home) {
  const auto filename = CurrentHostId() == "windows" ? "javac.exe" : "javac";
  const auto compiler = home / "bin" / filename;
  auto result = detail::CheckTool("java", "JDK 17–23",
                                  std::filesystem::is_regular_file(compiler) ? compiler : std::filesystem::path{},
                                  "17.0", {}, "-version");
  result.path = home;
  if (!home.empty())
    result.detail = home.string();
  if (detail::VersionAtLeast(result.version, "24.0"))
    result.status = EnvironmentDiagnosticStatus::Missing;
  return result;
}

EnvironmentDiagnostic AndroidJava(const EnvironmentRequest& request) {
  if (!request.java_home.empty())
    return CheckJava(request.java_home);
  const auto java_home = ReadEnvironmentVariable("JAVA_HOME");
  if (java_home && !java_home->empty()) {
    auto check = CheckJava(*java_home);
    if (check.status == EnvironmentDiagnosticStatus::Ready)
      return check;
  }

  std::vector<std::filesystem::path> candidates;
  if (const auto compiler = FindExecutable("javac");
      compiler && (CurrentHostId() != "macos" || *compiler != "/usr/bin/javac")) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(*compiler, error);
    if (!error)
      candidates.push_back(resolved.parent_path().parent_path());
  }
  if (CurrentHostId() == "macos") {
    if (std::filesystem::is_regular_file("/usr/libexec/java_home")) {
      const auto jdk = RunProcessCapture({"/usr/libexec/java_home", {"-v", "17", "-F"}, {}});
      if (jdk.exit_code == 0) {
        const auto end = jdk.output.find_last_not_of("\r\n");
        candidates.emplace_back(jdk.output.substr(0, end + 1));
      }
    }
    candidates.emplace_back("/Applications/Android Studio.app/Contents/jbr/Contents/Home");
  } else if (CurrentHostId() == "windows") {
    if (const auto program_files = ReadEnvironmentVariable("ProgramFiles")) {
      candidates.push_back(std::filesystem::path(*program_files) / "Android/Android Studio/jbr");
    }
  } else {
    candidates.emplace_back("/opt/android-studio/jbr");
  }
  candidates.push_back(ManagedJdk());
  for (const auto& candidate : candidates) {
    if (candidate.empty())
      continue;
    auto check = CheckJava(candidate);
    if (check.status == EnvironmentDiagnosticStatus::Ready)
      return check;
  }
  return CheckJava({});
}

SetupAction InstallJdk(const EnvironmentRequest& request) {
  constexpr std::string_view root = "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.18%2B8/";
  const auto host = CurrentHostId();
  const bool arm = detail::HostArchitecture() == "arm64";
  std::string checksum;
  if (host == "macos") {
    checksum = arm ? "d81de06d938384fe76c4aa3c13395933aa11e2d19b0428743f810db06b05e312"
                   : "47c2a8a62d6ba44bf46f2e468f9b3f26ad4b0bf6a51544e29c4520c18e709744";
  } else if (host == "linux") {
    checksum = arm ? "592a6702b3a07a0e0b82cb38aaab149bfce1b0c24d6b57ddb410bd9009333095"
                   : "0c94cbb54325c40dcf026143eb621562017db5525727f2d9131a11250f72c450";
  } else if (host == "windows") {
    checksum = "c24e6215fa75f861d2a900ceec67c620cd2eb6dfb81541087d5981293b16f0e5";
  }
  if (checksum.empty() || ManagedJdk().empty() || !request.java_home.empty()) {
    return {"Select a compatible JDK using JAVA_HOME or --java-home (https://adoptium.net/temurin/)",
            std::nullopt,
            "install_jdk",
            {},
            {"java"}};
  }
  const auto platform = host == "macos" ? "mac" : host;
  const auto architecture = arm && host != "windows" ? "aarch64" : "x64";
  const auto extension = host == "windows" ? ".zip" : ".tar.gz";
  auto destination = ManagedJdk();
  if (host == "macos")
    destination = destination.parent_path().parent_path();
  return detail::InstallTool("install_jdk", "Install Temurin JDK 17",
                             {std::string(root) + "OpenJDK17U-jdk_" + architecture + "_" + std::string(platform) +
                                  "_hotspot_17.0.18_8" + extension,
                              checksum, "jdk-17.0.18+8", destination},
                             {}, {"java"});
}

SetupAction InstallAndroidCommandLineTools(const std::filesystem::path& sdk_root) {
  const auto host = CurrentHostId();
  const auto suffix = host == "windows" ? "win" : host == "macos" ? "mac" : "linux";
  const auto digest = host == "windows" ? "54a582f3bf73e04253602f2d1c80bd5868aac115"
                      : host == "macos" ? "c3e06a1959762e89167d1cbaa988605f6f7c1d24"
                                        : "5fdcc763663eefb86a5b8879697aa6088b041e70";
  return detail::InstallTool(
      "install_command_line_tools", "Install Google Android Command-line Tools 19.0",
      {"https://dl.google.com/android/repository/commandlinetools-" + std::string(suffix) + "-13114758_latest.zip",
       digest, "cmdline-tools", sdk_root / "cmdline-tools/19.0"},
      {"java"}, {"sdkmanager"});
}

std::string AndroidVersion(const EnvironmentRequest& request, std::string_view key, std::string_view fallback) {
  auto value = AndroidProperty(request, "gradle.properties", key);
  return value.empty() ? std::string(fallback) : value;
}

std::filesystem::path AndroidCMake(const EnvironmentRequest& request) {
  const auto cmake_dir = AndroidProperty(request, "local.properties", "cmake.dir");
  if (cmake_dir.empty())
    return {};
  const auto root = std::filesystem::absolute(request.project_root / "platform/android" / cmake_dir);
  return root / "bin" / (CurrentHostId() == "windows" ? "cmake.exe" : "cmake");
}

ProcessEnvironment AndroidEnvironment(const EnvironmentRequest& request) {
  ProcessEnvironment environment;
  const auto sdk = AndroidSdkRoot(request);
  if (!sdk.empty()) {
    environment.emplace_back("ANDROID_HOME", sdk.string());
    environment.emplace_back("ANDROID_SDK_ROOT", sdk.string());
    detail::PrependPath(environment, sdk / "platform-tools");
  }
  if (CurrentHostId() != "android") {
    const auto java = AndroidJava(request);
    if (!request.java_home.empty() && java.status != EnvironmentDiagnosticStatus::Ready) {
      throw std::runtime_error("Java home does not contain a compatible JDK: " + request.java_home.string());
    }
    if (java.status == EnvironmentDiagnosticStatus::Ready) {
      environment.emplace_back("JAVA_HOME", java.path.string());
      detail::PrependPath(environment, java.path / "bin");
    }
  } else if (!request.java_home.empty()) {
    environment.emplace_back("JAVA_HOME", request.java_home.string());
  }
  if (const auto cmake = AndroidCMake(request); !cmake.empty()) {
    environment.emplace_back("HUXERUI_CMAKE_EXECUTABLE", cmake.string());
    detail::PrependPath(environment, cmake.parent_path());
  }
  return environment;
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

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"arm64-v8a"}, std::string_view{"x86_64"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux" ||
           CurrentHostId() == "android";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array android_tools{
        std::string_view{"java"},
        std::string_view{"aapt2"},
        std::string_view{"termux-open"},
    };
    return android_tools;
  }

  ProcessEnvironment ResolveEnvironment(const EnvironmentRequest& request) const override {
    return AndroidEnvironment(request);
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    if (!SupportsCurrentHost())
      return PlatformDriver::DiagnoseEnvironment(request);
    const bool termux = CurrentHostId() == "android";
    const bool devices = request.operation == EnvironmentOperation::Devices;
    if (termux && devices) {
      return {
          {EnvironmentDiagnosticStatus::Unavailable, "host", "Android device discovery is unavailable on Termux", {}}};
    }
    const auto sdk = AndroidSdkRoot(request);
    const bool sdk_ready = !sdk.empty() && std::filesystem::is_directory(sdk);
    const auto api = AndroidVersion(request, "huxeruiCompileSdk", android_compile_sdk);
    const auto ndk = AndroidVersion(request, "huxeruiNdkVersion", android_ndk_version);
    const auto ndk_toolchain = sdk / "ndk" / ndk / "build/cmake/android.toolchain.cmake";
    const auto platform_jar = sdk / "platforms" / ("android-" + api) / "android.jar";
    const auto build_tools = sdk / "build-tools" / android_build_tools / "source.properties";
    const auto adb = termux ? std::filesystem::path{} : AndroidAdb(request);
    const bool needs_adb =
        !termux && (devices || request.operation == EnvironmentOperation::Run || !request.device.empty());
    auto adb_check = detail::CheckTool("platform_tools", "Android SDK platform-tools", adb, "1.0", {}, "version");
    const bool adb_ready = adb_check.status == EnvironmentDiagnosticStatus::Ready;
    const bool packages_missing =
        (needs_adb && !adb_ready) || (!devices && (!sdk_ready || !std::filesystem::is_regular_file(platform_jar) ||
                                                    !std::filesystem::is_regular_file(ndk_toolchain) ||
                                                    (!termux && !std::filesystem::is_regular_file(build_tools))));
    auto checks = termux ? PlatformDriver::DiagnoseEnvironment(request) : std::vector<EnvironmentDiagnostic>{};
    const auto cmake = AndroidCMake(request);
    if (!devices && !cmake.empty()) {
      checks.push_back(detail::CheckTool("cmake", "CMake selected by cmake.dir", cmake, "3.20"));
      checks.back().host_toolchain = true;
    }
    if (!termux && (!devices || packages_missing)) {
      auto java = AndroidJava(request);
      java.required = !devices;
      checks.push_back(std::move(java));
    }
    if (!devices) {
      checks.push_back({sdk_ready ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
                        "android_sdk",
                        "Android SDK",
                        sdk.string(),
                        {},
                        {},
                        sdk});
      const auto check_package = [&](std::string id, std::string label, std::string_view version,
                                     const std::filesystem::path& file) {
        const bool usable = sdk_ready && std::filesystem::is_regular_file(file);
        checks.push_back({usable ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
                          std::move(id),
                          std::move(label),
                          file.string(),
                          {},
                          std::string(version),
                          file});
      };
      check_package("android_platform", "Android platform", api, platform_jar);
      check_package("android_ndk", "Android NDK", ndk, ndk_toolchain);
      // The Gradle shell needs the JDK and the SDK under every build system; the NDK compiles only the CMake core.
      checks.back().host_toolchain = true;
      if (!termux)
        check_package("build_tools", "Android SDK Build Tools", android_build_tools, build_tools);
    }
    if (!termux && (!devices || packages_missing)) {
      const auto path = AndroidSdkManager(sdk);
      auto installer = detail::CheckTool("sdkmanager", "Android SDK command-line tools", path);
      if (packages_missing && !path.empty() && !detail::NeedsSetup(checks, "java")) {
        installer =
            detail::CheckTool("sdkmanager", "Android SDK command-line tools", path, "1.0", AndroidEnvironment(request));
      }
      installer.required = false;
      checks.push_back(std::move(installer));
    }
    if (!termux) {
      adb_check.required = needs_adb;
      checks.push_back(std::move(adb_check));
    }
    if (!termux && (request.operation == EnvironmentOperation::Run || !request.device.empty())) {
      EnvironmentDiagnostic device_check{EnvironmentDiagnosticStatus::Blocked, "device", "Android device",
                                         "Android SDK platform-tools is unavailable"};
      if (adb_ready) {
        try {
          const auto destinations = DiscoverDevices(request);
          const auto matching = std::ranges::count_if(destinations, [&](const auto& device) {
            return device.state == DeviceState::Ready && (request.device.empty() || device.id == request.device);
          });
          device_check.status = matching == 1 ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing;
          device_check.detail = matching == 1 ? "" : "Connect and authorize a device or select one with --device";
        } catch (const std::runtime_error& error) {
          device_check.detail = error.what();
        }
      }
      checks.push_back(std::move(device_check));
    }
    return checks;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics,
                                     const EnvironmentRequest& request) const override {
    std::vector<SetupAction> actions;
    if (CurrentHostId() == "android") {
      for (const auto& [id, package] :
           {std::pair{"java", "openjdk-21"}, {"aapt2", "aapt2"}, {"termux-open", "termux-tools"}}) {
        if (detail::NeedsSetup(diagnostics, id)) {
          actions.push_back({"Install " + std::string(package) + " for Termux",
                             ProcessCommand{"pkg", {"install", package}, {}},
                             "install_" + std::string(id),
                             {},
                             {id},
                             {},
                             true});
        }
      }
      if (detail::NeedsSetup(diagnostics, "android_sdk") || detail::NeedsSetup(diagnostics, "android_platform") ||
          detail::NeedsSetup(diagnostics, "android_ndk")) {
        actions.push_back(
            {"Select a Termux-compatible Android SDK and NDK via ANDROID_HOME", std::nullopt, "select_termux_sdk"});
      }
      return actions;
    }
    const auto sdk = AndroidSdkRoot(request);
    const auto sdkmanager =
        detail::NeedsSetup(diagnostics, "sdkmanager") ? std::filesystem::path{} : AndroidSdkManager(sdk);
    if (detail::NeedsSetup(diagnostics, "java"))
      actions.push_back(InstallJdk(request));
    if (detail::NeedsSetup(diagnostics, "cmake")) {
      actions.push_back({"Correct cmake.dir in Android local.properties", std::nullopt, "select_cmake", {}, {"cmake"}});
    }
    const bool needs_packages = std::ranges::any_of(diagnostics, [](const auto& check) {
      return check.required && check.status == EnvironmentDiagnosticStatus::Missing &&
             (check.id == "platform_tools" || check.id == "android_platform" || check.id == "android_ndk" ||
              check.id == "build_tools");
    });
    if (needs_packages && detail::NeedsSetup(diagnostics, "sdkmanager") && !sdk.empty()) {
      actions.push_back(InstallAndroidCommandLineTools(sdk));
    }
    const std::string api = AndroidVersion(request, "huxeruiCompileSdk", android_compile_sdk);
    const std::string ndk = AndroidVersion(request, "huxeruiNdkVersion", android_ndk_version);
    const std::array packages{std::pair{"platform_tools", std::string("platform-tools")},
                              std::pair{"android_platform", "platforms;android-" + api},
                              std::pair{"build_tools", "build-tools;" + std::string(android_build_tools)},
                              std::pair{"android_ndk", "ndk;" + ndk}};
    for (const auto& [id, package] : packages) {
      const auto check = std::ranges::find(diagnostics, std::string_view{id}, &EnvironmentDiagnostic::id);
      if (check == diagnostics.end() || !check->required || check->status != EnvironmentDiagnosticStatus::Missing ||
          sdk.empty())
        continue;
      const auto executable = sdkmanager.empty() ? sdk / "cmdline-tools/19.0/bin" /
                                                       (CurrentHostId() == "windows" ? "sdkmanager.bat" : "sdkmanager")
                                                 : sdkmanager;
      ProcessCommand command{executable.string(), {"--sdk_root=" + sdk.string(), "--install", package}, sdk};
      if (!sdkmanager.empty() && !detail::NeedsSetup(diagnostics, "java"))
        command.environment = AndroidEnvironment(request);
      actions.push_back(
          {"Install Android SDK package " + package,
           std::move(command),
           "install_" + std::string(id),
           sdkmanager.empty() ? std::vector<std::string>{"java", "sdkmanager"} : std::vector<std::string>{"java"},
           {id},
           {},
           true});
    }
    if (detail::NeedsSetup(diagnostics, "device")) {
      actions.push_back({"Connect and authorize an Android device, then run huxerui devices android",
                         std::nullopt,
                         "select_device",
                         {},
                         {"device"}});
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

  GeneratedTree CreateLibraryPackage(const LibraryTemplateContext& context,
      const std::filesystem::path& library_root) const override {
    std::vector<GeneratedFile> files =
        RenderTemplateTree("platform/android/library", context.project, android_template_replacements);
    AppendFiles(files, AndroidWrapperFiles());
    files.push_back({"consumer-rules.pro", {}});
    return {library_root / "platform" / Id(), std::move(files)};
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
        std::string_view{"app/src/main/res/drawable/ic_launcher_foreground.xml"},
        std::string_view{"app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml"},
        std::string_view{"app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"},
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

  std::vector<PlatformDevice> DiscoverDevices(const EnvironmentRequest& request) const override {
    const auto executable = AndroidAdb(request);
    if (executable.empty())
      throw std::runtime_error("Android SDK platform-tools is unavailable");
    const ProcessCommand command{executable.string(), {"devices", "-l"}, {}};
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

  void ValidateBuildAbis(const PlatformCommandContext& context) const override {
    if (CurrentHostId() == "android" && !context.abis.empty() &&
        (context.abis.size() != 1 || context.abis.front() != "arm64-v8a")) {
      throw std::invalid_argument("HuxerUI Termux builds only support the arm64-v8a ABI");
    }
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

std::string AndroidMcppTriple(std::string_view abi) {
  return mcpp::AbiTriple("android", abi);
}

std::vector<ProcessCommand>
AndroidLibraryGraphCommands(const PlatformCommandContext& context, std::string_view host_id) {
  if (context.build_system == BuildSystem::Mcpp) {
    // The build program writes the library graph and the framework record the shell's settings read, so the first
    // ABI is built before Gradle runs; the shell's own build of that ABI then finds it current.
    const std::string abi = context.abis.empty() ? std::string("arm64-v8a") : context.abis.front();
    std::vector<std::string> arguments{"build", "--target", AndroidMcppTriple(abi)};
    const std::vector<std::string> profile = mcpp::ProfileArguments(context.mcpp_options.value());
    arguments.insert(arguments.end(), profile.begin(), profile.end());
    return {{"mcpp", std::move(arguments), context.project_root}};
  }
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
  } else if (!context.abis.empty()) {
    arguments.push_back("-PhuxeruiAbis=" + JoinTargetAbis(context.abis, ','));
  }
  arguments.push_back(std::string("-PhuxeruiBuildSystem=") +
                      (context.build_system == BuildSystem::Mcpp ? "mcpp" : "cmake"));
  if (context.build_system == BuildSystem::Mcpp) {
    const auto& options = context.mcpp_options.value();
    arguments.push_back("-PhuxeruiMcppProfile=" + options.profile_name);
    std::string features;
    for (const std::string& feature : options.features) {
      features += (features.empty() ? "" : ",") + feature;
    }
    if (!features.empty()) {
      arguments.push_back("-PhuxeruiMcppFeatures=" + features);
    }
  }
  if (const auto version = detail::EnvironmentValue(context.environment, "HUXERUI_CMAKE_VERSION"); !version.empty()) {
    arguments.push_back("-PhuxeruiCmakeVersion=" + version);
  }
  arguments.push_back(":app:assemble" + configuration);
  if (context.optimization) {
    arguments.push_back("-PhuxeruiOptimization=" + *context.optimization);
  }
  if (context.lto) {
    arguments.push_back(std::string("-PhuxeruiLto=") + (*context.lto ? "ON" : "OFF"));
  }
  if (context.profiling) {
    arguments.push_back("-PhuxeruiProfiling=ON");
  }
  if (context.watch && context.profile == "debug" && host_id != "android") {
    arguments.push_back("-PhuxeruiWatch=true");
  }
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
  EnvironmentRequest request;
  request.project_root = context.project_root;
  const auto executable = AndroidAdb(request);
  const std::string adb = executable.empty() ? "adb" : executable.string();
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
