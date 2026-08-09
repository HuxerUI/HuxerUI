#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "ios_project.h"
#include "platform.h"
#include "process_runner.h"
#include "project.h"
#include "sdk.h"

namespace {

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

Invocation Invoke(const std::filesystem::path& directory, std::initializer_list<std::string_view> arguments) {
  const std::vector<std::string_view> values(arguments);
  std::ostringstream output;
  std::ostringstream error;
  const int result = huxerui::cli::Run(values, directory, {}, output, error);
  return {result, output.str(), error.str()};
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

TEST_CASE("HuxerUICliCreatesSelectedPlatformShells") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "Sample-App", "--platform", "windows,android,web"});

  REQUIRE(invocation.result == 0);
  const std::filesystem::path project = temporary.Path() / "Sample-App";
  REQUIRE(std::filesystem::is_regular_file(project / "CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(project / "src/main.cpp"));
  REQUIRE(std::filesystem::is_directory(project / "assets/images"));
  REQUIRE(std::filesystem::is_directory(project / "assets/raw"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/app.manifest"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/web/index.html.in"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/macos"));
  REQUIRE(Read(project / ".gitignore").find("/.huxerui/") != std::string::npos);
  const std::string cmake = Read(project / "CMakeLists.txt");
  REQUIRE(cmake.find("huxerui_add_app(sample_app") != std::string::npos);
  REQUIRE(cmake.find("src/*.cpp") != std::string::npos);
  REQUIRE(cmake.find("SOURCES\n            ${APP_SOURCE_FILES}") != std::string::npos);
  REQUIRE(cmake.find("NO_CMAKE_FIND_ROOT_PATH") != std::string::npos);
  const std::string android_settings = Read(project / "platform/android/settings.gradle");
  const std::string android_app = Read(project / "platform/android/app/build.gradle");
  const std::string android_cmake = Read(project / "platform/android/app/src/main/cpp/CMakeLists.txt");
  REQUIRE(android_settings.find("project(\":HuxerUI\").projectDir") != std::string::npos);
  REQUIRE(android_settings.find("mavenCentral()") != std::string::npos);
  REQUIRE(android_app.find("implementation project(\":HuxerUI\")") != std::string::npos);
  REQUIRE(android_app.find("huxeruiSdkRoot") == std::string::npos);
  REQUIRE(android_cmake.find("HUXERUI_APP_SOURCE_FILES") != std::string::npos);
  REQUIRE(android_cmake.find("src/main.cpp") == std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(
      project / "platform/android/app/src/main/java/com/example/sample_app/MainActivity.java"
  ));
}

TEST_CASE("HuxerUICliRefusesInvalidAndExistingDestinations") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "../invalid"}).result == 2);

  const Invocation first = Invoke(temporary.Path(), {"create", "sample", "--platform", "windows"});
  REQUIRE(first.result == 0);
  const std::string cmake = Read(temporary.Path() / "sample/CMakeLists.txt");

  const Invocation second = Invoke(temporary.Path(), {"create", "sample", "--platform", "android"});
  REQUIRE(second.result == 1);
  REQUIRE(Read(temporary.Path() / "sample/CMakeLists.txt") == cmake);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "sample/platform/android"));
}

TEST_CASE("HuxerUICliAddsMissingPlatformsFromNestedProjectDirectories") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path nested = temporary.Path() / "sample/src/nested";
  std::filesystem::create_directories(nested);

  const Invocation invocation = Invoke(nested, {"platform", "add", "all"});

  REQUIRE(invocation.result == 0);
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/macos/Info.plist.in"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/ios/App/Info.plist"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/ios/sample.xcodeproj/project.pbxproj"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/web/index.html.in"));
  REQUIRE(Invoke(nested, {"platform", "add", "all"}).result == 1);
}

TEST_CASE("HuxerUICliDoctorReportsIncompleteAndUnknownPlatforms") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::remove(project / "platform/windows/app.manifest");
  std::filesystem::create_directories(project / "platform/custom");

  const Invocation invocation = Invoke(project, {"doctor", "all"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(invocation.output.find("missing app.manifest") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android") == std::string::npos);
}

TEST_CASE("HuxerUICliDoctorAcceptsAReorganizedSourceDirectory") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::rename(project / "src/main.cpp", project / "src/app.cpp");

  const Invocation invocation = Invoke(project, {"doctor", "windows"});

  REQUIRE(invocation.output.find("missing src") == std::string::npos);
}

TEST_CASE("HuxerUICliRejectsUnknownPlatformsAsUsageErrors") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "sample", "--platform", "plan9"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("unknown platform: plan9") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "sample"));
}

TEST_CASE("HuxerUICliCreatesStableDesktopBuildCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* windows = huxerui::cli::FindPlatformDriver("windows");
  REQUIRE(windows != nullptr);
  const huxerui::cli::PlatformCommandContext context{
      temporary.Path() / "sample",
      temporary.Path() / "sdk",
      temporary.Path() / "sample/.huxerui/build/windows/release",
      "Ninja",
      "release",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> commands = windows->BuildCommands(context);

  REQUIRE(commands.size() == 2);
  REQUIRE(commands[0].executable == "cmake");
  REQUIRE(
      commands[0].arguments ==
      std::vector<std::string>{
          "-G",
          "Ninja",
          "-S",
          context.project_root.string(),
          "-B",
          context.build_directory.string(),
          "-DCMAKE_BUILD_TYPE=Release",
          "-DHUXERUI_SDK_ROOT=" + context.sdk_root.string(),
      }
  );
  REQUIRE(
      commands[1].arguments ==
      std::vector<std::string>{
          "--build",
          context.build_directory.string(),
          "--config",
          "Release",
          "--parallel",
      }
  );
}

TEST_CASE("HuxerUICliCreatesAndroidBuildCommandsForSourceSdks") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* android = huxerui::cli::FindPlatformDriver("android");
  REQUIRE(android != nullptr);
  const std::filesystem::path sdk = temporary.Path() / "sdk";
  const std::filesystem::path helper = sdk / "cmake/HuxerUIAndroidPlan.cmake";
  std::filesystem::create_directories(helper.parent_path());
  std::ofstream(helper) << "# test\n";
  const huxerui::cli::PlatformCommandContext context{
      temporary.Path() / "sample",
      sdk,
      temporary.Path() / "sample/.huxerui/build/android/release",
      {},
      "release",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> commands = android->BuildCommands(context);

  REQUIRE(commands.size() == 2);
  REQUIRE(commands[0].executable == "cmake");
  REQUIRE(
      std::find(commands[0].arguments.begin(), commands[0].arguments.end(), "-DHUXERUI_SDK_ROOT=" + sdk.string()) !=
      commands[0].arguments.end()
  );
  REQUIRE(commands[1].arguments == std::vector<std::string>{":app:assembleRelease"});
}

TEST_CASE("HuxerUICliCreatesWebBuildAndRunCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* web = huxerui::cli::FindPlatformDriver("web");
  REQUIRE(web != nullptr);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path build = project / ".huxerui/build/web/debug";
  const huxerui::cli::PlatformCommandContext context{
      project,
      temporary.Path() / "sdk",
      build,
      "Ninja",
      "debug",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> build_commands = web->BuildCommands(context);

  REQUIRE(build_commands.size() == 2);
  REQUIRE(build_commands[0].executable == "emcmake");
  REQUIRE(
      build_commands[0].arguments ==
      std::vector<std::string>{
          "cmake",
          "-G",
          "Ninja",
          "-S",
          project.string(),
          "-B",
          build.string(),
          "-DCMAKE_BUILD_TYPE=Debug",
          "-DHUXERUI_SDK_ROOT=" + context.sdk_root.string(),
      }
  );
  REQUIRE(build_commands[1].executable == "cmake");

  const std::filesystem::path artifact = build / "sample.mjs";
  const std::filesystem::path entry = build / "sample.html";
  const std::filesystem::path plan = build / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(artifact) << "export default {};\n";
  std::ofstream(entry) << "<!doctype html>\n";
  std::ofstream(plan) << "{\n"
                         "  \"target\": \"sample\",\n"
                         "  \"artifact\": \""
                      << artifact.generic_string() << "\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> run_commands = web->RunCommands(context);

  REQUIRE(run_commands.size() == 1);
  REQUIRE(run_commands[0].executable == "emrun");
  REQUIRE(run_commands[0].arguments.size() == 1);
  REQUIRE(std::filesystem::equivalent(run_commands[0].arguments[0], entry));
  REQUIRE(std::filesystem::equivalent(run_commands[0].working_directory, build));
}

TEST_CASE("HuxerUICliCreatesIosBuildAndRunCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const std::vector<huxerui::cli::GeneratedFile> shell =
      ios->CreateShell(huxerui::cli::MakeProjectTemplateContext("Sample-App"));
  const auto base_configuration = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "Config/Base.xcconfig";
  });
  REQUIRE(base_configuration != shell.end());
  REQUIRE(base_configuration->content.find("com.example.sample-app") != std::string::npos);
  REQUIRE(base_configuration->content.find("HUXERUI_LINK_OPTIONS_FILE") != std::string::npos);
  REQUIRE(base_configuration->content.find("@\"$(HUXERUI_LINK_OPTIONS_FILE)\"") != std::string::npos);
  REQUIRE(base_configuration->content.find("-framework UIKit") == std::string::npos);
  const auto launch_screen = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "App/LaunchScreen.storyboard";
  });
  REQUIRE(launch_screen != shell.end());
  REQUIRE(launch_screen->content.find("initialViewController=\"hux-controller\"") != std::string::npos);
  REQUIRE(std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
            return file.path == "sample_app.xcodeproj/project.pbxproj";
          }) != shell.end());
  REQUIRE(std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
            return file.path == "App/main.mm";
          }) != shell.end());
  REQUIRE(std::none_of(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "huxerui.cmake";
  }));
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::create_directories(project);
  std::ofstream(project / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.20)\nproject(sample)\n";
  const std::filesystem::path xcode_project = project / "platform" / "ios" / "sample.xcodeproj";
  std::filesystem::create_directories(xcode_project);
  const std::filesystem::path build = project / ".huxerui/build/ios-simulator/debug";
  const huxerui::cli::PlatformCommandContext context{
      project,
      temporary.Path() / "sdk",
      build,
      {},
      "debug",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> build_commands = ios->BuildCommands(context);

  REQUIRE(build_commands.size() == 1);
  REQUIRE(build_commands[0].executable == "xcodebuild");
  REQUIRE(
      build_commands[0].arguments ==
      std::vector<std::string>{
          "-project",
          xcode_project.string(),
          "-scheme",
          "sample",
          "-configuration",
          "Debug",
          "-derivedDataPath",
          (build / "DerivedData").string(),
          "-destination",
          "generic/platform=iOS Simulator",
          "HUXERUI_SDK_ROOT=" + context.sdk_root.string(),
          "HUXERUI_INTEGRATION_PLAN=" + (build / "huxerui-integration/app.json").string(),
          "build",
      }
  );

  const std::filesystem::path bundle = build / "bin/Debug/sample.app";
  const std::filesystem::path plan = build / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(bundle);
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(plan) << "{\n"
                         "  \"bundle\": \""
                      << bundle.generic_string() << "\",\n"
                      << "  \"bundleIdentifier\": \"com.example.sample\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> run_commands = ios->RunCommands(context);

  REQUIRE(run_commands.size() == 2);
  REQUIRE(run_commands[0].executable == "xcrun");
  REQUIRE(
      run_commands[0].arguments == std::vector<std::string>{"simctl", "install", "booted", bundle.generic_string()}
  );
  REQUIRE(run_commands[1].executable == "xcrun");
  REQUIRE(run_commands[1].arguments == std::vector<std::string>{"simctl", "launch", "booted", "com.example.sample"});

  const huxerui::cli::PlatformCommandContext physical_context{
      project,
      temporary.Path() / "sdk",
      project / ".huxerui/build/ios-device/debug",
      {},
      "debug",
      huxerui::cli::PlatformDevice{
          "AC622B12-7BDD-5F92-9091-A0673B827C3A",
          "iPhone",
          huxerui::cli::DeviceState::Ready,
          huxerui::cli::DeviceKind::Physical,
          "00008130-000C048A1A90001C",
      },
  };

  const std::vector<huxerui::cli::ProcessCommand> physical_build_commands = ios->BuildCommands(physical_context);

  REQUIRE(physical_build_commands.size() == 1);
  REQUIRE(
      physical_build_commands[0].arguments ==
      std::vector<std::string>{
          "-project",
          xcode_project.string(),
          "-scheme",
          "sample",
          "-configuration",
          "Debug",
          "-derivedDataPath",
          (physical_context.build_directory / "DerivedData").string(),
          "-destination",
          "id=00008130-000C048A1A90001C",
          "HUXERUI_SDK_ROOT=" + physical_context.sdk_root.string(),
          "HUXERUI_INTEGRATION_PLAN=" + (physical_context.build_directory / "huxerui-integration/app.json").string(),
          "-allowProvisioningUpdates",
          "build",
      }
  );

  const std::filesystem::path physical_bundle = physical_context.build_directory / "bin/Debug/sample.app";
  const std::filesystem::path physical_plan =
      physical_context.build_directory / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(physical_bundle);
  std::filesystem::create_directories(physical_plan.parent_path());
  std::ofstream(physical_plan) << "{\n"
                                  "  \"bundle\": \""
                               << physical_bundle.generic_string() << "\",\n"
                               << "  \"bundleIdentifier\": \"com.example.sample\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> physical_run_commands = ios->RunCommands(physical_context);

  REQUIRE(physical_run_commands.size() == 2);
  REQUIRE(
      physical_run_commands[0].arguments ==
      std::vector<std::string>{
          "devicectl",
          "device",
          "install",
          "app",
          "--device",
          physical_context.device->id,
          physical_bundle.generic_string(),
      }
  );
  REQUIRE(
      physical_run_commands[1].arguments ==
      std::vector<std::string>{
          "devicectl",
          "device",
          "process",
          "launch",
          "--device",
          physical_context.device->id,
          "--terminate-existing",
          "com.example.sample",
      }
  );

  const std::vector<huxerui::cli::ProcessCommand> open_commands = ios->OpenCommands(physical_context);

  REQUIRE(open_commands.size() == 1);
  REQUIRE(
      open_commands[0].arguments ==
      std::vector<std::string>{
          "-a",
          "Xcode",
          xcode_project.string(),
      }
  );
}

TEST_CASE("HuxerUICliConfiguresIosSdkWithoutReplacingLocalSigningSettings") {
  TemporaryDirectory temporary;
  const std::filesystem::path configuration = temporary.Path() / "platform/ios/Config/Local.xcconfig";
  std::filesystem::create_directories(configuration.parent_path());
  std::ofstream(configuration) << "DEVELOPMENT_TEAM = ABC123\nHUXERUI_SDK_ROOT = /old/sdk\n";

  huxerui::cli::ConfigureIosLocalSdk(temporary.Path(), temporary.Path() / "installed sdk");

  const std::string content = Read(configuration);
  REQUIRE(content.find("DEVELOPMENT_TEAM = ABC123") != std::string::npos);
  REQUIRE(
      content.find("HUXERUI_SDK_ROOT = " + (temporary.Path() / "installed sdk").generic_string()) != std::string::npos
  );
  REQUIRE(content.find("/old/sdk") == std::string::npos);
}

TEST_CASE("HuxerUICliParsesIosDeviceStates") {
  const std::vector<huxerui::cli::PlatformDevice> physical = huxerui::cli::ParseIosPhysicalDevices(
      "iPhone         AC622B12-7BDD-5F92-9091-A0673B827C3A   available (paired)   "
      "00008130-000C048A1A90001C\n"
      "iPhone 15 test  9B6DBEB2-CF66-5977-9548-F1913028F37C   unavailable          "
      "00008120-001158460C50A01E\n"
  );
  REQUIRE(
      physical ==
      std::vector<huxerui::cli::PlatformDevice>{
          {"AC622B12-7BDD-5F92-9091-A0673B827C3A",
           "iPhone",
           huxerui::cli::DeviceState::Ready,
           huxerui::cli::DeviceKind::Physical,
           "00008130-000C048A1A90001C"},
          {"9B6DBEB2-CF66-5977-9548-F1913028F37C",
           "iPhone 15 test",
           huxerui::cli::DeviceState::Unavailable,
           huxerui::cli::DeviceKind::Physical,
           "00008120-001158460C50A01E"},
      }
  );

  const std::vector<huxerui::cli::PlatformDevice> simulators =
      huxerui::cli::ParseIosSimulatorDevices("== Devices ==\n"
                                             "-- iOS 26.5 --\n"
                                             "    iPhone 17 Pro (14C02540-D279-474E-8D33-06363B85A5B6) (Booted)\n"
                                             "    iPad (A16) (AE3792D8-0370-4B69-94F4-FF5A26C51A38) (Shutdown)\n"
                                             "-- Unavailable: com.apple.CoreSimulator.SimRuntime.iOS-16-4 --\n"
                                             "    Old iPhone (0A3792D8-0370-4B69-94F4-FF5A26C51A38) (Shutdown)\n");
  REQUIRE(
      simulators ==
      std::vector<huxerui::cli::PlatformDevice>{
          {"14C02540-D279-474E-8D33-06363B85A5B6",
           "iPhone 17 Pro — iOS 26.5",
           huxerui::cli::DeviceState::Ready,
           huxerui::cli::DeviceKind::Simulator,
           "14C02540-D279-474E-8D33-06363B85A5B6"},
          {"AE3792D8-0370-4B69-94F4-FF5A26C51A38",
           "iPad (A16) — iOS 26.5",
           huxerui::cli::DeviceState::Offline,
           huxerui::cli::DeviceKind::Simulator,
           "AE3792D8-0370-4B69-94F4-FF5A26C51A38"},
      }
  );
}

TEST_CASE("HuxerUICliDescribesProcessArgumentsWithoutShellEvaluation") {
  const huxerui::cli::ProcessCommand command{
      "tool",
      {"plain", "with space", "quoted\"value"},
      {},
  };

  REQUIRE(huxerui::cli::DescribeProcess(command) == R"(tool plain "with space" "quoted\"value")");
}

TEST_CASE("HuxerUICliCapturesProcessOutput") {
  TemporaryDirectory temporary;
  const huxerui::cli::ProcessResult result =
      huxerui::cli::RunProcessCapture({"cmake", {"-E", "echo", "captured output"}, temporary.Path()});

  REQUIRE(result.exit_code == 0);
  REQUIRE(result.output.find("captured output") != std::string::npos);
}

TEST_CASE("HuxerUICliParsesAndroidDeviceStates") {
  const std::vector<huxerui::cli::PlatformDevice> devices = huxerui::cli::ParseAdbDevices(
      "List of devices attached\r\n"
      "emulator-5554 device product:sdk_phone model:Pixel_8 device:emu64x transport_id:1\r\n"
      "R58M offline transport_id:2\r\n"
      "ABC unauthorized usb:1-2 transport_id:3\r\n"
      "???????????? no permissions (user in plugdev group)\r\n"
  );

  REQUIRE(
      devices ==
      std::vector<huxerui::cli::PlatformDevice>{
          {"emulator-5554", "Pixel_8", huxerui::cli::DeviceState::Ready, huxerui::cli::DeviceKind::Unspecified, {}},
          {"R58M", {}, huxerui::cli::DeviceState::Offline, huxerui::cli::DeviceKind::Unspecified, {}},
          {"ABC", {}, huxerui::cli::DeviceState::Unauthorized, huxerui::cli::DeviceKind::Unspecified, {}},
          {"????????????", {}, huxerui::cli::DeviceState::Unavailable, huxerui::cli::DeviceKind::Unspecified, {}},
      }
  );
}

TEST_CASE("HuxerUICliRejectsDeviceDiscoveryForDesktopPlatforms") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"devices", "windows"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.error.find("platform does not support device discovery: windows") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsDeviceSelectionForDesktopRunsBeforeBuilding") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::vector<std::string_view> arguments{"run", "windows", "--device", "phone"};
  std::ostringstream output;
  std::ostringstream error;

  const int result = huxerui::cli::Run(arguments, project, temporary.Path(), output, error);

  REQUIRE(result == 2);
  REQUIRE(error.str().find("--device is not supported for platform windows") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliRejectsUnknownProjectPlatformsBeforeRun") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::create_directories(project / "platform/custom");
  const std::vector<std::string_view> arguments{"run", "windows"};
  std::ostringstream output;
  std::ostringstream error;

  const int result = huxerui::cli::Run(arguments, project, temporary.Path(), output, error);

  REQUIRE(result == 1);
  REQUIRE(error.str().find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliFindsCMakeHelpersInPortableInstallLayouts") {
  TemporaryDirectory temporary;
  const std::filesystem::path helper = temporary.Path() / "lib64/cmake/HuxerUI/HuxerUIAndroidPlan.cmake";
  std::filesystem::create_directories(helper.parent_path());
  std::ofstream(helper) << "# test\n";
  std::ofstream(helper.parent_path() / "HuxerUIConfig.cmake") << "# test\n";

  REQUIRE(huxerui::cli::LocateSdkCMakeFile(temporary.Path(), "HuxerUIAndroidPlan.cmake") == helper);
}

TEST_CASE("HuxerUICliDoctorReportsTheResolvedSdk") {
  TemporaryDirectory temporary;
  const std::vector<std::string_view> arguments{"doctor"};
  std::ostringstream output;
  std::ostringstream error;

  const int result = huxerui::cli::Run(arguments, temporary.Path(), temporary.Path(), output, error);

  REQUIRE(result == 0);
  REQUIRE(output.str().find("[ok] HuxerUI SDK: " + temporary.Path().string()) != std::string::npos);
  REQUIRE(error.str().empty());
}

TEST_CASE("HuxerUICliDoctorChecksRequestedPlatformsOutsideAProject") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"doctor", "android"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("Project: not found") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android:") != std::string::npos);
}

#if defined(_WIN32)
TEST_CASE("HuxerUICliRunsWindowsBatchTools") {
  TemporaryDirectory temporary;
  const std::filesystem::path batch = temporary.Path() / "return seven.cmd";
  std::ofstream(batch) << "@exit /b 7\n";

  REQUIRE(huxerui::cli::RunProcess({batch.string(), {}, temporary.Path()}) == 7);
}
#endif

} // namespace
