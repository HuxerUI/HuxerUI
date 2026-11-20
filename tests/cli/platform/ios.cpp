#include "support.h"

#include <cstdlib>

using namespace huxerui::cli::test;

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
  REQUIRE(base_configuration->content.find("com.example.sampleapp") != std::string::npos);
  REQUIRE(base_configuration->content.find("HUXERUI_LINK_OPTIONS_FILE") != std::string::npos);
  REQUIRE(base_configuration->content.find("IPHONEOS_DEPLOYMENT_TARGET = 15.0") != std::string::npos);
  REQUIRE(base_configuration->content.find("HEADER_SEARCH_PATHS") != std::string::npos);
  REQUIRE(base_configuration->content.find("@\"$(HUXERUI_LINK_OPTIONS_FILE)\"") != std::string::npos);
  REQUIRE(base_configuration->content.find("-framework UIKit") == std::string::npos);
  const auto app_icon = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "App/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png";
  });
  REQUIRE(app_icon != shell.end());
  REQUIRE(app_icon->content.starts_with(std::string("\x89PNG\r\n\x1a\n", 8)));
  const auto launch_screen = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "App/LaunchScreen.storyboard";
  });
  REQUIRE(launch_screen != shell.end());
  REQUIRE(launch_screen->content.find("initialViewController=\"hux-controller\"") != std::string::npos);
  const auto xcode_project_file = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "sample_app.xcodeproj/project.pbxproj";
  });
  REQUIRE(xcode_project_file != shell.end());
  REQUIRE(xcode_project_file->content.find("XCLocalSwiftPackageReference") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("../../.huxerui/generated/ios/libraries") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("productName = HuxerUILibraries") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("productName = \"Sample-App\"") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("Scripts/build_huxerui_core.sh") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("Scripts/stage_huxerui_resources.sh") != std::string::npos);
  const auto build_script = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "Scripts/build_huxerui_core.sh";
  });
  REQUIRE(build_script != shell.end());
  REQUIRE(build_script->content.find("sample_app_huxerui_ios_core") != std::string::npos);
  REQUIRE(build_script->content.find("-DHUXERUI_ENABLE_PROFILING=\"${HUXERUI_ENABLE_PROFILING:-OFF}\"") !=
          std::string::npos);
  const auto resource_script = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "Scripts/stage_huxerui_resources.sh";
  });
  REQUIRE(resource_script != shell.end());
  REQUIRE(
      resource_script->content.find("$HUXERUI_CORE_BUILD_DIR/huxerui-ios/sample_app/resources/package") !=
      std::string::npos
  );
  REQUIRE(resource_script->content.find("HuxerUI resource package is missing") != std::string::npos);
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

  const std::vector<huxerui::cli::ProcessCommand> library_commands = ios->LibraryGraphCommands(context);
  const std::vector<huxerui::cli::ProcessCommand> build_commands = ios->BuildCommands(context);

  REQUIRE(library_commands.size() == 1);
  REQUIRE(library_commands[0].executable == "cmake");
  REQUIRE(library_commands[0].arguments == ExpectedLibraryGraphArguments(context));
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
          "HUXERUI_HOME=" + context.huxerui_home.string(),
          "HUXERUI_BUILD_SYSTEM=cmake",
          "HUXERUI_INTEGRATION_PLAN=" + (build / "huxerui-integration/app.json").string(),
          "build",
      }
  );

  // The Xcode shell builds an mcpp core too: mcpp builds it first, which records the framework it resolved, and
  // the shell compiles its entry against that framework.
  auto mcpp_context = context;
  mcpp_context.build_system = huxerui::cli::BuildSystem::Mcpp;
  mcpp_context.mcpp_options = huxerui::cli::mcpp::CommandOptions{
      .project_root = project,
      .platform = "ios",
      .profile_name = "dev",
      .features = {"huxerui/profiling"},
  };
  const auto mcpp_graph = ios->LibraryGraphCommands(mcpp_context);
  REQUIRE(mcpp_graph.size() == 1);
  REQUIRE(mcpp_graph[0].executable == "mcpp");
  REQUIRE(mcpp_graph[0].arguments ==
          std::vector<std::string>{"build", "--target", "aarch64-ios-sim", "--features", "huxerui/profiling"});
  REQUIRE_THROWS_WITH(ios->BuildCommands(mcpp_context), Catch::Matchers::ContainsSubstring("framework record"));
  const std::filesystem::path framework = temporary.Path() / "resolved";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::ofstream(project / ".huxerui/generated/framework.json")
      << "{\n  \"schema\": 1,\n  \"sourceRoot\": \"" << framework.generic_string() << "\"\n}\n";
  const auto mcpp_build = ios->BuildCommands(mcpp_context).front().arguments;
  REQUIRE(std::ranges::find(mcpp_build, "HUXERUI_HOME=" + framework.generic_string()) != mcpp_build.end());
  REQUIRE(std::ranges::find(mcpp_build, "HUXERUI_BUILD_SYSTEM=mcpp") != mcpp_build.end());
  REQUIRE(std::ranges::find(mcpp_build, "HUXERUI_MCPP_PROFILE=dev") != mcpp_build.end());
  REQUIRE(std::ranges::find(mcpp_build, "HUXERUI_MCPP_FEATURES=huxerui/profiling") != mcpp_build.end());
  REQUIRE(std::ranges::find(mcpp_build, "ARCHS=arm64") != mcpp_build.end());
  auto intel_context = mcpp_context;
  intel_context.abis = {"x86_64"};
  REQUIRE_THROWS_WITH(ios->ValidateBuildAbis(intel_context), Catch::Matchers::ContainsSubstring("arm64 ABI only"));

  auto profiling_context = context;
  profiling_context.profiling = true;
  const auto profiling_commands = ios->BuildCommands(profiling_context);
  const auto& profiling_arguments = profiling_commands.front().arguments;
  REQUIRE(std::find(profiling_arguments.begin(), profiling_arguments.end(), "HUXERUI_ENABLE_PROFILING=ON") !=
          profiling_arguments.end());

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

  auto universal_context = context;
  universal_context.abis = {"arm64", "x86_64"};
  REQUIRE_NOTHROW(ios->ValidateBuildAbis(universal_context));
  const auto universal_commands = ios->BuildCommands(universal_context);
  const auto& arguments = universal_commands.front().arguments;
  REQUIRE(std::ranges::find(arguments, "ARCHS=arm64 x86_64") != arguments.end());
  REQUIRE(std::ranges::find(arguments, "ONLY_ACTIVE_ARCH=NO") != arguments.end());
  universal_context.device = physical_context.device;
  REQUIRE_THROWS_AS(ios->ValidateBuildAbis(universal_context), std::invalid_argument);

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
          "HUXERUI_HOME=" + physical_context.huxerui_home.string(),
          "HUXERUI_BUILD_SYSTEM=cmake",
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

TEST_CASE("HuxerUICliGeneratesIosLibraryIntegrationFromTheCommonGraph") {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path camera = temporary.Path() / "camera library";
  const std::filesystem::path maps = temporary.Path() / "maps";
  const std::filesystem::path common = temporary.Path() / "common";
  const std::filesystem::path camera_package = camera / "platform/ios/HuxerUICameraKit";
  const std::filesystem::path maps_package = maps / "platform/ios/MapView";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::filesystem::create_directories(camera_package);
  std::filesystem::create_directories(maps_package);
  std::filesystem::create_directories(common / "platform/ios/src");
  std::ofstream(camera_package / "Package.swift") << "// camera\n";
  std::ofstream(maps_package / "Package.swift") << "// maps\n";
  std::filesystem::create_directories(project / "platform/ios/Config");
  std::ofstream(project / ".huxerui/generated/libraries.json")
      << "{\n  \"schema\": 1,\n  \"libraries\": [\n"
      << "    {\"target\": \"HuxerUICameraKit::HuxerUICameraKit\", \"sourceRoot\": \"" << camera.generic_string()
      << "\"},\n"
      << "    {\"target\": \"CommonTools::CommonTools\", \"sourceRoot\": \"" << common.generic_string() << "\"},\n"
      << "    {\"target\": \"map_view\", \"sourceRoot\": \"" << maps.generic_string() << "\"}\n"
      << "  ]\n}\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{project, temporary.Path() / "sdk", {}, {}, "debug", {}};
  ios->UpdateProjectIntegration(context);

  REQUIRE_FALSE(std::filesystem::exists(project / "platform/ios/Config/Local.xcconfig"));
  const std::filesystem::path integration = project / ".huxerui/generated/ios/libraries";
  const std::string manifest = Read(integration / "Package.swift");
  const std::size_t camera_dependency = manifest.find(".package(name: \"HuxerUICameraKit\"");
  const std::size_t maps_dependency = manifest.find(".package(name: \"MapView\"");
  REQUIRE(camera_dependency != std::string::npos);
  REQUIRE(maps_dependency != std::string::npos);
  REQUIRE(camera_dependency < maps_dependency);
  REQUIRE(manifest.find(".iOS(.v15)") != std::string::npos);
  REQUIRE(manifest.find("HuxerUICameraKit") != std::string::npos);
  REQUIRE(manifest.find("MapView") != std::string::npos);
  REQUIRE(manifest.find("CommonTools") == std::string::npos);
  REQUIRE(manifest.find(camera_package.generic_string()) != std::string::npos);
  REQUIRE(manifest.find(maps_package.generic_string()) != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(integration / "Sources/HuxerUILibraries/HuxerUILibraries.swift"));
  const std::array unchanged_files{
      integration / "Package.swift",
      integration / "Sources/HuxerUILibraries/HuxerUILibraries.swift",
  };
  const auto previous_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
  for (const auto& file : unchanged_files) {
    std::filesystem::last_write_time(file, previous_time);
  }
  ios->UpdateProjectIntegration(context);
  for (const auto& file : unchanged_files) {
    // Keep Catch2 from formatting filesystem clocks with a non-streamable representation.
    REQUIRE((std::filesystem::last_write_time(file) == previous_time));
  }
}

TEST_CASE("HuxerUICliRejectsIncompleteAndLegacyIosLibraryPackages") {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path library = temporary.Path() / "camera";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::filesystem::create_directories(library / "platform/ios");
  std::ofstream(project / ".huxerui/generated/libraries.json")
      << "{\n  \"schema\": 1,\n  \"libraries\": [\n"
      << "    {\"target\": \"CameraKit::CameraKit\", \"sourceRoot\": \"" << library.generic_string() << "\"}\n"
      << "  ]\n}\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{project, {}, {}, {}, "debug", {}};
  SECTION("Product directory without a manifest") {
    std::filesystem::create_directories(library / "platform/ios/CameraKit");
    REQUIRE_THROWS_WITH(
        ios->UpdateProjectIntegration(context),
        Catch::Matchers::ContainsSubstring("iOS library package is missing Package.swift")
    );
  }
  SECTION("Manifest in the old package directory") {
    std::ofstream(library / "platform/ios/Package.swift") << "// camera\n";
    REQUIRE_THROWS_WITH(
        ios->UpdateProjectIntegration(context),
        Catch::Matchers::ContainsSubstring("Move the package to: " + (library / "platform/ios/CameraKit").generic_string())
    );
  }
}

TEST_CASE("HuxerUICliPreservesIosLocalSettings") {
  TemporaryDirectory temporary;
  const std::filesystem::path configuration = temporary.Path() / "platform/ios/Config/Local.xcconfig";
  const std::filesystem::path huxerui_home = temporary.Path() / "installed sdk";
  std::filesystem::create_directories(configuration.parent_path());
  std::filesystem::create_directories(temporary.Path() / ".huxerui/generated");
  std::ofstream(temporary.Path() / ".huxerui/generated/libraries.json") << "{\"schema\":1,\"libraries\":[]}";
  const std::string original = "DEVELOPMENT_TEAM = ABC123\nHUXERUI_HOME = /ide/sdk\n";
  std::ofstream(configuration) << original;
  const auto timestamp = std::filesystem::last_write_time(configuration);

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  huxerui::cli::PlatformCommandContext context{temporary.Path(), huxerui_home, {}, {}, "debug", {}};
  ios->UpdateProjectIntegration(context);
  REQUIRE(Read(configuration) == original);
  REQUIRE((std::filesystem::last_write_time(configuration) == timestamp));
  context.huxerui_home = HUXERUI_TEST_SOURCE_DIRECTORY;
  ios->UpdateProjectIntegration(context);
  REQUIRE(Read(configuration) == original);
  REQUIRE((std::filesystem::last_write_time(configuration) == timestamp));
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

#if !defined(_WIN32)
TEST_CASE("HuxerUICliDiscoversOneDestinationPerIosDevice") {
  using namespace huxerui::cli;
  bool legacy = false;
  SECTION("devicectl supports physical-device filtering") {}
  SECTION("devicectl requires the legacy device table") {
    legacy = true;
  }

  TemporaryDirectory temporary;
  const auto xcrun = temporary.Path() / "xcrun";
  {
    std::ofstream script(xcrun);
    script << "#!/bin/sh\ncase \"$1 $2 $3\" in\n  'devicectl list devices')\n";
    if (legacy) {
      script << R"(    case " $* " in *" --filter "*) exit 1 ;; esac
    printf '%s\n' \
      'Paired iPhone  AC622B12-7BDD-5F92-9091-A0673B827C3A   available (paired)   00008130-000C048A1A90001C' \
      'Stale simulator  AE3792D8-0370-4B69-94F4-FF5A26C51A38   unavailable   AE3792D8-0370-4B69-94F4-FF5A26C51A38'
)";
    } else {
      script << R"(    case " $* " in
      *" CoreDevice ID "*" --filter Reality = 'physical' "*) ;;
      *) exit 1 ;;
    esac
    printf '%s\n' \
      'Paired iPhone   available (paired)   AC622B12-7BDD-5F92-9091-A0673B827C3A   00008130-000C048A1A90001C'
)";
    }
    script << R"(    ;;
  'simctl list devices')
    printf '%s\n' '-- iOS 26.5 --' \
      '    iPad (AE3792D8-0370-4B69-94F4-FF5A26C51A38) (Shutdown)' \
      '-- iOS 27.2 --' \
      '    iPad (AE3792D8-0370-4B69-94F4-FF5A26C51A38) (Booted)' \
      '    iPhone (14C02540-D279-474E-8D33-06363B85A5B6) (Shutdown)'
    ;;
  *) exit 1 ;;
esac
)";
  }
  std::filesystem::permissions(xcrun, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  struct RestorePath {
    std::optional<std::string> value = ReadEnvironmentVariable("PATH");
    ~RestorePath() {
      if (value) {
        setenv("PATH", value->c_str(), 1);
      } else {
        unsetenv("PATH");
      }
    }
  } previous_path;
  SetProcessEnvironmentVariable("PATH", temporary.Path().string() + ":" + previous_path.value.value_or(""));

  const PlatformDriver* ios = FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  REQUIRE(ios->DiscoverDevices({}) ==
          std::vector<PlatformDevice>{
              {"AC622B12-7BDD-5F92-9091-A0673B827C3A", "Paired iPhone", DeviceState::Ready, DeviceKind::Physical,
               "00008130-000C048A1A90001C"},
              {"AE3792D8-0370-4B69-94F4-FF5A26C51A38", "iPad — iOS 27.2", DeviceState::Ready,
               DeviceKind::Simulator, "AE3792D8-0370-4B69-94F4-FF5A26C51A38"},
              {"14C02540-D279-474E-8D33-06363B85A5B6", "iPhone — iOS 27.2", DeviceState::Offline,
               DeviceKind::Simulator, "14C02540-D279-474E-8D33-06363B85A5B6"},
          });
}
#endif
