#include "platform.h"

#include <array>
#include <iterator>
#include <stdexcept>

#include "template.h"

namespace huxerui::cli {
namespace {

class WindowsDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "windows";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"cmake"}};
    return tools;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/windows/app", context);
    files.insert(
        files.end(),
        std::make_move_iterator(platform_files.begin()),
        std::make_move_iterator(platform_files.end())
    );
    return files;
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext&) const override {
    return {{"src/.gitkeep", {}}};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"main.cpp"},
        std::string_view{"huxerui.cmake"},
        std::string_view{"app.manifest"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return detail::DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("Windows build artifact is missing: " + artifact);
    }
    return {{artifact, {}, std::filesystem::path(artifact).parent_path()}};
  }
};

} // namespace

namespace detail {

const PlatformDriver& WindowsPlatformDriver() noexcept {
  static const WindowsDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
