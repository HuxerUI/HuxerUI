#include "runtime_test_support.h"

#include <future>
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
  return {};
}

void InstallSystemTray(ApplicationContext&) {
  const auto tray = UseApplication().SystemTray();
  tray.OnActivate([] { ++tray_activations; });
  tray.Show(TrayIcon(), {
      .tooltip = "HuxerUI test",
      .menu = {MenuItem("Run", [] { ++tray_commands; })},
  });
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
  UiWindow runtime(SystemTrayApp, platform, {.application_hooks = {InstallSystemTray}});
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(tray_handle.has_value());
  REQUIRE_FALSE(tray_available);
  REQUIRE(platform.system_tray_transport->show_count == 0);

  platform.system_tray_transport->SetAvailable(true);
  platform.RunPlatformModuleTasks();
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
    UiWindow runtime(ApplicationOnlyApp, platform);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();

    REQUIRE(platform.system_tray_transport->event_handler_updates == 0);
    REQUIRE_FALSE(platform.system_tray_transport->event_handler);
  }
  REQUIRE(platform.system_tray_transport->event_handler_updates == 0);
}

TEST_CASE("System tray rejects worker-thread operations before native side effects") {
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  const auto transport = platform.system_tray_transport;
  transport->available = true;
  Application application(ApplicationOnlyApp, {.show_debug_overlay = false});
  std::optional<SystemTrayHandle> retained;
  {
    RuntimeLifetime runtime(application, platform);
    const auto tray = UseApplication().SystemTray();
    retained = tray;
    auto available = std::async(std::launch::async, [&] { return tray.IsAvailable(); });
    REQUIRE_THROWS_AS(available.get(), std::logic_error);
    REQUIRE(transport->event_handler_updates == 0);
    auto activation = std::async(std::launch::async, [&] { tray.OnActivate([] {}); });
    REQUIRE_THROWS_AS(activation.get(), std::logic_error);
    REQUIRE(transport->event_handler_updates == 0);
    REQUIRE_NOTHROW(tray.OnActivate([] {}));
    tray.Show(TrayIcon());
    REQUIRE(transport->show_count == 1);
    auto show = std::async(std::launch::async, [&] { tray.Show(TrayIcon()); });
    REQUIRE_THROWS_AS(show.get(), std::logic_error);
    REQUIRE(transport->show_count == 1);
    auto hide = std::async(std::launch::async, [&] { tray.Hide(); });
    REQUIRE_THROWS_AS(hide.get(), std::logic_error);
    REQUIRE(transport->hide_count == 0);
    REQUIRE(tray.IsAvailable());
    tray.Hide();
    REQUIRE(transport->hide_count == 1);
  }
  REQUIRE_FALSE(retained->IsAvailable());
  REQUIRE_NOTHROW(retained->Hide());
  REQUIRE_THROWS_AS(retained->OnActivate([] {}), std::logic_error);
}

TEST_CASE("System tray activation registration is shared and independent of presentation updates") {
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  const auto transport = platform.system_tray_transport;
  transport->available = true;
  Application application(ApplicationOnlyApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  const auto tray = UseApplication().SystemTray();
  int activations = 0;

  REQUIRE_THROWS_AS(tray.OnActivate({}), std::invalid_argument);
  REQUIRE(transport->event_handler_updates == 0);
  tray.OnActivate([&] { ++activations; });
  REQUIRE(transport->show_count == 0);
  REQUIRE_THROWS_AS(UseApplication().SystemTray().OnActivate([] {}), std::logic_error);
  transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 0);

  tray.Show(TrayIcon());
  transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 1);
  tray.Show(TrayIcon(), {.tooltip = "Replacement"});
  transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 2);

  tray.Hide();
  transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 2);
  REQUIRE_THROWS_AS(tray.OnActivate([] {}), std::logic_error);
  tray.Show(TrayIcon());
  transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 3);
}

TEST_CASE("System tray activation registered from a window keeps application lifetime and explicit window access") {
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  const auto transport = platform.system_tray_transport;
  transport->available = true;
  TestPlatform ui_platform;
  std::optional<SystemTrayHandle> retained;
  std::weak_ptr<int> handler_lifetime;
  int activations = 0;
  AppOptions options{.show_debug_overlay = false};
  options.window_hooks.push_back([&](WindowContext&) {
    const auto window = UseWindow();
    const auto tray = UseApplication().SystemTray();
    auto lifetime = std::make_shared<int>(0);
    handler_lifetime = lifetime;
    tray.OnActivate([window, lifetime, &activations] {
      ++*lifetime;
      ++activations;
      REQUIRE_THROWS_AS(UseWindow(), std::logic_error);
      window.Activate();
    });
    tray.Show(TrayIcon());
    retained = tray;
  });
  Application application(ApplicationOnlyApp, std::move(options));
  {
    RuntimeLifetime runtime(application, platform);
    {
      UiWindow ui(runtime, ui_platform);
      transport->Activate();
      platform.RunPlatformModuleTasks();
      REQUIRE(activations == 1);
      REQUIRE(ui_platform.window_commands == std::vector<WindowCommand>{WindowCommand::Activate});
    }
    REQUIRE_FALSE(handler_lifetime.expired());
    transport->Activate();
    platform.RunPlatformModuleTasks();
    REQUIRE(activations == 2);
    REQUIRE(ui_platform.window_commands == std::vector<WindowCommand>{WindowCommand::Activate});
  }
  REQUIRE(handler_lifetime.expired());
  REQUIRE_THROWS_AS(retained->OnActivate([] {}), std::logic_error);
  transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 2);

  TestPlatform replacement_platform;
  Application replacement(ApplicationOnlyApp, {.show_debug_overlay = false});
  RuntimeLifetime replacement_runtime(replacement, replacement_platform);
  REQUIRE_THROWS_AS(retained->OnActivate([] {}), std::logic_error);
  REQUIRE_NOTHROW(UseApplication().SystemTray().OnActivate([] {}));
}

TEST_CASE("System tray rejects stale platform commands after replacing its presentation") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  UiWindow runtime(SystemTrayApp, platform, {.application_hooks = {InstallSystemTray}});
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
  platform.RunPlatformModuleTasks();
  REQUIRE(tray_commands == 0);
  platform.system_tray_transport->Invoke(second.generation, second_item.command);
  platform.RunPlatformModuleTasks();
  REQUIRE(tray_commands == 10);
}

TEST_CASE("System tray activation and lifecycle cleanup remain shared-service owned") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  {
    UiWindow runtime(SystemTrayApp, platform, {.application_hooks = {InstallSystemTray}});
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();
    platform.system_tray_transport->Activate();
  platform.RunPlatformModuleTasks();
    REQUIRE(tray_activations == 1);
  }

  REQUIRE(platform.system_tray_transport->hide_count >= 1);
  REQUIRE_FALSE(tray_handle->IsAvailable());
  platform.system_tray_transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(tray_activations == 1);
}

TEST_CASE("Hidden system tray presentations reject activation and commands") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  UiWindow runtime(SystemTrayApp, platform, {.application_hooks = {InstallSystemTray}});
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto presentation = *platform.system_tray_transport->presentation;
  tray_handle->Hide();
  platform.system_tray_transport->Activate();
  platform.RunPlatformModuleTasks();
  platform.system_tray_transport->Invoke(presentation.generation, presentation.menu.front().command);
  platform.RunPlatformModuleTasks();

  REQUIRE(tray_activations == 0);
  REQUIRE(tray_commands == 0);
}

TEST_CASE("System tray rejects vector icons without replacing its raster presentation") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  UiWindow runtime(SystemTrayApp, platform, {.application_hooks = {InstallSystemTray}});
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

TEST_CASE("System tray survives UI retirement and replacement") {
  ResetSystemTrayState();
  TestPlatform platform;
  platform.system_tray_transport = std::make_shared<TestSystemTrayTransport>();
  platform.system_tray_transport->available = true;
  Application application(SystemTrayApp, {.show_debug_overlay = false, .application_hooks = {InstallSystemTray}});
  RuntimeLifetime runtime(application, platform);
  {
    TestPlatform ui_platform;
    UiWindow ui(runtime, ui_platform);
    ui.SetWindowMetrics({.viewport = {320, 240}});
    ui.BuildFrame();
  }
  REQUIRE(platform.system_tray_transport->hide_count == 0);
  platform.system_tray_transport->Activate();
  platform.RunPlatformModuleTasks();
  REQUIRE(tray_activations == 1);
  UseApplication().SystemTray().Show(TrayIcon(), {.tooltip = "Application replacement"});
  REQUIRE(platform.system_tray_transport->presentation->tooltip == "Application replacement");
  UseApplication().SystemTray().Hide();
  REQUIRE(platform.system_tray_transport->hide_count == 1);
}

} // namespace huxerui::test
