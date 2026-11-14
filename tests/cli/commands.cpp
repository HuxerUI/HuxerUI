#include "support.h"

#include "mcpp/mcpp.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliHelpListsSupportedAgents") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"--help"});

  REQUIRE(invocation.result == 0);
  for (const std::string_view agent : {
           std::string_view{"codex"},
           std::string_view{"claude"},
           std::string_view{"antigravity"},
           std::string_view{"opencode"},
           std::string_view{"command-code"},
           std::string_view{"omp"},
           std::string_view{"dsh"},
           std::string_view{"zcode"},
       }) {
    REQUIRE(invocation.output.find(agent) != std::string::npos);
  }
  REQUIRE(invocation.output.find("all") != std::string::npos);
  REQUIRE(invocation.output.find("none") != std::string::npos);
  REQUIRE(invocation.output.find("--namespace <cpp-namespace>") != std::string::npos);
  REQUIRE(invocation.output.find("--target <public-cmake-target>") != std::string::npos);
  REQUIRE(invocation.output.find("--sdk <path>") != std::string::npos);
  REQUIRE(invocation.output.find("--project <dir>") != std::string::npos);
  REQUIRE(invocation.output.find("-d|--device <id>") != std::string::npos);
  REQUIRE(invocation.output.find("--java-home <path>") != std::string::npos);
  REQUIRE(invocation.output.find("--abi <abi-list>") != std::string::npos);
  REQUIRE(invocation.output.find("a common library's Preview enable all platforms") != std::string::npos);
  REQUIRE(invocation.output.find("huxerui mcpp build") != std::string::npos);
}

TEST_CASE("HuxerUICliCreatesMcppBuildCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::mcpp::BuildOptions options{
      temporary.Path() / "mcpp-project",
      true,
      true,
      true,
      true,
  };
  const std::vector<huxerui::cli::ProcessCommand> commands = huxerui::cli::mcpp::BuildCommands(options);

  REQUIRE(commands.size() == 1);
  REQUIRE(commands[0].executable == "mcpp");
  REQUIRE(
      commands[0].arguments == std::vector<std::string>{"build", "--release", "--locked", "--offline", "--verbose"}
  );
  REQUIRE(commands[0].working_directory == options.project_root);
}

TEST_CASE("HuxerUICliMcppRequiresAManifest") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"mcpp", "build"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.error.find("mcpp.toml") != std::string::npos);

  std::filesystem::create_directory(temporary.Path() / "other project");
  const auto selected = Invoke(temporary.Path(), {"mcpp", "build", "--project", "other project"});
  REQUIRE(selected.result == 1);
  REQUIRE(selected.error.find((temporary.Path() / "other project").string()) != std::string::npos);
  const auto missing = Invoke(temporary.Path(), {"mcpp", "build", "--project", "--release"});
  REQUIRE(missing.result == 2);
  REQUIRE(missing.error.find("--project requires a directory") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsInvalidSdkAndDeviceOptions") {
  TemporaryDirectory temporary;
  for (const auto option : {"--sdk", "-d", "--generator", "--profile", "--abi", "--java-home"}) {
    const Invocation missing = Invoke(temporary.Path(), {"build", "windows", option, "--help"});
    REQUIRE(missing.result == 2);
    REQUIRE(missing.error.find(std::string(option) + " requires a value") != std::string::npos);
  }

  const Invocation duplicate = Invoke(temporary.Path(), {"build", "windows", "--sdk", ".", "--sdk", "."});
  REQUIRE(duplicate.result == 2);
  REQUIRE(duplicate.error.find("--sdk may be specified only once") != std::string::npos);

  const Invocation duplicate_device = Invoke(temporary.Path(), {"r", "ios", "-d", "one", "--device", "two"});
  REQUIRE(duplicate_device.result == 2);
  REQUIRE(duplicate_device.error.find("--device may be specified only once") != std::string::npos);

  const Invocation invalid = Invoke(temporary.Path(), {"build", "windows", "--sdk", "missing"});
  REQUIRE(invalid.result == 1);
  REQUIRE(invalid.error.find("HuxerUI SDK or source checkout is invalid:") != std::string::npos);

  REQUIRE(Invoke(temporary.Path(), {"build", "--source", "."}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"mcpp", "build", "--source", "."}).result == 2);
}

TEST_CASE("HuxerUICliShortCommandsShareDispatchAndHelp") {
  TemporaryDirectory temporary;
  const auto general_help = Invoke(temporary.Path(), {"--help"});
  for (const auto& [alias, command] : std::array{
           std::pair{"b", "build"}, std::pair{"r", "run"}, std::pair{"pkg", "package"},
       }) {
    const auto help = Invoke(temporary.Path(), {command, "--help"});
    REQUIRE(help.result == 0);
    REQUIRE(help.output.find(std::string("huxerui ") + command + " ") != std::string::npos);
    REQUIRE(Invoke(temporary.Path(), {alias, "-h"}).output == help.output);
    REQUIRE(general_help.output.find(std::string(alias) + "  " + command) != std::string::npos);

    const auto full = Invoke(temporary.Path(), {command, "windows", "--sdk", "missing"});
    const auto short_form = Invoke(temporary.Path(), {alias, "windows", "--sdk", "missing"});
    REQUIRE(short_form.result == full.result);
    REQUIRE(short_form.error == full.error);
  }
  REQUIRE(Invoke(temporary.Path(), {"open", "--help"}).output.find("open ios [--sdk <path>]") != std::string::npos);
}

TEST_CASE("HuxerUICliDoctorReportsIncompleteAndUnknownPlatforms") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::remove(project / "platform/windows/app.manifest");
  std::filesystem::create_directories(project / "platform/custom");

  const Invocation invocation = Invoke(project, {"doctor", "all"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(invocation.output.find("missing app.manifest") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android") == std::string::npos);
}

TEST_CASE("HuxerUICliRejectsInvalidOptimizationOverrides") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--optimization"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--optimization", "fast"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--lto"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--lto", "auto"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--lto", "off", "--lto", "on"}).result == 2);
}

TEST_CASE("HuxerUICliRestrictsPackageFormatToLinuxPackaging") {
  TemporaryDirectory temporary;
  const auto help = Invoke(temporary.Path(), {"package", "--help"});
  REQUIRE(help.output.find("[-f|--format appimage|deb|rpm]") != std::string::npos);
  REQUIRE(Invoke(temporary.Path(), {"package", "linux", "-f"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"package", "linux", "--format", "zip"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"pkg", "linux", "-f", "deb", "--format", "rpm"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "linux", "--format", "deb"}).result == 2);

  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample"}).result == 0);
  const auto project = temporary.Path() / "sample";
  for (const auto platforms : {"windows", "linux,windows"}) {
    const auto rejected =
        Invoke(project, {"package", platforms, "--format", "rpm", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
    REQUIRE(rejected.result == 2);
    REQUIRE(rejected.error.find("--format requires Linux as the sole packaging platform") != std::string::npos);
  }
}

TEST_CASE("HuxerUICliNormalizesAbiNamesForTheSelectedPlatform") {
  using huxerui::cli::FindPlatformDriver;
  using huxerui::cli::detail::NormalizeTargetAbi;

  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("windows"), "x64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("windows"), "x86_64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("windows"), "arm64").empty());
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("android"), "arm64") == "arm64-v8a");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("android"), "arm64-v8a") == "arm64-v8a");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("android"), "x64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("linux"), "arm64") == "aarch64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("macos"), "aarch64") == "arm64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("ios"), "x64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("web"), "wasm32") == "wasm32");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("web"), "wasm64").empty());
}

TEST_CASE("HuxerUICliRejectsInvalidAbiSelectionsBeforeBuilding") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--abi"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--abi", "arm64,"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--abi", "arm64", "--abi", "x64"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"open", "ios", "--abi", "arm64"}).result == 2);

  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample"}).result == 0);
  const auto project = temporary.Path() / "sample";
  const auto unsupported =
      Invoke(project, {"build", "web", "--abi", "wasm64", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
  REQUIRE(unsupported.result == 2);
  REQUIRE(unsupported.error.find("unsupported ABI for web: wasm64") != std::string::npos);
  REQUIRE(Invoke(project, {"build", "android,web", "--abi", "arm64", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY})
              .result == 2);
  REQUIRE(Invoke(project, {"run", "linux", "--abi", "x64,arm64", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY})
              .result == 2);
}

TEST_CASE("HuxerUICliRejectsUnknownPlatformsAsUsageErrors") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "plan9"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("unknown platform: plan9") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "sample"));
}

TEST_CASE("HuxerUICliRejectsInvalidAgentLists") {
  TemporaryDirectory temporary;
  const Invocation unknown =
      Invoke(temporary.Path(), {"create", "app", "unknown", "--platform", "windows", "--agent", "other"});
  REQUIRE(unknown.result == 2);
  REQUIRE(unknown.error.find("unknown agent: other") != std::string::npos);

  const Invocation combined = Invoke(
      temporary.Path(),
      {"create", "app", "combined", "--platform", "windows", "--agent", "all,claude"}
  );
  REQUIRE(combined.result == 2);
  REQUIRE(combined.error.find("all cannot be combined with another agent") != std::string::npos);

  const Invocation disabled = Invoke(
      temporary.Path(),
      {"create", "app", "disabled", "--platform", "windows", "--agent", "none,codex"}
  );
  REQUIRE(disabled.result == 2);
  REQUIRE(disabled.error.find("none cannot be combined with another agent") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsDeviceDiscoveryForDesktopPlatforms") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"devices", "windows"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.error.find("platform does not support device discovery: windows") != std::string::npos);
}

TEST_CASE("HuxerUICliRequiresAValueForJavaHomeOverrides") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"build", "android", "--java-home"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("--java-home requires a value") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsJavaHomeOverridesForNonAndroidBuilds") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::vector<std::string_view> arguments{
      "build", "windows", "--java-home", "jdk", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY
  };
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;

  const int result = huxerui::cli::Run(
      arguments,
      project,
      {HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable},
      input,
      output,
      error
  );

  REQUIRE(result == 2);
  REQUIRE(error.str().find("--java-home is supported only for Android builds") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliRejectsUnknownProjectPlatformsBeforeRun") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::create_directories(project / "platform/custom");
  const std::vector<std::string_view> arguments{"run", "windows", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY};
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

  REQUIRE(result == 1);
  REQUIRE(error.str().find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliSetupRequiresAnExplicitPlatformList") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"setup"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("setup requires an explicit platform list") != std::string::npos);
}

TEST_CASE("HuxerUICliSetupUsesSharedDiagnosisAndCanBeCancelled") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"setup", "android"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("Common environment:") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android:") != std::string::npos);
  REQUIRE(invocation.output.find("Setup plan:") != std::string::npos);
  REQUIRE(invocation.output.find("Setup cancelled.") != std::string::npos);
  REQUIRE(invocation.error.empty());
}

TEST_CASE("HuxerUICliPlatformEnvironmentDiagnosisOwnsHostAndToolChecks") {
  const std::string_view unavailable_id = huxerui::cli::CurrentHostId() == "windows" ? "ios" : "windows";
  const huxerui::cli::PlatformDriver* unavailable = huxerui::cli::FindPlatformDriver(unavailable_id);
  REQUIRE(unavailable != nullptr);
  const std::vector<huxerui::cli::EnvironmentDiagnostic> unavailable_diagnostics = unavailable->DiagnoseEnvironment();
  REQUIRE(unavailable_diagnostics.size() == 1);
  REQUIRE(unavailable_diagnostics.front().status == huxerui::cli::EnvironmentDiagnosticStatus::Unavailable);
  REQUIRE(unavailable_diagnostics.front().id == "host");
}

TEST_CASE("HuxerUICliDoctorChecksRequestedPlatformsOutsideAProject") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"doctor", "android"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("Project: not found") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android:") != std::string::npos);
  REQUIRE(invocation.output.find("  [ok] cmake") == std::string::npos);
}
