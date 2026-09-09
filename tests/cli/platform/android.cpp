#include "support.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliCreatesAndroidPlatformShell") {
  TemporaryDirectory temporary;
  const auto invocation = Invoke(temporary.Path(),
      {"create", "app", "Sample-App", "--id", "dev.example.sampleapp", "--platform", "android"});
  REQUIRE(invocation.result == 0);
  const auto project = temporary.Path() / "Sample-App";
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/settings.gradle"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradlew"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradlew.bat"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradle/wrapper/gradle-wrapper.jar"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/android/gradle/wrapper/gradle-wrapper.properties"));
  const std::filesystem::path android_resource_root = project / "platform/android/app/src/main/res";
  REQUIRE(std::filesystem::is_regular_file(android_resource_root / "drawable/ic_launcher_foreground.xml"));
  REQUIRE(std::filesystem::is_regular_file(android_resource_root / "mipmap-anydpi-v26/ic_launcher.xml"));
  REQUIRE(std::filesystem::is_regular_file(android_resource_root / "mipmap-xxxhdpi/ic_launcher.png"));
#if !defined(_WIN32)
  REQUIRE(IsExecutable(project / "platform/android/gradlew"));
#endif
  const std::string android_settings = Read(project / "platform/android/settings.gradle");
  const std::string android_app = Read(project / "platform/android/app/build.gradle");
  const std::string android_manifest = Read(project / "platform/android/app/src/main/AndroidManifest.xml");
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
  REQUIRE(android_app.find("buildConfig = true") != std::string::npos);
  REQUIRE(android_app.find("lintOptions {") != std::string::npos);
  REQUIRE(android_app.find("checkReleaseBuilds = false") != std::string::npos);
  REQUIRE(android_app.find("abortOnError = false") != std::string::npos);
  REQUIRE(android_app.find("HUXERUI_ANDROID_APP_INTEGRATION_ROOT") != std::string::npos);
  REQUIRE(android_app.find("variant.buildConfigFields.put(\"HUXERUI_APP_LIBRARY\", resolveLibrary.map") !=
          std::string::npos);
  REQUIRE(android_app.find("variant.artifacts.get(SingleArtifact.MERGED_NATIVE_LIBS.INSTANCE)") != std::string::npos);
  REQUIRE(android_app.find("inputs.dir(nativeLibraries)") != std::string::npos);
  REQUIRE(android_app.find("new File(integrationRoot, \"${abiDirectory.name}/app.json\")") != std::string::npos);
  REQUIRE(android_app.find("new JsonSlurper().parse(planFile)") != std::string::npos);
  REQUIRE(android_app.find("must match across Android ABIs") != std::string::npos);
  REQUIRE(android_app.find("new File(abiDirectory, fileName).isFile()") != std::string::npos);
  REQUIRE(android_app.find("variant.sources.assets.addGeneratedSourceDirectory(stageAssets)") != std::string::npos);
  REQUIRE(android_app.find("packageDirectories.from(packagedAbis.map") != std::string::npos);
  REQUIRE(android_app.find("assets.srcDir") == std::string::npos);
  REQUIRE(android_app.find("registerHuxerUIResourceStaging") == std::string::npos);
  REQUIRE(android_app.find("huxerui_app") == std::string::npos);
  REQUIRE(android_app.find("huxeruiAbis") != std::string::npos);
  REQUIRE(android_app.find(".cxx") == std::string::npos);
  REQUIRE(android_app.find("lastModified") == std::string::npos);
  REQUIRE(android_manifest.find("android:icon=\"@mipmap/ic_launcher\"") != std::string::npos);
  REQUIRE(android_manifest.find("android:roundIcon=\"@mipmap/ic_launcher\"") != std::string::npos);
  REQUIRE(
      ReadBinary(android_resource_root / "mipmap-xxxhdpi/ic_launcher.png")
          .starts_with(std::string("\x89PNG\r\n\x1a\n", 8))
  );
  REQUIRE(android_properties.find("huxeruiBuildNative=false") != std::string::npos);
  REQUIRE(android_properties.find("huxeruiAppLibrary") == std::string::npos);
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
  }));}

TEST_CASE("HuxerUICliAndroidEnvironmentDiagnosisOwnsToolChecks") {
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

}

TEST_CASE("HuxerUICliPreservesAndroidLocalSettings") {
  using namespace huxerui::cli;
  TemporaryDirectory temporary;
  const auto local = temporary.Path() / "platform/android/local.properties";
  PlatformCommandContext context;
  context.project_root = temporary.Path();
  context.huxerui_home = HUXERUI_TEST_SOURCE_DIRECTORY;
  const auto* android = FindPlatformDriver("android");
  android->UpdateProjectIntegration(context);
  REQUIRE_FALSE(std::filesystem::exists(local));

  std::filesystem::create_directories(local.parent_path());
  const std::string original = "sdk.dir=D:/Android/Sdk\n";
  std::ofstream(local) << original;
  const auto timestamp = std::filesystem::last_write_time(local);
  android->UpdateProjectIntegration(context);
  REQUIRE(Read(local) == original);
  // Keep Catch2 from formatting filesystem clocks with a non-streamable representation.
  REQUIRE((std::filesystem::last_write_time(local) == timestamp));
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

TEST_CASE("HuxerUICliValidatesJavaHomeOverridesBeforeAndroidBuilds") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "android"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::filesystem::path java_home = project / "jdk";
  std::filesystem::create_directories(java_home);
  const std::vector<std::string_view> arguments{"build", "android", "--java-home", "jdk", "--source", HUXERUI_TEST_SOURCE_DIRECTORY};
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

TEST_CASE("HuxerUICliRendersAndroidTemplatePathsAndContents") {
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
  REQUIRE(activity->content.find("System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY)") != std::string::npos);
  REQUIRE(activity->content.find("@PROJECT_") == std::string::npos);
  const std::vector<huxerui::cli::GeneratedFile> wrapper = huxerui::cli::CopyTemplateTree("platform/android/wrapper");
  const auto wrapper_jar = std::find_if(wrapper.begin(), wrapper.end(), [](const huxerui::cli::GeneratedFile& file) {
    return file.path == "gradle/wrapper/gradle-wrapper.jar";
  });
  REQUIRE(wrapper_jar != wrapper.end());
  REQUIRE(wrapper_jar->content.starts_with("PK"));
}
