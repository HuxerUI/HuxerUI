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
  REQUIRE(invocation.output.find("--source <path>") != std::string::npos);
  REQUIRE(invocation.output.find("--java-home <path>") != std::string::npos);
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
}

TEST_CASE("HuxerUICliRejectsInvalidSourceBuildOptions") {
  TemporaryDirectory temporary;
  const Invocation missing = Invoke(temporary.Path(), {"build", "windows", "--source"});
  REQUIRE(missing.result == 2);
  REQUIRE(missing.error.find("--source requires a value") != std::string::npos);

  const Invocation duplicate =
      Invoke(temporary.Path(), {"build", "windows", "--source", ".", "--source", "."});
  REQUIRE(duplicate.result == 2);
  REQUIRE(duplicate.error.find("--source may be specified only once") != std::string::npos);

  const Invocation invalid = Invoke(temporary.Path(), {"build", "windows", "--source", "missing"});
  REQUIRE(invalid.result == 1);
  REQUIRE(invalid.error.find("HuxerUI source checkout is invalid:") != std::string::npos);
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
  const std::vector<std::string_view> arguments{"build", "windows", "--java-home", "jdk", "--source", HUXERUI_TEST_SOURCE_DIRECTORY};
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
  const std::vector<std::string_view> arguments{"run", "windows", "--source", HUXERUI_TEST_SOURCE_DIRECTORY};
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
