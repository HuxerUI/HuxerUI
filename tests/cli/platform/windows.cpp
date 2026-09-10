#include "support.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliCreatesWindowsPlatformShell") {
  TemporaryDirectory temporary;
  const auto invocation = Invoke(temporary.Path(),
      {"create", "app", "Sample-App", "--id", "dev.example.sampleapp", "--platform", "windows"});
  REQUIRE(invocation.result == 0);
  const auto project = temporary.Path() / "Sample-App";
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/app.manifest"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/app.ico"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/app.rc.in"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/huxerui.cmake"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/package/Bundle.wxs.in"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/package/Package.wxs.in"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/package/src/app.cpp"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/package/src/main.cpp"));
  const std::filesystem::path installer_strings = project / "platform/windows/package/resources/strings";
  for (const char* catalog : {"default.properties", "zh.properties", "zh-TW.properties", "zh-HK.properties",
                              "ja.properties", "ko.properties", "de.properties", "fr.properties",
                              "es.properties", "pt-BR.properties"}) {
    REQUIRE(std::filesystem::is_regular_file(installer_strings / catalog));
  }
  const std::string installer_app = Read(project / "platform/windows/package/src/app.cpp");
  REQUIRE(installer_app.find("status.default_destination") != std::string::npos);
  REQUIRE(installer_app.find("status.default_create_desktop_shortcut") != std::string::npos);
  REQUIRE(installer_app.find("UseTaskScope()") != std::string::npos);
  REQUIRE(installer_app.find("co_await installer.ChooseDestinationAsync") != std::string::npos);
  REQUIRE(installer_app.find("installer.Install({") != std::string::npos);
  REQUIRE(installer_app.find("#include <installer_resources.h>") != std::string::npos);
  REQUIRE(installer_app.find("installer_strings::create_desktop_shortcut") != std::string::npos);
  REQUIRE(installer_app.find("installer_strings::application_setup") != std::string::npos);
  REQUIRE(installer_app.find("installer_strings::ready_to_install") != std::string::npos);
  REQUIRE(installer_app.find("installer_strings::files_in_use") != std::string::npos);
  REQUIRE(installer_app.find("InstallerPromptChoice::TryAgain") != std::string::npos);
  REQUIRE(installer_app.find("\"Ready to install\"") == std::string::npos);
  REQUIRE(installer_app.find("TextFieldVariant::Outlined") != std::string::npos);
  const std::string default_installer_strings = Read(installer_strings / "default.properties");
  REQUIRE(default_installer_strings.find("ready_to_install = Ready to install") != std::string::npos);
  REQUIRE(default_installer_strings.find("application_setup = APPLICATION SETUP") != std::string::npos);
  REQUIRE(default_installer_strings.find("files_in_use = Close applications") != std::string::npos);
  const std::string installer_bundle = Read(project / "platform/windows/package/Bundle.wxs.in");
  REQUIRE(installer_bundle.find("IconSourceFile=\"!(bindpath.Project)\\app.ico\"") != std::string::npos);
  REQUIRE(installer_bundle.find("Name=\"InstallFolder\" Type=\"formatted\"") != std::string::npos);
  REQUIRE(installer_bundle.find("Name=\"CreateDesktopShortcut\" Type=\"numeric\"") != std::string::npos);
  const std::string installer_package = Read(project / "platform/windows/package/Package.wxs.in");
  REQUIRE(installer_package.find("Icon Id=\"ApplicationIcon\"") != std::string::npos);
  REQUIRE(installer_package.find("Property Id=\"ARPPRODUCTICON\" Value=\"ApplicationIcon\"") !=
          std::string::npos);
  REQUIRE(installer_package.find("Icon=\"ApplicationIcon\"") != std::string::npos);
  REQUIRE(installer_package.find("Feature Id=\"DesktopShortcut\"") != std::string::npos);
  REQUIRE(installer_package.find("StandardDirectory Id=\"DesktopFolder\"") != std::string::npos);
  REQUIRE(Read(project / "platform/windows/main.cpp").find("RunApplication()") != std::string::npos);
  REQUIRE(Read(project / "platform/windows/app.ico").starts_with(std::string("\0\0\1\0", 4)));
}

TEST_CASE("HuxerUICliCreatesStableWindowsBuildCommands") {
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
          "-DHUXERUI_HOME=" + context.huxerui_home.string(),
          "-DHUXERUI_PACKAGE=OFF",
          "-DCMAKE_CXX_COMPILER=cl",
      }
  );
  REQUIRE(
      commands[1].arguments ==
      std::vector<std::string>{
          "--build",
          context.build_directory.string(),
          "--config",
          "Release",
      }
  );

  std::filesystem::create_directories(context.huxerui_home / "cmake");
  std::ofstream(context.huxerui_home / "cmake/HuxerUIGenerateWixPayloads.cmake") << "# payloads\n";
  huxerui::cli::PlatformCommandContext package_context = context;
  package_context.package = true;
  const std::vector<huxerui::cli::ProcessCommand> package_build_commands = windows->BuildCommands(package_context);
  const std::filesystem::path wix_root = context.project_root / ".huxerui/package/windows/dependencies/wix";
  REQUIRE(package_build_commands.size() == 2);
  REQUIRE(
      package_build_commands[0].arguments ==
      std::vector<std::string>{
          "-G", "Ninja", "-S", context.project_root.string(), "-B", context.build_directory.string(),
          "-DCMAKE_BUILD_TYPE=Release", "-DHUXERUI_HOME=" + context.huxerui_home.string(),
          "-DHUXERUI_PACKAGE=ON", "-DCMAKE_CXX_COMPILER=cl", "-DHUXERUI_WIX_ROOT=" + wix_root.string(),
      }
  );

  const std::filesystem::path package_plan = context.build_directory / "huxerui-package/windows/Release/package.json";
  const std::filesystem::path installer_plan = temporary.Path() / "installer.json";
  const std::filesystem::path installer = "sample-installer.exe";
  const std::filesystem::path package_source = temporary.Path() / "Package.wxs";
  const std::filesystem::path bundle_source = temporary.Path() / "Bundle.wxs";
  const std::filesystem::path wix = temporary.Path() / "wix.exe";
  std::filesystem::create_directories(package_plan.parent_path());
  std::ofstream(package_plan) << "{\n"
                                 "  \"target\": \"sample\",\n"
                                 "  \"version\": \"1.2.3\",\n"
                                 "  \"installComponent\": \"HuxerUIApplication\",\n"
                                 "  \"packageSource\": \""
                              << package_source.generic_string() << "\",\n"
                              << "  \"bundleSource\": \"" << bundle_source.generic_string() << "\",\n"
                              << "  \"installerPlan\": \"" << installer_plan.generic_string() << "\"\n"
                              << "}\n";
  std::ofstream(installer_plan) << "{\n"
                                   "  \"wix\": \""
                                << wix.generic_string() << "\",\n"
                                << "  \"installer\": \"" << installer.generic_string() << "\",\n"
                                << "  \"installComponent\": \"HuxerUIInstaller_sample\"\n"
                                << "}\n";

  const std::vector<huxerui::cli::ProcessCommand> package_commands = windows->PackageCommands(package_context);
  REQUIRE(package_commands.size() == 9);
  REQUIRE(package_commands[5].arguments[5] == "HuxerUIInstaller_sample");
  REQUIRE(std::filesystem::path(package_commands[6].executable).generic_string() == wix.generic_string());
  REQUIRE(package_commands[7].executable == "cmake");
  REQUIRE(std::filesystem::path(package_commands[8].executable).generic_string() == wix.generic_string());
  const std::filesystem::path installer_payloads =
      context.project_root / ".huxerui/package/windows/release/installer-payloads.wxs";
  const std::string project_bind_path = "Project=" + (context.project_root / "platform/windows").string();
  REQUIRE(std::find(package_commands[6].arguments.begin(), package_commands[6].arguments.end(), project_bind_path) !=
          package_commands[6].arguments.end());
  REQUIRE(std::find(package_commands[8].arguments.begin(), package_commands[8].arguments.end(), project_bind_path) !=
          package_commands[8].arguments.end());
  REQUIRE(std::any_of(package_commands[8].arguments.begin(), package_commands[8].arguments.end(),
                      [&installer_payloads](const std::string& argument) {
                        return std::filesystem::path(argument).generic_string() == installer_payloads.generic_string();
                      }));
  const std::vector<huxerui::cli::PackageArtifact> package_artifacts = windows->PackageArtifacts(package_context);
  REQUIRE(package_artifacts == std::vector<huxerui::cli::PackageArtifact>{{
                                   context.project_root / ".huxerui/package/windows/release/sample-Setup-1.2.3.exe",
                                   "sample-Setup-1.2.3.exe",
                               }});
}

TEST_CASE("HuxerUICliRejectsDeviceSelectionForDesktopRunsBeforeBuilding") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::vector<std::string_view> arguments{"run", "windows", "--device", "phone", "--source", HUXERUI_TEST_SOURCE_DIRECTORY};
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;

  const int result = huxerui::cli::Run(
      arguments,
      project,
      {temporary.Path(), huxerui::cli::SdkLocationSource::Executable},
      input,
      output,
      error
  );

  REQUIRE(result == 2);
  REQUIRE(error.str().find("--device is not supported for platform windows") != std::string::npos);
  REQUIRE(output.str().empty());
}

#if defined(_WIN32)
TEST_CASE("HuxerUICliRunsWindowsBatchTools") {
  TemporaryDirectory temporary;
  const std::filesystem::path batch = temporary.Path() / "return seven.cmd";
  std::ofstream(batch) << "@exit /b 7\n";

  REQUIRE(huxerui::cli::RunProcess({batch.string(), {}, temporary.Path()}) == 7);
}
#endif
