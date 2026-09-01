#include "runtime_test_support.h"

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace huxerui::test {
namespace {

std::optional<ApplicationHandle> permission_application;
TaskScope permission_tasks;
std::vector<PermissionStatus> permission_results;
std::vector<bool> settings_results;

View PermissionApp() {
  permission_application = UseApplication();
  permission_tasks = UseTaskScope();
  return {};
}

void ResetPermissionState() {
  permission_application.reset();
  permission_tasks = {};
  permission_results.clear();
  settings_results.clear();
}

void MountPermissionApp(TestPlatform& platform, Runtime& runtime) {
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(permission_application.has_value());
  static_cast<void>(platform);
}

TaskHandle CheckPermission(Permission permission) {
  return permission_tasks.Launch([application = *permission_application, permission]() -> Task<void> {
    permission_results.push_back(co_await application.CheckPermissionAsync(permission));
  });
}

TaskHandle RequestPermission(Permission permission) {
  return permission_tasks.Launch([application = *permission_application, permission]() -> Task<void> {
    permission_results.push_back(co_await application.RequestPermissionAsync(permission));
  });
}

TaskHandle OpenSettings(Permission permission) {
  return permission_tasks.Launch([application = *permission_application, permission]() -> Task<void> {
    settings_results.push_back(co_await application.OpenPermissionSettingsAsync(permission));
  });
}

} // namespace

TEST_CASE("Application permission operations report unavailable without a platform transport") {
  ResetPermissionState();
  TestPlatform platform;
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  CheckPermission(Permission::Camera);
  RequestPermission(Permission::Microphone);
  OpenSettings(Permission::Camera);
  platform.RunPlatformModuleTasks();

  REQUIRE(permission_results == std::vector{PermissionStatus::Unavailable, PermissionStatus::Unavailable});
  REQUIRE(settings_results == std::vector{false});
}

TEST_CASE("Application permission queries resume through the owning task execution") {
  ResetPermissionState();
  TestPlatform platform;
  platform.permission_transport = std::make_shared<TestPermissionTransport>();
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  CheckPermission(Permission::Camera);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->checks.size() == 1);
  REQUIRE(permission_results.empty());

  platform.permission_transport->CompleteCheck(0, PermissionStatus::Granted);
  REQUIRE(permission_results.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(permission_results == std::vector{PermissionStatus::Granted});
}

TEST_CASE("Application permission operations support synchronous platform completion") {
  ResetPermissionState();
  TestPlatform platform([](std::function<void()> operation) { operation(); });
  auto transport = std::make_shared<TestPermissionTransport>();
  transport->immediate_check = PermissionStatus::Granted;
  transport->immediate_request = PermissionStatus::Denied;
  transport->immediate_settings = true;
  platform.permission_transport = transport;
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  CheckPermission(Permission::Camera);
  RequestPermission(Permission::Microphone);
  OpenSettings(Permission::Camera);

  REQUIRE(permission_results == std::vector{PermissionStatus::Granted, PermissionStatus::Denied});
  REQUIRE(settings_results == std::vector{true});
  REQUIRE_FALSE(transport->checks.front().canceled);
  REQUIRE_FALSE(transport->requests.front().canceled);
  REQUIRE_FALSE(transport->settings.front().canceled);
}

TEST_CASE("Application permission prompts and settings are serialized") {
  ResetPermissionState();
  TestPlatform platform;
  platform.permission_transport = std::make_shared<TestPermissionTransport>();
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  RequestPermission(Permission::Camera);
  RequestPermission(Permission::Microphone);
  OpenSettings(Permission::Camera);
  platform.RunPlatformModuleTasks();

  REQUIRE(platform.permission_transport->requests.size() == 1);
  REQUIRE(platform.permission_transport->settings.empty());

  platform.permission_transport->CompleteRequest(0, PermissionStatus::Denied);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->requests.size() == 2);
  REQUIRE(platform.permission_transport->settings.empty());

  platform.permission_transport->CompleteRequest(1, PermissionStatus::Granted);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->settings.size() == 1);

  platform.permission_transport->CompleteSettings(0, true);
  platform.RunPlatformModuleTasks();
  REQUIRE(permission_results == std::vector{PermissionStatus::Denied, PermissionStatus::Granted});
  REQUIRE(settings_results == std::vector{true});
}

TEST_CASE("Canceling an application permission task detaches its continuation") {
  ResetPermissionState();
  TestPlatform platform;
  platform.permission_transport = std::make_shared<TestPermissionTransport>();
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  const TaskHandle request = RequestPermission(Permission::Camera);
  RequestPermission(Permission::Microphone);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->requests.size() == 1);

  request.Cancel();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->requests.front().canceled);
  REQUIRE(platform.permission_transport->requests.size() == 2);

  platform.permission_transport->CompleteRequest(0, PermissionStatus::Granted);
  platform.permission_transport->CompleteRequest(1, PermissionStatus::Denied);
  platform.RunPlatformModuleTasks();
  REQUIRE(permission_results == std::vector{PermissionStatus::Denied});
}

TEST_CASE("Canceling a non-cancelable permission prompt preserves serialization until platform completion") {
  ResetPermissionState();
  TestPlatform platform;
  platform.permission_transport = std::make_shared<TestPermissionTransport>();
  platform.permission_transport->request_cancellable = false;
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  const TaskHandle request = RequestPermission(Permission::Camera);
  RequestPermission(Permission::Microphone);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->requests.size() == 1);

  request.Cancel();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->requests.size() == 1);

  platform.permission_transport->CompleteRequest(0, PermissionStatus::Granted);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.permission_transport->requests.size() == 2);
  REQUIRE(permission_results.empty());

  platform.permission_transport->CompleteRequest(1, PermissionStatus::Denied);
  platform.RunPlatformModuleTasks();
  REQUIRE(permission_results == std::vector{PermissionStatus::Denied});
}

TEST_CASE("Application permission methods reject invalid identifiers before launching a task") {
  ResetPermissionState();
  TestPlatform platform;
  Runtime runtime(PermissionApp, platform);
  MountPermissionApp(platform, runtime);

  REQUIRE_THROWS_AS(
      permission_application->CheckPermissionAsync(static_cast<Permission>(-1)), std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      permission_application->RequestPermissionAsync(static_cast<Permission>(-1)), std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      permission_application->OpenPermissionSettingsAsync(static_cast<Permission>(-1)), std::invalid_argument
  );
}

} // namespace huxerui::test
