#include "support.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliLinuxEnvironmentDiagnosisOwnsToolChecks") {
  if (huxerui::cli::CurrentHostId() != "linux") {
    SKIP("Linux prerequisite diagnosis requires a Linux host");
  }
  const huxerui::cli::PlatformDriver* linux = huxerui::cli::FindPlatformDriver("linux");
  REQUIRE(linux != nullptr);
  const std::vector<huxerui::cli::EnvironmentDiagnostic> linux_diagnostics = linux->DiagnoseEnvironment();
  const auto has_linux_diagnostic = [&linux_diagnostics](std::string_view id) {
    return std::any_of(
        linux_diagnostics.begin(),
        linux_diagnostics.end(),
        [id](const huxerui::cli::EnvironmentDiagnostic& diagnostic) { return diagnostic.id == id; }
    );
  };
  REQUIRE(has_linux_diagnostic("pkg-config"));
  REQUIRE(has_linux_diagnostic("pkg:gtk4"));
  REQUIRE(has_linux_diagnostic("pkg:epoxy"));
  REQUIRE(has_linux_diagnostic("pkg:gio-2.0"));
  REQUIRE(has_linux_diagnostic("pkg:libsoup-3.0"));
  const auto gtk_diagnostic = std::find_if(
      linux_diagnostics.begin(),
      linux_diagnostics.end(),
      [](const huxerui::cli::EnvironmentDiagnostic& diagnostic) { return diagnostic.id == "pkg:gtk4"; }
  );
  REQUIRE(gtk_diagnostic != linux_diagnostics.end());
  REQUIRE(gtk_diagnostic->label.find("gtk4 >= 4.14") != std::string::npos);
  REQUIRE_FALSE(has_linux_diagnostic("pkg:x11"));
  REQUIRE_FALSE(has_linux_diagnostic("pkg:egl"));
  REQUIRE_FALSE(has_linux_diagnostic("pkg:glesv2"));
  REQUIRE_FALSE(has_linux_diagnostic("meson"));
  REQUIRE_FALSE(has_linux_diagnostic("ninja"));
  REQUIRE_FALSE(has_linux_diagnostic("gperf"));
  REQUIRE_FALSE(has_linux_diagnostic("git"));
}

TEST_CASE("HuxerUICliCreatesAndRunsLinuxApplicationsThroughTheRootCMakeProject") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* linux = huxerui::cli::FindPlatformDriver("linux");
  REQUIRE(linux != nullptr);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path build = project / ".huxerui/build/linux/debug";
  const huxerui::cli::PlatformCommandContext context{
      project,
      temporary.Path() / "sdk",
      build,
      "Ninja",
      "debug",
      {},
  };

  const std::vector<huxerui::cli::GeneratedFile> shell =
      linux->CreateShell(huxerui::cli::MakeProjectTemplateContext("sample"));
  const auto main = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "main.cpp";
  });
  REQUIRE(main != shell.end());
  REQUIRE(main->content.find("RunApplication()") != std::string::npos);
  REQUIRE(std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
            return file.path == "huxerui.cmake";
          }) != shell.end());
  REQUIRE(std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
            return file.path == "package/AppRun";
          }) != shell.end());
  REQUIRE(linux->LibraryGraphCommands(context).empty());

  const std::vector<huxerui::cli::ProcessCommand> build_commands = linux->BuildCommands(context);
  REQUIRE(build_commands.size() == 2);
  REQUIRE(
      build_commands[0].arguments ==
      std::vector<std::string>{
          "-G",
          "Ninja",
          "-S",
          project.string(),
          "-B",
          build.string(),
          "-DCMAKE_BUILD_TYPE=Debug",
          "-DHUXERUI_HOME=" + context.huxerui_home.string(),
          "-DHUXERUI_PACKAGE=OFF",
      }
  );
  REQUIRE(
      build_commands[1].arguments ==
      std::vector<std::string>{"--build", build.string(), "--config", "Debug", "--parallel"}
  );

  const std::filesystem::path artifact = build / "bin/sample";
  const std::filesystem::path plan = build / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(artifact.parent_path());
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(artifact) << "test\n";
  std::ofstream(plan) << "{\n"
                         "  \"target\": \"sample\",\n"
                         "  \"version\": \"1.2.3\",\n"
                         "  \"installComponent\": \"HuxerUIApplication\",\n"
                         "  \"artifact\": \""
                      << artifact.generic_string() << "\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> run_commands = linux->RunCommands(context);
  REQUIRE(run_commands.size() == 1);
  REQUIRE(std::filesystem::equivalent(run_commands[0].executable, artifact));
  REQUIRE(run_commands[0].arguments.empty());
  REQUIRE(std::filesystem::equivalent(run_commands[0].working_directory, artifact.parent_path()));

  const std::vector<huxerui::cli::PackageArtifact> package_artifacts = linux->PackageArtifacts(context);
  const std::filesystem::path app_image = project / ".huxerui/package/linux/debug/sample-1.2.3.AppImage";
  const std::vector<huxerui::cli::PackageArtifact> expected_package_artifacts{{app_image, app_image.filename()}};
  REQUIRE(package_artifacts == expected_package_artifacts);
}
