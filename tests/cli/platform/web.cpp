#include "support.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliCreatesWebPlatformShell") {
  TemporaryDirectory temporary;
  const auto invocation = Invoke(temporary.Path(),
      {"create", "app", "Sample-App", "--id", "dev.example.sampleapp", "--platform", "web"});
  REQUIRE(invocation.result == 0);
  const auto project = temporary.Path() / "Sample-App";
  REQUIRE(std::filesystem::is_regular_file(project / "platform/web/index.html.in"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/web/favicon.svg"));
  REQUIRE(std::filesystem::is_regular_file(project / "platform/web/apple-touch-icon.png"));
  const std::string web_html = Read(project / "platform/web/index.html.in");
  REQUIRE(web_html.find("<div id=\"huxerui-root\"></div>") != std::string::npos);
  REQUIRE(web_html.find("rel=\"icon\"") != std::string::npos);
  REQUIRE(web_html.find("rel=\"apple-touch-icon\"") != std::string::npos);
  REQUIRE(web_html.find("mountHuxerUI(\"#huxerui-root\")") != std::string::npos);
  REQUIRE(web_html.find("huxerui-canvas") == std::string::npos);
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
  REQUIRE(configuration->content.find("${target_name}.favicon.svg") != std::string::npos);
  REQUIRE(configuration->content.find("${target_name}.apple-touch-icon.png") != std::string::npos);
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
  const std::array web_icon_files{
      build / "sample.favicon.svg",
      build / "sample.apple-touch-icon.png",
  };
  const std::filesystem::path plan = build / "huxerui-integration/sample/Debug/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(artifact) << "export default {};\n";
  std::ofstream(entry) << "<!doctype html>\n";
  std::ofstream(module) << "wasm\n";
  for (const std::filesystem::path& icon : web_icon_files) {
    std::ofstream(icon) << "icon\n";
  }
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
  REQUIRE(package_artifacts.size() == 5);
  REQUIRE(std::any_of(package_artifacts.begin(), package_artifacts.end(), [&module](const auto& packaged) {
    return packaged.source == module && packaged.destination == module.filename();
  }));
  for (const std::filesystem::path& icon : web_icon_files) {
    REQUIRE(std::any_of(package_artifacts.begin(), package_artifacts.end(), [&icon](const auto& packaged) {
      return packaged.source == icon && packaged.destination == icon.filename();
    }));
  }
}
