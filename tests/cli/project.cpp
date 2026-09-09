#include "support.h"

using namespace huxerui::cli::test;

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
  REQUIRE(project_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(project_cmake.find("if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)\n    enable_language(CXX)") !=
          std::string::npos);
  REQUIRE(project_cmake.find("enable_language(RC)") != std::string::npos);
  REQUIRE(project_cmake.find("\"${HUXERUI_WINDOWS_RESOURCE}\"") != std::string::npos);
  REQUIRE(project_cmake.find("set(HUXERUI_BUILD_SHARED ON") != std::string::npos);
  REQUIRE(project_cmake.find("set(HUXERUI_BUILD_STATIC OFF") != std::string::npos);
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
  REQUIRE(std::filesystem::is_regular_file(library / "platform/ios/Package.swift"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/ios/Sources/HuxerUICameraKit/HuxerUICameraKit.swift"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/linux/src/.gitkeep"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/macos/src/.gitkeep"));
  REQUIRE(std::filesystem::is_regular_file(library / "platform/windows/src/.gitkeep"));
  const std::string swift_package = Read(library / "platform/ios/Package.swift");
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
  REQUIRE(library_project_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
  REQUIRE(library_project_cmake.find("enable_language(OBJCXX)") != std::string::npos);
  REQUIRE(library_project_cmake.find("\"publicTarget\": \"HuxerUICameraKit::HuxerUICameraKit\"") !=
          std::string::npos);
  REQUIRE(library_cmake.find("set(CMAKE_CXX_STANDARD") == std::string::npos);
  const std::string preview_cmake = Read(preview / "CMakeLists.txt");
  const std::string preview_project_cmake = Read(preview / "HuxerUIProject.cmake");
  REQUIRE(preview_cmake.find("huxerui_add_app(example_huxer_ui_camera_kit") != std::string::npos);
  REQUIRE(preview_cmake.find("TARGET HuxerUICameraKit::HuxerUICameraKit") != std::string::npos);
  REQUIRE(preview_cmake.find("PATH \"${CMAKE_CURRENT_SOURCE_DIR}/../..\"") != std::string::npos);
  REQUIRE(preview_project_cmake.find("CMAKE_OSX_DEPLOYMENT_TARGET \"12.0\"") != std::string::npos);
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
  REQUIRE(std::filesystem::is_regular_file(temporary.Path() / "sample/platform/ios/App/Info.plist"));
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
