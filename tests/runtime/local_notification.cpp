#include "runtime_test_support.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/application_internal.h"
#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

class TestLocalNotificationTransport final : public detail::LocalNotificationTransport {
public:
  struct AuthorizationRequest {
    detail::PermissionStatusCompletion completion;
    bool canceled = false;
  };

  struct NotificationRequest {
    detail::ResolvedLocalNotification notification;
    std::optional<std::chrono::system_clock::time_point> delivery_time;
    detail::LocalNotificationOperationCompletion completion;
    bool canceled = false;
  };

  struct CancelRequest {
    std::string identifier;
    detail::LocalNotificationOperationCompletion completion;
    bool canceled = false;
  };

  [[nodiscard]] LocalNotificationCapabilities Capabilities() const noexcept override {
    return capabilities;
  }

  std::function<void()> CheckAuthorization(detail::PermissionStatusCompletion completion) override {
    started_operations.emplace_back("check");
    checks.push_back({std::move(completion)});
    const std::size_t index = checks.size() - 1;
    if (immediate_check.has_value()) {
      checks[index].completion(*immediate_check);
    }
    return [this, index] { checks[index].canceled = true; };
  }

  std::function<void()> RequestAuthorization(detail::PermissionStatusCompletion completion) override {
    started_operations.emplace_back("request");
    requests.push_back({std::move(completion)});
    const std::size_t index = requests.size() - 1;
    if (immediate_request.has_value()) {
      requests[index].completion(*immediate_request);
    }
    if (!authorization_request_cancellable) {
      return {};
    }
    return [this, index] { requests[index].canceled = true; };
  }

  std::function<void()> Show(detail::ResolvedLocalNotification notification,
                             detail::LocalNotificationOperationCompletion completion) override {
    started_operations.emplace_back("show");
    shows.push_back({std::move(notification), std::nullopt, std::move(completion)});
    const std::size_t index = shows.size() - 1;
    if (immediate_operation.has_value()) {
      shows[index].completion(*immediate_operation);
    }
    return [this, index] { shows[index].canceled = true; };
  }

  std::function<void()> Schedule(detail::ResolvedLocalNotification notification,
                                 std::chrono::system_clock::time_point delivery_time,
                                 detail::LocalNotificationOperationCompletion completion) override {
    started_operations.emplace_back("schedule");
    schedules.push_back({std::move(notification), delivery_time, std::move(completion)});
    const std::size_t index = schedules.size() - 1;
    if (immediate_operation.has_value()) {
      schedules[index].completion(*immediate_operation);
    }
    return [this, index] { schedules[index].canceled = true; };
  }

  std::function<void()> Cancel(std::string identifier,
                               detail::LocalNotificationOperationCompletion completion) override {
    started_operations.emplace_back("cancel");
    cancellations.push_back({std::move(identifier), std::move(completion)});
    const std::size_t index = cancellations.size() - 1;
    if (immediate_operation.has_value()) {
      cancellations[index].completion(*immediate_operation);
    }
    return [this, index] { cancellations[index].canceled = true; };
  }

  void CompleteCheck(std::size_t index, PermissionStatus status) {
    checks.at(index).completion(status);
  }

  void CompleteRequest(std::size_t index, PermissionStatus status) {
    requests.at(index).completion(status);
  }

  void CompleteShow(std::size_t index, LocalNotificationOperationStatus status) {
    shows.at(index).completion(status);
  }

  void CompleteSchedule(std::size_t index, LocalNotificationOperationStatus status) {
    schedules.at(index).completion(status);
  }

  void CompleteCancel(std::size_t index, LocalNotificationOperationStatus status) {
    cancellations.at(index).completion(status);
  }

  LocalNotificationCapabilities capabilities;
  std::vector<AuthorizationRequest> checks;
  std::vector<AuthorizationRequest> requests;
  std::vector<NotificationRequest> shows;
  std::vector<NotificationRequest> schedules;
  std::vector<CancelRequest> cancellations;
  std::vector<std::string> started_operations;
  std::optional<PermissionStatus> immediate_check;
  std::optional<PermissionStatus> immediate_request;
  std::optional<LocalNotificationOperationStatus> immediate_operation;
  bool authorization_request_cancellable = true;
};

class LocalNotificationTestPlatform final : public TestPlatform {
public:
  LocalNotificationTestPlatform() = default;
  explicit LocalNotificationTestPlatform(UIThreadDispatcher dispatch_to_ui_thread)
      : TestPlatform(std::move(dispatch_to_ui_thread)) {}

  std::shared_ptr<detail::LocalNotificationTransport> CreateLocalNotificationTransport() override {
    return local_notification_transport;
  }

  std::shared_ptr<TestLocalNotificationTransport> local_notification_transport;
};

std::optional<LocalNotificationHandle> notification_handle;
TaskScope notification_tasks;
LocalNotificationCapabilities notification_capabilities;
std::vector<PermissionStatus> authorization_results;
std::vector<LocalNotificationOperationStatus> operation_results;

View LocalNotificationApp() {
  const ApplicationHandle application = UseApplication();
  notification_handle = application.LocalNotifications();
  notification_tasks = UseTaskScope();
  notification_capabilities = notification_handle->Capabilities();
  return {};
}

void ResetLocalNotificationState() {
  notification_handle.reset();
  notification_tasks = {};
  notification_capabilities = {};
  authorization_results.clear();
  operation_results.clear();
}

void MountLocalNotificationApp(Runtime& runtime) {
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(notification_handle.has_value());
}

TaskHandle CheckAuthorization() {
  return notification_tasks.Launch([notifications = *notification_handle]() -> Task<void> {
    authorization_results.push_back(co_await notifications.CheckAuthorizationAsync());
  });
}

TaskHandle RequestAuthorization() {
  return notification_tasks.Launch([notifications = *notification_handle]() -> Task<void> {
    authorization_results.push_back(co_await notifications.RequestAuthorizationAsync());
  });
}

TaskHandle ShowNotification(std::string identifier = "message",
                            LocalNotificationPresentation presentation = DefaultNotificationPresentation{},
                            PlatformPayload data = {}) {
  return notification_tasks.Launch([notifications = *notification_handle, identifier = std::move(identifier),
                                    presentation = std::move(presentation),
                                    data = std::move(data)]() mutable -> Task<void> {
    operation_results.push_back(co_await notifications.ShowAsync({
        .identifier = std::move(identifier),
        .title = "Title",
        .body = "Body",
        .presentation = std::move(presentation),
        .data = std::move(data),
    }));
  });
}

TaskHandle ScheduleNotification(std::chrono::system_clock::time_point delivery_time,
                                std::string identifier = "scheduled",
                                LocalNotificationPresentation presentation = DefaultNotificationPresentation{},
                                PlatformPayload data = {}) {
  return notification_tasks.Launch([notifications = *notification_handle, delivery_time,
                                    identifier = std::move(identifier), presentation = std::move(presentation),
                                    data = std::move(data)]() mutable -> Task<void> {
    operation_results.push_back(co_await notifications.ScheduleAsync(
        {
            .identifier = std::move(identifier),
            .title = "Scheduled title",
            .body = "Scheduled body",
            .presentation = std::move(presentation),
            .data = std::move(data),
        },
        delivery_time));
  });
}

TaskHandle CancelNotification(std::string identifier = "message") {
  return notification_tasks.Launch(
      [notifications = *notification_handle, identifier = std::move(identifier)]() mutable -> Task<void> {
        operation_results.push_back(co_await notifications.CancelAsync(identifier));
      });
}

} // namespace

TEST_CASE("Local notifications report unavailable without a platform transport") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  REQUIRE(notification_capabilities == LocalNotificationCapabilities{});

  CheckAuthorization();
  RequestAuthorization();
  ShowNotification();
  ScheduleNotification(std::chrono::system_clock::now() + std::chrono::hours(1));
  CancelNotification();
  platform.RunPlatformModuleTasks();

  REQUIRE(authorization_results == std::vector{
                                       PermissionStatus::Unavailable,
                                       PermissionStatus::Unavailable,
                                   });
  REQUIRE(operation_results == std::vector{
                                   LocalNotificationOperationStatus::Unavailable,
                                   LocalNotificationOperationStatus::Unavailable,
                                   LocalNotificationOperationStatus::Unavailable,
                               });
}

TEST_CASE("Local notification capabilities report the current transport snapshot") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  platform.local_notification_transport = std::make_shared<TestLocalNotificationTransport>();
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  REQUIRE(notification_capabilities == LocalNotificationCapabilities{});

  const LocalNotificationCapabilities available{
      .can_show = true,
      .can_schedule = true,
      .can_cancel = true,
      .can_activate = true,
      .can_use_templates = true,
  };
  platform.local_notification_transport->capabilities = available;
  REQUIRE(notification_capabilities == LocalNotificationCapabilities{});
  REQUIRE(notification_handle->Capabilities() == available);
}

TEST_CASE("Local notification queries run independently while mutations preserve submission order") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  platform.local_notification_transport = std::make_shared<TestLocalNotificationTransport>();
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  const auto delivery_time = std::chrono::system_clock::now() + std::chrono::hours(2);
  CheckAuthorization();
  RequestAuthorization();
  ShowNotification("stable-id");
  ScheduleNotification(delivery_time, "stable-id");
  CancelNotification("stable-id");
  platform.RunPlatformModuleTasks();

  auto& transport = *platform.local_notification_transport;
  REQUIRE(transport.started_operations == std::vector<std::string>{"check", "request"});
  REQUIRE(transport.shows.empty());

  transport.CompleteCheck(0, PermissionStatus::Granted);
  platform.RunPlatformModuleTasks();
  REQUIRE(authorization_results == std::vector{PermissionStatus::Granted});
  REQUIRE(transport.shows.empty());

  transport.CompleteRequest(0, PermissionStatus::Provisional);
  platform.RunPlatformModuleTasks();
  REQUIRE(authorization_results == std::vector{
                                       PermissionStatus::Granted,
                                       PermissionStatus::Provisional,
                                   });
  REQUIRE(transport.shows.size() == 1);
  REQUIRE(transport.shows.front().notification.identifier == "stable-id");
  REQUIRE(transport.shows.front().notification.title == "Title");
  REQUIRE(transport.shows.front().notification.body == "Body");
  REQUIRE(transport.shows.front().notification.presentation ==
          LocalNotificationPresentation{DefaultNotificationPresentation{}});

  transport.CompleteShow(0, LocalNotificationOperationStatus::Accepted);
  platform.RunPlatformModuleTasks();
  REQUIRE(transport.schedules.size() == 1);
  REQUIRE(transport.schedules.front().notification.identifier == "stable-id");
  REQUIRE(transport.schedules.front().delivery_time == delivery_time);

  transport.CompleteSchedule(0, LocalNotificationOperationStatus::Failed);
  platform.RunPlatformModuleTasks();
  REQUIRE(transport.cancellations.size() == 1);
  REQUIRE(transport.cancellations.front().identifier == "stable-id");

  transport.CompleteCancel(0, LocalNotificationOperationStatus::Accepted);
  platform.RunPlatformModuleTasks();
  REQUIRE(operation_results == std::vector{
                                   LocalNotificationOperationStatus::Accepted,
                                   LocalNotificationOperationStatus::Failed,
                                   LocalNotificationOperationStatus::Accepted,
                               });
  REQUIRE(transport.started_operations == std::vector<std::string>{"check", "request", "show", "schedule", "cancel"});
}

TEST_CASE("Local notification template presentation reaches immediate and scheduled transports") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  auto transport = std::make_shared<TestLocalNotificationTransport>();
  transport->immediate_operation = LocalNotificationOperationStatus::Accepted;
  platform.local_notification_transport = transport;
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  const auto delivery_time = std::chrono::system_clock::now() + std::chrono::hours(1);
  const PlatformPayload data = PlatformPayload::Object{{"id", 42}, {"details", PlatformPayload::List{true, "ready"}}};
  ShowNotification("templated-now", TemplateNotificationPresentation{"reminder.rich"}, data);
  ScheduleNotification(delivery_time, "templated-later", TemplateNotificationPresentation{"reminder.rich"}, data);
  platform.RunPlatformModuleTasks();

  REQUIRE(transport->shows.size() == 1);
  REQUIRE(detail::DecodeLocalNotificationData(transport->shows.front().notification.data) == data);
  REQUIRE(transport->shows.front().notification.presentation ==
          LocalNotificationPresentation{TemplateNotificationPresentation{"reminder.rich"}});
  REQUIRE(transport->schedules.size() == 1);
  REQUIRE(detail::DecodeLocalNotificationData(transport->schedules.front().notification.data) == data);
  REQUIRE(transport->schedules.front().notification.presentation ==
          LocalNotificationPresentation{TemplateNotificationPresentation{"reminder.rich"}});
  REQUIRE(operation_results == std::vector{
                                   LocalNotificationOperationStatus::Accepted,
                                   LocalNotificationOperationStatus::Accepted,
                               });
}

TEST_CASE("Local notification operations support synchronous platform completion") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform([](std::function<void()> operation) { operation(); });
  auto transport = std::make_shared<TestLocalNotificationTransport>();
  transport->immediate_check = PermissionStatus::Granted;
  transport->immediate_request = PermissionStatus::Denied;
  transport->immediate_operation = LocalNotificationOperationStatus::Accepted;
  platform.local_notification_transport = transport;
  {
    Runtime runtime(LocalNotificationApp, platform);
    MountLocalNotificationApp(runtime);

    CheckAuthorization();
    RequestAuthorization();
    ShowNotification();
    ScheduleNotification(std::chrono::system_clock::now() + std::chrono::hours(1));
    CancelNotification();

    REQUIRE(authorization_results == std::vector{
                                         PermissionStatus::Granted,
                                         PermissionStatus::Denied,
                                     });
    REQUIRE(operation_results == std::vector{
                                     LocalNotificationOperationStatus::Accepted,
                                     LocalNotificationOperationStatus::Accepted,
                                     LocalNotificationOperationStatus::Accepted,
                                 });
  }

  REQUIRE_FALSE(transport->checks.front().canceled);
  REQUIRE_FALSE(transport->requests.front().canceled);
  REQUIRE_FALSE(transport->shows.front().canceled);
  REQUIRE_FALSE(transport->schedules.front().canceled);
  REQUIRE_FALSE(transport->cancellations.front().canceled);
}

TEST_CASE("Canceling a local notification mutation advances the ordered queue") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  platform.local_notification_transport = std::make_shared<TestLocalNotificationTransport>();
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  const TaskHandle show = ShowNotification();
  ScheduleNotification(std::chrono::system_clock::now() + std::chrono::hours(1));
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.local_notification_transport->shows.size() == 1);
  REQUIRE(platform.local_notification_transport->schedules.empty());

  show.Cancel();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.local_notification_transport->shows.front().canceled);
  REQUIRE(platform.local_notification_transport->schedules.size() == 1);

  platform.local_notification_transport->CompleteShow(0, LocalNotificationOperationStatus::Accepted);
  platform.local_notification_transport->CompleteSchedule(0, LocalNotificationOperationStatus::Accepted);
  platform.RunPlatformModuleTasks();
  REQUIRE(operation_results == std::vector{LocalNotificationOperationStatus::Accepted});
}

TEST_CASE("Canceling a queued notification prevents native submission without blocking later operations") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  auto transport = std::make_shared<TestLocalNotificationTransport>();
  platform.local_notification_transport = transport;
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  RequestAuthorization();
  const TaskHandle queued = ShowNotification("canceled");
  ShowNotification("retained");
  platform.RunPlatformModuleTasks();
  REQUIRE(transport->requests.size() == 1);
  REQUIRE(transport->shows.empty());

  queued.Cancel();
  platform.RunPlatformModuleTasks();
  REQUIRE(transport->shows.empty());
  REQUIRE_FALSE(transport->requests.front().canceled);

  transport->CompleteRequest(0, PermissionStatus::Granted);
  platform.RunPlatformModuleTasks();
  REQUIRE(transport->shows.size() == 1);
  REQUIRE(transport->shows.front().notification.identifier == "retained");

  transport->CompleteShow(0, LocalNotificationOperationStatus::Accepted);
  platform.RunPlatformModuleTasks();
  REQUIRE(operation_results == std::vector{LocalNotificationOperationStatus::Accepted});
}

TEST_CASE("Canceling a non-cancelable notification prompt preserves mutation ordering") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  platform.local_notification_transport = std::make_shared<TestLocalNotificationTransport>();
  platform.local_notification_transport->authorization_request_cancellable = false;
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  const TaskHandle request = RequestAuthorization();
  ShowNotification();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.local_notification_transport->requests.size() == 1);
  REQUIRE(platform.local_notification_transport->shows.empty());

  request.Cancel();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.local_notification_transport->shows.empty());

  platform.local_notification_transport->CompleteRequest(0, PermissionStatus::Granted);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.local_notification_transport->shows.size() == 1);
  REQUIRE(authorization_results.empty());

  platform.local_notification_transport->CompleteShow(0, LocalNotificationOperationStatus::Accepted);
  platform.RunPlatformModuleTasks();
  REQUIRE(operation_results == std::vector{LocalNotificationOperationStatus::Accepted});
}

TEST_CASE("Local notification handles disconnect safely with their Runtime") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  platform.local_notification_transport = std::make_shared<TestLocalNotificationTransport>();
  {
    Runtime runtime(LocalNotificationApp, platform);
    MountLocalNotificationApp(runtime);
    ShowNotification();
    platform.RunPlatformModuleTasks();
    REQUIRE(platform.local_notification_transport->shows.size() == 1);
  }

  REQUIRE(platform.local_notification_transport->shows.front().canceled);
  REQUIRE(notification_handle->Capabilities() == LocalNotificationCapabilities{});

  platform.local_notification_transport->CompleteShow(0, LocalNotificationOperationStatus::Accepted);
  platform.RunPlatformModuleTasks();
  REQUIRE(operation_results.empty());
}

TEST_CASE("Local notification methods validate application input before launching a Task") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);

  REQUIRE_THROWS_AS(notification_handle->ShowAsync({.identifier = "", .title = "Title"}), std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({.identifier = std::string("id\0x", 4), .title = "Title"}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(
      notification_handle->ShowAsync({.identifier = std::string(1, static_cast<char>(0xC3)), .title = "Title"}),
      std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({.identifier = "message"}), std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({
                        .identifier = "message",
                        .title = std::string(1, static_cast<char>(0xC3)),
                    }),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({
                        .identifier = "message",
                        .title = "Title",
                        .presentation = TemplateNotificationPresentation{""},
                    }),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({
                        .identifier = "message",
                        .title = "Title",
                        .presentation = TemplateNotificationPresentation{std::string("template\0x", 10)},
                    }),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({
                        .identifier = "message",
                        .title = "Title",
                        .presentation = TemplateNotificationPresentation{std::string(1, static_cast<char>(0xC3))},
                    }),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->CancelAsync(""), std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ScheduleAsync({.identifier = "message", .title = "Title"},
                                                       std::chrono::system_clock::now() - std::chrono::seconds(1)),
                    std::invalid_argument);
}

TEST_CASE("Local notification data preserves durable values and rejects malformed envelopes") {
  const PlatformPayload data = PlatformPayload::Object{
      {"null", nullptr}, {"boolean", true}, {"integer", std::int64_t{9007199254740993LL}}, {"double", 1.25},
      {"text", std::string("a\0b", 3)}, {"bytes", Bytes{std::byte{0}, std::byte{255}}},
      {"list", PlatformPayload::List{42, "nested"}},
  };
  REQUIRE(detail::DecodeLocalNotificationData(detail::EncodeLocalNotificationData(data)) == data);
  REQUIRE(detail::DecodeLocalNotificationData(detail::EncodeLocalNotificationData({})).IsNull());
  REQUIRE_THROWS_AS(detail::DecodeLocalNotificationData(Bytes{std::byte{1}}), std::invalid_argument);
  REQUIRE_THROWS_AS(detail::DecodeLocalNotificationData(Bytes(65537)), std::invalid_argument);
  const PlatformPayload boundary = Bytes(65536 - 13);
  REQUIRE(detail::EncodeLocalNotificationData(boundary).size() == 65536);
  REQUIRE_THROWS_AS(detail::EncodeLocalNotificationData(PlatformPayload(Bytes(65536 - 12))), std::invalid_argument);
}

TEST_CASE("Local notification data rejects nested buffer references without copying") {
  const PlatformPayload data = PlatformPayload::Object{{"frame", PlatformPayload::List{BufferReference{}}}};
  REQUIRE_THROWS_AS(detail::EncodeLocalNotificationData(data), std::invalid_argument);
  REQUIRE_THROWS_AS(detail::DecodeLocalNotificationData(data.Encode().bytes), std::invalid_argument);
}

TEST_CASE("Local notification submission rejects nested retained resources before launching a Task") {
  ResetLocalNotificationState();
  LocalNotificationTestPlatform platform;
  Runtime runtime(LocalNotificationApp, platform);
  MountLocalNotificationApp(runtime);
  const PlatformPayload data = PlatformPayload::Object{
      {"nested", PlatformPayload::List{MakeTestExternalTexture({16.0F, 16.0F})}},
  };
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({.identifier = "resource", .title = "Title", .data = data}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ScheduleAsync({.identifier = "resource", .title = "Title", .data = data},
                                                       std::chrono::system_clock::now() + std::chrono::hours(1)),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(detail::DecodeLocalNotificationData(data.Encode().bytes), std::invalid_argument);
  REQUIRE_THROWS_AS(notification_handle->ShowAsync({.identifier = "large", .title = "Title", .data = Bytes(65536)}),
                    std::invalid_argument);
}

} // namespace huxerui::test
