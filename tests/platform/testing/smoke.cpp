#include "smoke.h"

#include <exception>
#include <memory>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <ui_testing_smoke_resources.h>

namespace {
using namespace huxerui;
using namespace huxerui::testing;

class PackageResources final : public PlatformResources {
public:
  explicit PackageResources(std::string root) : root_(std::move(root)) {}
  ResourceConfiguration Configuration() const override { return {}; }
  std::optional<InputStream> OpenRead(std::string_view path) override {
    auto result = File(root_ + "/" + std::string(path)).OpenRead();
    if (!result.Succeeded()) return std::nullopt;
    return std::move(result).Value();
  }
private:
  std::string root_;
};

View Content() {
  auto count = UseState(0);
  const auto raw = UseRawResource(ui_testing_smoke::raw::library_value_txt);
  return Column {
    Text(raw.ReadString()).Key("resource"),
    Text(std::to_string(count.Get())).Key("count"),
    Button("Increment").OnClick([count]() mutable { count = count.Get() + 1; }),
    Checkbox("Resource icon", true),
  };
}

void Require(bool result, const char* message) {
  if (!result) throw UiTestFailure(message);
}
} // namespace

std::string RunUiTestingSmoke(const std::string& package_root) noexcept {
  try {
    Application application(Content, {
      .show_debug_overlay = false,
      .root_hooks = {[](RootContext& root) {
        root.Layers().Attach({}, [] { return Text("Presentation layer").Key("layer"); });
      }},
    });
    UiTest ui(application, {.resource_provider = std::make_shared<PackageResources>(package_root)});
    Require(ui.Find(UiSelector::Key("resource")).One().text == "application\n", "HuxerUI resource smoke failed");
    Require(ui.Find(UiSelector::Key("layer")).Exists(), "HuxerUI presentation smoke failed");
    const auto before = ui.CaptureSnapshot();
    ui.Find(UiSelector::AllOf(UiSelector::Type<Button>(), UiSelector::Text("Increment"))).Tap();
    Require(ui.Find(UiSelector::Key("count")).One().text == "1", "HuxerUI input smoke failed");
    Require(ui.CaptureSnapshot().ToString() != before.ToString(), "HuxerUI capture smoke failed");
    return {};
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "HuxerUI testing smoke failed with a non-standard exception";
  }
}
