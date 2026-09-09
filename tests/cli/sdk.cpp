#include "support.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliBuildSelectionRequiresAnExplicitSourceOverride") {
  using namespace huxerui::cli;
  TemporaryDirectory temporary;
  const auto source = std::filesystem::path(HUXERUI_TEST_SOURCE_DIRECTORY);
  REQUIRE_THROWS_WITH(ResolveBuildHome(source, std::nullopt, temporary.Path()),
                      Catch::Matchers::ContainsSubstring("--source"));
  REQUIRE_THROWS_AS(ResolveBuildHome({}, std::nullopt, temporary.Path()), std::runtime_error);
  const auto selected = ResolveBuildHome(temporary.Path() / "missing-sdk", source, temporary.Path());
  REQUIRE(std::filesystem::equivalent(selected, source));
  REQUIRE(BuildHomeKey(selected) == BuildHomeKey(source / "."));
  REQUIRE(BuildHomeKey(selected) != BuildHomeKey(temporary.Path()));
}

TEST_CASE("HuxerUICliValidatesExplicitSourceCheckouts") {
  const std::filesystem::path source = huxerui::cli::ResolveHuxerUISource(HUXERUI_TEST_SOURCE_DIRECTORY);
  REQUIRE(std::filesystem::equivalent(source, HUXERUI_TEST_SOURCE_DIRECTORY));

  TemporaryDirectory temporary;
  REQUIRE_THROWS_AS(huxerui::cli::ResolveHuxerUISource(temporary.Path()), std::runtime_error);
}

TEST_CASE("HuxerUICliValidatesUpdateOptionsBeforeLocatingSdk") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"--help"}).output.find("huxerui update") != std::string::npos);
  for (const auto arguments : {std::initializer_list<std::string_view>{"update", "--version"},
                               {"update", "--version", ""}, {"update", "--version", "--check"},
                               {"update", "--check", "--check"}, {"update", "--yes", "--yes"},
                               {"update", "--prefix", "elsewhere"}, {"update", "--unknown"}}) {
    REQUIRE(Invoke(temporary.Path(), arguments).result == 2);
  }
  const auto missing = Invoke(temporary.Path(), {"update", "--check"});
  REQUIRE(missing.result == 1);
  REQUIRE(missing.error.find("requires an installed SDK") != std::string::npos);
}

TEST_CASE("HuxerUICliUpdateRequiresTheSelectedInstallationsExecutable") {
  using namespace huxerui::cli;
  TemporaryDirectory temporary;
  const auto root = temporary.Path();
  for (const auto path : {"bin", "include/huxerui", "lib/cmake/HuxerUI", "share/huxerui/tools",
                          "share/huxerui/resources/huxerui", "share/huxerui/skills/huxerui-app-development"}) {
    std::filesystem::create_directories(root / path);
  }
#if defined(_WIN32)
  const auto executable = root / "bin/huxerui.exe";
#else
  const auto executable = root / "bin/huxerui";
#endif
  for (const auto& path : {executable, root / "include/huxerui/huxerui.h",
                          root / "lib/cmake/HuxerUI/HuxerUIConfig.cmake",
                          root / "share/huxerui/resources/huxerui/resources.bin"}) {
    std::ofstream(path) << "fixture";
  }
  const SdkLocation sdk{root, SdkLocationSource::Environment};
  REQUIRE_NOTHROW(ValidateSdkUpdate(sdk, executable));
  REQUIRE_THROWS_WITH(ValidateSdkUpdate(sdk, ExecutablePath({})),
                      Catch::Matchers::ContainsSubstring("different installations"));
  REQUIRE_THROWS_WITH(ValidateSdkUpdate({HUXERUI_TEST_SOURCE_DIRECTORY, SdkLocationSource::Environment}, executable),
                      Catch::Matchers::ContainsSubstring("source workflow"));
}

TEST_CASE("HuxerUICliDoctorAcceptsAReorganizedSourceDirectory") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::rename(project / "src/app.cpp", project / "src/application.cpp");

  const Invocation invocation = Invoke(project, {"doctor", "windows"});

  REQUIRE(invocation.output.find("missing src") == std::string::npos);
}

TEST_CASE("HuxerUICliDoctorReportsTheResolvedSdk") {
  TemporaryDirectory temporary;
  for (const auto directory : {"include/huxerui", "lib/cmake/HuxerUI", "share/huxerui/tools",
                               "share/huxerui/resources/huxerui", "share/huxerui/skills/huxerui-app-development"}) {
    std::filesystem::create_directories(temporary.Path() / directory);
  }
  for (const auto file : {"include/huxerui/huxerui.h", "lib/cmake/HuxerUI/HuxerUIConfig.cmake",
                          "share/huxerui/resources/huxerui/resources.bin"}) {
    std::ofstream(temporary.Path() / file) << "fixture";
  }
  const std::optional<std::filesystem::path> cmake = huxerui::cli::FindExecutable("cmake");
  REQUIRE(cmake);
  const std::vector<std::string_view> arguments{"doctor"};
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;

  const int result = huxerui::cli::Run(
      arguments,
      temporary.Path(),
      {temporary.Path(), huxerui::cli::SdkLocationSource::Environment},
      input,
      output,
      error
  );

  REQUIRE(result == 0);
  REQUIRE(output.str().find("[ok] HUXERUI_HOME (environment): " + temporary.Path().string()) != std::string::npos);
  REQUIRE(output.str().find("[ok] cmake: " + cmake->string()) != std::string::npos);
  REQUIRE(error.str().empty());
}
