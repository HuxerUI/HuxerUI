#include "runtime_test_support.h"

#include <huxerui_test_module/module.h>
#include <huxerui_test_module_resources.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

namespace huxerui::test {

namespace {

int observed_module_value = 0;
std::string observed_module_only_resource;
std::string observed_overridden_resource;

class ModulePackageResources final : public PlatformResources {
public:
  ResourceConfiguration Configuration() const override {
    return {};
  }

  RawAsset Read(std::string_view package_path) override {
    const std::filesystem::path path =
        std::filesystem::path(HUXERUI_MODULE_TEST_RESOURCE_PACKAGE) / std::string(package_path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return {};
    }
    const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return RawAsset::CopyBytes(std::as_bytes(std::span<const char>(contents.data(), contents.size())));
  }
};

View ModuleApp() {
  observed_module_value = UseService<huxerui_test_module::Service>()->value;
  observed_module_only_resource = UseRawResource(huxerui_test_module::raw::module_only_txt).ToString();
  observed_overridden_resource = UseRawResource(huxerui_test_module::raw::module_value_txt).ToString();
  return Text("module");
}

} // namespace

TEST_CASE("ModuleResourcesAreMergedAndExplicitRootHooksInstallServices") {
  observed_module_value = 0;
  observed_module_only_resource.clear();
  observed_overridden_resource.clear();
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back(huxerui_test_module::Install);
  REQUIRE(options.root_hooks.size() == 1);

  TestPlatform platform;
  ModulePackageResources resources;
  platform.platform_resources = &resources;
  Runtime runtime{ModuleApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  REQUIRE(ContainsText(runtime.BuildFrame(), "module"));
  REQUIRE(observed_module_value == 42);
  REQUIRE(observed_module_only_resource == "module only\n");
  REQUIRE(observed_overridden_resource == "application\n");
}

} // namespace huxerui::test
