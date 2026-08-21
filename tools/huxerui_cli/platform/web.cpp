#include "platform.h"

#include <array>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

class WebDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "web";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"cmake"},
        std::string_view{"emcmake"},
        std::string_view{"emrun"},
    };
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/web/app", context);
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"huxerui.cmake"},
        std::string_view{"index.html.in"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::string configuration = detail::ProfileConfiguration(context.profile);
    std::vector<std::string> configure_arguments{
        "cmake",
        "-S",
        context.project_root.string(),
        "-B",
        context.build_directory.string(),
        "-DCMAKE_BUILD_TYPE=" + configuration,
    };
    if (!context.cmake_generator.empty()) {
      configure_arguments.insert(configure_arguments.begin() + 1, {"-G", context.cmake_generator});
    }
    return {
        {"emcmake", std::move(configure_arguments), context.project_root},
        {"cmake",
         {"--build", context.build_directory.string(), "--config", configuration, "--parallel"},
         context.project_root},
    };
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string target = detail::JsonString(plan, "target");
    const std::filesystem::path artifact = detail::JsonString(plan, "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("Web build artifact is missing: " + artifact.string());
    }
    const std::filesystem::path entry = artifact.parent_path() / (target + ".html");
    if (!std::filesystem::is_regular_file(entry)) {
      throw std::runtime_error("Web entry file is missing: " + entry.string());
    }
    return {{"emrun", {entry.string()}, artifact.parent_path()}};
  }
};

} // namespace

namespace detail {

const PlatformDriver& WebPlatformDriver() noexcept {
  static const WebDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
