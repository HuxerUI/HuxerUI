#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
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
  std::istringstream input("n\n");
  huxerui::cli::SdkLocation sdk;
  if (!values.empty() && values.front() == "create") {
    sdk = {HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable};
  }
  const int result = huxerui::cli::Run(values, directory, sdk, input, output, error);
  return {result, output.str(), error.str()};
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  std::erase(content, '\r');
  return content;
}

std::vector<std::string> ExpectedLibraryGraphArguments(const huxerui::cli::PlatformCommandContext& context) {
  std::vector<std::string> arguments{
      "-S",
      context.project_root.string(),
      "-B",
      (context.project_root / ".huxerui/build/library-graph").string(),
      "-DCMAKE_BUILD_TYPE=Debug",
      "-DHUXERUI_LIBRARY_GRAPH_ONLY=ON",
      "-DHUXERUI_HOME=" + context.huxerui_home.string(),
  };
  if (!huxerui::cli::ReadEnvironmentVariable("CMAKE_GENERATOR") && huxerui::cli::FindExecutable("ninja")) {
    arguments.insert(arguments.begin(), {"-G", "Ninja"});
  }
  return arguments;
}

#if !defined(_WIN32)
bool IsExecutable(const std::filesystem::path& path) {
  const std::filesystem::perms permissions = std::filesystem::status(path).permissions();
  return (permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                         std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
}
#endif

#if !defined(_WIN32)
TEST_CASE("HuxerUICliExecutableSearchSkipsInaccessiblePathEntries") {
  TemporaryDirectory temporary;
  const std::filesystem::path inaccessible = temporary.Path() / "inaccessible";
  const std::filesystem::path available = temporary.Path() / "available";
  const std::filesystem::path executable = available / "huxerui-test-tool";
  std::filesystem::create_directories(inaccessible);
  std::filesystem::create_directories(available);
  std::ofstream(executable) << "#!/bin/sh\n";
  std::filesystem::permissions(executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  std::filesystem::permissions(inaccessible, std::filesystem::perms::none);

  const std::optional<std::string> old_path = huxerui::cli::ReadEnvironmentVariable("PATH");
  REQUIRE(old_path);
  std::optional<std::filesystem::path> resolved;
  try {
    huxerui::cli::SetProcessEnvironmentVariable("PATH", inaccessible.string() + ":" + available.string());
    resolved = huxerui::cli::FindExecutable(executable.filename().string());
  } catch (...) {
    std::filesystem::permissions(inaccessible, std::filesystem::perms::owner_all);
    huxerui::cli::SetProcessEnvironmentVariable("PATH", *old_path);
    throw;
  }
  std::filesystem::permissions(inaccessible, std::filesystem::perms::owner_all);
  huxerui::cli::SetProcessEnvironmentVariable("PATH", *old_path);

  REQUIRE(resolved);
  REQUIRE(std::filesystem::equivalent(*resolved, executable));
}
#endif

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
}

TEST_CASE("HuxerUICliValidatesExplicitSourceCheckouts") {
  const std::filesystem::path source = huxerui::cli::ResolveHuxerUISource(HUXERUI_TEST_SOURCE_DIRECTORY);
  REQUIRE(std::filesystem::equivalent(source, HUXERUI_TEST_SOURCE_DIRECTORY));

  TemporaryDirectory temporary;
  REQUIRE_THROWS_AS(huxerui::cli::ResolveHuxerUISource(temporary.Path()), std::runtime_error);
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

TEST_CASE("HuxerUICliRendersEmbeddedTemplatePathsAndContents") {
  const huxerui::cli::ProjectTemplateContext context =
      huxerui::cli::MakeProjectTemplateContext("Sample-App", "dev.example.sample");

  const std::vector<huxerui::cli::GeneratedFile> files = huxerui::cli::RenderTemplateTree(
      "platform/android/app",
      context,
      std::array{
          huxerui::cli::TemplateReplacement{"@ANDROID_AGP_VERSION@", "8.13.2"},
          huxerui::cli::TemplateReplacement{"@ANDROID_COMPILE_SDK@", "36"},
          huxerui::cli::TemplateReplacement{"@ANDROID_MIN_COMPILE_SDK@", "23"},
          huxerui::cli::TemplateReplacement{"@ANDROID_MIN_SDK@", "23"},
          huxerui::cli::TemplateReplacement{"@ANDROID_TARGET_SDK@", "36"},
          huxerui::cli::TemplateReplacement{"@ANDROID_NDK_VERSION@", "29.0.14206865"},
      }
  );
  const auto activity = std::find_if(files.begin(), files.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "app/src/main/java/dev/example/sample/MainActivity.java";
  });

  REQUIRE(activity != files.end());
  REQUIRE(activity->content.find("package dev.example.sample;") != std::string::npos);
  REQUIRE(activity->content.find("@PROJECT_") == std::string::npos);
  REQUIRE_THROWS_WITH(
      huxerui::cli::RenderTemplateTree("project/library", context),
      "HuxerUI CLI template contains an unresolved replacement: @LIBRARY_NAMESPACE@"
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
  REQUIRE(std::filesystem::is_regular_file(
      project / ".agents/skills/huxerui-app-development/SKILL.md"
  ));
  REQUIRE(std::filesystem::is_regular_file(
      project / ".agents/skills/huxerui-app-development/references/project-workflow.md"
  ));
  REQUIRE(std::filesystem::is_regular_file(
      project / ".agents/skills/huxerui-app-development/references/resources-files-network.md"
  ));
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
  REQUIRE(cmake.find("HUXERUI_LIBRARY_GRAPH_OUTPUT") != std::string::npos);
  REQUIRE(cmake.find("project(sample_app VERSION 0.1.0 LANGUAGES NONE)") != std::string::npos);
  REQUIRE(cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(cmake.find("if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)\n    enable_language(CXX)") != std::string::npos);
  REQUIRE(cmake.find("set(CMAKE_CXX_STANDARD") == std::string::npos);
  REQUIRE(cmake.find("set(HUXERUI_BUILD_SHARED ON CACHE BOOL \"\" FORCE)") != std::string::npos);
  REQUIRE(cmake.find("set(HUXERUI_BUILD_STATIC OFF CACHE BOOL \"\" FORCE)") != std::string::npos);
  REQUIRE(cmake.find("RESOURCE_OUTPUT_DIRECTORY") != std::string::npos);
  const std::string application = Read(project / "src/app.cpp");
  REQUIRE(application.find("const Application application") != std::string::npos);
  REQUIRE(application.find("MaterialTheme") == std::string::npos);
  REQUIRE(Read(project / "platform/windows/main.cpp").find("RunApplication()") != std::string::npos);
  const std::string android_settings = Read(project / "platform/android/settings.gradle");
  const std::string android_app = Read(project / "platform/android/app/build.gradle");
  const std::string android_properties = Read(project / "platform/android/gradle.properties");
  REQUIRE(android_settings.find("project(\":HuxerUI\").projectDir") != std::string::npos);
  REQUIRE(android_settings.find("share/huxerui/platform/android/HuxerUI.aar") != std::string::npos);
  REQUIRE(android_settings.find("huxeruiUsesSource") != std::string::npos);
  REQUIRE(android_settings.find("huxeruiLibraryGraph.libraries.eachWithIndex") != std::string::npos);
  REQUIRE(android_settings.find("library.sourceRoot") != std::string::npos);
  REQUIRE(android_settings.find("providers.environmentVariable(\"HUXERUI_HOME\")") != std::string::npos);
  REQUIRE(android_settings.find("mavenCentral()") != std::string::npos);
  REQUIRE(android_app.find("implementation project(\":HuxerUI\")") != std::string::npos);
  REQUIRE(android_app.find("implementation files(rootProject.gradle.ext.huxeruiAndroidArchive)") != std::string::npos);
  REQUIRE(android_app.find("implementation project(library.projectPath)") != std::string::npos);
  REQUIRE(android_app.find("path = file(\"../../../CMakeLists.txt\")") != std::string::npos);
  REQUIRE(android_app.find("-DHUXERUI_HOME=${huxeruiHome.absolutePath}") != std::string::npos);
  REQUIRE(android_app.find("compileSdk = huxeruiCompileSdk") != std::string::npos);
  REQUIRE(android_app.find("HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT") != std::string::npos);
  REQUIRE(android_app.find("huxeruiAbis") != std::string::npos);
  REQUIRE(android_app.find(".cxx") == std::string::npos);
  REQUIRE(android_app.find("lastModified") == std::string::npos);
  REQUIRE(android_properties.find("huxeruiBuildNative=false") != std::string::npos);
  REQUIRE(android_properties.find("huxeruiCompileSdk=36") != std::string::npos);
  REQUIRE(android_properties.find("huxeruiNdkVersion=29.0.14206865") != std::string::npos);
  const std::string wrapper_properties = Read(project / "platform/android/gradle/wrapper/gradle-wrapper.properties");
  REQUIRE(wrapper_properties.find("gradle-8.13-bin.zip") != std::string::npos);
  REQUIRE(
      wrapper_properties.find(
          "distributionSha256Sum=20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78"
      ) != std::string::npos
  );
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/android/app/src/main/cpp/CMakeLists.txt"));
  REQUIRE_FALSE(std::filesystem::exists(project / "platform/android/huxerui.cmake"));
  REQUIRE(std::filesystem::is_regular_file(
      project / "platform/android/app/src/main/java/dev/example/sampleapp/MainActivity.java"
  ));

  const huxerui::cli::PlatformDriver* android = huxerui::cli::FindPlatformDriver("android");
  REQUIRE(android != nullptr);
  REQUIRE(android->Diagnose(project / "platform/android").empty());
  std::filesystem::remove(project / "platform/android/gradle/wrapper/gradle-wrapper.jar");
  const std::vector<huxerui::cli::Diagnostic> diagnostics = android->Diagnose(project / "platform/android");
  REQUIRE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const huxerui::cli::Diagnostic& diagnostic) {
    return diagnostic.error && diagnostic.message == "missing gradle/wrapper/gradle-wrapper.jar";
  }));
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
          "android,ios,linux,windows",
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
  REQUIRE(std::filesystem::is_regular_file(library / "platform/ios/Package.swift"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/ios/Sources/HuxerUICameraKit/HuxerUICameraKit.swift"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/linux/src/.gitkeep"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/windows/src/.gitkeep"));
  const std::string swift_package = Read(library / "platform/ios/Package.swift");
  REQUIRE(swift_package.find("name: \"HuxerUI-CameraKit\"") != std::string::npos);
  REQUIRE(swift_package.find(".iOS(.v15)") != std::string::npos);
  REQUIRE(swift_package.find("targets: [\"HuxerUICameraKit\"]") != std::string::npos);
  const std::string library_cmake = Read(library / "CMakeLists.txt");
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
  REQUIRE(library_cmake.find("platform/web/src/*.cpp") != std::string::npos);
  REQUIRE(library_cmake.find("platform/windows/src/*.cpp") != std::string::npos);
  REQUIRE(library_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(library_cmake.find("set(CMAKE_CXX_STANDARD") == std::string::npos);
  const std::string preview_cmake = Read(preview / "CMakeLists.txt");
  REQUIRE(preview_cmake.find("huxerui_add_app(example_huxer_ui_camera_kit") != std::string::npos);
  REQUIRE(preview_cmake.find("TARGET HuxerUICameraKit::HuxerUICameraKit") != std::string::npos);
  REQUIRE(preview_cmake.find("PATH \"${CMAKE_CURRENT_SOURCE_DIR}/../..\"") != std::string::npos);
  REQUIRE(preview_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
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
  REQUIRE(std::filesystem::is_regular_file(preview / "platform/windows/main.cpp"));

  std::filesystem::remove_all(library / ".huxerui");
  std::filesystem::remove_all(preview / ".huxerui");
  const huxerui::cli::Project application =
      huxerui::cli::ResolveApplicationProject(huxerui::cli::DiscoverProject(library));
  REQUIRE(std::filesystem::equivalent(application.root, preview));
  REQUIRE(application.platforms == std::vector<std::string>{"android", "ios", "linux", "windows"});
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
  REQUIRE(library_cmake.find("\"schema\": 1") != std::string::npos);
  REQUIRE(library_cmake.find("\"namespace\": \"scave::camera\"") != std::string::npos);
  REQUIRE(library_cmake.find("\"publicTarget\": \"Scave::Camera\"") != std::string::npos);
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

  const Invocation platform_add = Invoke(library, {"platform", "add", "android,ios"});
  REQUIRE(platform_add.result == 0);
  REQUIRE(Read(library / "platform/android/build.gradle").find("namespace = \"dev.example.camera\"") !=
          std::string::npos);
  const std::string swift_package = Read(library / "platform/ios/Package.swift");
  REQUIRE(swift_package.find("name: \"CameraKit\"") != std::string::npos);
  REQUIRE(swift_package.find("targets: [\"Camera\"]") != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(library / "platform/ios/Sources/Camera/Camera.swift"));
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
  REQUIRE(Invoke(temporary.Path(), {"create", "library", "ReservedTarget", "--target", "HuxerUI::Camera"})
              .result == 2);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "ReservedTarget"));
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

TEST_CASE("HuxerUICliCreatesCommonOnlyLibrariesWithoutPlatformShells") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "library", "AudioTools"});

  REQUIRE(invocation.result == 0);
  const std::filesystem::path library = temporary.Path() / "AudioTools";
  REQUIRE(std::filesystem::is_regular_file(library / "CMakeLists.txt"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/CMakeLists.txt"));
  REQUIRE_FALSE(std::filesystem::exists(library / "platform"));
  REQUIRE_FALSE(std::filesystem::exists(library / "examples/preview/platform"));
  const Invocation platform_add = Invoke(library, {"platform", "add", "android,macos"});
  REQUIRE(platform_add.result == 0);
  REQUIRE(std::filesystem::is_regular_file(library / "platform/android/build.gradle"));
  REQUIRE_FALSE(std::filesystem::exists(library / "platform/macos"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(library / "examples/preview/platform/macos/Info.plist.in"));

  const Invocation duplicate = Invoke(library, {"platform", "add", "windows,macos"});
  REQUIRE(duplicate.result == 1);
  REQUIRE_FALSE(std::filesystem::exists(library / "examples/preview/platform/windows"));
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
  REQUIRE(std::filesystem::equivalent(run_commands[0].executable, artifact));
  REQUIRE(run_commands[0].arguments.empty());
  REQUIRE(std::filesystem::equivalent(run_commands[0].working_directory, artifact.parent_path()));

  const std::filesystem::path resources = artifact.parent_path() / "sample.resources";
  std::filesystem::create_directories(resources);
  const std::vector<huxerui::cli::PackageArtifact> package_artifacts = linux->PackageArtifacts(context);
  const std::vector<huxerui::cli::PackageArtifact> expected_package_artifacts{
      {artifact, artifact.filename()},
      {resources, resources.filename()},
  };
  REQUIRE(package_artifacts == expected_package_artifacts);
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

  const std::vector<huxerui::cli::ProcessCommand> library_commands =
      huxerui::cli::detail::AndroidLibraryGraphCommands(context, "windows");
  const std::vector<huxerui::cli::ProcessCommand> windows_commands =
      huxerui::cli::detail::AndroidBuildCommands(context, "windows", {});
  const std::vector<huxerui::cli::ProcessCommand> posix_commands =
      huxerui::cli::detail::AndroidBuildCommands(context, "linux", {});

  REQUIRE(library_commands.size() == 1);
  REQUIRE(library_commands[0].executable == "cmake");
  REQUIRE(library_commands[0].arguments == ExpectedLibraryGraphArguments(context));
  REQUIRE(windows_commands.size() == 1);
  REQUIRE(
      std::filesystem::path(windows_commands[0].executable).generic_string() ==
      (context.project_root / "platform/android/gradlew.bat").generic_string()
  );
  REQUIRE(windows_commands[0].arguments == std::vector<std::string>{":app:assembleRelease"});
  REQUIRE(posix_commands.size() == 1);
  REQUIRE(
      std::filesystem::path(posix_commands[0].executable).generic_string() ==
      (context.project_root / "platform/android/gradlew").generic_string()
  );
  REQUIRE(posix_commands[0].arguments == std::vector<std::string>{":app:assembleRelease"});
  REQUIRE(library_commands[0].working_directory == context.project_root);
  REQUIRE(windows_commands[0].working_directory == context.project_root / "platform/android");
  REQUIRE(posix_commands[0].working_directory == context.project_root / "platform/android");

  const std::vector<huxerui::cli::ProcessCommand> termux_library_commands =
      huxerui::cli::detail::AndroidLibraryGraphCommands(context, "android");
  std::vector<std::string> termux_library_arguments = ExpectedLibraryGraphArguments(context);
  termux_library_arguments.push_back("-DANDROID_ABI=arm64-v8a");
  REQUIRE(termux_library_commands.size() == 1);
  REQUIRE(termux_library_commands[0].arguments == termux_library_arguments);

  const std::filesystem::path termux_aapt2 = "/data/data/com.termux/files/usr/bin/aapt2";
  const std::vector<huxerui::cli::ProcessCommand> termux_commands =
      huxerui::cli::detail::AndroidBuildCommands(context, "android", termux_aapt2);
  REQUIRE(termux_commands.size() == 1);
  REQUIRE(
      std::filesystem::path(termux_commands[0].executable).generic_string() ==
      (context.project_root / "platform" / "android" / "gradlew").generic_string()
  );
  REQUIRE(
      termux_commands[0].arguments ==
      std::vector<std::string>{
          "-PhuxeruiAbis=arm64-v8a",
          "-Pandroid.aapt2FromMavenOverride=" + termux_aapt2.string(),
          ":app:assembleRelease",
      }
  );

  huxerui::cli::PlatformCommandContext explicit_generator = context;
  explicit_generator.cmake_generator = "Ninja";
  REQUIRE_THROWS_WITH(
      android->LibraryGraphCommands(explicit_generator),
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

  const std::vector<huxerui::cli::ProcessCommand> commands =
      huxerui::cli::detail::AndroidRunCommands(context, "windows");

  REQUIRE(commands.size() == 2);
  REQUIRE(commands[0].arguments.size() == 3);
  REQUIRE(commands[0].arguments[0] == "install");
  REQUIRE(commands[0].arguments[1] == "-r");
  REQUIRE(std::filesystem::equivalent(commands[0].arguments[2], apk));
  REQUIRE(
      commands[1].arguments ==
      std::vector<std::string>{"shell", "am", "start", "-n", "dev.example.sample.debug/.MainActivity"}
  );

  const std::vector<huxerui::cli::ProcessCommand> termux_commands =
      huxerui::cli::detail::AndroidRunCommands(context, "android");
  REQUIRE(termux_commands.size() == 1);
  REQUIRE(termux_commands[0].executable == "termux-open");
  REQUIRE(termux_commands[0].arguments.size() == 4);
  REQUIRE(termux_commands[0].arguments[0] == "--view");
  REQUIRE(termux_commands[0].arguments[1] == "--content-type");
  REQUIRE(termux_commands[0].arguments[2] == "application/vnd.android.package-archive");
  REQUIRE(std::filesystem::equivalent(termux_commands[0].arguments[3], apk));
  const std::vector<huxerui::cli::PackageArtifact> package_artifacts = android->PackageArtifacts(context);
  REQUIRE(package_artifacts.size() == 1);
  const huxerui::cli::PackageArtifact expected_package_artifact{apk, apk.filename()};
  REQUIRE(package_artifacts.front() == expected_package_artifact);
}

TEST_CASE("HuxerUICliCreatesWebBuildAndRunCommands") {
  TemporaryDirectory temporary;
  const huxerui::cli::PlatformDriver* web = huxerui::cli::FindPlatformDriver("web");
  REQUIRE(web != nullptr);
  const std::vector<huxerui::cli::GeneratedFile> shell =
      web->CreateShell(huxerui::cli::MakeProjectTemplateContext("Sample-App"));
  const auto configuration = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "huxerui.cmake";
  });
  REQUIRE(configuration != shell.end());
  REQUIRE(configuration->content.find("${target_name}.js") != std::string::npos);
  REQUIRE(configuration->content.find(".mjs") == std::string::npos);
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
          "-DHUXERUI_HOME=" + context.huxerui_home.string(),
      }
  );
  REQUIRE(build_commands[1].executable == "cmake");

  std::filesystem::create_directories(build);
  std::ofstream(build / "CMakeCache.txt") << "CMAKE_GENERATOR:INTERNAL=Ninja\n";
  huxerui::cli::PlatformCommandContext cached_context = context;
  cached_context.cmake_generator.clear();
  const std::vector<huxerui::cli::ProcessCommand> cached_build_commands = web->BuildCommands(cached_context);
  REQUIRE(cached_build_commands[0].arguments == build_commands[0].arguments);

  const std::filesystem::path artifact = build / "sample.js";
  const std::filesystem::path entry = build / "sample.html";
  const std::filesystem::path module = build / "sample.wasm";
  const std::filesystem::path plan = build / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(artifact) << "export default {};\n";
  std::ofstream(entry) << "<!doctype html>\n";
  std::ofstream(module) << "wasm\n";
  std::ofstream(plan) << "{\n"
                         "  \"target\": \"sample\",\n"
                         "  \"artifact\": \""
                      << artifact.generic_string() << "\"\n}\n";

  const std::vector<huxerui::cli::ProcessCommand> windows_run_commands =
      huxerui::cli::detail::WebRunCommands(context, "windows");

  REQUIRE(windows_run_commands.size() == 1);
  REQUIRE(windows_run_commands[0].executable == "emrun");
  REQUIRE(windows_run_commands[0].arguments.size() == 3);
  REQUIRE(windows_run_commands[0].arguments[0] == "--browser");
  REQUIRE(windows_run_commands[0].arguments[1] == "explorer.exe");
  REQUIRE(std::filesystem::equivalent(windows_run_commands[0].arguments[2], entry));
  REQUIRE(std::filesystem::equivalent(windows_run_commands[0].working_directory, build));

  const std::vector<huxerui::cli::ProcessCommand> posix_run_commands =
      huxerui::cli::detail::WebRunCommands(context, "linux");
  REQUIRE(posix_run_commands.size() == 1);
  REQUIRE(posix_run_commands[0].executable == "emrun");
  REQUIRE(posix_run_commands[0].arguments.size() == 1);
  REQUIRE(std::filesystem::equivalent(posix_run_commands[0].arguments[0], entry));
  REQUIRE(std::filesystem::equivalent(posix_run_commands[0].working_directory, build));

  const std::vector<huxerui::cli::ProcessCommand> termux_run_commands =
      huxerui::cli::detail::WebRunCommands(context, "android");
  REQUIRE(termux_run_commands.size() == 1);
  REQUIRE(termux_run_commands[0].executable == "python");
  REQUIRE(termux_run_commands[0].arguments.size() == 3);
  REQUIRE(termux_run_commands[0].arguments[0] == "-c");
  REQUIRE(termux_run_commands[0].arguments[1].find("ThreadingHTTPServer") != std::string::npos);
  REQUIRE(termux_run_commands[0].arguments[1].find("127.0.0.1") != std::string::npos);
  REQUIRE(termux_run_commands[0].arguments[1].find("termux-open") != std::string::npos);
  REQUIRE(termux_run_commands[0].arguments[1].find("serve_forever") != std::string::npos);
  REQUIRE(termux_run_commands[0].arguments[2] == entry.filename());
  REQUIRE(std::filesystem::equivalent(termux_run_commands[0].working_directory, build));

  const std::vector<huxerui::cli::PackageArtifact> package_artifacts = web->PackageArtifacts(context);
  REQUIRE(package_artifacts.size() == 3);
  REQUIRE(std::any_of(package_artifacts.begin(), package_artifacts.end(), [&module](const auto& packaged) {
    return packaged.source == module && packaged.destination == module.filename();
  }));
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
  REQUIRE(base_configuration->content.find("IPHONEOS_DEPLOYMENT_TARGET = 15.0") != std::string::npos);
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
  REQUIRE(xcode_project_file->content.find("../../.huxerui/generated/ios/libraries") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("productName = HuxerUILibraries") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("productName = \"Sample-App\"") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("Scripts/build_huxerui_core.sh") != std::string::npos);
  REQUIRE(xcode_project_file->content.find("Scripts/stage_huxerui_resources.sh") != std::string::npos);
  const auto build_script = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "Scripts/build_huxerui_core.sh";
  });
  REQUIRE(build_script != shell.end());
  REQUIRE(build_script->content.find("sample_app_huxerui_ios_core") != std::string::npos);
  const auto resource_script = std::find_if(shell.begin(), shell.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "Scripts/stage_huxerui_resources.sh";
  });
  REQUIRE(resource_script != shell.end());
  REQUIRE(
      resource_script->content.find("$HUXERUI_CORE_BUILD_DIR/huxerui-ios/sample_app/resources/package") !=
      std::string::npos
  );
  REQUIRE(resource_script->content.find("HuxerUI resource package is missing") != std::string::npos);
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

  const std::vector<huxerui::cli::ProcessCommand> library_commands = ios->LibraryGraphCommands(context);
  const std::vector<huxerui::cli::ProcessCommand> build_commands = ios->BuildCommands(context);

  REQUIRE(library_commands.size() == 1);
  REQUIRE(library_commands[0].executable == "cmake");
  REQUIRE(library_commands[0].arguments == ExpectedLibraryGraphArguments(context));
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

TEST_CASE("HuxerUICliGeneratesIosLibraryIntegrationFromTheCommonGraph") {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path camera = temporary.Path() / "camera library";
  const std::filesystem::path maps = temporary.Path() / "maps";
  const std::filesystem::path common = temporary.Path() / "common";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::filesystem::create_directories(camera / "platform/ios");
  std::filesystem::create_directories(maps / "platform/ios");
  std::filesystem::create_directories(common);
  std::ofstream(camera / "platform/ios/Package.swift") << "// camera\n";
  std::ofstream(maps / "platform/ios/Package.swift") << "// maps\n";
  std::filesystem::create_directories(project / "platform/ios/Config");
  std::ofstream(project / ".huxerui/generated/libraries.json")
      << "{\n  \"schema\": 1,\n  \"libraries\": [\n"
      << "    {\"target\": \"HuxerUICameraKit::HuxerUICameraKit\", \"sourceRoot\": \"" << camera.generic_string()
      << "\"},\n"
      << "    {\"target\": \"CommonTools::CommonTools\", \"sourceRoot\": \"" << common.generic_string() << "\"},\n"
      << "    {\"target\": \"map_view\", \"sourceRoot\": \"" << maps.generic_string() << "\"}\n"
      << "  ]\n}\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{project, temporary.Path() / "sdk", {}, {}, "debug", {}};
  ios->UpdateProjectIntegration(context);

  const std::filesystem::path integration = project / ".huxerui/generated/ios/libraries";
  const std::string manifest = Read(integration / "Package.swift");
  const std::size_t camera_dependency = manifest.find(".package(name: \"HuxerUICameraKit\"");
  const std::size_t maps_dependency = manifest.find(".package(name: \"MapView\"");
  REQUIRE(camera_dependency != std::string::npos);
  REQUIRE(maps_dependency != std::string::npos);
  REQUIRE(camera_dependency < maps_dependency);
  REQUIRE(manifest.find(".iOS(.v15)") != std::string::npos);
  REQUIRE(manifest.find("HuxerUICameraKit") != std::string::npos);
  REQUIRE(manifest.find("MapView") != std::string::npos);
  REQUIRE(manifest.find("CommonTools") == std::string::npos);
  REQUIRE(manifest.find(camera.generic_string()) != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(integration / "Sources/HuxerUILibraries/HuxerUILibraries.swift"));
}

TEST_CASE("HuxerUICliRejectsIncompleteIosLibraryPackages") {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path library = temporary.Path() / "camera";
  std::filesystem::create_directories(project / ".huxerui/generated");
  std::filesystem::create_directories(library / "platform/ios");
  std::ofstream(project / ".huxerui/generated/libraries.json")
      << "{\n  \"schema\": 1,\n  \"libraries\": [\n"
      << "    {\"target\": \"CameraKit::CameraKit\", \"sourceRoot\": \"" << library.generic_string() << "\"}\n"
      << "  ]\n}\n";

  const huxerui::cli::PlatformDriver* ios = huxerui::cli::FindPlatformDriver("ios");
  REQUIRE(ios != nullptr);
  const huxerui::cli::PlatformCommandContext context{project, {}, {}, {}, "debug", {}};
  REQUIRE_THROWS_WITH(
      ios->UpdateProjectIntegration(context),
      Catch::Matchers::ContainsSubstring("iOS library package is missing Package.swift")
  );
}

TEST_CASE("HuxerUICliConfiguresIosHomeWithoutReplacingLocalSigningSettings") {
  TemporaryDirectory temporary;
  const std::filesystem::path configuration = temporary.Path() / "platform/ios/Config/Local.xcconfig";
  const std::filesystem::path huxerui_home = temporary.Path() / "installed sdk";
  std::filesystem::create_directories(configuration.parent_path());
  std::filesystem::create_directories(temporary.Path() / ".huxerui/generated");
  std::ofstream(temporary.Path() / ".huxerui/generated/libraries.json") << "{\"schema\":1,\"libraries\":[]}";
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

  REQUIRE(huxerui::cli::DescribeProcess(command) == "tool plain \"with space\" \"quoted\\\"value\"");
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
  const std::vector<std::string_view> arguments{"build", "windows", "--java-home", "jdk"};
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

TEST_CASE("HuxerUICliValidatesJavaHomeOverridesBeforeAndroidBuilds") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "android"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path java_home = project / "jdk";
  std::filesystem::create_directories(java_home);
  const std::vector<std::string_view> arguments{"build", "android", "--java-home", "jdk"};
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

  REQUIRE(result == 1);
#if defined(_WIN32)
  REQUIRE(error.str().find("Java home does not contain bin/java.exe") != std::string::npos);
#else
  REQUIRE(error.str().find("Java home does not contain bin/java") != std::string::npos);
#endif
  REQUIRE(error.str().find(java_home.string()) != std::string::npos);
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

TEST_CASE("HuxerUICliDoctorReportsTheResolvedSdk") {
  TemporaryDirectory temporary;
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
  const huxerui::cli::PlatformDriver* android = huxerui::cli::FindPlatformDriver("android");
  REQUIRE(android != nullptr);
  const std::vector<huxerui::cli::EnvironmentDiagnostic> android_diagnostics = android->DiagnoseEnvironment();
  const auto has_android_diagnostic = [&android_diagnostics](std::string_view id) {
    return std::any_of(
        android_diagnostics.begin(),
        android_diagnostics.end(),
        [id](const huxerui::cli::EnvironmentDiagnostic& diagnostic) { return diagnostic.id == id; }
    );
  };
  REQUIRE(has_android_diagnostic("java"));
  const bool android_host = huxerui::cli::CurrentHostId() == "android";
  REQUIRE(has_android_diagnostic("sdkmanager") != android_host);
  REQUIRE(has_android_diagnostic("platform_tools") != android_host);
  REQUIRE(has_android_diagnostic("aapt2") == android_host);
  REQUIRE(has_android_diagnostic("termux-open") == android_host);
  REQUIRE_FALSE(has_android_diagnostic("cmake"));
  REQUIRE_FALSE(has_android_diagnostic("gradle"));

  if (huxerui::cli::CurrentHostId() == "linux") {
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
    REQUIRE(has_linux_diagnostic("pkg:gio-2.0"));
    REQUIRE(has_linux_diagnostic("pkg:libsoup-3.0"));
    REQUIRE_FALSE(has_linux_diagnostic("pkg:x11"));
    REQUIRE_FALSE(has_linux_diagnostic("pkg:egl"));
    REQUIRE_FALSE(has_linux_diagnostic("pkg:glesv2"));
    REQUIRE_FALSE(has_linux_diagnostic("meson"));
    REQUIRE_FALSE(has_linux_diagnostic("ninja"));
    REQUIRE_FALSE(has_linux_diagnostic("gperf"));
    REQUIRE_FALSE(has_linux_diagnostic("git"));
  }

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

#if defined(_WIN32)
TEST_CASE("HuxerUICliRunsWindowsBatchTools") {
  TemporaryDirectory temporary;
  const std::filesystem::path batch = temporary.Path() / "return seven.cmd";
  std::ofstream(batch) << "@exit /b 7\n";

  REQUIRE(huxerui::cli::RunProcess({batch.string(), {}, temporary.Path()}) == 7);
}
#endif

} // namespace
