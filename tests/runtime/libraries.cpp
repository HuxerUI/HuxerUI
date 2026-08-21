#include "runtime_test_support.h"

#include <huxerui_test_library/library.h>
#include <huxerui_test_library_resources.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

namespace huxerui::test {

namespace {

int observed_library_value = 0;
std::string observed_library_only_resource;
std::string observed_overridden_resource;

class LibraryPackageResources final : public PlatformResources {
public:
  ResourceConfiguration Configuration() const override {
    return {};
  }

  RawAsset Read(std::string_view package_path) override {
    const std::filesystem::path path =
        std::filesystem::path(HUXERUI_LIBRARY_TEST_RESOURCE_PACKAGE) / std::string(package_path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return {};
    }
    const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return RawAsset::CopyBytes(std::as_bytes(std::span<const char>(contents.data(), contents.size())));
  }
};

View LibraryApp() {
  observed_library_value = UseService<huxerui_test_library::Service>()->value;
  observed_library_only_resource = UseRawResource(huxerui_test_library::raw::library_only_txt).ToString();
  observed_overridden_resource = UseRawResource(huxerui_test_library::raw::library_value_txt).ToString();
  return Text("library");
}

} // namespace

TEST_CASE("LibraryResourcesAreMergedAndExplicitRootHooksInstallServices") {
  observed_library_value = 0;
  observed_library_only_resource.clear();
  observed_overridden_resource.clear();
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back(huxerui_test_library::Install);
  REQUIRE(options.root_hooks.size() == 1);

  TestPlatform platform;
  LibraryPackageResources resources;
  platform.platform_resources = &resources;
  Runtime runtime{LibraryApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  REQUIRE(ContainsText(runtime.BuildFrame(), "library"));
  REQUIRE(observed_library_value == 42);
  REQUIRE(observed_library_only_resource == "library only\n");
  REQUIRE(observed_overridden_resource == "application\n");
}

} // namespace huxerui::test
