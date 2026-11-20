#include "support.h"

#include <set>

#include "prompt.h"

using namespace huxerui::cli::test;

namespace {

/// Every table header of a manifest, once each: TOML refuses a table declared twice.
void RequireUniqueTables(const std::string& manifest) {
  std::set<std::string> tables;
  std::istringstream lines(manifest);
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with('[') && !line.starts_with("[[")) {
      INFO(line);
      REQUIRE(tables.insert(line).second);
    }
  }
}

} // namespace

TEST_CASE("HuxerUICliRendersEmbeddedTemplatePathsAndContents") {
  const auto context = huxerui::cli::MakeProjectTemplateContext("Sample-App", "dev.example.sample");
  REQUIRE_THROWS_WITH(
      huxerui::cli::RenderTemplateTree("project/library", context),
      Catch::Matchers::ContainsSubstring("HuxerUI CLI template contains an unresolved replacement: @LIBRARY_")
  );
  REQUIRE_THROWS_WITH(
      huxerui::cli::RenderTemplateTree("missing/templates", context),
      "HuxerUI CLI template directory is missing: missing/templates"
  );

}

TEST_CASE("HuxerUICliResolvesCreateSelections") {
  using huxerui::cli::BuildSystem;
  using huxerui::cli::ProjectKind;
  using huxerui::cli::SourceStyle;
  using Systems = std::vector<BuildSystem>;
  const auto resolve = [](ProjectKind kind, Systems systems) {
    return huxerui::cli::ResolveCreateSelection(kind, systems);
  };

  auto selection = resolve(ProjectKind::App, {});
  REQUIRE(selection.style == SourceStyle::Headers);
  REQUIRE(selection.build_systems == Systems{BuildSystem::CMake});
  selection = resolve(ProjectKind::Library, {});
  REQUIRE(selection.style == SourceStyle::Headers);
  REQUIRE(selection.build_systems == Systems{BuildSystem::CMake, BuildSystem::Mcpp});
  // mcpp alone is C++ modules, for an application and a library alike.
  for (const ProjectKind kind : {ProjectKind::App, ProjectKind::Library}) {
    selection = resolve(kind, {BuildSystem::Mcpp});
    REQUIRE(selection.style == SourceStyle::Modules);
    REQUIRE(selection.build_systems == Systems{BuildSystem::Mcpp});
  }
  selection = resolve(ProjectKind::App, {BuildSystem::Mcpp, BuildSystem::CMake, BuildSystem::Mcpp});
  REQUIRE(selection.style == SourceStyle::Headers);
  REQUIRE(selection.build_systems == Systems{BuildSystem::CMake, BuildSystem::Mcpp});
  selection = resolve(ProjectKind::Library, {BuildSystem::CMake});
  REQUIRE(selection.style == SourceStyle::Headers);
  REQUIRE(selection.build_systems == Systems{BuildSystem::CMake});
}

TEST_CASE("HuxerUICliCreatesModulesApplications") {
  TemporaryDirectory temporary;
  const Invocation invocation =
      Invoke(temporary.Path(), {"create", "app", "Sample-App", "--build-system", "mcpp", "--agent", "none"});

  REQUIRE(invocation.result == 0);
  REQUIRE(invocation.output.find("Style: modules\nBuild systems: mcpp\n") != std::string::npos);
  REQUIRE(invocation.output.find("Command: huxerui create app Sample-App --build-system mcpp "
                                 "--platform ") != std::string::npos);
  const std::filesystem::path project = temporary.Path() / "Sample-App";
  REQUIRE(std::filesystem::is_regular_file(project / "mcpp.toml"));
  REQUIRE(std::filesystem::is_regular_file(project / "build.mcpp"));
  REQUIRE_FALSE(std::filesystem::exists(project / "CMakeLists.txt"));
  // The page is a module interface unit; the process entry belongs to each platform's shell.
  REQUIRE(Read(project / "src/app.cppm").find("export module app;") != std::string::npos);
  REQUIRE(Read(project / "src/app.cppm").find("\n#include") == std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(project / "src/main.cpp"));
  for (const std::string_view id : {"linux", "windows", "macos"}) {
    const std::string entry = Read(project / "platform" / id / "main.cpp");
    REQUIRE(entry.find("import huxerui;") != std::string::npos);
    REQUIRE(entry.find("#include") == std::string::npos);
  }
  // The rest of every shell is the same as under any other build system.
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/ios/App/main.mm"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/web/index.html.in"));
  REQUIRE(Read(project / "platform/macos/Info.plist.in").find("com.example.sampleapp") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(project / "template.toml"));
  REQUIRE_FALSE(std::filesystem::exists(project / "mcpp.toml.in"));

  const std::string manifest = Read(project / "mcpp.toml");
  REQUIRE(manifest.find("name     = \"Sample-App\"") != std::string::npos);
  REQUIRE(manifest.find("kind = \"app\"") != std::string::npos);
  REQUIRE(manifest.find("main = \"platform/linux/main.cpp\"") != std::string::npos);
  REQUIRE(manifest.find("main = \"platform/windows/main.cpp\"") != std::string::npos);
  REQUIRE(manifest.find("{{") == std::string::npos);
  // Created against a source checkout, so the dependency is a path; the published identity stays as a comment.
  REQUIRE(manifest.find("huxerui = { path = \"") != std::string::npos);
  REQUIRE(manifest.find(HUXERUI_TEST_SOURCE_DIRECTORY) != std::string::npos);
  REQUIRE(manifest.find("#   huxerui.huxerui = ") != std::string::npos);
  REQUIRE(manifest.find("\nhuxerui.huxerui = ") == std::string::npos);
  // The identity goes into the template's own table, whose identifier `mcpp new` leaves commented out.
  RequireUniqueTables(manifest);
  REQUIRE(manifest.find("id   = \"com.example.sampleapp\"") != std::string::npos);
  REQUIRE(manifest.find("# id = ") == std::string::npos);
  // The release profiles and the iOS deployment minimum the CLI's options and the Xcode shell rely on.
  REQUIRE(manifest.find("release-minsize-lto = {") != std::string::npos);
  REQUIRE(manifest.find("ios_deployment_target = \"15.0\"") != std::string::npos);
}

TEST_CASE("HuxerUICliKeepsOnlySelectedPlatformEntries") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "Solo", "--build-system", "mcpp", "--platform", "windows",
                                    "--agent", "none"})
              .result == 0);
  const std::filesystem::path project = temporary.Path() / "Solo";
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/main.cpp"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/linux"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/macos"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/ios"));
}

TEST_CASE("HuxerUICliStatesNoProgramStartOnTheWebAndAndroid") {
  // The platform layer starts the application on the Web and Android, so a project without the Linux shell still
  // builds there: those rows state that the image has no program start.
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "Web-Only", "--build-system", "mcpp", "--platform", "web,android",
                                    "--agent", "none"})
              .result == 0);
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "Dual-Web", "--build-system", "cmake,mcpp", "--platform", "web",
                                    "--agent", "none"})
              .result == 0);
  for (const std::string_view name : {"Web-Only", "Dual-Web"}) {
    const std::filesystem::path project = temporary.Path() / name;
    REQUIRE_FALSE(std::filesystem::exists(project / "platform/linux"));
    const std::string manifest = Read(project / "mcpp.toml");
    REQUIRE(manifest.find("[target.'cfg(os = \"emscripten\")'.targets.") != std::string::npos);
    REQUIRE(manifest.find("[target.'cfg(env = \"android\")'.targets.") != std::string::npos);
    REQUIRE(manifest.find("main = false") != std::string::npos);
    REQUIRE(manifest.find("platform/linux/main.cpp") != std::string::npos);
  }
}

TEST_CASE("HuxerUICliInfersModulesFromAnMcppOnlyList") {
  TemporaryDirectory temporary;
  const Invocation invocation =
      Invoke(temporary.Path(), {"create", "app", "Nav-App", "--build-system", "mcpp", "--template", "navigation",
                                "--agent", "none"});
  REQUIRE(invocation.result == 0);
  REQUIRE(invocation.output.find("Style: modules") != std::string::npos);
  const std::string page = Read(temporary.Path() / "Nav-App/src/app.cppm");
  REQUIRE(page.find("NavigationStack(HomePage)") != std::string::npos);
  REQUIRE(page.find("{{") == std::string::npos);
}

TEST_CASE("HuxerUICliCreatesApplicationsWithBothDeclarations") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "Dual-App", "--build-system", "cmake,mcpp", "--agent",
                                    "none"})
              .result == 0);
  const std::filesystem::path project = temporary.Path() / "Dual-App";
  REQUIRE(std::filesystem::is_regular_file(project / "CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(project / "HuxerUIProject.cmake"));
  REQUIRE(std::filesystem::is_regular_file(project / "build.mcpp"));
  REQUIRE(Read(project / "src/app.cpp").find("#include <huxerui/huxerui.h>") != std::string::npos);
  REQUIRE(Read(project / "platform/linux/main.cpp").find("#include <huxerui/app.h>") != std::string::npos);
  const std::string manifest = Read(project / "mcpp.toml");
  RequireUniqueTables(manifest);
  // Both declarations name the same application target, which is the artifact's basename.
  REQUIRE(manifest.find("name     = \"dual_app\"") != std::string::npos);
  REQUIRE(manifest.find("[targets.dual_app]") != std::string::npos);
  REQUIRE(manifest.find("standard = \"c++23\"") != std::string::npos);
  REQUIRE(manifest.find("huxerui = { path = \"") != std::string::npos);
  REQUIRE(manifest.find("@") == std::string::npos);
  REQUIRE(Read(project / ".gitignore").find("/target/") != std::string::npos);
}

TEST_CASE("HuxerUICliCreatesLibrariesForEveryBuildSystemByDefault") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "library", "data-grid", "--agent", "none"});
  REQUIRE(invocation.result == 0);
  REQUIRE(invocation.output.find("Style: headers + modules\nBuild systems: cmake, mcpp\n") != std::string::npos);
  const std::filesystem::path project = temporary.Path() / "data-grid";
  REQUIRE(std::filesystem::is_regular_file(project / "CMakeLists.txt"));
  const std::string manifest = Read(project / "mcpp.toml");
  // The package is named after the project, whichever build systems the library declares.
  REQUIRE(manifest.find("name     = \"data-grid\"") != std::string::npos);
  REQUIRE(manifest.find("library   = \"DataGrid::DataGrid\"") != std::string::npos);
  REQUIRE(manifest.find("kind = \"lib\"") != std::string::npos);
  REQUIRE(manifest.find("include_dirs = [\"include\"]") != std::string::npos);
  // The headers are also a module named after the package, `data_grid` with the dash a module name cannot hold,
  // whose interface mcpp's build regenerates from the names they declare in the C++ namespace `data_grid`.
  REQUIRE(manifest.find("[lib]\npath = \"modules/data_grid.cppm\"") != std::string::npos);
  const std::string interface = Read(project / "modules/data_grid.cppm");
  REQUIRE(interface.find("#include <datagrid/datagrid.h>\n") != std::string::npos);
  REQUIRE(interface.find("export module data_grid;\n") != std::string::npos);
  REQUIRE(interface.find("    using data_grid::Install;\n") != std::string::npos);
  REQUIRE(interface.find("@") == std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(project / "examples/preview/CMakeLists.txt"));
  const std::string preview = Read(project / "examples/preview/mcpp.toml");
  REQUIRE(preview.find("data-grid = { path = \"../..\" }") != std::string::npos);
  REQUIRE(preview.find("[targets.example_data_grid]") != std::string::npos);
}

TEST_CASE("HuxerUICliCreatesModulesLibraries") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "my-widgets", "--build-system", "mcpp", "--agent", "none"})
              .result == 0);
  const std::filesystem::path project = temporary.Path() / "my-widgets";
  REQUIRE_FALSE(std::filesystem::exists(project / "CMakeLists.txt"));
  // The template's placeholder module takes the library's name, in the unit, the manifest, and the test.
  REQUIRE_FALSE(std::filesystem::exists(project / "src/component.cppm"));
  REQUIRE(Read(project / "src/my_widgets.cppm").find("export module my_widgets;") != std::string::npos);
  REQUIRE(Read(project / "tests/component.cpp").find("import my_widgets;") != std::string::npos);
  const std::string manifest = Read(project / "mcpp.toml");
  REQUIRE(manifest.find("kind = \"lib\"") != std::string::npos);
  REQUIRE(manifest.find("path = \"src/my_widgets.cppm\"") != std::string::npos);
  REQUIRE(manifest.find("library   = \"MyWidgets::MyWidgets\"") != std::string::npos);
  REQUIRE(manifest.find("component") == std::string::npos);

  // The Preview is a C++ modules application that imports the library.
  const std::filesystem::path preview = project / "examples/preview";
  const std::string page = Read(preview / "src/app.cppm");
  REQUIRE(page.find("import my_widgets;") != std::string::npos);
  REQUIRE(page.find("#include") == std::string::npos);
  REQUIRE(page.find("@") == std::string::npos);
  const std::string preview_manifest = Read(preview / "mcpp.toml");
  RequireUniqueTables(manifest);
  RequireUniqueTables(preview_manifest);
  REQUIRE(preview_manifest.find("my-widgets = { path = \"../..\" }") != std::string::npos);
  REQUIRE(preview_manifest.find("[targets.example_my_widgets]") != std::string::npos);
  REQUIRE(Read(preview / "platform/linux/main.cpp").find("import huxerui;") != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(preview / "build.mcpp"));
  REQUIRE_FALSE(std::filesystem::exists(preview / "CMakeLists.txt"));
  // The iOS standard library the Preview names is the one the application template names.
  const auto libcxx = [](const std::string& text) {
    const std::size_t start = text.find("llvm.libcxx = ");
    REQUIRE(start != std::string::npos);
    return text.substr(start, text.find('\n', start) - start);
  };
  REQUIRE(libcxx(preview_manifest) == libcxx(Read(std::filesystem::path(HUXERUI_TEST_SOURCE_DIRECTORY) /
                                                  "templates/app/mcpp.toml.in")));
}

TEST_CASE("HuxerUICliGivesModulesLibrariesPlatformPackages") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "my-widgets", "--build-system", "mcpp", "--platform",
                                    "android", "--agent", "none"})
              .result == 0);
  const std::filesystem::path project = temporary.Path() / "my-widgets";
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/build.gradle"));
  REQUIRE(std::filesystem::is_directory(project / "examples/preview/platform/android"));
  REQUIRE_FALSE(std::filesystem::exists(project / "examples/preview/platform/linux"));

  // `platform add` reads the identity from the manifest and writes the package and a C++ modules Preview shell.
  const Invocation added = Invoke(project / "src", {"platform", "add", "linux"});
  INFO(added.error);
  REQUIRE(added.result == 0);
  REQUIRE(std::filesystem::is_directory(project / "platform/linux"));
  REQUIRE(Read(project / "examples/preview/platform/linux/main.cpp").find("import huxerui;") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsUnsupportedCreateSelections") {
  TemporaryDirectory temporary;
  const auto usage = [&](std::initializer_list<std::string_view> arguments) {
    const Invocation invocation = Invoke(temporary.Path(), arguments);
    INFO(invocation.error);
    REQUIRE(invocation.result == 2);
    return invocation.error;
  };
  REQUIRE(usage({"create", "app", "Sample-App", "--build-system", "meson"}).find("unknown build system: meson") !=
          std::string::npos);
  // The build systems select the source style, so there is no separate style option; `--build` was renamed to
  // `--build-system` without an alias.
  REQUIRE(usage({"create", "app", "Sample-App", "--style", "modules"}).find("unexpected create argument: --style") !=
          std::string::npos);
  usage({"create", "app", "Sample-App", "--build", "mcpp"});
  usage({"create", "app", "Sample-App", "--build-system", "mcpp", "--template", "nonesuch"});
  // `--template` selects C++ modules content.
  REQUIRE(usage({"create", "app", "Sample-App", "--template", "navigation"}).find("--build-system mcpp") !=
          std::string::npos);
  REQUIRE(usage({"create", "app", "Sample-App", "--build-system", "cmake,mcpp", "--template", "app"})
              .find("--build-system mcpp") != std::string::npos);
  // Without a terminal nothing can ask for the missing name.
  usage({"create", "app"});
  usage({"create"});
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "Sample-App"));
}

TEST_CASE("HuxerUICliPromptsForMissingCreateSelections") {
  TemporaryDirectory temporary;
  // Kind: app; name: Asked; build system: mcpp; template: navigation; platforms: 1 and 2 of the list; then the
  // confirmation's default, yes.
  std::istringstream answers("1\nAsked\n2\n2\n1,2\n\n");
  std::ostringstream prompts;
  huxerui::cli::prompt::LinePrompter prompter(answers, prompts);
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;
  const std::vector<std::string_view> arguments{"create", "--agent", "none"};
  const huxerui::cli::SdkLocation sdk{HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable};
  REQUIRE(huxerui::cli::Run(arguments, temporary.Path(), sdk, input, output, error, &prompter) == 0);
  for (const std::string_view question : {"? Project kind", "? Project name", "? Build system", "? Template",
                                          "? Platforms", "? Create Asked? [Y/n]"}) {
    REQUIRE(prompts.str().find(question) != std::string::npos);
  }
  // The build-system answer selects the source style, so no separate question asks for it.
  REQUIRE(prompts.str().find("? Source style") == std::string::npos);
  const auto platforms = huxerui::cli::PlatformIds();
  // After questions the outcome closes them, with the same choices as options.
  const std::string expected = "same as  huxerui create app Asked --build-system mcpp --template "
                               "navigation --platform " + std::string(platforms[0]) + "," +
                               std::string(platforms[1]) + " --agent none\n";
  REQUIRE(prompts.str().find(expected) != std::string::npos);
  REQUIRE(prompts.str().find("   or ") == std::string::npos);
  REQUIRE(prompts.str().find("+  HuxerUI - create a project\n") != std::string::npos);
  REQUIRE(output.str().empty());
  REQUIRE(Read(temporary.Path() / "Asked/src/app.cppm").find("NavigationStack") != std::string::npos);
}

TEST_CASE("HuxerUICliAsksForAgentSkillsAndCreatesNothingWhenDeclined") {
  TemporaryDirectory temporary;
  // Kind: library; name: default; build systems: default; agents: none; then declining the confirmation.
  std::istringstream answers("2\n\n\n\nn\n");
  std::ostringstream prompts;
  huxerui::cli::prompt::LinePrompter prompter(answers, prompts);
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;
  const std::vector<std::string_view> arguments{"create"};
  const huxerui::cli::SdkLocation sdk{HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable};
  REQUIRE(huxerui::cli::Run(arguments, temporary.Path(), sdk, input, output, error, &prompter) == 0);
  REQUIRE(prompts.str().find("? Project name [hello-hui-lib]") != std::string::npos);
  REQUIRE(prompts.str().find("? Agent skills") != std::string::npos);
  REQUIRE(prompts.str().find("Nothing was created.") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "hello-hui-lib"));
}

TEST_CASE("HuxerUICliYesAppliesCreateDefaultsWithoutPrompting") {
  TemporaryDirectory temporary;
  std::istringstream answers;
  std::ostringstream prompts;
  huxerui::cli::prompt::LinePrompter prompter(answers, prompts);
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;
  const std::vector<std::string_view> arguments{"create", "library", "lib-a", "--agent", "none", "--yes"};
  const huxerui::cli::SdkLocation sdk{HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable};
  REQUIRE(huxerui::cli::Run(arguments, temporary.Path(), sdk, input, output, error, &prompter) == 0);
  REQUIRE(prompts.str().empty());
  REQUIRE(output.str().find("Build systems: cmake, mcpp") != std::string::npos);
}

TEST_CASE("HuxerUICliOffersMcppForAProjectDeclaringBoth") {
  TemporaryDirectory temporary;
  // Kind: app; name: Dual; build systems: both; platforms: every one; then the confirmation's default, yes.
  std::istringstream answers("1\nDual\n3\n\n\n");
  std::ostringstream prompts;
  huxerui::cli::prompt::LinePrompter prompter(answers, prompts);
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;
  const huxerui::cli::SdkLocation sdk{HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable};
  const std::vector<std::string_view> create{"create", "--agent", "none"};
  {
    // Without mcpp the outcome says how to find out how to install it, and offers no command that would fail.
    const ScopedPath empty(temporary.Path() / "no-tools");
    REQUIRE(huxerui::cli::Run(create, temporary.Path(), sdk, input, output, error, &prompter) == 0);
  }
  REQUIRE(prompts.str().find("   or       huxerui run ") == std::string::npos);
  REQUIRE(prompts.str().find("mcpp is unavailable; 'huxerui doctor' shows how to install it") != std::string::npos);
  if (huxerui::cli::CurrentHostId() != "windows") {
    // With mcpp, the default, CMake, needs an SDK and system packages, so the outcome also names the mcpp command.
    const std::filesystem::path tools = temporary.Path() / "tools";
    std::filesystem::create_directories(tools);
    std::ofstream(tools / "mcpp") << "#!/bin/sh\necho mcpp 2026.11.17.1\n";
    std::filesystem::permissions(tools / "mcpp", std::filesystem::perms::owner_all);
    const ScopedPath with_mcpp(tools);
    std::istringstream again("1\nDual-Again\n3\n\n\n");
    std::ostringstream again_prompts;
    huxerui::cli::prompt::LinePrompter again_prompter(again, again_prompts);
    REQUIRE(huxerui::cli::Run(create, temporary.Path(), sdk, input, output, error, &again_prompter) == 0);
    REQUIRE(again_prompts.str().find("   or       huxerui run ") != std::string::npos);
    REQUIRE(again_prompts.str().find(" --build-system mcpp  (fetches its own toolchain)\n") != std::string::npos);
  }

  const std::vector<std::string_view> cmake_only{"create", "app", "Single", "--build-system", "cmake", "--agent",
                                                 "none", "--yes"};
  REQUIRE(huxerui::cli::Run(cmake_only, temporary.Path(), sdk, input, output, error, nullptr) == 0);

  const huxerui::cli::SdkLocation no_sdk{};
  const auto build = [&](const std::filesystem::path& directory, std::vector<std::string_view> arguments) {
    std::ostringstream build_output;
    std::ostringstream build_error;
    const int status = huxerui::cli::Run(arguments, directory, no_sdk, input, build_output, build_error, nullptr);
    return std::pair{status, build_output.str()};
  };
  constexpr std::string_view note = "Using cmake, the default (or --build-system mcpp)\n";

  // Left to the default, a project declaring both states it before the CMake path stops on the missing SDK.
  const auto [failed, failed_output] = build(temporary.Path() / "Dual", {"build", "linux"});
  REQUIRE(failed == 1);
  REQUIRE(failed_output.starts_with(note));
  const auto [run, run_output] = build(temporary.Path() / "Dual", {"run", "linux", "--sdk", "missing"});
  REQUIRE(run == 1);
  REQUIRE(run_output.starts_with(note));

  // An explicit choice and a project declaring one build system have nothing to choose between.
  const auto [chosen, chosen_output] = build(temporary.Path() / "Dual", {"build", "linux", "--build-system", "cmake"});
  REQUIRE(chosen == 1);
  REQUIRE(chosen_output.find("Using cmake") == std::string::npos);
  const auto [single, single_output] = build(temporary.Path() / "Single", {"build", "linux"});
  REQUIRE(single == 1);
  REQUIRE(single_output.find("Using cmake") == std::string::npos);
}

TEST_CASE("HuxerUICliPackageTemplatesCarryTheShellEntries") {
  // `mcpp new --template` renders templates/ without the CLI, so those trees carry the entries and macOS bundle
  // metadata the mcpp build reads. They must be the CLI's shell files rendered in mcpp's vocabulary.
  const huxerui::cli::ProjectTemplateContext context{
      "{{project.name}}", "{{project.name}}", "com.example.{{project.name}}", huxerui::cli::SourceStyle::Modules};
  const auto find = [](const std::vector<huxerui::cli::GeneratedFile>& files, std::string_view path) {
    const auto file = std::ranges::find_if(files, [path](const auto& candidate) {
      return candidate.path.generic_string() == path;
    });
    REQUIRE(file != files.end());
    return file->content;
  };
  const std::string entry = find(huxerui::cli::RenderTemplateTree("platform/desktop/app_modules", context), "main.cpp");
  const std::string ios = find(huxerui::cli::RenderTemplateTree("platform/ios/app", context), "App/main.mm");
  const auto macos = huxerui::cli::RenderTemplateTree("platform/macos/app", context);
  for (const std::string_view name : {"app", "navigation"}) {
    const std::filesystem::path root = std::filesystem::path(HUXERUI_TEST_SOURCE_DIRECTORY) / "templates" / name;
    INFO(root.string());
    for (const std::string_view id : {"linux", "windows", "macos"}) {
      REQUIRE(Read(root / "platform" / id / "main.cpp") == entry);
    }
    REQUIRE(Read(root / "platform/ios/App/main.mm") == ios);
    REQUIRE(Read(root / "platform/macos/Info.plist.in.in") == find(macos, "Info.plist.in"));
    REQUIRE(ReadBinary(root / "platform/macos/AppIcon.icns") == find(macos, "AppIcon.icns"));
  }
}

TEST_CASE("HuxerUICliCreatesSelectedPlatformShells") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(
      temporary.Path(),
      {"create", "app", "Sample-App", "--id", "dev.example.sampleapp", "--platform", "windows,android,web"}
  );

  REQUIRE(invocation.result == 0);
  const std::filesystem::path project = temporary.Path() / "Sample-App";
  REQUIRE(std::filesystem::is_regular_file(project / "CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(project / "HuxerUIProject.cmake"));
  REQUIRE(std::filesystem::is_regular_file(project / "src/app.cpp"));
  REQUIRE(std::filesystem::is_directory(project / "resources/images"));
  REQUIRE(std::filesystem::is_directory(project / "resources/raw"));
  REQUIRE(std::filesystem::is_regular_file(project / "resources/strings/default.properties"));
  REQUIRE(std::filesystem::is_regular_file(
      project / ".agents/skills/huxerui-app-development/SKILL.md"
  ));
  REQUIRE(std::filesystem::is_regular_file(
      project / ".agents/skills/huxerui-app-development/references/project-workflow.md"
  ));
  REQUIRE(std::filesystem::is_regular_file(
      project / ".agents/skills/huxerui-app-development/references/resources-files-network.md"
  ));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/macos"));
  REQUIRE_FALSE(std::filesystem::exists(project / ".huxerui"));
  REQUIRE(Read(project / ".gitignore").find("/.huxerui/") != std::string::npos);
  const std::string cmake = Read(project / "CMakeLists.txt");
  REQUIRE(cmake.find("huxerui_add_app(sample_app") != std::string::npos);
  REQUIRE(cmake.find("src/*.cpp") != std::string::npos);
  REQUIRE(cmake.find("SOURCES\n            ${APP_SOURCE_FILES}") != std::string::npos);
  REQUIRE(cmake.find("RESOURCES\n            resources") != std::string::npos);
  REQUIRE(cmake.find("project(sample_app VERSION 0.1.0 LANGUAGES NONE)") != std::string::npos);
  REQUIRE(cmake.find("huxerui_configure_project_app(sample_app)") != std::string::npos);
  REQUIRE(cmake.find("set(CMAKE_CXX_STANDARD") == std::string::npos);
  const std::string project_cmake = Read(project / "HuxerUIProject.cmake");
  REQUIRE(project_cmake.find("NO_CMAKE_FIND_ROOT_PATH") != std::string::npos);
  REQUIRE(project_cmake.find("\"id\": \"dev.example.sampleapp\"") != std::string::npos);
  REQUIRE(project_cmake.find("HUXERUI_LIBRARY_GRAPH_OUTPUT") == std::string::npos);
  REQUIRE(cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(project_cmake.find("if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)\n    enable_language(CXX)") !=
          std::string::npos);
  REQUIRE(project_cmake.find("enable_language(RC)") != std::string::npos);
  REQUIRE(project_cmake.find("\"${HUXERUI_WINDOWS_RESOURCE}\"") != std::string::npos);
  REQUIRE(project_cmake.find("set(HUXERUI_BUILD_SHARED OFF") != std::string::npos);
  REQUIRE(project_cmake.find("set(HUXERUI_BUILD_STATIC ON") != std::string::npos);
  REQUIRE(project_cmake.find("RESOURCE_OUTPUT_DIRECTORY") != std::string::npos);
  REQUIRE(project_cmake.find("function(huxerui_configure_project_app target_name)") != std::string::npos);
  const std::string application = Read(project / "src/app.cpp");
  REQUIRE(application.find("const Application application") != std::string::npos);
  REQUIRE(application.find("MaterialTheme") == std::string::npos);

}

TEST_CASE("HuxerUICliCreatesLibraryAndPreviewProjects") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(
      temporary.Path(),
      {
          "create",
          "library",
          "HuxerUI-CameraKit",
          "--id",
          "dev.example.camera.kit",
          "--platform",
          "android,ios,linux,macos,windows",
      }
  );

  REQUIRE(invocation.result == 0);
  const std::filesystem::path library = temporary.Path() / "HuxerUI-CameraKit";
  const std::filesystem::path preview = library / "examples/preview";
  REQUIRE(std::filesystem::is_regular_file(library / "include/huxeruicamerakit/huxeruicamerakit.h"));
  REQUIRE(std::filesystem::is_regular_file(library / "src/huxer_ui_camera_kit.cpp"));
  REQUIRE(std::filesystem::is_directory(library / "resources/images"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/android/build.gradle"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/android/gradlew"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/android/gradle/wrapper/gradle-wrapper.jar"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/android/src/main/AndroidManifest.xml"));
  const std::string android_library = Read(library / "platform/android/build.gradle");
  REQUIRE(android_library.find("id \"com.android.library\"") != std::string::npos);
  REQUIRE(android_library.find("namespace = \"dev.example.camera.kit\"") != std::string::npos);
  REQUIRE(Read(library / "platform/android/settings.gradle").find("version \"8.13.2\"") != std::string::npos);
  const std::filesystem::path ios_package = library / "platform/ios/HuxerUICameraKit";
  REQUIRE(std::filesystem::is_regular_file(ios_package / "Sources/HuxerUICameraKit/HuxerUICameraKit.swift"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/linux/src/.gitkeep"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/macos/src/.gitkeep"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/windows/src/.gitkeep"));
  const std::string swift_package = Read(ios_package / "Package.swift");
  REQUIRE(swift_package.find("name: \"HuxerUI-CameraKit\"") != std::string::npos);
  REQUIRE(swift_package.find(".iOS(.v15)") != std::string::npos);
  REQUIRE(swift_package.find("targets: [\"HuxerUICameraKit\"]") != std::string::npos);
  const std::string library_cmake = Read(library / "CMakeLists.txt");
  const std::string library_project_cmake = Read(library / "HuxerUIProject.cmake");
  REQUIRE(library_cmake.find("huxerui_add_library(huxeruicamerakit") != std::string::npos);
  REQUIRE(library_cmake.find("RESOURCES\n            resources") != std::string::npos);
  REQUIRE(library_cmake.find("RESOURCE_NAMESPACE\n            huxeruicamerakit") != std::string::npos);
  REQUIRE(library_cmake.find("huxerui_add_resources") == std::string::npos);
  REQUIRE(
      library_cmake.find("add_library(HuxerUICameraKit::HuxerUICameraKit ALIAS huxeruicamerakit)") !=
      std::string::npos
  );
  REQUIRE(library_cmake.find("platform/android/src/main/cpp/*.cpp") != std::string::npos);
  REQUIRE(library_cmake.find("platform/linux/src/*.cpp") != std::string::npos);
  REQUIRE(library_cmake.find("platform/macos/src/*.mm") != std::string::npos);
  REQUIRE(library_cmake.find("platform/web/src/*.cpp") != std::string::npos);
  REQUIRE(library_cmake.find("platform/windows/src/*.cpp") != std::string::npos);
  REQUIRE(library_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(library_project_cmake.find("enable_language(OBJCXX)") != std::string::npos);
  REQUIRE(library_project_cmake.find("\"publicTarget\": \"HuxerUICameraKit::HuxerUICameraKit\"") !=
          std::string::npos);
  REQUIRE(library_cmake.find("set(CMAKE_CXX_STANDARD") == std::string::npos);
  const std::string preview_cmake = Read(preview / "CMakeLists.txt");
  const std::string preview_project_cmake = Read(preview / "HuxerUIProject.cmake");
  REQUIRE(preview_cmake.find("huxerui_add_app(example_huxer_ui_camera_kit") != std::string::npos);
  REQUIRE(preview_cmake.find("TARGET HuxerUICameraKit::HuxerUICameraKit") != std::string::npos);
  REQUIRE(preview_cmake.find("PATH \"${CMAKE_CURRENT_SOURCE_DIR}/../..\"") != std::string::npos);
  REQUIRE(preview_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(preview_project_cmake.find("\"kind\": \"app\"") != std::string::npos);
  const std::string preview_application = Read(preview / "src/app.cpp");
  REQUIRE(preview_application.find("#include <huxeruicamerakit/huxeruicamerakit.h>") != std::string::npos);
  REQUIRE(preview_application.find("huxer_ui_camera_kit::Install") != std::string::npos);
  REQUIRE(preview_application.find("MaterialTheme") == std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/gradlew"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/gradle/wrapper/gradle-wrapper.jar"));
  REQUIRE(
      std::filesystem::is_regular_file(preview / "platform/ios/example_huxer_ui_camera_kit.xcodeproj/project.pbxproj")
  );
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/linux/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/macos/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/windows/main.cpp"));

  std::filesystem::remove_all(library / ".huxerui");
  std::filesystem::remove_all(preview / ".huxerui");
  const huxerui::cli::Project application =
      huxerui::cli::ResolveApplicationProject(huxerui::cli::DiscoverProject(library));
  REQUIRE(std::filesystem::equivalent(application.root, preview));
  REQUIRE(application.platforms == std::vector<std::string>{"android", "ios", "linux", "macos", "windows"});
  REQUIRE_FALSE(std::filesystem::exists(library / ".huxerui"));
  REQUIRE_FALSE(std::filesystem::exists(preview / ".huxerui"));

  const Invocation doctor = Invoke(library, {"doctor", "all"});
  REQUIRE(doctor.output.find("Project: " + preview.string()) != std::string::npos);
  REQUIRE(doctor.output.find("missing app/build.gradle") == std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(library / ".huxerui"));
  REQUIRE_FALSE(std::filesystem::exists(preview / ".huxerui"));
}

TEST_CASE("HuxerUICliCreatesLibrariesWithIndependentPublicIdentities") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(
      temporary.Path(),
      {
          "create",
          "library",
          "CameraKit",
          "--namespace",
          "scave::camera",
          "--target",
          "Scave::Camera",
          "--id",
          "dev.example.camera",
          "--agent",
          "none",
      }
  );

  REQUIRE(invocation.result == 0);
  const std::filesystem::path library = temporary.Path() / "CameraKit";
  const std::filesystem::path preview = library / "examples/preview";
  REQUIRE(std::filesystem::is_regular_file(library / "include/scave/camera.h"));
  REQUIRE(std::filesystem::is_regular_file(library / "src/camera_kit.cpp"));
  const std::string header = Read(library / "include/scave/camera.h");
  REQUIRE(header.find("namespace scave::camera") != std::string::npos);
  const std::string source = Read(library / "src/camera_kit.cpp");
  REQUIRE(source.find("#include <scave/camera.h>") != std::string::npos);
  REQUIRE(source.find("namespace scave::camera") != std::string::npos);

  const std::string library_cmake = Read(library / "CMakeLists.txt");
  const std::string library_project_cmake = Read(library / "HuxerUIProject.cmake");
  REQUIRE(library_project_cmake.find("\"schema\": 1") != std::string::npos);
  REQUIRE(library_project_cmake.find("\"namespace\": \"scave::camera\"") != std::string::npos);
  REQUIRE(library_project_cmake.find("\"publicTarget\": \"Scave::Camera\"") != std::string::npos);
  REQUIRE(library_cmake.find("huxerui_add_library(scave_camera") != std::string::npos);
  REQUIRE(library_cmake.find("RESOURCE_NAMESPACE\n            scave_camera") != std::string::npos);
  REQUIRE(library_cmake.find("add_library(Scave::Camera ALIAS scave_camera)") != std::string::npos);

  const std::string preview_cmake = Read(preview / "CMakeLists.txt");
  REQUIRE(preview_cmake.find("huxerui_add_app(example_camera_kit") != std::string::npos);
  REQUIRE(preview_cmake.find("TARGET Scave::Camera") != std::string::npos);
  const std::string preview_source = Read(preview / "src/app.cpp");
  REQUIRE(preview_source.find("#include <scave/camera.h>") != std::string::npos);
  REQUIRE(preview_source.find("scave::camera::Install") != std::string::npos);

  const huxerui::cli::ProjectTemplate project_template =
      huxerui::cli::LoadProjectTemplate(huxerui::cli::DiscoverProject(library));
  const auto* identity = std::get_if<huxerui::cli::LibraryTemplateContext>(&project_template);
  REQUIRE(identity != nullptr);
  REQUIRE(identity->project.target_name == "camera_kit");
  REQUIRE(identity->cpp_namespace == "scave::camera");
  REQUIRE(identity->public_target == "Scave::Camera");

  const std::filesystem::path ios_source = library / "platform/ios/src/camera.mm";
  std::filesystem::create_directories(ios_source.parent_path());
  std::ofstream(ios_source) << "// Camera adapter\n";
  const Invocation platform_add = Invoke(library, {"platform", "add", "android,ios"});
  REQUIRE(platform_add.result == 0);
  REQUIRE(Read(library / "platform/android/build.gradle").find("namespace = \"dev.example.camera\"") !=
          std::string::npos);
  const std::filesystem::path ios_package = library / "platform/ios/Camera";
  const std::string swift_package = Read(ios_package / "Package.swift");
  REQUIRE(swift_package.find("name: \"CameraKit\"") != std::string::npos);
  REQUIRE(swift_package.find("targets: [\"Camera\"]") != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(ios_package / "Sources/Camera/Camera.swift"));
  REQUIRE(Read(ios_source) == "// Camera adapter\n");
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/ios/example_camera_kit.xcodeproj/project.pbxproj"));
}

TEST_CASE("HuxerUICliUsesUnqualifiedLibraryTargetsDirectly") {
  TemporaryDirectory temporary;
  const Invocation lowercase = Invoke(
      temporary.Path(),
      {"create", "library", "Scave", "--namespace", "scave", "--target", "scave", "--agent", "none"}
  );

  REQUIRE(lowercase.result == 0);
  const std::filesystem::path library = temporary.Path() / "Scave";
  REQUIRE(std::filesystem::is_regular_file(library / "include/scave/scave.h"));
  const std::string cmake = Read(library / "CMakeLists.txt");
  REQUIRE(cmake.find("huxerui_add_library(scave") != std::string::npos);
  REQUIRE(cmake.find("RESOURCE_NAMESPACE\n            scave") != std::string::npos);
  REQUIRE(cmake.find("\nadd_library(") == std::string::npos);
  REQUIRE(Read(library / "examples/preview/CMakeLists.txt").find("TARGET scave") != std::string::npos);

  const Invocation mixed_case = Invoke(
      temporary.Path(),
      {"create", "library", "Camera", "--namespace", "camera", "--target", "CameraKit", "--agent", "none"}
  );
  REQUIRE(mixed_case.result == 0);
  const std::filesystem::path mixed_case_library = temporary.Path() / "Camera";
  REQUIRE(std::filesystem::is_regular_file(mixed_case_library / "include/camerakit/camerakit.h"));
  const std::string mixed_case_cmake = Read(mixed_case_library / "CMakeLists.txt");
  REQUIRE(mixed_case_cmake.find("huxerui_add_library(CameraKit") != std::string::npos);
  REQUIRE(mixed_case_cmake.find("RESOURCE_NAMESPACE\n            camerakit") != std::string::npos);
  REQUIRE(mixed_case_cmake.find("\nadd_library(") == std::string::npos);
  REQUIRE(Read(mixed_case_library / "examples/preview/CMakeLists.txt").find("TARGET CameraKit") !=
          std::string::npos);
}

TEST_CASE("HuxerUICliAllowsHuxerUILibraryTargetPrefixes") {
  for (const std::string_view target : {"HuxerUI::Camera", "huxerui::Camera", "HUXERUI::Camera"}) {
    CAPTURE(target);
    TemporaryDirectory temporary;
    const Invocation invocation = Invoke(temporary.Path(),
        {"create", "library", "CameraKit", "--target", target, "--agent", "none"});
    REQUIRE(invocation.result == 0);
    const std::filesystem::path library = temporary.Path() / "CameraKit";
    REQUIRE(std::filesystem::is_regular_file(library / "include/huxerui/camera.h"));
    REQUIRE(Read(library / "CMakeLists.txt").find(
                "add_library(" + std::string(target) + " ALIAS huxerui_camera)") != std::string::npos);
    const auto project_template = huxerui::cli::LoadProjectTemplate(huxerui::cli::DiscoverProject(library));
    const auto* identity = std::get_if<huxerui::cli::LibraryTemplateContext>(&project_template);
    REQUIRE(identity != nullptr);
    REQUIRE(identity->public_target == target);
  }
}

TEST_CASE("HuxerUICliRejectsInvalidLibraryPublicIdentitiesBeforePublication") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "InvalidNamespace", "--namespace", "class"}).result ==
          2);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "InvalidNamespace"));
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "ReservedNamespace", "--namespace", "scave::__camera"})
              .result == 2);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "ReservedNamespace"));
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "InvalidTarget", "--target", "Scave::Camera::View"})
              .result == 2);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "InvalidTarget"));
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "EmptyTarget", "--target", ""}).result == 2);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "EmptyTarget"));
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "AppNamespace", "--namespace", "app", "--platform", "macos"})
              .result == 2);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "AppNamespace"));
}

TEST_CASE("HuxerUICliCreatesSkillsForSelectedAgents") {
  TemporaryDirectory temporary;
  const Invocation selected = Invoke(
      temporary.Path(),
      {"create", "app", "selected", "--platform", "windows", "--agent", "claude,zcode"}
  );

  REQUIRE(selected.result == 0);
  const std::filesystem::path selected_project = temporary.Path() / "selected";
  REQUIRE(std::filesystem::is_regular_file(
      selected_project / ".claude/skills/huxerui-app-development/SKILL.md"
  ));
  REQUIRE(std::filesystem::is_regular_file(
      selected_project / ".zcode/skills/huxerui-app-development/SKILL.md"
  ));
  REQUIRE_FALSE(std::filesystem::exists(selected_project / ".agents"));

  const Invocation portable = Invoke(
      temporary.Path(),
      {
          "create",
          "app",
          "portable",
          "--platform",
          "windows",
          "--agent",
          "codex,antigravity,opencode,command-code,omp,dsh",
      }
  );

  REQUIRE(portable.result == 0);
  const std::filesystem::path portable_project = temporary.Path() / "portable";
  REQUIRE(std::filesystem::is_regular_file(
      portable_project / ".agents/skills/huxerui-app-development/SKILL.md"
  ));
  REQUIRE_FALSE(std::filesystem::exists(portable_project / ".claude"));
  REQUIRE_FALSE(std::filesystem::exists(portable_project / ".zcode"));

  const Invocation all =
      Invoke(temporary.Path(), {"create", "app", "all", "--platform", "windows", "--agent", "all"});
  REQUIRE(all.result == 0);
  const std::filesystem::path all_project = temporary.Path() / "all";
  REQUIRE(std::filesystem::is_regular_file(all_project / ".agents/skills/huxerui-app-development/SKILL.md"));
  REQUIRE(std::filesystem::is_regular_file(all_project / ".claude/skills/huxerui-app-development/SKILL.md"));
  REQUIRE(std::filesystem::is_regular_file(all_project / ".zcode/skills/huxerui-app-development/SKILL.md"));

  const Invocation disabled =
      Invoke(temporary.Path(), {"create", "app", "disabled", "--platform", "windows", "--agent", "none"});
  REQUIRE(disabled.result == 0);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "disabled/.agents"));
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "disabled/.claude"));
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "disabled/.zcode"));
}

TEST_CASE("HuxerUICliCreatesCommonOnlyLibrariesWithRunnablePreviews") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "library", "AudioTools"});

  REQUIRE(invocation.result == 0);
  REQUIRE(invocation.output.find("Library platforms: none") != std::string::npos);
  REQUIRE(invocation.output.find("Preview platforms: android windows linux macos ios web") != std::string::npos);
  const std::filesystem::path library = temporary.Path() / "AudioTools";
  REQUIRE(std::filesystem::is_regular_file(library / "CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(library / "HuxerUIProject.cmake"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/HuxerUIProject.cmake"));
  REQUIRE_FALSE(std::filesystem::exists(library / "platform"));
  const huxerui::cli::Project preview =
      huxerui::cli::ResolveApplicationProject(huxerui::cli::DiscoverProject(library));
  REQUIRE(preview.platforms == std::vector<std::string>{"android", "ios", "linux", "macos", "web", "windows"});
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/ios/Config/Base.xcconfig"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/linux/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/macos/Info.plist.in"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/web/index.html.in"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/windows/main.cpp"));

  const Invocation platform_add = Invoke(library, {"platform", "add", "android,macos"});
  REQUIRE(platform_add.result == 0);
  REQUIRE(std::filesystem::is_regular_file(library / "platform/android/build.gradle"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/macos/src/.gitkeep"));

  const Invocation duplicate = Invoke(library, {"platform", "add", "android,macos"});
  REQUIRE(duplicate.result == 1);
  const Invocation partial = Invoke(library, {"platform", "add", "windows,macos"});
  REQUIRE(partial.result == 0);
  REQUIRE(std::filesystem::is_regular_file(library / "platform/windows/src/.gitkeep"));
}

TEST_CASE("HuxerUICliRefusesInvalidAndExistingDestinations") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "../invalid"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "camera--kit"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--id", "Invalid.ID"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"create", "sample"}).result == 2);

  const Invocation first = Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"});
  REQUIRE(first.result == 0);
  const std::string cmake = Read(temporary.Path() / "sample/CMakeLists.txt");

  const Invocation second = Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "android"});
  REQUIRE(second.result == 1);
  REQUIRE(Read(temporary.Path() / "sample/CMakeLists.txt") == cmake);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "sample/platform/android"));
}

TEST_CASE("HuxerUICliAddsMissingPlatformsFromNestedProjectDirectories") {
  TemporaryDirectory temporary;
  REQUIRE(
      Invoke(temporary.Path(), {"create", "app", "sample", "--id", "dev.example.custom", "--platform", "windows"})
          .result == 0
  );
  const std::filesystem::path nested = temporary.Path() / "sample/src/nested";
  std::filesystem::create_directories(nested);
  std::filesystem::remove_all(temporary.Path() / "sample/.huxerui");

  const Invocation invocation = Invoke(nested, {"platform", "add", "all"});

  REQUIRE(invocation.result == 0);
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/macos/Info.plist.in"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/macos/AppIcon.icns"));
  REQUIRE(Read(temporary.Path() / "sample/platform/macos/AppIcon.icns").starts_with("icns"));
  REQUIRE(Read(temporary.Path() / "sample/platform/macos/Info.plist.in").find("CFBundleIconFile") !=
          std::string::npos);
  REQUIRE(Read(temporary.Path() / "sample/platform/macos/huxerui.cmake").find("MACOSX_PACKAGE_LOCATION Resources") !=
          std::string::npos);
  const auto ios_plist = Read(temporary.Path() / "sample/platform/ios/App/Info.plist");
  REQUIRE(ios_plist.find("UIApplicationSceneManifest") != std::string::npos);
  REQUIRE(ios_plist.find("<key>UIApplicationSupportsMultipleScenes</key>\n    <false/>") != std::string::npos);
  const std::filesystem::path ios_app_icons =
      temporary.Path() / "sample/platform/ios/App/Assets.xcassets/AppIcon.appiconset";
  REQUIRE(std::filesystem::is_regular_file(ios_app_icons / "Contents.json"));
  REQUIRE(std::filesystem::is_regular_file(ios_app_icons / "AppIcon-1024.png"));
  REQUIRE(Read(ios_app_icons / "Contents.json").find("AppIcon-1024.png") != std::string::npos);
  REQUIRE(ReadBinary(ios_app_icons / "AppIcon-1024.png").starts_with(std::string("\x89PNG\r\n\x1a\n", 8)));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/ios/sample.xcodeproj/project.pbxproj"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/web/index.html.in"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/web/favicon.svg"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/web/apple-touch-icon.png"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/linux/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/linux/huxerui.cmake"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/linux/package/AppRun"));
  REQUIRE(
      Read(temporary.Path() / "sample/platform/android/app/build.gradle").find("dev.example.custom") !=
      std::string::npos
  );
  REQUIRE(
      Read(temporary.Path() / "sample/platform/ios/Config/Base.xcconfig").find("dev.example.custom") !=
      std::string::npos
  );
  REQUIRE(Invoke(nested, {"platform", "add", "all"}).result == 1);
}

TEST_CASE("HuxerUICliHelpReadsTheSameWithAndWithoutATerminal") {
  // A terminal without colors still gets the help's exact text: styling only ever adds color.
  for (const std::vector<std::string_view>& arguments :
       {std::vector<std::string_view>{"--help"}, std::vector<std::string_view>{"build", "--help"}}) {
    std::istringstream answers;
    std::ostringstream prompts;
    huxerui::cli::prompt::LinePrompter prompter(answers, prompts);
    std::ostringstream styled;
    std::ostringstream plain;
    std::ostringstream error;
    std::istringstream input;
    REQUIRE(huxerui::cli::Run(arguments, ".", {}, input, styled, error, &prompter) == 0);
    REQUIRE(huxerui::cli::Run(arguments, ".", {}, input, plain, error, nullptr) == 0);
    REQUIRE(styled.str() == plain.str());
    REQUIRE(plain.str().find("Usage:\n") != std::string::npos);
  }
}
