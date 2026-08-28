#include "runtime_test_support.h"

#include <optional>

#include "image_test_support.h"

namespace huxerui::test {
namespace {

std::optional<SystemTrayHandle> tray_handle;
int tray_compositions = 0;
int tray_activations = 0;
int tray_commands = 0;
bool tray_available = false;

ImageAsset TrayIcon() {
  return ImageAsset::FromEncoded(MakeTestPng(16, 16));
}

VectorAsset TrayVectorIcon() {
  return VectorAsset::Create({16.0F, 16.0F}, [](VectorBuilder&) {});
}

View SystemTrayApp() {
  const auto application = UseApplication();
  const auto tray = application.SystemTray();
  tray_handle = tray;
  tray_available = tray.IsAvailable();
  ++tray_compositions;
  tray.OnActivate([] { ++tray_activations; });
  Lifecycle([tray] {
    tray.Show(
        TrayIcon(),
        SystemTrayOptions{
            .tooltip = "HuxerUI test",
            .menu = {MenuItem("Run", [] { ++tray_commands; })},
        }
    );
    return [tray] { tray.Hide(); };
  });
  return {};
}

View ApplicationOnlyApp() {
  static_cast<void>(UseApplication());
  return {};
}

void ResetSystemTrayState() {
  tray_handle.reset();
  tray_compositions = 0;
  tray_activations = 0;
  tray_commands = 0;
  tray_available = false;
}

} // namespace

TEST_CASE("System tray retains its desired presentation until the transport becomes available") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  Runtime runtime(SystemTrayApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(tray_handle.has_value());
  REQUIRE_FALSE(tray_available);
  REQUIRE(platform.system_tray_transport->show_count == 0);

  platform.system_tray_transport->SetAvailable(true);
  REQUIRE(platform.system_tray_transport->show_count == 1);
  REQUIRE(platform.system_tray_transport->presentation.has_value());
  REQUIRE(platform.system_tray_transport->presentation->tooltip == "HuxerUI test");

  runtime.BuildFrame();
  REQUIRE(tray_available);
  REQUIRE(tray_compositions == 2);
}

TEST_CASE("System tray transport remains disconnected until the application uses the tray") {
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  {
    Runtime runtime(ApplicationOnlyApp, platform);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();

    REQUIRE(platform.system_tray_transport->event_handler_updates == 0);
    REQUIRE_FALSE(platform.system_tray_transport->event_handler);
  }
  REQUIRE(platform.system_tray_transport->event_handler_updates == 0);
}

TEST_CASE("System tray rejects stale platform commands after replacing its presentation") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  Runtime runtime(SystemTrayApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto first = *platform.system_tray_transport->presentation;
  const auto& first_item = first.menu.front();
  tray_handle->Show(
      TrayIcon(),
      SystemTrayOptions{
          .tooltip = "Replacement",
          .menu = {MenuItem("Replace", [] { tray_commands += 10; })},
      }
  );
  const auto second = *platform.system_tray_transport->presentation;
  const auto& second_item = second.menu.front();

  platform.system_tray_transport->Invoke(first.generation, first_item.command);
  REQUIRE(tray_commands == 0);
  platform.system_tray_transport->Invoke(second.generation, second_item.command);
  REQUIRE(tray_commands == 10);
}

TEST_CASE("System tray activation and lifecycle cleanup remain shared-service owned") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  {
    Runtime runtime(SystemTrayApp, platform);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();
    platform.system_tray_transport->Activate();
    REQUIRE(tray_activations == 1);
  }

  REQUIRE(platform.system_tray_transport->hide_count >= 1);
  REQUIRE_FALSE(tray_handle->IsAvailable());
  platform.system_tray_transport->Activate();
  REQUIRE(tray_activations == 1);
}

TEST_CASE("Hidden system tray presentations reject activation and commands") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  Runtime runtime(SystemTrayApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto presentation = *platform.system_tray_transport->presentation;
  tray_handle->Hide();
  platform.system_tray_transport->Activate();
  platform.system_tray_transport->Invoke(presentation.generation, presentation.menu.front().command);

  REQUIRE(tray_activations == 0);
  REQUIRE(tray_commands == 0);
}

TEST_CASE("System tray rejects vector icons without replacing its raster presentation") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  Runtime runtime(SystemTrayApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto original = platform.system_tray_transport->presentation;
  REQUIRE(original.has_value());
  REQUIRE_THROWS_AS(tray_handle->Show(TrayVectorIcon()), std::invalid_argument);
  REQUIRE(platform.system_tray_transport->presentation->generation == original->generation);
  REQUIRE(platform.system_tray_transport->presentation->tooltip == original->tooltip);
  REQUIRE_THROWS_AS(
      tray_handle->Show(TrayIcon(), SystemTrayOptions{.menu = {MenuItem(TrayVectorIcon(), "Vector", [] {})}}),
      std::invalid_argument
  );
  REQUIRE(platform.system_tray_transport->presentation->generation == original->generation);
  REQUIRE(platform.system_tray_transport->presentation->tooltip == original->tooltip);
}

} // namespace huxerui::test
