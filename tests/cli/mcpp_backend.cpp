#include "support.h"

#include "mcpp_backend.h"

#include <fstream>

using namespace huxerui::cli::test;
using huxerui::cli::McppBuildCommand;
using huxerui::cli::McppPackCommand;
using huxerui::cli::McppPlatformsLine;
using huxerui::cli::McppProjectPlatforms;
using huxerui::cli::McppRunCommand;
using huxerui::cli::ResolveMcppTarget;

namespace {

void Write(const std::filesystem::path& path, std::string_view content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
}

} // namespace

TEST_CASE("HuxerUICliMapsPlatformsToMcppTargets") {
  // The desktop rows follow the host; the mobile rows default to what mcpp can
  // run without a signature and switch to the device row for hardware.
  REQUIRE(ResolveMcppTarget("linux", false, "x86_64").triple == "x86_64-linux-gnu");
  REQUIRE(ResolveMcppTarget("linux", false, "x86_64").format == "appimage");
  REQUIRE(ResolveMcppTarget("windows", false, "x86_64").triple == "x86_64-windows-msvc");
  REQUIRE(ResolveMcppTarget("windows", false, "x86_64").format == "msi");
  REQUIRE(ResolveMcppTarget("macos", false, "aarch64").triple == "aarch64-macos");
  REQUIRE(ResolveMcppTarget("macos", false, "aarch64").format == "app");
  REQUIRE(ResolveMcppTarget("web", false, "x86_64").triple == "wasm32-emscripten");
  REQUIRE(ResolveMcppTarget("web", false, "x86_64").format == "web");
  REQUIRE_FALSE(ResolveMcppTarget("web", false, "x86_64").runs_packaged);
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").triple == "x86_64-linux-android");
  REQUIRE(ResolveMcppTarget("android", true, "x86_64").triple == "aarch64-linux-android");
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").format == "apk");
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").runs_packaged);
  REQUIRE(ResolveMcppTarget("ios", false, "aarch64").triple == "aarch64-ios-sim");
  REQUIRE(ResolveMcppTarget("ios", true, "aarch64").triple == "aarch64-ios");
  REQUIRE(ResolveMcppTarget("ios", false, "aarch64").runs_packaged);
  REQUIRE(ResolveMcppTarget("plan9", false, "x86_64").triple.empty());
}

TEST_CASE("HuxerUICliBuildsMcppCommandsFromATarget") {
  const auto android = ResolveMcppTarget("android", false, "x86_64");
  const auto build = McppBuildCommand("/p", android, false);
  REQUIRE(build.executable == "mcpp");
  REQUIRE(build.arguments == std::vector<std::string>{"build", "--target", "x86_64-linux-android"});
  REQUIRE(build.working_directory == std::filesystem::path("/p"));
  REQUIRE(McppBuildCommand("/p", android, true).arguments.back() == "--release");
  // An APK is what the runner installs, so the run packages first.
  REQUIRE(McppRunCommand("/p", android, false).arguments ==
          std::vector<std::string>{"run", "--target", "x86_64-linux-android", "--format", "apk"});
  const auto linux = ResolveMcppTarget("linux", false, "x86_64");
  REQUIRE(McppRunCommand("/p", linux, false).arguments == std::vector<std::string>{"run", "--target", "x86_64-linux-gnu"});
  REQUIRE(McppPackCommand("/p", linux, true).arguments ==
          std::vector<std::string>{"pack", "--target", "x86_64-linux-gnu", "--format", "appimage", "--release"});
}

TEST_CASE("HuxerUICliReadsAnMcppProjectsPlatforms") {
  TemporaryDirectory temporary;
  const std::filesystem::path manifest = temporary.Path() / "mcpp.toml";
  Write(manifest, "[package]\nname = \"sample\"\nplatforms = [\"linux\", \"android\", \"emscripten\", \"plan9\"]\n");
  // mcpp says `emscripten` where the CLI says `web`; an unknown name is dropped.
  REQUIRE(McppProjectPlatforms(manifest) == std::vector<std::string>{"linux", "android", "web"});
  Write(manifest, "[package]\nname = \"sample\"\n");
  REQUIRE(McppProjectPlatforms(manifest) == std::vector<std::string>{"linux", "windows", "macos"});
  REQUIRE(McppPlatformsLine(std::vector<std::string>{"web", "ios"}) == "platforms = [\"emscripten\", \"ios\"]");
}

TEST_CASE("HuxerUICliDiscoversAnMcppProject") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "sample";
  std::filesystem::create_directories(root / "src");
  Write(root / "mcpp.toml", "[package]\nname = \"sample\"\nplatforms = [\"linux\", \"web\"]\n");
  Write(root / "build.mcpp", "int main() { return 0; }\n");
  const huxerui::cli::Project project = huxerui::cli::DiscoverProject(root / "src");
  REQUIRE(project.build_system == huxerui::cli::BuildSystem::Mcpp);
  REQUIRE(project.root == root);
  REQUIRE(project.platforms == std::vector<std::string>{"linux", "web"});
}

TEST_CASE("HuxerUICliCreatesAnMcppProjectForSelectedPlatforms") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(),
      {"create", "app", "Sample-App", "--build", "mcpp", "--platform", "android,web", "--agent", "none"});
  REQUIRE(invocation.result == 0);
  const std::string manifest = Read(temporary.Path() / "Sample-App" / "mcpp.toml");
  REQUIRE(manifest.find("\nplatforms = [\"android\", \"emscripten\"]\n") != std::string::npos);
}
