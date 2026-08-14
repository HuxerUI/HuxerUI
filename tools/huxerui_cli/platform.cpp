#include "platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "ios_project.h"
#include "sdk.h"

namespace huxerui::cli {
namespace {

void ReplaceAll(std::string& value, std::string_view token, std::string_view replacement) {
  std::size_t position = 0;
  while ((position = value.find(token, position)) != std::string::npos) {
    value.replace(position, token.size(), replacement);
    position += replacement.size();
  }
}

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

std::string_view Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

bool IsUuidCharacter(char character) {
  return std::isxdigit(static_cast<unsigned char>(character)) || character == '-';
}

std::optional<std::size_t> FindUuid(std::string_view value) {
  constexpr std::size_t uuid_length = 36;
  for (std::size_t position = 0; position + uuid_length <= value.size(); ++position) {
    const std::string_view candidate = value.substr(position, uuid_length);
    if (candidate[8] != '-' || candidate[13] != '-' || candidate[18] != '-' || candidate[23] != '-') {
      continue;
    }
    if (std::all_of(candidate.begin(), candidate.end(), IsUuidCharacter)) {
      return position;
    }
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> IosProjects(const std::filesystem::path& shell_root) {
  std::vector<std::filesystem::path> projects;
  if (!std::filesystem::is_directory(shell_root)) {
    return projects;
  }
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(shell_root)) {
    if (entry.is_directory() && entry.path().extension() == ".xcodeproj") {
      projects.push_back(entry.path());
    }
  }
  return projects;
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

bool IsIosPhysicalDevice(const PlatformCommandContext& context) {
  if (!context.device) {
    return false;
  }
  if (context.device->kind == DeviceKind::Physical) {
    return true;
  }
  if (context.device->kind == DeviceKind::Simulator) {
    return false;
  }
  throw std::invalid_argument("iOS device kind is unspecified");
}

std::filesystem::path IosProjectPath(const PlatformCommandContext& context) {
  std::vector<std::filesystem::path> projects = IosProjects(context.project_root / "platform" / "ios");
  if (projects.size() != 1) {
    throw std::runtime_error(projects.empty() ? "iOS Xcode project is missing" : "multiple iOS Xcode projects found");
  }
  return std::move(projects.front());
}

std::string IosDestination(const PlatformCommandContext& context) {
  if (!context.device) {
    return "generic/platform=iOS Simulator";
  }
  return "id=" + context.device->destination_id;
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
       },
       context.project_root},
  };
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
        std::string_view{"gradle"},
        std::string_view{"adb"},
    };
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::string java_path = "app/src/main/java/";
    std::string package_path = context.project_id;
    std::replace(package_path.begin(), package_path.end(), '.', '/');
    java_path += package_path + "/MainActivity.java";

    return {
        {".gitignore",
         R"TEMPLATE(.gradle/
local.properties
.cxx/
.externalNativeBuild/
**/build/
)TEMPLATE"},
        {"settings.gradle", context.Render(R"TEMPLATE(import groovy.json.JsonSlurper

pluginManagement {
    repositories {
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

def huxeruiHomeValue = providers.environmentVariable("HUXERUI_HOME").orNull
if (huxeruiHomeValue == null || huxeruiHomeValue.isBlank()) {
    throw new GradleException("HUXERUI_HOME is required")
}
def huxeruiHome = file(huxeruiHomeValue)
def huxeruiLibrary = new File(huxeruiHome, "platform/android/huxerui")
if (!huxeruiLibrary.isDirectory()) {
    throw new GradleException("HuxerUI Android library does not exist: ${huxeruiLibrary}")
}

def huxeruiModuleGraphFile = file("../../.huxerui/generated/modules.json")
if (!huxeruiModuleGraphFile.isFile()) {
    throw new GradleException("Run 'huxerui build android' to generate the module graph")
}
def huxeruiModuleGraph = new JsonSlurper().parse(huxeruiModuleGraphFile)
def huxeruiModules = []

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

gradle.ext.huxeruiHome = huxeruiHome
rootProject.name = "@PROJECT_NAME@"
include(":HuxerUI")
project(":HuxerUI").projectDir = huxeruiLibrary
huxeruiModuleGraph.modules.eachWithIndex { module, index ->
    def moduleDirectory = file("${module.sourceRoot}/platform/android")
    if (moduleDirectory.isDirectory()) {
        if (!new File(moduleDirectory, "build.gradle").isFile()) {
            throw new GradleException("Android module package is missing build.gradle: ${moduleDirectory}")
        }
        def projectPath = ":huxeruiModule${index}"
        include(projectPath)
        project(projectPath).projectDir = moduleDirectory
        huxeruiModules.add([target: module.target, projectPath: projectPath])
    }
}
gradle.ext.huxeruiModules = huxeruiModules
include(":app")
)TEMPLATE")},
        {"build.gradle",
         R"TEMPLATE(plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
}
)TEMPLATE"},
        {"gradle.properties",
         R"TEMPLATE(org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8
org.gradle.parallel=true
android.useAndroidX=true
huxeruiCompileSdk=36
huxeruiMinCompileSdk=23
huxeruiMinSdk=23
huxeruiTargetSdk=36
huxeruiNdkVersion=29.0.14206865
huxeruiAbis=arm64-v8a,x86_64
huxeruiStl=c++_shared
huxeruiBuildNative=false
)TEMPLATE"},
        {"gradle/libs.versions.toml",
         R"TEMPLATE([versions]
agp = "8.13.2"

[plugins]
android-application = { id = "com.android.application", version.ref = "agp" }
android-library = { id = "com.android.library", version.ref = "agp" }
)TEMPLATE"},
        {"app/build.gradle", context.Render(R"TEMPLATE(plugins {
    alias(libs.plugins.android.application)
}

def huxeruiProperty = { String name -> providers.gradleProperty(name).get() }
def huxeruiHome = rootProject.gradle.ext.huxeruiHome
def huxeruiModules = rootProject.gradle.ext.huxeruiModules
def huxeruiCompileSdk = huxeruiProperty("huxeruiCompileSdk").toInteger()
def huxeruiMinSdk = huxeruiProperty("huxeruiMinSdk").toInteger()
def huxeruiTargetSdk = huxeruiProperty("huxeruiTargetSdk").toInteger()
def huxeruiNdkVersion = huxeruiProperty("huxeruiNdkVersion")
def huxeruiAbis = huxeruiProperty("huxeruiAbis").split(",")
def huxeruiStl = huxeruiProperty("huxeruiStl")
def huxeruiDebugAssets = layout.buildDirectory.dir("generated/huxerui/assets/debug").get().asFile
def huxeruiReleaseAssets = layout.buildDirectory.dir("generated/huxerui/assets/release").get().asFile

android {
    namespace = "@PROJECT_ID@"
    compileSdk = huxeruiCompileSdk
    ndkVersion = huxeruiNdkVersion

    defaultConfig {
        applicationId = "@PROJECT_ID@"
        minSdk = huxeruiMinSdk
        targetSdk = huxeruiTargetSdk
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                arguments "-DANDROID_STL=${huxeruiStl}",
                        "-DHUXERUI_HOME=${huxeruiHome.absolutePath}"
            }
        }

        ndk {
            abiFilters(*huxeruiAbis)
        }
    }

    buildTypes {
        release {
            minifyEnabled = false
            proguardFiles getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro"
        }
    }

    sourceSets {
        debug {
            assets.srcDir huxeruiDebugAssets
        }
        release {
            assets.srcDir huxeruiReleaseAssets
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
        }
    }
}

dependencies {
    implementation project(":HuxerUI")
    huxeruiModules.each { module ->
        implementation project(module.projectPath)
    }
}

def registerHuxerUIResourceStaging = { String buildType, File assetsDirectory ->
    def capitalizedBuildType = buildType.capitalize()
    def nativeTaskName = "externalNativeBuild${capitalizedBuildType}"
    def stagingTask = tasks.register("stageHuxerUI${capitalizedBuildType}Resources", Sync) {
        dependsOn nativeTaskName
        from {
            def indexFiles = fileTree(file(".cxx/${capitalizedBuildType}")) {
                include "**/huxerui-resources/@TARGET_NAME@/package/huxerui/resources.bin"
            }.files
            def preferredIndexFiles = indexFiles.findAll {
                it.absolutePath.contains("${File.separator}arm64-v8a${File.separator}")
            }
            def candidates = preferredIndexFiles.empty ? indexFiles : preferredIndexFiles
            def selected = candidates.max { left, right ->
                def modified = left.lastModified() <=> right.lastModified()
                modified != 0 ? modified : left.absolutePath <=> right.absolutePath
            }
            selected == null ? [] : selected.parentFile.parentFile
        }
        into assetsDirectory
    }
    tasks.matching { it.name == "merge${capitalizedBuildType}Assets" }.configureEach {
        dependsOn stagingTask
    }
}

registerHuxerUIResourceStaging("debug", huxeruiDebugAssets)
registerHuxerUIResourceStaging("release", huxeruiReleaseAssets)
)TEMPLATE")},
        {"app/proguard-rules.pro", ""},
        {"app/src/main/AndroidManifest.xml", context.Render(R"TEMPLATE(<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <application
        android:allowBackup="true"
        android:label="@PROJECT_NAME@"
        android:supportsRtl="true"
        android:theme="@android:style/Theme.Material.Light.NoActionBar">
        <activity
            android:name=".MainActivity"
            android:exported="true"
            android:windowSoftInputMode="adjustResize">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
)TEMPLATE")},
        {java_path, context.Render(R"TEMPLATE(package @PROJECT_ID@;

import org.huxerui.HuxerUIActivity;

public final class MainActivity extends HuxerUIActivity {}
)TEMPLATE")},
    };
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext& context) const override {
    return {
        {".gitignore", ".gradle/\nlocal.properties\nbuild/\n"},
        {"settings.gradle", context.Render(R"TEMPLATE(pluginManagement {
    repositories {
        gradlePluginPortal()
        google()
        mavenCentral()
    }
    plugins {
        id "com.android.library" version "8.13.2"
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "@PROJECT_NAME@"
)TEMPLATE")},
        {"build.gradle", context.Render(R"TEMPLATE(plugins {
    id "com.android.library"
}

android {
    namespace = "@PROJECT_ID@"
    compileSdk = 36

    defaultConfig {
        minSdk = 23
        consumerProguardFiles "consumer-rules.pro"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
}
)TEMPLATE")},
        {"gradle.properties",
         "org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8\norg.gradle.parallel=true\nandroid.useAndroidX=true\n"},
        {"consumer-rules.pro", ""},
        {"src/main/AndroidManifest.xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<manifest />\n"},
    };
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"settings.gradle"},
        std::string_view{"build.gradle"},
        std::string_view{"gradle.properties"},
        std::string_view{"gradle/libs.versions.toml"},
        std::string_view{"app/build.gradle"},
        std::string_view{"app/src/main/AndroidManifest.xml"},
    };
    std::vector<Diagnostic> diagnostics = ValidateRequiredFiles(shell_root, required);
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
    return ModuleGraphConfigureCommands(context);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::filesystem::path shell = context.project_root / "platform/android";
    const std::string configuration = ProfileConfiguration(context.profile);
    const std::filesystem::path wrapper = shell / (CurrentHostId() == "windows" ? "gradlew.bat" : "gradlew");
    const std::string gradle = std::filesystem::is_regular_file(wrapper) ? wrapper.string() : "gradle";
    return {{gradle, {":app:assemble" + configuration}, shell}};
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::filesystem::path output_directory =
        context.project_root / "platform/android/app/build/outputs/apk" / context.profile;
    const std::string metadata = ReadFile(output_directory / "output-metadata.json");
    const std::string application_id = JsonString(metadata, "applicationId");
    const std::filesystem::path output_file = JsonString(metadata, "outputFile");
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

    std::vector<std::string> install_arguments = DeviceArguments(context.device);
    install_arguments.insert(install_arguments.end(), {"install", "-r", apk.string()});
    std::vector<std::string> launch_arguments = DeviceArguments(context.device);
    launch_arguments.insert(launch_arguments.end(), {"shell", "am", "start", "-n", application_id + "/.MainActivity"});
    return {
        {"adb", std::move(install_arguments), context.project_root},
        {"adb", std::move(launch_arguments), context.project_root},
    };
  }
};

class WindowsDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "windows";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"cmake"}};
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return {
        {".gitignore", "build/\nout/\npackages/\n"},
        {"huxerui.cmake",
         R"TEMPLATE(set(HUXERUI_WINDOWS_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/app.manifest")
)TEMPLATE"},
        {"app.manifest", context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity name="@PROJECT_ID@" version="1.0.0.0" type="win32" />
  <description>@PROJECT_NAME@</description>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
    </windowsSettings>
  </application>
</assembly>
)TEMPLATE")},
    };
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"huxerui.cmake"},
        std::string_view{"app.manifest"},
    };
    return ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = JsonString(ReadFile(AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("Windows build artifact is missing: " + artifact);
    }
    return {{artifact, {}, std::filesystem::path(artifact).parent_path()}};
  }
};

class LinuxDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "linux";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"cmake"}};
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext&) const override {
    return {{".gitkeep", {}}};
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext&) const override {
    return {{"src/.gitkeep", {}}};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path&) const override {
    return {};
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = JsonString(ReadFile(AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("HuxerUI Linux build artifact is missing: " + artifact);
    }
    return {{artifact, {}, std::filesystem::path(artifact).parent_path()}};
  }
};

class MacOSDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "macos";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "macos";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"cmake"}, std::string_view{"xcodebuild"}};
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return {
        {".gitignore", "DerivedData/\nxcuserdata/\n*.xcuserstate\narchives/\n"},
        {"huxerui.cmake", context.Render(R"TEMPLATE(set(HUXERUI_MACOS_BUNDLE_NAME "@PROJECT_NAME@")
set(HUXERUI_MACOS_BUNDLE_IDENTIFIER "@PROJECT_ID@")
set(HUXERUI_MACOS_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in")
)TEMPLATE")},
        {"Info.plist.in", context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDisplayName</key>
  <string>@PROJECT_NAME@</string>
  <key>CFBundleIdentifier</key>
  <string>@PROJECT_ID@</string>
  <key>CFBundleName</key>
  <string>@PROJECT_NAME@</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
</dict>
</plist>
)TEMPLATE")},
    };
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"huxerui.cmake"},
        std::string_view{"Info.plist.in"},
    };
    return ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = ReadFile(AppIntegrationPlan(context));
    const std::string bundle = JsonString(plan, "bundle");
    if (!std::filesystem::is_directory(bundle)) {
      throw std::runtime_error("macOS application bundle is missing: " + bundle);
    }
    return {{"open", {bundle}, context.project_root}};
  }
};

class IosDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "ios";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "macos";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"cmake"},
        std::string_view{"xcodebuild"},
        std::string_view{"xcrun"},
    };
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return CreateIosProject(context);
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext& context) const override {
    return CreateIosModulePackage(context);
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"App/main.mm"},
        std::string_view{"App/Info.plist"},
        std::string_view{"App/LaunchScreen.storyboard"},
        std::string_view{"App/Assets.xcassets/Contents.json"},
        std::string_view{"Config/Base.xcconfig"},
        std::string_view{"Config/Debug.xcconfig"},
        std::string_view{"Config/Release.xcconfig"},
    };
    std::vector<Diagnostic> diagnostics = ValidateRequiredFiles(shell_root, required);
    const std::vector<std::filesystem::path> projects = IosProjects(shell_root);
    if (projects.size() != 1) {
      diagnostics.push_back({true, projects.empty() ? "missing Xcode project" : "multiple Xcode projects"});
      return diagnostics;
    }
    if (!std::filesystem::is_regular_file(projects.front() / "project.pbxproj")) {
      diagnostics.push_back({true, "missing Xcode project.pbxproj"});
    }
    const std::filesystem::path scheme =
        projects.front() / "xcshareddata/xcschemes" / (projects.front().stem().string() + ".xcscheme");
    if (!std::filesystem::is_regular_file(scheme)) {
      diagnostics.push_back({true, "missing shared Xcode scheme"});
    }
    return diagnostics;
  }

  bool SupportsDeviceDiscovery() const noexcept override {
    return true;
  }

  std::vector<PlatformDevice> DiscoverDevices() const override {
    const ProcessCommand physical_command{
        "xcrun",
        {
            "devicectl",
            "list",
            "devices",
            "--hide-default-columns",
            "--columns",
            "name",
            "identifier",
            "state",
            "udid",
            "--hide-headers",
        },
        {},
    };
    const ProcessCommand simulator_command{"xcrun", {"simctl", "list", "devices", "booted"}, {}};
    const ProcessResult physical_result = RunProcessCapture(physical_command);
    const ProcessResult simulator_result = RunProcessCapture(simulator_command);
    if (physical_result.exit_code != 0 && simulator_result.exit_code != 0) {
      throw std::runtime_error("cannot discover iOS devices through devicectl or simctl");
    }

    std::vector<PlatformDevice> devices;
    if (physical_result.exit_code == 0) {
      devices = ParseIosPhysicalDevices(physical_result.output);
    }
    if (simulator_result.exit_code == 0) {
      std::vector<PlatformDevice> simulators = ParseIosSimulatorDevices(simulator_result.output);
      devices.insert(
          devices.end(),
          std::make_move_iterator(simulators.begin()),
          std::make_move_iterator(simulators.end())
      );
    }
    return devices;
  }

  std::vector<ProcessCommand> ModuleGraphCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("iOS native builds do not use a CMake generator option");
    }
    return ModuleGraphConfigureCommands(context);
  }

  void UpdateModuleIntegration(const PlatformCommandContext& context) const override {
    UpdateIosModuleIntegration(context.project_root);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    if (!context.cmake_generator.empty()) {
      throw std::invalid_argument("iOS native builds do not use a CMake generator option");
    }
    const std::string configuration = ProfileConfiguration(context.profile);
    const std::filesystem::path project = IosProjectPath(context);
    std::vector<std::string> build_arguments{
        "-project",
        project.string(),
        "-scheme",
        project.stem().string(),
        "-configuration",
        configuration,
        "-derivedDataPath",
        (context.build_directory / "DerivedData").string(),
        "-destination",
        IosDestination(context),
        "HUXERUI_HOME=" + context.huxerui_home.string(),
        "HUXERUI_INTEGRATION_PLAN=" + (context.build_directory / "huxerui-integration/app.json").string(),
    };
    if (IsIosPhysicalDevice(context)) {
      build_arguments.push_back("-allowProvisioningUpdates");
    }
    build_arguments.push_back("build");
    return {{"xcodebuild", std::move(build_arguments), context.project_root}};
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = ReadFile(AppIntegrationPlan(context));
    const std::string bundle = JsonString(plan, "bundle");
    const std::string bundle_identifier = JsonString(plan, "bundleIdentifier");
    if (!std::filesystem::is_directory(bundle)) {
      throw std::runtime_error("iOS application bundle is missing: " + bundle);
    }
    if (bundle_identifier.empty()) {
      throw std::runtime_error("iOS application bundle identifier is missing");
    }
    if (IsIosPhysicalDevice(context)) {
      return {
          {"xcrun",
           {"devicectl", "device", "install", "app", "--device", context.device->id, bundle},
           context.project_root},
          {"xcrun",
           {"devicectl",
            "device",
            "process",
            "launch",
            "--device",
            context.device->id,
            "--terminate-existing",
            bundle_identifier},
           context.project_root},
      };
    }
    const std::string simulator = context.device ? context.device->id : "booted";
    return {
        {"xcrun", {"simctl", "install", simulator, bundle}, context.project_root},
        {"xcrun", {"simctl", "launch", simulator, bundle_identifier}, context.project_root},
    };
  }

  std::vector<ProcessCommand> OpenCommands(const PlatformCommandContext& context) const override {
    return {{"open", {"-a", "Xcode", IosProjectPath(context).string()}, context.project_root}};
  }
};

class WebDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "web";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"cmake"},
        std::string_view{"emcmake"},
        std::string_view{"emrun"},
    };
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return {
        {".gitignore", "dist/\n"},
        {"huxerui.cmake", R"TEMPLATE(function(huxerui_configure_web_app target_name)
    if (NOT TARGET ${target_name})
        message(FATAL_ERROR "huxerui_configure_web_app() target does not exist: ${target_name}")
    endif ()

    set_target_properties(${target_name} PROPERTIES SUFFIX ".mjs")
    set(HUXERUI_WEB_MODULE_FILE "${target_name}.mjs")
    set(HUXERUI_WEB_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/huxerui-web")
    file(MAKE_DIRECTORY "${HUXERUI_WEB_GENERATED_DIRECTORY}")
    configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/index.html.in"
            "${HUXERUI_WEB_GENERATED_DIRECTORY}/${target_name}.html"
            @ONLY
    )
    add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HUXERUI_WEB_GENERATED_DIRECTORY}/${target_name}.html"
                    "$<TARGET_FILE_DIR:${target_name}>/${target_name}.html"
            VERBATIM
    )
endfunction()
)TEMPLATE"},
        {"index.html.in", context.Render(R"TEMPLATE(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>@PROJECT_NAME@</title>
  <style>
    html, body {
      width: 100%;
      height: 100%;
      margin: 0;
      overflow: hidden;
    }

    #huxerui-canvas {
      display: block;
      width: 100%;
      height: 100%;
      outline: none;
      touch-action: none;
    }
  </style>
</head>
<body>
  <canvas id="huxerui-canvas" tabindex="0"></canvas>
  <script type="module">
    import createHuxerUIApp from "./@HUXERUI_WEB_MODULE_FILE@";

    const module = await createHuxerUIApp();
    await document.fonts.ready;
    const session = module.mountHuxerUI("#huxerui-canvas");
    if (!session) {
      throw new Error("HuxerUI Web session could not be mounted");
    }
  </script>
</body>
</html>
)TEMPLATE")},
    };
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"huxerui.cmake"},
        std::string_view{"index.html.in"},
    };
    return ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::string configuration = ProfileConfiguration(context.profile);
    std::vector<std::string> configure_arguments{
        "cmake",
        "-S",
        context.project_root.string(),
        "-B",
        context.build_directory.string(),
        "-DCMAKE_BUILD_TYPE=" + configuration,
    };
    if (!context.cmake_generator.empty()) {
      configure_arguments.insert(configure_arguments.begin() + 1, {"-G", context.cmake_generator});
    }
    return {
        {"emcmake", std::move(configure_arguments), context.project_root},
        {"cmake",
         {"--build", context.build_directory.string(), "--config", configuration, "--parallel"},
         context.project_root},
    };
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = ReadFile(AppIntegrationPlan(context));
    const std::string target = JsonString(plan, "target");
    const std::filesystem::path artifact = JsonString(plan, "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("Web build artifact is missing: " + artifact.string());
    }
    const std::filesystem::path entry = artifact.parent_path() / (target + ".html");
    if (!std::filesystem::is_regular_file(entry)) {
      throw std::runtime_error("Web entry file is missing: " + entry.string());
    }
    return {{"emrun", {entry.string()}, artifact.parent_path()}};
  }
};

const AndroidDriver android_driver;
const WindowsDriver windows_driver;
const LinuxDriver linux_driver;
const MacOSDriver macos_driver;
const IosDriver ios_driver;
const WebDriver web_driver;
constexpr std::array<const PlatformDriver*, 6> platform_drivers{
    &android_driver,
    &windows_driver,
    &linux_driver,
    &macos_driver,
    &ios_driver,
    &web_driver,
};

} // namespace

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

void PlatformDriver::UpdateModuleIntegration(const PlatformCommandContext&) const {}

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

std::vector<PlatformDevice> ParseIosPhysicalDevices(std::string_view output) {
  std::vector<PlatformDevice> devices;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::optional<std::size_t> identifier = FindUuid(line);
    if (!identifier) {
      continue;
    }
    const std::string_view value(line);
    const std::string_view name = Trim(value.substr(0, *identifier));
    const std::string_view details = Trim(value.substr(*identifier + 36));
    const std::size_t destination_separator = details.find_last_of(" \t");
    if (destination_separator == std::string_view::npos) {
      continue;
    }
    const std::string_view state = Trim(details.substr(0, destination_separator));
    const std::string_view destination = Trim(details.substr(destination_separator + 1));
    if (destination.empty()) {
      continue;
    }
    devices.push_back({
        std::string(value.substr(*identifier, 36)),
        std::string(name),
        state.starts_with("available") ? DeviceState::Ready : DeviceState::Unavailable,
        DeviceKind::Physical,
        std::string(destination),
    });
  }
  return devices;
}

std::vector<PlatformDevice> ParseIosSimulatorDevices(std::string_view output) {
  std::vector<PlatformDevice> devices;
  std::istringstream lines{std::string(output)};
  std::string runtime;
  std::string line;
  while (std::getline(lines, line)) {
    const std::string_view trimmed = Trim(line);
    if (trimmed.starts_with("-- ") && trimmed.ends_with(" --")) {
      runtime = std::string(trimmed.substr(3, trimmed.size() - 6));
      continue;
    }
    const std::optional<std::size_t> identifier = FindUuid(line);
    if (!identifier || runtime.starts_with("Unavailable")) {
      continue;
    }
    const std::string_view value(line);
    std::string_view name_value = Trim(value.substr(0, *identifier));
    if (name_value.ends_with('(')) {
      name_value = Trim(name_value.substr(0, name_value.size() - 1));
    }
    std::string name(name_value);
    if (!runtime.empty()) {
      name += " — " + runtime;
    }
    const std::string_view state = Trim(value.substr(*identifier + 36));
    devices.push_back({
        std::string(value.substr(*identifier, 36)),
        std::move(name),
        state.find("(Booted)") != std::string_view::npos ? DeviceState::Ready : DeviceState::Offline,
        DeviceKind::Simulator,
        std::string(value.substr(*identifier, 36)),
    });
  }
  return devices;
}

std::string ProjectTemplateContext::Render(std::string_view value) const {
  std::string rendered(value);
  ReplaceAll(rendered, "@PROJECT_NAME@", project_name);
  ReplaceAll(rendered, "@TARGET_NAME@", target_name);
  ReplaceAll(rendered, "@PROJECT_ID@", project_id);
  return rendered;
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
  const auto iterator =
      std::find_if(platform_drivers.begin(), platform_drivers.end(), [id](const PlatformDriver* driver) {
        return driver->Id() == id;
      });
  return iterator == platform_drivers.end() ? nullptr : *iterator;
}

std::vector<std::string_view> PlatformIds() {
  std::vector<std::string_view> ids;
  ids.reserve(platform_drivers.size());
  for (const PlatformDriver* driver : platform_drivers) {
    ids.push_back(driver->Id());
  }
  return ids;
}

} // namespace huxerui::cli
