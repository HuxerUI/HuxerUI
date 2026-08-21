#include "platform.h"

#include <array>
#include <stdexcept>

#include "template.h"

namespace huxerui::cli {
namespace {

class LinuxDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "linux";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    return {};
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/desktop/app", context);
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext&) const override {
    return {{"src/.gitkeep", {}}};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{std::string_view{"main.cpp"}};
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return detail::DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("HuxerUI Linux build artifact is missing: " + artifact);
    }
    return {{artifact, {}, std::filesystem::path(artifact).parent_path()}};
  }
};

} // namespace

namespace detail {

const PlatformDriver& LinuxPlatformDriver() noexcept {
  static const LinuxDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
