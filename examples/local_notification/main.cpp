#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace huxerui;

namespace {

constexpr const char* download_file_name = "huxerui-sdk-0.2.0-windows-x86_64.zip";
constexpr const char* download_url =
    "https://github.com/HuxerUI/HuxerUI/releases/download/v0.2.0/huxerui-sdk-0.2.0-windows-x86_64.zip";
constexpr const char* download_identifier = "local-notification-example.download";

struct DownloadState {
  bool busy = false;
  bool expanded = true;
  std::uint64_t downloaded_bytes = 0;
  std::optional<std::uint64_t> total_bytes;
  std::string status{"Ready"};
  std::string detail{"The ZIP is downloaded only; it is never extracted or installed."};
  std::string notification_result{"not submitted"};

  bool operator==(const DownloadState&) const = default;
};

std::string DownloadProgressText(const DownloadState& state) {
  std::string text = std::to_string(state.downloaded_bytes / 1024) + " KiB";
  if (state.total_bytes && *state.total_bytes > 0) {
    text += " / " + std::to_string(*state.total_bytes / 1024) + " KiB";
  }
  return text;
}

std::string DescribePermissionStatus(PermissionStatus status) {
  switch (status) {
  case PermissionStatus::NotDetermined:
    return "not determined";
  case PermissionStatus::Granted:
    return "granted";
  case PermissionStatus::Denied:
    return "denied";
  case PermissionStatus::PermanentlyDenied:
    return "permanently denied";
  case PermissionStatus::Restricted:
    return "restricted";
  case PermissionStatus::Provisional:
    return "provisional";
  case PermissionStatus::Unavailable:
    return "unavailable";
  }
  return "unknown";
}

std::string DescribeLocalNotificationOperationStatus(LocalNotificationOperationStatus status) {
  switch (status) {
  case LocalNotificationOperationStatus::Accepted:
    return "accepted";
  case LocalNotificationOperationStatus::Unauthorized:
    return "unauthorized";
  case LocalNotificationOperationStatus::Unavailable:
    return "unavailable";
  case LocalNotificationOperationStatus::Failed:
    return "failed";
  }
  return "unknown";
}

std::string DescribeLocalNotificationCapabilities(const LocalNotificationCapabilities& capabilities) {
  return std::string{capabilities.can_show ? "show" : "no show"} +
         (capabilities.can_schedule ? ", schedule" : ", no schedule") +
         (capabilities.can_cancel ? ", cancel" : ", no cancel") +
         (capabilities.can_activate ? ", activate" : ", no activate") +
         (capabilities.can_use_templates ? ", templates" : ", no templates");
}

std::string DescribeNotificationActivation(const ApplicationActivation& activation) {
  const auto* notification = std::get_if<NotificationActivation>(&activation);
  if (notification == nullptr) {
    return "No notification data";
  }
  std::string result = "Notification: " + notification->identifier;
  if (notification->data.Kind() != PlatformPayloadKind::Object) {
    return result + " (no example data object)";
  }
  const auto& fields = notification->data.AsObject();
  // Activation is an external snapshot, not authority to open a path or resume a download.
  for (const char* key : {"detail", "file_name", "status"}) {
    if (const auto field = fields.find(key); field != fields.end()) {
      result += "\n" + std::string(key) + ": ";
      result += field->second.Kind() == PlatformPayloadKind::String
                    ? std::string(field->second.AsString()) : "(invalid)";
    }
  }
  for (const char* key : {"downloaded_bytes", "total_bytes"}) {
    if (const auto field = fields.find(key); field != fields.end()) {
      result += "\n" + std::string(key) + ": ";
      if (field->second.IsNull() && std::string_view(key) == "total_bytes") {
        result += "unknown";
      } else if (field->second.Kind() == PlatformPayloadKind::Integer && field->second.AsInteger() >= 0) {
        result += std::to_string(field->second.AsInteger());
      } else {
        result += "(invalid)";
      }
    }
  }
  return result;
}

[[huxerui::composable]]
View LocalNotificationCard(const ApplicationHandle& application, TaskScope tasks) {
  const LocalNotificationHandle notifications = application.LocalNotifications();
  const LocalNotificationCapabilities capabilities = notifications.Capabilities();
  auto result = UseState(std::string{"not checked"});
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Local notification", TextRole::Title),
    Text(DescribeLocalNotificationCapabilities(capabilities), TextRole::Label)
        .With(Foreground(theme.colors.primary)),
    Text("Configured Windows, Android, iOS, and macOS hosts provide native delivery. The download below uses an "
         "application-owned native template on supported hosts."),
    Text("The default reminder and download use separate identifiers, so they do not replace each other."),
    Text("Last result: " + result.Get()),
    Button("Check authorization").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        result = DescribePermissionStatus(co_await notifications.CheckAuthorizationAsync());
      });
    }),
    Button("Request authorization").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        result = DescribePermissionStatus(co_await notifications.RequestAuthorizationAsync());
      });
    }),
    Text("Default presentation", TextRole::Label),
    Button("Show default now")
        .With(Enabled(capabilities.can_show))
        .OnClick([=] {
          tasks.Launch([=]() -> Task<void> {
            result = DescribeLocalNotificationOperationStatus(co_await notifications.ShowAsync({
                .identifier = "local-notification-example.reminder",
                .title = "HuxerUI default notification",
                .body = "This request uses the default system presentation.",
                .data = PlatformPayload::Object{{"detail", "Immediate default notification"}},
            }));
          });
        }),
    Button("Schedule default in one minute")
        .With(Enabled(capabilities.can_schedule))
        .OnClick([=] {
          tasks.Launch([=]() -> Task<void> {
            result = DescribeLocalNotificationOperationStatus(co_await notifications.ScheduleAsync({
                .identifier = "local-notification-example.reminder",
                .title = "HuxerUI scheduled default notification",
                .body = "This one-shot request uses the default system presentation.",
                .data = PlatformPayload::Object{{"detail", "Scheduled default notification"}},
            }, std::chrono::system_clock::now() + std::chrono::minutes(1)));
          });
        }),
    Button("Cancel reminder").With(Enabled(capabilities.can_cancel)).OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        result = DescribeLocalNotificationOperationStatus(
            co_await notifications.CancelAsync("local-notification-example.reminder"));
      });
    }),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.small),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.medium)
  );
}

Task<LocalNotificationOperationStatus> PublishDownload(LocalNotificationHandle notifications, DownloadState snapshot) {
  co_return co_await notifications.ShowAsync({
      .identifier = download_identifier,
      .title = "HuxerUI SDK download",
      .body = snapshot.status + " · " + DownloadProgressText(snapshot),
      .presentation = TemplateNotificationPresentation{.identifier = "huxerui.local-notification.download"},
      .data = PlatformPayload::Object{
          {"file_name", download_file_name},
          {"downloaded_bytes", snapshot.downloaded_bytes},
          {"total_bytes", snapshot.total_bytes ? PlatformPayload(*snapshot.total_bytes) : PlatformPayload{}},
          {"status", snapshot.status},
          {"expanded", snapshot.expanded},
      },
  });
}

Task<void> FinishDownload(LocalNotificationHandle notifications, State<DownloadState> state,
                          std::string status, std::string detail) {
  auto snapshot = state.Get();
  snapshot.status = std::move(status);
  snapshot.detail = std::move(detail);
  snapshot.busy = false;
  state.Update([snapshot](DownloadState& value) {
    value.status = snapshot.status;
    value.detail = snapshot.detail;
  });
  snapshot.notification_result =
      DescribeLocalNotificationOperationStatus(co_await PublishDownload(notifications, snapshot));
  // Keep the controls busy until the terminal notification completes; a retry cannot overtake this update.
  state = std::move(snapshot);
}

Task<void> DownloadRelease(std::shared_ptr<HttpClient> http, LocalNotificationHandle notifications,
                           File directory, State<DownloadState> state) {
  const auto initial = co_await PublishDownload(notifications, state.Get());
  state.Update([initial](DownloadState& value) {
    value.notification_result = DescribeLocalNotificationOperationStatus(initial);
  });
  if (initial != LocalNotificationOperationStatus::Accepted) {
    state.Update([](DownloadState& value) {
      value.busy = false;
      value.status = "Not started";
      value.detail = "Request notification authorization before downloading; no file was downloaded.";
    });
    co_return;
  }

  HttpRequest request{
      .url = download_url,
      .headers = {{"Accept", "application/octet-stream"}},
      .timeout = std::chrono::minutes(10),
  };
  auto opened = co_await http->SendStreamAsync(std::move(request), [state](HttpProgress progress) {
    if (progress.kind == HttpProgressKind::Download) {
      state.Update([progress](DownloadState& value) { value.total_bytes = progress.total_bytes; });
    }
  });
  if (!opened.Succeeded()) {
    co_await FinishDownload(notifications, state, "Failed", opened.Error().message);
    co_return;
  }
  auto response = std::move(opened).Value();
  if (response.StatusCode() != 200) {
    co_await FinishDownload(notifications, state, "Failed", "HTTP " + std::to_string(response.StatusCode()));
    co_return;
  }
  if (!(co_await directory.CreateDirectoriesAsync())) {
    co_await FinishDownload(notifications, state, "Failed", "Could not create the download cache directory.");
    co_return;
  }
  const File partial = directory.Child(std::string(download_file_name) + ".part");
  auto output = co_await partial.OpenWriteAsync();
  if (!output.Succeeded()) {
    co_await FinishDownload(notifications, state, "Failed", output.Error().message);
    co_return;
  }
  auto writer = std::move(output).Value();
  state.Update([partial](DownloadState& value) { value.detail = "Writing " + partial.Path(); });
  auto last_notification = std::chrono::steady_clock::now();

  while (true) {
    auto chunk = co_await response.Body().ReadAsync(256U * 1024U);
    if (!chunk.Succeeded()) {
      co_await FinishDownload(notifications, state, "Failed",
                             chunk.Error().message + " Partial file: " + partial.Path());
      co_return;
    }
    Bytes bytes = std::move(chunk).Value();
    if (bytes.empty()) {
      break;
    }
    const auto count = bytes.size();
    auto written = co_await writer.WriteAsync(std::move(bytes));
    if (!written.Succeeded()) {
      co_await FinishDownload(notifications, state, "Failed",
                             written.Error().message + " Partial file: " + partial.Path());
      co_return;
    }
    state.Update([count](DownloadState& value) { value.downloaded_bytes += count; });
    if (std::chrono::steady_clock::now() - last_notification >= std::chrono::seconds(1)) {
      // Await each replacement instead of launching a notification Task for every network progress callback.
      const auto result = co_await PublishDownload(notifications, state.Get());
      state.Update([result](DownloadState& value) {
        value.notification_result = DescribeLocalNotificationOperationStatus(result);
      });
      last_notification = std::chrono::steady_clock::now();
    }
  }

  auto closed = co_await writer.CloseAsync();
  if (!closed.Succeeded()) {
    co_await FinishDownload(notifications, state, "Failed", closed.Error().message);
    co_return;
  }
  if (state->total_bytes && state->downloaded_bytes != *state->total_bytes) {
    co_await FinishDownload(notifications, state, "Failed", "The received size does not match the response length.");
    co_return;
  }
  const File completed = directory.Child(download_file_name);
  if (!(co_await partial.MoveToAsync(completed))) {
    co_await FinishDownload(notifications, state, "Failed", "Could not finalize " + partial.Path());
    co_return;
  }
  co_await FinishDownload(notifications, state, "Complete", "Saved to " + completed.Path());
}

[[huxerui::composable]] View DownloadNotificationCard(const ApplicationHandle& application) {
  auto http = UseService<HttpClient>();
  auto tasks = UseTaskScope();
  auto state = UseState(DownloadState{});
  auto active = UseState(std::shared_ptr<TaskHandle>{});
  auto expanded = UseState(true);
  const auto notifications = application.LocalNotifications();
  const auto capabilities = notifications.Capabilities();
  const auto& theme = UseTheme();
  const auto& download = state.Get();
  const float fraction = download.status == "Complete" ? 1.0F : download.total_bytes && *download.total_bytes > 0
      ? static_cast<float>(std::clamp(static_cast<double>(download.downloaded_bytes) / *download.total_bytes, 0.0, 1.0))
      : 0.0F;

  return Column {
    Text("Download progress template", TextRole::Title),
    Text(download_file_name, TextRole::Label),
    Text("HuxerUI v0.2.0, 54.8 MB. Templates use Windows Toast XML, Android RemoteViews, or an iOS Content Extension."),
    Text("Windows progress bars require Windows 10 version 1703 or later. Updates replace the notification and may "
         "show another popup; native in-place progress updates are not demonstrated."),
    Text("Downloads require this app to keep running; no background service or process-restart recovery is provided."),
    Button(expanded.Get() ? "Detailed template layout: on" : "Detailed template layout: off")
        .With(Enabled(!download.busy))
        .OnClick([=] { expanded = !expanded.Get(); }),
    Button(download.busy ? "Downloading..." : "Download HuxerUI (54.8 MB)")
        .With(Enabled(!download.busy && capabilities.can_show && capabilities.can_use_templates))
        .OnClick([=] {
          if (state->busy) { return; }
          state = DownloadState{.busy = true, .expanded = expanded.Get(), .status = "Downloading",
                                .detail = "Waiting for notification authorization and response headers..."};
          // Each attempt has its own cache directory: a canceled worker cannot overwrite a later attempt.
          const auto attempt = std::chrono::steady_clock::now().time_since_epoch().count();
          const File directory = application.Directories().cache_directory.Child(
              "notification-download-" + std::to_string(attempt));
          active = std::make_shared<TaskHandle>(tasks.Launch(DownloadRelease(http, notifications, directory, state)));
        }),
    Button("Cancel download")
        .With(Enabled(download.busy && download.status == "Downloading"))
        .OnClick([=] {
          if (!state->busy || state->status != "Downloading") { return; }
          if (auto task = active.Get()) { task->Cancel(); }
          active = {};
          state.Update([](DownloadState& value) { value.status = "Cancelling"; });
          tasks.Launch(FinishDownload(notifications, state, "Cancelled",
              "Cancelled. Partial files remain in cache; an in-flight file operation may still finish."));
        }),
    download.busy && download.status == "Downloading" && (!download.total_bytes || *download.total_bytes == 0)
        ? ProgressBar() : ProgressBar(fraction),
    Text(download.status + " · " + DownloadProgressText(download)),
    Text(download.detail),
    Text("Notification result: " + download.notification_result),
    Text("Tap the notification to inspect its file name, byte counts, and status in the activation log below."),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.small),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.medium)
  );
}

[[huxerui::composable]] View LocalNotificationContent() {
  const ApplicationHandle application = UseApplication();
  auto tasks = UseTaskScope();
  auto activations = UseStateList<std::string>({
      "Startup: " + DescribeNotificationActivation(application.StartupActivation()),
  });

  // Apple notification responses arrive through OnActivation even when the interaction launches the process.
  application.OnActivation([=](ApplicationActivation activation) {
    activations.PushBack("Subsequent: " + DescribeNotificationActivation(activation));
  });

  const ThemeSpec& theme = UseTheme();
  return ScrollView {
    Column {
      Text("Local notifications", TextRole::Title),
      Text("Request authorization, submit a notification, then tap it in the system UI to inspect its data snapshot."),
      Text("Scheduling is best effort. An accepted request does not guarantee visible or exact-time delivery."),
      LocalNotificationCard(application, tasks),
      DownloadNotificationCard(application),
      Column {
        Text("Notification activation data", TextRole::Title),
        Text("Startup and subsequent activations are shown here. Data is the submitted snapshot, not live app state."),
        ForEach(activations, [](const std::string& activation) { return Text(activation); }),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          CrossAlign(CrossAxisAlignment::Stretch),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.background));
}

View App() {
  return MaterialTheme {
    LocalNotificationContent(),
  };
}

AppOptions Options() {
  AppOptions options;
  options.window = {
      .title = "HuxerUI Local Notifications",
      .initial_size = {720.0F, 720.0F},
  };
  return options;
}

} // namespace

const Application application{App, Options()};
