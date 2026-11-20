#include "support.h"

#include "mcpp/mcpp.h"
#include "toolchain.h"

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
  REQUIRE(invocation.output.find("-d|--device <id>") != std::string::npos);
  REQUIRE(invocation.output.find("--java-home <path>") != std::string::npos);
  REQUIRE(invocation.output.find("--abi <abi-list>") != std::string::npos);
  REQUIRE(invocation.output.find("a common library's Preview enable all platforms") != std::string::npos);
  REQUIRE(invocation.output.find("[--build-system cmake|mcpp]") != std::string::npos);
  REQUIRE(invocation.output.find("huxerui mcpp") == std::string::npos);
}

TEST_CASE("HuxerUICliCreatesMcppDispatchCommands") {
  using huxerui::cli::mcpp::TargetTriple;
  REQUIRE(TargetTriple("linux").empty());
  REQUIRE(TargetTriple("windows").empty());
  REQUIRE(TargetTriple("web") == "wasm32-emscripten");
  REQUIRE(TargetTriple("android") == "aarch64-linux-android");
  REQUIRE(TargetTriple("ios") == "aarch64-ios-sim");
  REQUIRE_THROWS_AS(TargetTriple("plan9"), std::invalid_argument);

  // Linux builds for the host's ABI, whichever it is, and for no other; Apple platforms build for arm64.
  using huxerui::cli::mcpp::AbiTriple;
  const bool arm_host = huxerui::cli::detail::HostArchitecture() == "arm64";
  REQUIRE(AbiTriple("linux", arm_host ? "aarch64" : "x86_64").empty());
  REQUIRE_THROWS_WITH(AbiTriple("linux", arm_host ? "x86_64" : "aarch64"),
                      Catch::Matchers::ContainsSubstring("for the host's ABI"));
  REQUIRE(AbiTriple("macos", "arm64").empty());
  REQUIRE(AbiTriple("ios", "arm64") == "aarch64-ios-sim");
  REQUIRE_THROWS_AS(AbiTriple("macos", "x86_64"), std::invalid_argument);
  REQUIRE(AbiTriple("android", "x86_64") == "x86_64-linux-android");

  const huxerui::cli::mcpp::CommandOptions web{"/project", "web", "release"};
  const huxerui::cli::ProcessCommand build = huxerui::cli::mcpp::BuildCommand(web);
  REQUIRE(build.executable == "mcpp");
  REQUIRE(build.arguments == std::vector<std::string>{"build", "--target", "wasm32-emscripten", "--release"});
  REQUIRE(build.working_directory == "/project");
  const huxerui::cli::ProcessCommand run = huxerui::cli::mcpp::RunCommand({"/project", "linux", "debug"});
  REQUIRE(run.arguments == std::vector<std::string>{"run"});
}

TEST_CASE("HuxerUICliCreatesMcppPackCommands") {
  using huxerui::cli::mcpp::PackageFormat;
  REQUIRE(PackageFormat("linux", "") == "appimage");
  REQUIRE(PackageFormat("linux", "rpm") == "rpm");
  REQUIRE(PackageFormat("windows", "") == "setup");
  REQUIRE(PackageFormat("macos", "") == "dmg");
  REQUIRE(PackageFormat("web", "") == "web");
  REQUIRE(PackageFormat("android", "") == "apk");
  REQUIRE(PackageFormat("ios", "") == "app");
  REQUIRE_THROWS_WITH(PackageFormat("plan9", ""), Catch::Matchers::ContainsSubstring("no package format"));

  const auto release = huxerui::cli::mcpp::PackCommand({"/project", "web", "release"}, "web");
  REQUIRE(release.arguments == std::vector<std::string>{"pack", "--pkg-format", "web", "--message-format", "json",
                                                        "--target", "wasm32-emscripten"});
  const auto debug = huxerui::cli::mcpp::PackCommand({"/project", "linux", "debug"}, "deb");
  REQUIRE(debug.arguments == std::vector<std::string>{"pack", "--pkg-format", "deb", "--message-format", "json",
                                                      "--dev"});
}

TEST_CASE("HuxerUICliReadsMcppPackResults") {
  const std::string envelope = R"({
 "data": {
  "artifacts": [
   {"format": "deb", "path": "/out/app_0.1.0_amd64.deb", "sha256": "00", "targets": ["x86_64-linux-gnu"],
    "type": "file"},
   {"format": "web", "path": "C:\\out\\web", "type": "dir"}
  ],
  "stage": {"dir": "/out/stage"}
 },
 "kind": "mcpp.pack"
})";
  const auto artifacts = huxerui::cli::mcpp::ParsePackArtifacts(envelope);
  REQUIRE(artifacts.size() == 2);
  REQUIRE(artifacts[0].path == std::filesystem::path("/out/app_0.1.0_amd64.deb"));
  REQUIRE(artifacts[0].type == "file");
  REQUIRE(artifacts[1].path == std::filesystem::path("C:\\out\\web"));
  REQUIRE(artifacts[1].type == "dir");
  REQUIRE_THROWS(huxerui::cli::mcpp::ParsePackArtifacts(R"({"kind": "mcpp.pack", "data": {"artifacts": []}})"));
  REQUIRE_THROWS(huxerui::cli::mcpp::ParsePackArtifacts("not json"));

  // The launch envelope is the last line; an artifact event before it has an empty `argv` of its own.
  const std::string launch =
      R"({"artifact":{"kind":"bin","launch":{"argv":[],"cwd":"/p","via":"runner"},"name":"app","path":"/o/bin/app.js"},)"
      R"("event":"artifact","kind":"mcpp.event"})"
      "\n"
      R"({"data":{"argv":["/emsdk/emscripten/emrun","--hostname","localhost","/o/bin/app.js"],"cwd":"/p"},)"
      R"("kind":"mcpp.launch","schemaVersion":1})"
      "\n";
  REQUIRE(huxerui::cli::mcpp::ParseLaunchProgram(launch) == std::filesystem::path("/emsdk/emscripten/emrun"));
  REQUIRE_THROWS(huxerui::cli::mcpp::ParseLaunchProgram(launch.substr(0, launch.find('\n'))));
  const huxerui::cli::ProcessCommand command =
      huxerui::cli::mcpp::LaunchCommand({.project_root = "/p", .platform = "web"}, "web");
  REQUIRE(command.arguments == std::vector<std::string>{"run", "--target", "wasm32-emscripten", "--runner", "web",
                                                        "--print-launch", "--message-format", "json"});
}

TEST_CASE("HuxerUICliHasNoCommandNamedAfterABuildSystem") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"mcpp", "build"});
  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("unknown command: mcpp") != std::string::npos);
}

TEST_CASE("HuxerUICliDispatchesToTheProjectsBuildSystem") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "mods", "--build-system", "mcpp", "--agent", "none"}).result == 0);
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "both", "--build-system", "cmake,mcpp", "--agent", "none"})
              .result == 0);
  const std::filesystem::path modules = temporary.Path() / "mods";
  const std::filesystem::path both = temporary.Path() / "both";

  // An option that would change the product is invalid usage for an integration that cannot honor it.
  for (const std::initializer_list<std::string_view> arguments : {
           std::initializer_list<std::string_view>{"build", "web", "--abi", "wasm32"},
           std::initializer_list<std::string_view>{"build", "web", "--sdk", "."},
           std::initializer_list<std::string_view>{"run", "android", "--watch", "--no-shell"},
       }) {
    const Invocation invocation = Invoke(modules / "src", arguments);
    INFO(invocation.error);
    REQUIRE(invocation.result == 2);
    REQUIRE(invocation.error.find("mcpp project") != std::string::npos);
  }

  // --optimization and --lto select a release profile the manifest declares; one it lacks is invalid usage.
  const std::filesystem::path bare = temporary.Path() / "bare";
  std::filesystem::create_directories(bare / "platform/linux");
  std::ofstream(bare / "mcpp.toml") << "[package]\nname = \"bare\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                                       "huxerui = { path = \"/nowhere\" }\n";
  std::ofstream(bare / "build.mcpp") << "int main() { return 0; }\n";
  for (const Invocation& undeclared_profile : {
           Invoke(bare, {"build", "linux", "--config", "release", "--optimization", "size", "--lto", "on"}),
           Invoke(bare, {"package", "linux", "--optimization", "size", "--lto", "on"}),
       }) {
    REQUIRE(undeclared_profile.result == 2);
    REQUIRE(undeclared_profile.error.find("profile release-size-lto, which mcpp.toml does not declare") !=
            std::string::npos);
  }

  // A root declaring both build systems builds with CMake unless --build-system selects mcpp, and a selection must
  // name a declared one. `--no-shell` tells the two paths apart without building: only the CMake path refuses it.
  const Invocation defaulted = Invoke(both, {"build", "web", "--no-shell"});
  REQUIRE(defaulted.result == 2);
  REQUIRE(defaulted.error.find("not available for a CMake project") != std::string::npos);
  const Invocation selected = Invoke(both, {"build", "web", "--no-shell", "--build-system", "mcpp"});
  REQUIRE(selected.result == 2);
  REQUIRE(selected.error.find("web has none") != std::string::npos);
  REQUIRE(Invoke(both, {"build", "linux", "--build-system", "meson"}).result == 2);
  const Invocation undeclared = Invoke(modules, {"build", "linux", "--build-system", "cmake"});
  REQUIRE(undeclared.result == 2);
  REQUIRE(undeclared.error.find("no cmake declaration") != std::string::npos);

  // An option that only steers the CMake driver is reported and not applied; the command continues, here to
  // the platform check that stops it.
  const Invocation steered = Invoke(both, {"build", "plan9", "--build-system", "mcpp", "--generator", "Ninja"});
  REQUIRE(steered.result == 2);
  REQUIRE(steered.error.find("warning: --generator") != std::string::npos);
  REQUIRE(steered.error.find("unknown platform: plan9") != std::string::npos);

  // Android and iOS build through their standard shells for every integration, so a platform the project does not
  // enable stops either. `--no-shell` leaves the shell out for mcpp, and a platform the host cannot build still
  // stops the command.
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "linuxonly", "--build-system", "cmake,mcpp", "--platform",
                                    "linux", "--agent", "none"})
              .result == 0);
  const std::filesystem::path linux_only = temporary.Path() / "linuxonly";
  const std::string foreign{huxerui::cli::CurrentHostId() == "macos" ? "windows" : "ios"};
  for (const std::string_view command : {"build", "package"}) {
    const Invocation mcpp = Invoke(linux_only, {command, foreign, "--build-system", "mcpp"});
    INFO(mcpp.error);
    REQUIRE(mcpp.result == 1);
    REQUIRE(mcpp.error.find("platform is not enabled by this project: " + foreign) != std::string::npos);
    const Invocation cmake =
        Invoke(linux_only, {command, foreign, "--build-system", "cmake", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
    INFO(cmake.error);
    REQUIRE(cmake.result == 1);
    REQUIRE(cmake.error.find("platform is not enabled by this project: " + foreign) != std::string::npos);
    if (foreign == "ios") {
      const Invocation shellless = Invoke(linux_only, {command, foreign, "--build-system", "mcpp", "--no-shell"});
      INFO(shellless.error);
      REQUIRE(shellless.result == 1);
      REQUIRE(shellless.error.find("cannot be built from host") != std::string::npos);
    }
  }
  // `--no-shell` is a capability of the mcpp integration, for a platform that has a standard shell to leave out.
  const Invocation cmake_shellless = Invoke(linux_only, {"build", "linux", "--build-system", "cmake", "--no-shell"});
  REQUIRE(cmake_shellless.result == 2);
  REQUIRE(cmake_shellless.error.find("not available for a CMake project") != std::string::npos);

  // The Xcode shell builds an mcpp core too, so `open ios` dispatches like `build`: away from macOS it stops at the
  // host check, and an mcpp project takes no --sdk.
  if (huxerui::cli::CurrentHostId() != "macos") {
    const Invocation open = Invoke(modules, {"open", "ios"});
    INFO(open.error);
    REQUIRE(open.result == 1);
    REQUIRE(open.error.find("cannot be built from host") != std::string::npos);
  }
  const Invocation open_sdk = Invoke(modules, {"open", "ios", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
  REQUIRE(open_sdk.result == 2);
  REQUIRE(open_sdk.error.find("--sdk does not apply to an mcpp project") != std::string::npos);
}

TEST_CASE("HuxerUICliDiscoversTheNearestDeclaringRoot") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "outer", "--platform", "linux", "--agent", "none"}).result == 0);
  const std::filesystem::path outer = temporary.Path() / "outer";
  REQUIRE(Invoke(outer, {"create", "app", "inner", "--build-system", "mcpp", "--platform", "linux", "--agent", "none"})
              .result == 0);
  const huxerui::cli::Project project = huxerui::cli::DiscoverProject(outer / "inner/src");
  REQUIRE(project.root == outer / "inner");
  REQUIRE(project.build_systems == std::vector{huxerui::cli::BuildSystem::Mcpp});
  REQUIRE(huxerui::cli::DiscoverProject(outer / "src").root == outer);
}

TEST_CASE("HuxerUICliAddsPlatformsToMcppProjects") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "Mods", "--build-system", "mcpp", "--id", "dev.example.mods",
                                    "--platform", "linux", "--agent", "none"})
              .result == 0);
  const std::filesystem::path project = temporary.Path() / "Mods";
  const Invocation added = Invoke(project, {"platform", "add", "macos"});
  INFO(added.error);
  REQUIRE(added.result == 0);
  // The shell carries the identity recorded at creation and the modules form of the entry.
  REQUIRE(Read(project / "platform/macos/Info.plist.in").find("dev.example.mods") != std::string::npos);
  REQUIRE(Read(project / "platform/macos/main.cpp").find("import huxerui;") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsInvalidSdkAndDeviceOptions") {
  TemporaryDirectory temporary;
  for (const auto option : {"--sdk", "-d", "--generator", "--config", "--abi", "--java-home"}) {
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
}

TEST_CASE("HuxerUICliAcceptsBuildConfigurations") {
  TemporaryDirectory temporary;
  for (const auto command : {"build", "run", "package"}) {
    const auto help = Invoke(temporary.Path(), {command, "--help"});
    REQUIRE(help.output.find("[--config debug|release]") != std::string::npos);

    for (const auto config : {"debug", "release"}) {
      const auto invocation = Invoke(temporary.Path(), {command, "windows", "--config", config, "--sdk", "missing"});
      REQUIRE(invocation.result == 1);
      REQUIRE(invocation.error.find("HuxerUI SDK or source checkout is invalid:") != std::string::npos);
    }

    const auto invalid = Invoke(temporary.Path(), {command, "windows", "--config", "Release"});
    REQUIRE(invalid.result == 2);
    REQUIRE(invalid.error.find("--config must be debug or release") != std::string::npos);
  }
}

TEST_CASE("HuxerUICliEnablesProfilingForSourceBuilds") {
  TemporaryDirectory temporary;
  for (const auto command : {"build", "run", "package"}) {
    REQUIRE(Invoke(temporary.Path(), {command, "--help"}).output.find("--profile enables profiling") !=
            std::string::npos);

    const auto missing = Invoke(temporary.Path(), {command, "windows", "--profile"});
    REQUIRE(missing.result == 2);
    REQUIRE(missing.error.find("--profile requires --sdk <path>") != std::string::npos);
    const auto invalid = Invoke(temporary.Path(), {command, "windows", "--profile", "--sdk", "."});
    REQUIRE(invalid.result == 2);
    REQUIRE(invalid.error.find("pointing to a HuxerUI source checkout") != std::string::npos);

    const auto source = Invoke(temporary.Path(), {command, "windows", "--profile", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
    REQUIRE(source.result == 1);
    REQUIRE(source.error.find("no HuxerUI project found") != std::string::npos);
  }
  REQUIRE(Invoke(temporary.Path(), {"build", "windows", "--profile", "release"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "windows", "--profile", "--profile"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"open", "ios", "--profile"}).result == 2);
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
  REQUIRE(Invoke(temporary.Path(), {"open", "--help"}).output.find("open ios [--build-system cmake|mcpp] [--sdk <path>]") != std::string::npos);
}

TEST_CASE("HuxerUICliRestrictsWatchToRun") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"run", "--help"}).output.find("[--watch]") != std::string::npos);
  REQUIRE(Invoke(temporary.Path(), {"package", "windows", "--watch"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "windows", "--watch"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"open", "ios", "--watch"}).result == 2);
  const auto duplicate = Invoke(temporary.Path(), {"run", "windows", "--watch", "--watch"});
  REQUIRE(duplicate.result == 2);
  REQUIRE(duplicate.error.find("--watch may be specified only once") != std::string::npos);
  const auto selected = Invoke(temporary.Path(), {"r", "windows", "--watch", "--sdk", "missing"});
  REQUIRE(selected.result == 1);
  REQUIRE(selected.error.find("HuxerUI SDK or source checkout is invalid:") != std::string::npos);
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

  REQUIRE(invocation.result == 3);
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
  REQUIRE(invocation.output.find("cmake") < invocation.output.find("Platform android:"));
}

TEST_CASE("HuxerUICliDoctorStatesWhatALibraryOffersImporters") {
  TemporaryDirectory temporary;
  for (const std::string_view build_systems : {"cmake,mcpp", "cmake"}) {
    const std::string name = build_systems == "cmake" ? "headers" : "meter";
    REQUIRE(Invoke(temporary.Path(), {"create", "library", name, "--build-system", build_systems, "--platform",
                                      "android", "--agent", "none", "--yes"})
                .result == 0);
  }
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "plain", "--platform", "android", "--agent", "none", "--yes"})
              .result == 0);
  const std::filesystem::path meter = temporary.Path() / "meter";

  // A library declaring both is a module, whichever build system doctor checks, named as its [lib] unit declares.
  REQUIRE(Invoke(meter, {"doctor"}).output.find("C++ modules: import meter\n") != std::string::npos);
  REQUIRE(Invoke(meter, {"doctor", "--json"}).output.find(
              "\"library\":{\"modules\":\"module\",\"module\":\"meter\",\"reexports\":[]}") != std::string::npos);
  const auto rewrite = [](const std::filesystem::path& path, std::string_view from, std::string_view to) {
    std::string content = Read(path);
    const auto at = content.find(from);
    REQUIRE(at != std::string::npos);
    content.replace(at, from.size(), to);
    std::ofstream(path, std::ios::binary) << content;
  };
  // What the interface re-exports under the module's name is named, as importers reach it.
  rewrite(meter / "modules/meter.cppm", "export import huxerui;\n",
          "export import huxerui;\nexport import meter.macros;\n");
  REQUIRE(Invoke(meter, {"doctor"}).output.find("C++ modules: import meter (re-exports meter.macros)\n") !=
          std::string::npos);
  // A module named otherwise still works; doctor recommends the package's name and fails nothing for it.
  rewrite(meter / "modules/meter.cppm", "export module meter;", "export module gauges;");
  const auto renamed = Invoke(meter, {"doctor", "--json"});
  REQUIRE(renamed.output.find("\"module\":\"gauges\",\"reexports\":[],\"recommended\":\"meter\"") !=
          std::string::npos);
  REQUIRE(Invoke(meter, {"doctor"}).output.find(
              "C++ modules: import gauges; recommended name: meter, the package\n") != std::string::npos);

  // A headers library without mcpp still builds and is still included: the line is optional and fails nothing.
  const auto headers = Invoke(temporary.Path() / "headers", {"doctor", "--json"});
  REQUIRE(headers.output.find("\"library\":{\"modules\":\"no-mcpp\"}") != std::string::npos);
  REQUIRE(headers.output.find("C++ modules: ") == std::string::npos);
  REQUIRE(Invoke(temporary.Path() / "headers", {"doctor"}).output.find(
              "C++ modules: not exported; an mcpp declaration builds the module") !=
          std::string::npos);

  // A headers library that declares mcpp without a module is told what it still lacks.
  rewrite(meter / "mcpp.toml", "namespace = \"meter\"\n", "");
  rewrite(meter / "mcpp.toml", "[lib]\npath = \"modules/meter.cppm\"\n", "");
  const auto unnamed = Invoke(meter, {"doctor"}).output;
  REQUIRE(unnamed.find("C++ modules: not exported; name the C++ namespace to export") !=
          std::string::npos);

  // An application has no importer, so doctor says nothing about modules.
  const auto plain = Invoke(temporary.Path() / "plain", {"doctor", "--json"});
  REQUIRE(plain.output.find("\"library\"") == std::string::npos);
  REQUIRE(Invoke(temporary.Path() / "plain", {"doctor"}).output.find("C++ modules") == std::string::npos);
}

TEST_CASE("HuxerUICliDoctorChecksTheBuildSystemAProjectDispatchesTo") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "modular", "--build-system", "mcpp", "--platform", "android",
                                    "--agent", "none", "--yes"})
              .result == 0);
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "dual", "--build-system", "cmake,mcpp", "--platform", "android",
                                    "--agent", "none", "--yes"})
              .result == 0);

  // mcpp brings its own toolchain: the plan checks the engine and the shell's needs, not an SDK, CMake, or the NDK.
  const auto modular = Invoke(temporary.Path() / "modular", {"doctor", "--json"});
  REQUIRE(modular.output.find("\"id\":\"mcpp\"") != std::string::npos);
  REQUIRE(modular.output.find("\"id\":\"xlings\"") != std::string::npos);
  REQUIRE(modular.output.find("\"id\":\"huxerui_home\"") == std::string::npos);
  REQUIRE(modular.output.find("\"id\":\"cmake\"") == std::string::npos);
  REQUIRE(modular.output.find("\"id\":\"android.java\"") != std::string::npos);
  REQUIRE(modular.output.find("\"id\":\"android.android_sdk\"") != std::string::npos);
  REQUIRE(modular.output.find("\"id\":\"android.android_ndk\"") == std::string::npos);
  const auto report = Invoke(temporary.Path() / "modular", {"doctor"}).output;
  REQUIRE(report.find("Build systems: mcpp\n") != std::string::npos);
  // What mcpp brings itself is named, not silently dropped.
  REQUIRE(report.find("  [provided] by mcpp: Android NDK\n") != std::string::npos);

  // A root declaring both is checked for the default, CMake, as a build without --build-system reaches it.
  const auto dual = Invoke(temporary.Path() / "dual", {"doctor", "--json"});
  REQUIRE(dual.output.find("\"id\":\"huxerui_home\"") != std::string::npos);
  REQUIRE(dual.output.find("\"id\":\"android.android_ndk\"") != std::string::npos);
  const auto dual_report = Invoke(temporary.Path() / "dual", {"doctor"}).output;
  REQUIRE(dual_report.find("Build systems: cmake, mcpp (checking cmake, the default)\n") != std::string::npos);
  // The other build system the root declares is reported with how to select it, and is never set up.
  REQUIRE(dual.output.find("\"id\":\"mcpp\"") != std::string::npos);
  REQUIRE(dual_report.find("mcpp (optional)") != std::string::npos);
  REQUIRE(dual_report.find("(for --build-system mcpp)") != std::string::npos);
  REQUIRE(dual_report.find("Install mcpp") == std::string::npos);

  // Without mcpp or xlings the plan installs xlings first, then mcpp, by hand; an mcpp command stops before it prints
  // anything and names doctor.
  const ScopedPath empty(temporary.Path() / "no-tools");
  const auto missing = Invoke(temporary.Path() / "modular", {"doctor"});
  REQUIRE(missing.result == 1);
  const auto xlings = missing.output.find("[manual] Install xlings: ");
  const auto mcpp = missing.output.find("[manual] Install mcpp: xlings install mcpp -y");
  REQUIRE(xlings != std::string::npos);
  REQUIRE(mcpp != std::string::npos);
  REQUIRE(xlings < mcpp);
  const auto build = Invoke(temporary.Path() / "modular", {"build", "android"});
  REQUIRE(build.result == 1);
  REQUIRE(build.output.empty());
  REQUIRE(build.error.find("mcpp is unavailable; run 'huxerui doctor android'") != std::string::npos);
}

TEST_CASE("HuxerUICliDoctorReportsMcppOutsideAProjectWithoutRequiringIt") {
  TemporaryDirectory temporary;
  const ScopedPath empty(temporary.Path() / "no-tools");
  const auto plan = Invoke(temporary.Path(), {"doctor"});
  REQUIRE(plan.output.find("[missing] mcpp (optional)") != std::string::npos);
  REQUIRE(plan.output.find("[missing] xlings (optional)") != std::string::npos);
  REQUIRE(plan.output.find("Install mcpp") == std::string::npos);
}

TEST_CASE("HuxerUICliDoctorEmitsAnOperationScopedJsonPlan") {
  TemporaryDirectory temporary;
  const auto plan = Invoke(temporary.Path(), {"doctor", "android", "--operation", "devices", "--json"});

  REQUIRE(plan.error.empty());
  REQUIRE(plan.output.starts_with("{\"schema\":1,"));
  REQUIRE(plan.output.find("\"operation\":\"devices\"") != std::string::npos);
  REQUIRE(plan.output.find("\"platform\":\"android\"") != std::string::npos);
  REQUIRE(plan.output.find("\"id\":\"cmake\"") == std::string::npos);
  REQUIRE(plan.output.find("\"id\":\"android.android_ndk\"") == std::string::npos);
  REQUIRE(Invoke(temporary.Path(), {"setup", "android", "--json"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"doctor", "android", "--operation", "devices", "--format", "rpm"}).result == 2);
}
