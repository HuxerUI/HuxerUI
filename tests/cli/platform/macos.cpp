#include "support.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliCreatesMacOSPackageCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* macos = huxerui::cli::FindPlatformDriver("macos");
  REQUIRE(macos != nullptr);
  huxerui::cli::PlatformCommandContext context{
      temporary.Path() / "sample",
      temporary.Path() / "sdk",
      temporary.Path() / "sample/.huxerui/build/macos/release",
      "Ninja",
      "release",
      {},
      true,
  };
  const std::filesystem::path plan = context.build_directory / "huxerui-integration/sample/Release/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(plan) << "{\n"
                         "  \"target\": \"sample\",\n"
                         "  \"name\": \"Sample\",\n"
                         "  \"version\": \"1.2.3\",\n"
                         "  \"installComponent\": \"HuxerUIApplication\"\n"
                         "}\n";

  const std::vector<huxerui::cli::ProcessCommand> build_commands = macos->BuildCommands(context);
  REQUIRE(std::find(build_commands[0].arguments.begin(), build_commands[0].arguments.end(),
                    "-DHUXERUI_PACKAGE=ON") != build_commands[0].arguments.end());

  const std::vector<huxerui::cli::ProcessCommand> package_commands = macos->PackageCommands(context);
  const std::filesystem::path root = context.project_root / ".huxerui/package/macos/release";
  REQUIRE(package_commands.size() == 5);
  REQUIRE(package_commands[2].arguments.size() == 8);
  REQUIRE(package_commands[2].arguments[0] == "--install");
  REQUIRE(std::filesystem::path(package_commands[2].arguments[1]).generic_string() ==
          context.build_directory.generic_string());
  REQUIRE(package_commands[2].arguments[2] == "--config");
  REQUIRE(package_commands[2].arguments[3] == "Release");
  REQUIRE(package_commands[2].arguments[4] == "--component");
  REQUIRE(package_commands[2].arguments[5] == "HuxerUIApplication");
  REQUIRE(package_commands[2].arguments[6] == "--prefix");
  REQUIRE(std::filesystem::path(package_commands[2].arguments[7]).generic_string() ==
          (root / "staging").generic_string());
  REQUIRE(package_commands[4].executable == "hdiutil");
  REQUIRE(std::filesystem::path(package_commands[4].arguments.back()).generic_string() ==
          (root / "sample-1.2.3.dmg").generic_string());

  const std::vector<huxerui::cli::PackageArtifact> package_artifacts = macos->PackageArtifacts(context);
  REQUIRE(package_artifacts.size() == 1);
  REQUIRE(package_artifacts.front().source.generic_string() == (root / "sample-1.2.3.dmg").generic_string());
  REQUIRE(package_artifacts.front().destination == "sample-1.2.3.dmg");
}
