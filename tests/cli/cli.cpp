#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "platform.h"
#include "process_runner.h"
#include "project.h"
#include "sdk.h"
#include "template.h"

namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("huxerui-cli-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

struct Invocation {
  int result = 0;
  std::string output;
  std::string error;
};

Invocation Invoke(const std::filesystem::path& directory, std::initializer_list<std::string_view> arguments) {
  const std::vector<std::string_view> values(arguments);
  std::ostringstream output;
  std::ostringstream error;
  const int result = huxerui::cli::Run(values, directory, {}, output, error);
  return {result, output.str(), error.str()};
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

#if !defined(_WIN32)
bool IsExecutable(const std::filesystem::path& path) {
  const std::filesystem::perms permissions = std::filesystem::status(path).permissions();
  return (permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                         std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
}
#endif

TEST_CASE("HuxerUICliRendersEmbeddedTemplatePathsAndContents") {
  const huxerui::cli::ProjectTemplateContext context =
      huxerui::cli::MakeProjectTemplateContext("Sample-App", "dev.example.sample");

  const std::vector<huxerui::cli::GeneratedFile> files =
      huxerui::cli::RenderTemplateTree("platform/android/app", context);
  const auto activity = std::find_if(files.begin(), files.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "app/src/main/java/dev/example/sample/MainActivity.java";
  });

  REQUIRE(activity != files.end());
  REQUIRE(activity->content.find("package dev.example.sample;") != std::string::npos);
  REQUIRE(activity->content.find("@PROJECT_") == std::string::npos);
  REQUIRE_THROWS_WITH(
      huxerui::cli::RenderTemplateTree("project/module", context),
      "HuxerUI CLI template contains an unresolved replacement: @MODULE_PRODUCT_NAME@"
  );
  REQUIRE_THROWS_WITH(
      huxerui::cli::RenderTemplateTree("missing/templates", context),
      "HuxerUI CLI template directory is missing: missing/templates"
  );

  const std::vector<huxerui::cli::GeneratedFile> wrapper = huxerui::cli::CopyTemplateTree("platform/android/wrapper");
  const auto wrapper_jar = std::find_if(wrapper.begin(), wrapper.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "gradle/wrapper/gradle-wrapper.jar";
  });
  REQUIRE(wrapper_jar != wrapper.end());
  REQUIRE(wrapper_jar->content.starts_with("PK"));
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
  REQUIRE(std::filesystem::is_regular_file(project / "src/app.cpp"));
  REQUIRE(std::filesystem::is_directory(project / "resources/images"));
  REQUIRE(std::filesystem::is_directory(project / "resources/raw"));
  REQUIRE(std::filesystem::is_regular_file(project / "resources/strings/default.properties"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/windows/app.manifest"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradlew"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradlew.bat"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradle/wrapper/gradle-wrapper.jar"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradle/wrapper/gradle-wrapper.properties"));
#if !defined(_WIN32)
  REQUIRE(IsExecutable(project / "platform/android/gradlew"));
#endif
  REQUIRE(std::filesystem::is_regular_file(project / "platform/web/index.html.in"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/macos"));
  REQUIRE_FALSE(std::filesystem::exists(project / ".huxerui"));
  REQUIRE(Read(project / ".gitignore").find("/.huxerui/") != std::string::npos);
  const std::string web_html = Read(project / "platform/web/index.html.in");
  REQUIRE(web_html.find("<div id=\"huxerui-root\"></div>") != std::string::npos);
  REQUIRE(web_html.find("mountHuxerUI(\"#huxerui-root\")") != std::string::npos);
  REQUIRE(web_html.find("huxerui-canvas") == std::string::npos);
  const std::string cmake = Read(project / "CMakeLists.txt");
  REQUIRE(cmake.find("huxerui_add_app(sample_app") != std::string::npos);
  REQUIRE(cmake.find("src/*.cpp") != std::string::npos);
  REQUIRE(cmake.find("SOURCES\n            ${APP_SOURCE_FILES}") != std::string::npos);
  REQUIRE(cmake.find("RESOURCES\n            resources") != std::string::npos);
  REQUIRE(cmake.find("NO_CMAKE_FIND_ROOT_PATH") != std::string::npos);
  REQUIRE(cmake.find("\"id\": \"dev.example.sampleapp\"") != std::string::npos);
  REQUIRE(cmake.find("HUXERUI_MODULE_GRAPH_OUTPUT") != std::string::npos);
  REQUIRE(Read(project / "src/app.cpp").find("const Application application") != std::string::npos);
  REQUIRE(Read(project / "platform/windows/main.cpp").find("RunApplication()") != std::string::npos);
  const std::string android_settings = Read(project / "platform/android/settings.gradle");
  const std::string android_app = Read(project / "platform/android/app/build.gradle");
  const std::string android_properties = Read(project / "platform/android/gradle.properties");
  REQUIRE(android_settings.find("project(\":HuxerUI\").projectDir") != std::string::npos);
  REQUIRE(android_settings.find("huxeruiModuleGraph.modules.eachWithIndex") != std::string::npos);
  REQUIRE(android_settings.find("module.sourceRoot") != std::string::npos);
  REQUIRE(android_settings.find("providers.environmentVariable(\"HUXERUI_HOME\")") != std::string::npos);
  REQUIRE(android_settings.find("mavenCentral()") != std::string::npos);
  REQUIRE(android_app.find("implementation project(\":HuxerUI\")") != std::string::npos);
  REQUIRE(android_app.find("implementation project(module.projectPath)") != std::string::npos);
  REQUIRE(android_app.find("path = file(\"../../../CMakeLists.txt\")") != std::string::npos);
  REQUIRE(android_app.find("-DHUXERUI_HOME=${huxeruiHome.absolutePath}") != std::string::npos);
  REQUIRE(android_app.find("compileSdk = huxeruiCompileSdk") != std::string::npos);
  REQUIRE(android_properties.find("huxeruiBuildNative=false") != std::string::npos);
  const std::string wrapper_properties = Read(project / "platform/android/gradle/wrapper/gradle-wrapper.properties");
  REQUIRE(wrapper_properties.find("gradle-8.13-bin.zip") != std::string::npos);
  REQUIRE(
      wrapper_properties.find("distributionSha256Sum=20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78"
      ) != std::string::npos
  );
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/android/app/src/main/cpp/CMakeLists.txt"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/android/huxerui.cmake"));
  REQUIRE(std::filesystem::is_regular_file(
      project / "platform/android/app/src/main/java/dev/example/sampleapp/MainActivity.java"
  ));

  const huxerui::cli::PlatformDriver* android = huxerui::cli::FindPlatformDriver("android");
  REQUIRE(android != nullptr);
  REQUIRE(
      std::find(android->RequiredTools().begin(), android->RequiredTools().end(), "gradle") ==
      android->RequiredTools().end()
  );
  REQUIRE(android->Diagnose(project / "platform/android").empty());
  std::filesystem::remove(project / "platform/android/gradle/wrapper/gradle-wrapper.jar");
  const std::vector<huxerui::cli::Diagnostic> diagnostics = android->Diagnose(project / "platform/android");
  REQUIRE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const huxerui::cli::Diagnostic& diagnostic) {
    return diagnostic.error && diagnostic.message == "missing gradle/wrapper/gradle-wrapper.jar";
  }));
}

TEST_CASE("HuxerUICliCreatesModuleAndPreviewProjects") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(
      temporary.Path(),
      {
          "create",
          "module",
          "HuxerUI-CameraKit",
          "--id",
          "dev.example.camera.kit",
          "--platform",
          "android,ios,linux,windows",
      }
  );

  REQUIRE(invocation.result == 0);
  const std::filesystem::path module = temporary.Path() / "HuxerUI-CameraKit";
  const std::filesystem::path preview = module / "examples/preview";
  REQUIRE(std::filesystem::is_regular_file(module / "include/huxer_ui_camera_kit/huxer_ui_camera_kit.h"));
  REQUIRE(std::filesystem::is_regular_file(module / "src/huxer_ui_camera_kit.cpp"));
  REQUIRE(std::filesystem::is_directory(module / "resources/images"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/android/build.gradle"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/android/gradlew"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/android/gradle/wrapper/gradle-wrapper.jar"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/android/src/main/AndroidManifest.xml"));
  const std::string android_module = Read(module / "platform/android/build.gradle");
  REQUIRE(android_module.find("id \"com.android.library\"") != std::string::npos);
  REQUIRE(android_module.find("namespace = \"dev.example.camera.kit\"") != std::string::npos);
  REQUIRE(Read(module / "platform/android/settings.gradle").find("version \"8.13.2\"") != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(module / "platform/ios/Package.swift"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/ios/Sources/HuxerUICameraKit/HuxerUICameraKit.swift"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/linux/src/.gitkeep"));
  REQUIRE(std::filesystem::is_regular_file(module / "platform/windows/src/.gitkeep"));
  const std::string swift_package = Read(module / "platform/ios/Package.swift");
  REQUIRE(swift_package.find("name: \"HuxerUI-CameraKit\"") != std::string::npos);
  REQUIRE(swift_package.find("targets: [\"HuxerUICameraKit\"]") != std::string::npos);
  const std::string module_cmake = Read(module / "CMakeLists.txt");
  REQUIRE(module_cmake.find("huxerui_add_module(huxer_ui_camera_kit") != std::string::npos);
  REQUIRE(module_cmake.find("RESOURCES\n            resources") != std::string::npos);
  REQUIRE(module_cmake.find("RESOURCE_NAMESPACE\n            huxer_ui_camera_kit") != std::string::npos);
  REQUIRE(module_cmake.find("huxerui_add_resources") == std::string::npos);
  REQUIRE(
      module_cmake.find("add_library(HuxerUICameraKit::HuxerUICameraKit ALIAS huxer_ui_camera_kit)") !=
      std::string::npos
  );
  REQUIRE(module_cmake.find("platform/android/src/main/cpp/*.cpp") != std::string::npos);
  REQUIRE(module_cmake.find("platform/linux/src/*.cpp") != std::string::npos);
  REQUIRE(module_cmake.find("platform/windows/src/*.cpp") != std::string::npos);
  const std::string preview_cmake = Read(preview / "CMakeLists.txt");
  REQUIRE(preview_cmake.find("huxerui_add_app(example_huxer_ui_camera_kit") != std::string::npos);
  REQUIRE(preview_cmake.find("TARGET HuxerUICameraKit::HuxerUICameraKit") != std::string::npos);
  REQUIRE(preview_cmake.find("PATH \"${CMAKE_CURRENT_SOURCE_DIR}/../..\"") != std::string::npos);
  REQUIRE(Read(preview / "src/app.cpp").find("huxer_ui_camera_kit::Install") != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/gradlew"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/android/gradle/wrapper/gradle-wrapper.jar"));
  REQUIRE(
      std::filesystem::is_regular_file(preview / "platform/ios/example_huxer_ui_camera_kit.xcodeproj/project.pbxproj")
  );
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/linux/main.cpp"));
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/windows/main.cpp"));

  std::filesystem::remove_all(module / ".huxerui");
  std::filesystem::remove_all(preview / ".huxerui");
  const huxerui::cli::Project application =
      huxerui::cli::ResolveApplicationProject(huxerui::cli::DiscoverProject(module));
  REQUIRE(std::filesystem::equivalent(application.root, preview));
  REQUIRE(application.platforms == std::vector<std::string>{"android", "ios", "linux", "windows"});
  REQUIRE_FALSE(std::filesystem::exists(module / ".huxerui"));
  REQUIRE_FALSE(std::filesystem::exists(preview / ".huxerui"));

  const Invocation doctor = Invoke(module, {"doctor", "all"});
  REQUIRE(doctor.output.find("Project: " + preview.string()) != std::string::npos);
  REQUIRE(doctor.output.find("missing app/build.gradle") == std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(module / ".huxerui"));
  REQUIRE_FALSE(std::filesystem::exists(preview / ".huxerui"));
}

TEST_CASE("HuxerUICliCreatesCommonOnlyModulesWithoutPlatformShells") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "module", "AudioTools"});

  REQUIRE(invocation.result == 0);
  const std::filesystem::path module = temporary.Path() / "AudioTools";
  REQUIRE(std::filesystem::is_regular_file(module / "CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(module / "examples/preview/CMakeLists.txt"));
  REQUIRE_FALSE(std::filesystem::exists(module / "platform"));
  REQUIRE_FALSE(std::filesystem::exists(module / "examples/preview/platform"));
  const Invocation platform_add = Invoke(module, {"platform", "add", "android,macos"});
  REQUIRE(platform_add.result == 0);
  REQUIRE(std::filesystem::is_regular_file(module / "platform/android/build.gradle"));
  REQUIRE_FALSE(std::filesystem::exists(module / "platform/macos"));
  REQUIRE(std::filesystem::is_regular_file(module / "examples/preview/platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(module / "examples/preview/platform/macos/Info.plist.in"));

  const Invocation duplicate = Invoke(module, {"platform", "add", "windows,macos"});
  REQUIRE(duplicate.result == 1);
  REQUIRE_FALSE(std::filesystem::exists(module / "examples/preview/platform/windows"));
}

TEST_CASE("HuxerUICliRefusesInvalidAndExistingDestinations") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "../invalid"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"create", "module", "camera--kit"}).result == 2);
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
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/ios/App/Info.plist"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/ios/sample.xcodeproj/project.pbxproj"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/web/index.html.in"));
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/linux/main.cpp"));
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

TEST_CASE("HuxerUICliDoctorAcceptsAReorganizedSourceDirectory") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::rename(project / "src/app.cpp", project / "src/application.cpp");

  const Invocation invocation = Invoke(project, {"doctor", "windows"});

  REQUIRE(invocation.output.find("missing src") == std::string::npos);
}

TEST_CASE("HuxerUICliRejectsUnknownPlatformsAsUsageErrors") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "plan9"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("unknown platform: plan9") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "sample"));
}

TEST_CASE("HuxerUICliCreatesStableDesktopBuildCommands") {
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
      }
  );
  REQUIRE(
      commands[1].arguments ==
      std::vector<std::string>{
          "--build",
          context.build_directory.string(),
          "--config",
          "Release",
          "--parallel",
      }
  );
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
  REQUIRE(shell.size() == 1);
  REQUIRE(shell.front().path == "main.cpp");
  REQUIRE(shell.front().content.find("RunApplication()") != std::string::npos);
  REQUIRE(linux->ModuleGraphCommands(context).empty());

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
  std::ofstream(plan) << "{\n  \"artifact\": \"" << artifact.generic_string() << "\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> run_commands = linux->RunCommands(context);
  REQUIRE(run_commands.size() == 1);
  REQUIRE(run_commands[0].executable == artifact.string());
  REQUIRE(run_commands[0].arguments.empty());
  REQUIRE(std::filesystem::equivalent(run_commands[0].working_directory, artifact.parent_path()));
}

TEST_CASE("HuxerUICliCreatesAndroidBuildCommandsForSourceSdks") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* android = huxerui::cli::FindPlatformDriver("android");
  REQUIRE(android != nullptr);
  const std::filesystem::path sdk = temporary.Path() / "sdk";
  const huxerui::cli::PlatformCommandContext context{
      temporary.Path() / "sample",
      sdk,
      temporary.Path() / "sample/.huxerui/build/android/release",
      {},
      "release",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> module_commands = android->ModuleGraphCommands(context);
  const std::vector<huxerui::cli::ProcessCommand> commands = android->BuildCommands(context);

  REQUIRE(module_commands.size() == 1);
  REQUIRE(module_commands[0].executable == "cmake");
  REQUIRE(
      module_commands[0].arguments ==
      std::vector<std::string>{
          "-S",
          context.project_root.string(),
          "-B",
          (context.project_root / ".huxerui/build/module-graph").string(),
          "-DCMAKE_BUILD_TYPE=Debug",
          "-DHUXERUI_MODULE_GRAPH_ONLY=ON",
      }
  );
  REQUIRE(commands.size() == 1);
  const std::filesystem::path wrapper = context.project_root / "platform/android" /
                                        (huxerui::cli::CurrentHostId() == "windows" ? "gradlew.bat" : "gradlew");
  REQUIRE(commands[0].executable == wrapper.string());
  REQUIRE(commands[0].arguments == std::vector<std::string>{":app:assembleRelease"});
  REQUIRE(module_commands[0].working_directory == context.project_root);
  REQUIRE(commands[0].working_directory == context.project_root / "platform/android");

  huxerui::cli::PlatformCommandContext explicit_generator = context;
  explicit_generator.cmake_generator = "Ninja";
  REQUIRE_THROWS_WITH(
      android->ModuleGraphCommands(explicit_generator),
      "Android native builds do not use a CMake generator option"
  );
}

TEST_CASE("HuxerUICliUsesGradleMetadataToLaunchAndroidApplications") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* android = huxerui::cli::FindPlatformDriver("android");
  REQUIRE(android != nullptr);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path output = project / "platform/android/app/build/outputs/apk/debug";
  const std::filesystem::path metadata = output / "output-metadata.json";
  const std::filesystem::path apk = output / "sample-custom.apk";
  std::filesystem::create_directories(metadata.parent_path());
  std::ofstream(metadata) << "{\n"
                             "  \"applicationId\": \"dev.example.sample.debug\",\n"
                             "  \"elements\": [{\"outputFile\": \"sample-custom.apk\"}]\n"
                             "}\n";
  std::ofstream(apk) << "test\n";
  const huxerui::cli::PlatformCommandContext context{
      project,
      temporary.Path() / "sdk",
      project / ".huxerui/build/android/debug",
      {},
      "debug",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> commands = android->RunCommands(context);

  REQUIRE(commands.size() == 2);
  REQUIRE(commands[0].arguments == std::vector<std::string>{"install", "-r", apk.string()});
  REQUIRE(
      commands[1].arguments ==
      std::vector<std::string>{"shell", "am", "start", "-n", "dev.example.sample.debug/.MainActivity"}
  );
}

TEST_CASE("HuxerUICliCreatesWebBuildAndRunCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* web = huxerui::cli::FindPlatformDriver("web");
  REQUIRE(web != nullptr);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path build = project / ".huxerui/build/web/debug";
  const huxerui::cli::PlatformCommandContext context{
      project,
      temporary.Path() / "sdk",
      build,
      "Ninja",
      "debug",
      {},
  };

  const std::vector<huxerui::cli::ProcessCommand> build_commands = web->BuildCommands(context);

  REQUIRE(build_commands.size() == 2);
  REQUIRE(build_commands[0].executable == "emcmake");
  REQUIRE(
      build_commands[0].arguments ==
      std::vector<std::string>{
          "cmake",
          "-G",
          "Ninja",
          "-S",
          project.string(),
          "-B",
          build.string(),
          "-DCMAKE_BUILD_TYPE=Debug",
      }
  );
  REQUIRE(build_commands[1].executable == "cmake");

  const std::filesystem::path artifact = build / "sample.mjs";
  const std::filesystem::path entry = build / "sample.html";
  const std::filesystem::path plan = build / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(artifact) << "export default {};\n";
  std::ofstream(entry) << "<!doctype html>\n";
  std::ofstream(plan) << "{\n"
                         "  \"target\": \"sample\",\n"
                         "  \"artifact\": \""
                      << artifact.generic_string() << "\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> run_commands = web->RunCommands(context);

  REQUIRE(run_commands.size() == 1);
  REQUIRE(run_commands[0].executable == "emrun");
  REQUIRE(run_commands[0].arguments.size() == 1);
  REQUIRE(std::filesystem::equivalent(run_commands[0].arguments[0], entry));
  REQUIRE(std::filesystem::equivalent(run_commands[0].working_directory, build));
}

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
  REQUIRE(base_configuration->content.find("HEADER_SEARCH_PATHS") != std::string::npos);
  REQUIRE(base_configuration->content.find("@\"$(HUXERUI_LINK_OPTIONS_FILE)\"") != std::string::npos);
  REQUIRE(base_configuration->content.find("-framework UIKit") == std::string::npos);
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
  REQUIRE(xcode_project_file->content.find("../../.huxerui/generated/ios/modules") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("productName = HuxerUIModules") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("productName = \"Sample-App\"") != std::string::npos);
  REQUIRE(
      xcode_project_file->content.find("$HUXERUI_CORE_BUILD_DIR/huxerui-ios/sample_app/resources/package") !=
      std::string::npos
  );
  REQUIRE(xcode_project_file->content.find("HuxerUI resource package is missing") != std::string::npos);
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

  const std::vector<huxerui::cli::ProcessCommand> module_commands = ios->ModuleGraphCommands(context);
  const std::vector<huxerui::cli::ProcessCommand> build_commands = ios->BuildCommands(context);

  REQUIRE(module_commands.size() == 1);
  REQUIRE(module_commands[0].executable == "cmake");
  REQUIRE(
      module_commands[0].arguments ==
      std::vector<std::string>{
          "-S",
          context.project_root.string(),
          "-B",
          (context.project_root / ".huxerui/build/module-graph").string(),
          "-DCMAKE_BUILD_TYPE=Debug",
          "-DHUXERUI_MODULE_GRAPH_ONLY=ON",
      }
  );
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
          "HUXERUI_INTEGRATION_PLAN=" + (build / "huxerui-integration/app.json").string(),
          "build",
      }
  );

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

TEST_CASE("HuxerUICliGeneratesIosModuleIntegrationFromTheCommonGraph") {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path camera = temporary.Path() / "camera module";
  const std::filesystem::path maps = temporary.Path() / "maps";
  const std::filesystem::path common = temporary.Path() / "common";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::filesystem::create_directories(camera / "platform/ios");
  std::filesystem::create_directories(maps / "platform/ios");
  std::filesystem::create_directories(common);
  std::ofstream(camera / "platform/ios/Package.swift") << "// camera\n";
  std::ofstream(maps / "platform/ios/Package.swift") << "// maps\n";
  std::filesystem::create_directories(project / "platform/ios/Config");
  std::ofstream(project / ".huxerui/generated/modules.json")
      << "{\n  \"schema\": 1,\n  \"modules\": [\n"
      << "    {\"target\": \"HuxerUICameraKit::HuxerUICameraKit\", \"sourceRoot\": \"" << camera.generic_string()
      << "\"},\n"
      << "    {\"target\": \"CommonTools::CommonTools\", \"sourceRoot\": \"" << common.generic_string() << "\"},\n"
      << "    {\"target\": \"map_view\", \"sourceRoot\": \"" << maps.generic_string() << "\"}\n"
      << "  ]\n}\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{project, temporary.Path() / "sdk", {}, {}, "debug", {}};
  ios->UpdateProjectIntegration(context);

  const std::filesystem::path integration = project / ".huxerui/generated/ios/modules";
  const std::string manifest = Read(integration / "Package.swift");
  const std::size_t camera_dependency = manifest.find(".package(name: \"HuxerUICameraKit\"");
  const std::size_t maps_dependency = manifest.find(".package(name: \"MapView\"");
  REQUIRE(camera_dependency != std::string::npos);
  REQUIRE(maps_dependency != std::string::npos);
  REQUIRE(camera_dependency < maps_dependency);
  REQUIRE(manifest.find("HuxerUICameraKit") != std::string::npos);
  REQUIRE(manifest.find("MapView") != std::string::npos);
  REQUIRE(manifest.find("CommonTools") == std::string::npos);
  REQUIRE(manifest.find(camera.generic_string()) != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(integration / "Sources/HuxerUIModules/HuxerUIModules.swift"));
}

TEST_CASE("HuxerUICliRejectsIncompleteIosModulePackages") {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path module = temporary.Path() / "camera";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::filesystem::create_directories(module / "platform/ios");
  std::ofstream(project / ".huxerui/generated/modules.json")
      << "{\n  \"schema\": 1,\n  \"modules\": [\n"
      << "    {\"target\": \"CameraKit::CameraKit\", \"sourceRoot\": \"" << module.generic_string() << "\"}\n"
      << "  ]\n}\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{project, {}, {}, {}, "debug", {}};
  REQUIRE_THROWS_WITH(
      ios->UpdateProjectIntegration(context),
      Catch::Matchers::ContainsSubstring("iOS module package is missing Package.swift")
  );
}

TEST_CASE("HuxerUICliConfiguresIosHomeWithoutReplacingLocalSigningSettings") {
  TemporaryDirectory temporary;
  const std::filesystem::path configuration = temporary.Path() / "platform/ios/Config/Local.xcconfig";
  const std::filesystem::path huxerui_home = temporary.Path() / "installed sdk";
  std::filesystem::create_directories(configuration.parent_path());
  std::filesystem::create_directories(temporary.Path() / ".huxerui/generated");
  std::ofstream(temporary.Path() / ".huxerui/generated/modules.json") << "{\"schema\":1,\"modules\":[]}";
  std::ofstream(configuration) << "DEVELOPMENT_TEAM = ABC123\nHUXERUI_HOME = /old/sdk\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{temporary.Path(), huxerui_home, {}, {}, "debug", {}};
  ios->UpdateProjectIntegration(context);

  const std::string content = Read(configuration);
  REQUIRE(content.find("DEVELOPMENT_TEAM = ABC123") != std::string::npos);
  REQUIRE(content.find("HUXERUI_HOME = " + huxerui_home.generic_string()) != std::string::npos);
  REQUIRE(content.find("/old/sdk") == std::string::npos);
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

TEST_CASE("HuxerUICliDescribesProcessArgumentsWithoutShellEvaluation") {
  const huxerui::cli::ProcessCommand command{
      "tool",
      {"plain", "with space", "quoted\"value"},
      {},
  };

  REQUIRE(huxerui::cli::DescribeProcess(command) == R"(tool plain "with space" "quoted\"value")");
}

TEST_CASE("HuxerUICliCapturesProcessOutput") {
  TemporaryDirectory temporary;
  const huxerui::cli::ProcessResult result =
      huxerui::cli::RunProcessCapture({"cmake", {"-E", "echo", "captured output"}, temporary.Path()});

  REQUIRE(result.exit_code == 0);
  REQUIRE(result.output.find("captured output") != std::string::npos);
}

TEST_CASE("HuxerUICliParsesAndroidDeviceStates") {
  const std::vector<huxerui::cli::PlatformDevice> devices = huxerui::cli::ParseAdbDevices(
      "List of devices attached\r\n"
      "emulator-5554 device product:sdk_phone model:Pixel_8 device:emu64x transport_id:1\r\n"
      "R58M offline transport_id:2\r\n"
      "ABC unauthorized usb:1-2 transport_id:3\r\n"
      "???????????? no permissions (user in plugdev group)\r\n"
  );

  REQUIRE(
      devices ==
      std::vector<huxerui::cli::PlatformDevice>{
          {"emulator-5554", "Pixel_8", huxerui::cli::DeviceState::Ready, huxerui::cli::DeviceKind::Unspecified, {}},
          {"R58M", {}, huxerui::cli::DeviceState::Offline, huxerui::cli::DeviceKind::Unspecified, {}},
          {"ABC", {}, huxerui::cli::DeviceState::Unauthorized, huxerui::cli::DeviceKind::Unspecified, {}},
          {"????????????", {}, huxerui::cli::DeviceState::Unavailable, huxerui::cli::DeviceKind::Unspecified, {}},
      }
  );
}

TEST_CASE("HuxerUICliRejectsDeviceDiscoveryForDesktopPlatforms") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"devices", "windows"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.error.find("platform does not support device discovery: windows") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsDeviceSelectionForDesktopRunsBeforeBuilding") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::vector<std::string_view> arguments{"run", "windows", "--device", "phone"};
  std::ostringstream output;
  std::ostringstream error;

  const int result = huxerui::cli::Run(
      arguments,
      project,
      {temporary.Path(), huxerui::cli::SdkLocationSource::Executable},
      output,
      error
  );

  REQUIRE(result == 2);
  REQUIRE(error.str().find("--device is not supported for platform windows") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliRejectsUnknownProjectPlatformsBeforeRun") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::create_directories(project / "platform/custom");
  const std::vector<std::string_view> arguments{"run", "windows"};
  std::ostringstream output;
  std::ostringstream error;

  const int result = huxerui::cli::Run(
      arguments,
      project,
      {temporary.Path(), huxerui::cli::SdkLocationSource::Executable},
      output,
      error
  );

  REQUIRE(result == 1);
  REQUIRE(error.str().find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliDoctorReportsTheResolvedSdk") {
  TemporaryDirectory temporary;
  const std::vector<std::string_view> arguments{"doctor"};
  std::ostringstream output;
  std::ostringstream error;

  const int result = huxerui::cli::Run(
      arguments,
      temporary.Path(),
      {temporary.Path(), huxerui::cli::SdkLocationSource::Environment},
      output,
      error
  );

  REQUIRE(result == 0);
  REQUIRE(output.str().find("[ok] HUXERUI_HOME (environment): " + temporary.Path().string()) != std::string::npos);
  REQUIRE(error.str().empty());
}

TEST_CASE("HuxerUICliDoctorChecksRequestedPlatformsOutsideAProject") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"doctor", "android"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("Project: not found") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android:") != std::string::npos);
}

#if defined(_WIN32)
TEST_CASE("HuxerUICliRunsWindowsBatchTools") {
  TemporaryDirectory temporary;
  const std::filesystem::path batch = temporary.Path() / "return seven.cmd";
  std::ofstream(batch) << "@exit /b 7\n";

  REQUIRE(huxerui::cli::RunProcess({batch.string(), {}, temporary.Path()}) == 7);
}
#endif

} // namespace
