#include "platform.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

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

std::vector<std::string> DeviceArguments(std::string_view device) {
  if (device.empty()) {
    return {};
  }
  return {"-s", std::string(device)};
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
      "-DHUXERUI_SDK_ROOT=" + context.sdk_root.string(),
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
    std::string package_path = context.package_name;
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

def huxeruiPlanFile = file("../../.huxerui/generated/android/app.json")
if (!huxeruiPlanFile.isFile()) {
    throw new GradleException("Run 'huxerui build android' to generate the platform integration plan")
}
def huxeruiPlan = new JsonSlurper().parse(huxeruiPlanFile)

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

gradle.ext.huxeruiPlan = huxeruiPlan
rootProject.name = "@PROJECT_NAME@"
include(":HuxerUI")
project(":HuxerUI").projectDir = file(huxeruiPlan.huxeruiSourceDirectory)
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

def huxeruiPlan = rootProject.gradle.ext.huxeruiPlan
def huxeruiDebugAssets = layout.buildDirectory.dir("generated/huxerui/assets/debug").get().asFile
def huxeruiReleaseAssets = layout.buildDirectory.dir("generated/huxerui/assets/release").get().asFile

android {
    namespace = huxeruiPlan.applicationId
    compileSdk = huxeruiPlan.compileSdk
    ndkVersion = huxeruiPlan.ndkVersion

    defaultConfig {
        applicationId = huxeruiPlan.applicationId
        minSdk = huxeruiPlan.minSdk
        targetSdk = huxeruiPlan.targetSdk
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                arguments "-DANDROID_STL=${huxeruiPlan.stl}",
                        "-DHUXERUI_CMAKE_PACKAGE_DIR=${huxeruiPlan.cmakePackageDirectory}",
                        "-DHUXERUI_HOST_TOOL_ROOT=${huxeruiPlan.hostToolDirectory}"
            }
        }

        ndk {
            abiFilters(*huxeruiPlan.abis)
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
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    buildFeatures {
        prefab = true
    }
}

dependencies {
    implementation project(":HuxerUI")
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
        {java_path, context.Render(R"TEMPLATE(package @PACKAGE_NAME@;

import org.huxerui.HuxerUIActivity;

public final class MainActivity extends HuxerUIActivity {}
)TEMPLATE")},
        {"app/src/main/cpp/CMakeLists.txt", context.Render(R"TEMPLATE(cmake_minimum_required(VERSION 3.20)
project(@TARGET_NAME@_android LANGUAGES CXX)

if (NOT HUXERUI_CMAKE_PACKAGE_DIR OR NOT HUXERUI_HOST_TOOL_ROOT)
    message(FATAL_ERROR
            "HUXERUI_CMAKE_PACKAGE_DIR and HUXERUI_HOST_TOOL_ROOT are required"
    )
endif ()

find_package(huxerui REQUIRED CONFIG)
include("${HUXERUI_CMAKE_PACKAGE_DIR}/HuxerUITargets.cmake")
include("${HUXERUI_CMAKE_PACKAGE_DIR}/HuxerUIApp.cmake")

get_filename_component(HUXERUI_APP_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../../../.."
        ABSOLUTE
)
file(GLOB_RECURSE HUXERUI_APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${HUXERUI_APP_ROOT}/src/*.cpp"
        "${HUXERUI_APP_ROOT}/src/*.cc"
        "${HUXERUI_APP_ROOT}/src/*.cxx"
)

huxerui_add_app(@TARGET_NAME@
        SOURCES
            ${HUXERUI_APP_SOURCE_FILES}
        RESOURCES
            "${HUXERUI_APP_ROOT}/assets"
        RESOURCE_NAMESPACE
            app
)
)TEMPLATE")},
        {"huxerui.cmake", context.Render(R"TEMPLATE(set(HUXERUI_ANDROID_APPLICATION_ID "@PACKAGE_NAME@")
set(HUXERUI_ANDROID_COMPILE_SDK 36)
set(HUXERUI_ANDROID_MIN_SDK 23)
set(HUXERUI_ANDROID_TARGET_SDK 36)
set(HUXERUI_ANDROID_NDK_VERSION "29.0.14206865")
set(HUXERUI_ANDROID_ABIS arm64-v8a x86_64)
)TEMPLATE")},
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
        std::string_view{"app/src/main/cpp/CMakeLists.txt"},
        std::string_view{"huxerui.cmake"},
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

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::filesystem::path shell = context.project_root / "platform/android";
    const std::filesystem::path plan = context.project_root / ".huxerui/generated/android/app.json";
    const std::filesystem::path plan_script = LocateSdkCMakeFile(context.sdk_root, "HuxerUIAndroidPlan.cmake");
    const std::string configuration = ProfileConfiguration(context.profile);
    const std::filesystem::path wrapper = shell / (CurrentHostId() == "windows" ? "gradlew.bat" : "gradlew");
    const std::string gradle = std::filesystem::is_regular_file(wrapper) ? wrapper.string() : "gradle";
    return {
        {"cmake",
         {
             "-DHUXERUI_PLATFORM_FILE=" + (shell / "huxerui.cmake").string(),
             "-DHUXERUI_PLATFORM_PLAN_OUTPUT=" + plan.string(),
             "-DHUXERUI_SDK_ROOT=" + context.sdk_root.string(),
             "-P",
             plan_script.string(),
         },
         context.project_root},
        {gradle, {":app:assemble" + configuration}, shell},
    };
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::filesystem::path plan = context.project_root / ".huxerui/generated/android/app.json";
    const std::string application_id = JsonString(ReadFile(plan), "applicationId");
    const std::filesystem::path apk = context.project_root / "platform/android/app/build/outputs/apk" /
                                      context.profile / ("app-" + context.profile + ".apk");
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
  <assemblyIdentity name="@PACKAGE_NAME@" version="1.0.0.0" type="win32" />
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
set(HUXERUI_MACOS_BUNDLE_IDENTIFIER "@PACKAGE_NAME@")
set(HUXERUI_MACOS_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist.in")
)TEMPLATE")},
        {"Info.plist.in", context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDisplayName</key>
  <string>@PROJECT_NAME@</string>
  <key>CFBundleIdentifier</key>
  <string>@PACKAGE_NAME@</string>
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
        "-DHUXERUI_SDK_ROOT=" + context.sdk_root.string(),
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
const MacOSDriver macos_driver;
const WebDriver web_driver;
constexpr std::array<const PlatformDriver*, 4> platform_drivers{
    &android_driver,
    &windows_driver,
    &macos_driver,
    &web_driver,
};

} // namespace

bool PlatformDriver::SupportsDeviceDiscovery() const noexcept {
  return false;
}

std::vector<PlatformDevice> PlatformDriver::DiscoverDevices() const {
  throw std::logic_error("platform does not support device discovery: " + std::string(Id()));
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
    devices.push_back({std::move(id), std::move(name), state});
  }
  return devices;
}

std::string ProjectTemplateContext::Render(std::string_view value) const {
  std::string rendered(value);
  ReplaceAll(rendered, "@PROJECT_NAME@", project_name);
  ReplaceAll(rendered, "@TARGET_NAME@", target_name);
  ReplaceAll(rendered, "@PACKAGE_NAME@", package_name);
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
