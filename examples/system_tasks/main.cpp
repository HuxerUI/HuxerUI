#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

using namespace huxerui;

namespace {

constexpr std::size_t block_size = 1024U * 1024U;
constexpr auto export_instance = "report";

/// Application-owned inputs and results remain available when the UI detaches.
struct DemoViewModel {
  explicit DemoViewModel(File directory) : directory(std::move(directory)) {}

  File directory;
  HttpClient http;
  State<int> export_mebibytes{128};
  State<TextEditingValue> stream_url{TextEditingValue{}};
  State<std::string> export_result{std::string{"Choose an export size, then start."}};
  State<std::string> index_result{std::string{"No completed maintenance index."}};
  State<std::string> stream_result{std::string{"Enter an HTTPS streaming endpoint."}};
  State<std::uint64_t> received_bytes{0};
  State<std::string> message{std::string{"Ready"}};
  std::uint64_t export_serial = 0;
};

/// Converts operational failures into the handler error exposed by TaskState.
template <class T, class Error> void Check(const Result<T, Error>& result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

/// Generates and appends one actual MiB, checking both cancellation sources before writing.
/// @param worker_stop Cancellation of this queued worker operation.
/// @param system_stop The original system execution's copied stop token.
/// @param partial Attempt-specific file that is published only after all blocks finish.
void AppendBlock(std::stop_token worker_stop, std::stop_token system_stop, File partial) {
  if (worker_stop.stop_requested() || system_stop.stop_requested()) return;
  Bytes block(block_size);
  for (std::size_t index = 0; index < block.size(); ++index) {
    block[index] = static_cast<std::byte>(index & 0xFFU);
  }
  if (worker_stop.stop_requested() || system_stop.stop_requested()) return;
  if (!partial.AppendBytes(block)) throw std::runtime_error("HuxerUI example could not append an export block");
}

/// Runs after earlier writes in the attempt's WorkerSequence, within Context::Cleanup's budget.
/// @param stop Cancellation of the queued cleanup operation.
/// @param partial This attempt's unpublished output; completed exports are retained.
void DiscardPartial(std::stop_token stop, File partial) {
  if (!stop.stop_requested() && partial.Exists() && !partial.Delete()) {
    throw std::runtime_error("HuxerUI example could not remove its partial export");
  }
}

/// Exports a deterministic binary dataset with platform progress measured in written bytes.
Task<void> Export(ContinuedTask::Context context) {
  auto model = UseService<DemoViewModel>();
  const int blocks = model->export_mebibytes;
  const auto identity = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
                        std::to_string(++model->export_serial);
  const File partial = model->directory.Child("report-" + identity + ".partial");
  const File completed = model->directory.Child("report-" + identity + ".bin");
  WorkerSequence writes;
  std::exception_ptr failure;
  try {
    if (!co_await model->directory.CreateDirectoriesAsync()) {
      throw std::runtime_error("HuxerUI example could not create its export directory");
    }
    model->export_result = "Writing " + partial.Path();
    for (int block = 0; block < blocks; ++block) {
      co_await writes.Run(AppendBlock, context.StopToken(), partial);
      Check(context.ReportProgress({static_cast<std::uint64_t>(block + 1) * block_size,
                                    static_cast<std::uint64_t>(blocks) * block_size}));
    }
    if (!co_await partial.MoveToAsync(completed)) {
      throw std::runtime_error("HuxerUI example could not publish its completed export");
    }
    model->export_result = "Saved " + completed.Path();
  } catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    model->export_result = "Cleaning up " + partial.Path();
    co_await context.Cleanup(writes.Run(DiscardPartial, partial));
    model->export_result = "Partial output removed; completed exports remain in " + model->directory.Path();
    std::rethrow_exception(failure);
  }
}

/// Checks immutable exported bytes on a worker and returns an index entry.
/// @param stop Copied system stop token, checked between bounded reads.
/// @param file A completed .bin file owned by this example.
std::string VerifyFile(std::stop_token stop, File file) {
  auto opened = file.OpenRead();
  Check(opened);
  auto input = std::move(opened).Value();
  Bytes buffer(64U * 1024U);
  std::uint64_t offset = 0;
  std::uint64_t checksum = 14695981039346656037ULL;
  for (;;) {
    if (stop.stop_requested()) throw TaskStopped{};
    auto read = input.Read(buffer);
    Check(read);
    if (read.Value() == 0) break;
    for (std::size_t index = 0; index < read.Value(); ++index) {
      const auto value = std::to_integer<std::uint64_t>(buffer[index]);
      if (value != ((offset + index) & 0xFFU)) {
        throw std::runtime_error("HuxerUI example export validation failed for " + file.Name());
      }
      checksum = (checksum ^ value) * 1099511628211ULL;
    }
    offset += read.Value();
  }
  return file.Name() + " | " + std::to_string(offset) + " bytes | FNV-1a " + std::to_string(checksum) + "\n";
}

/// Rebuilds an idempotent index whenever the system grants a maintenance opportunity.
Task<BackgroundTask::Result> Maintain(BackgroundTask::Context context) {
  auto model = UseService<DemoViewModel>();
  if (!co_await model->directory.CreateDirectoriesAsync()) {
    throw std::runtime_error("HuxerUI example could not create its export directory");
  }
  auto listed = co_await model->directory.ListChildrenAsync();
  Check(listed);
  auto files = std::move(listed).Value();
  std::erase_if(files, [](const File& file) {
    return !file.Name().starts_with("report-") || file.Extension() != ".bin";
  });
  std::ranges::sort(files, {}, &File::Name);
  std::string index = "Verified exports\n";
  std::uint64_t checked = 0;
  for (const File& file : files) {
    index += co_await RunWorker(VerifyFile, context.StopToken(), file);
    Check(context.ReportProgress({++checked, files.size()}));
  }
  const File output = model->directory.Child("index.txt");
  if (!co_await output.WriteStringAsync(index)) {
    throw std::runtime_error("HuxerUI example could not save its maintenance index");
  }
  model->index_result = std::move(index);
  co_return BackgroundTask::Result::Completed;
}

/// Saves a small session checkpoint after the response stream has released its connection.
Task<void> SaveSession(File directory, std::uint64_t bytes) {
  if (!co_await directory.CreateDirectoriesAsync() ||
      !co_await directory.Child("stream-session.txt").WriteStringAsync("Last session received " +
          std::to_string(bytes) + " bytes.\n")) {
    throw std::runtime_error("HuxerUI example could not save its stream checkpoint");
  }
}

/// Receives real streaming HTTP data until EOF, failure, platform expiration, or an explicit stop.
Task<void> Receive(OngoingTask::Context context) {
  auto model = UseService<DemoViewModel>();
  model->received_bytes = 0;
  std::exception_ptr failure;
  try {
    model->stream_result = "Connecting";
    auto opened = co_await model->http.SendStreamAsync({.url = model->stream_url->text, .timeout = std::nullopt});
    Check(opened);
    auto response = std::move(opened).Value();
    if (response.StatusCode() < 200 || response.StatusCode() >= 300) {
      throw std::runtime_error("HuxerUI example stream returned HTTP " + std::to_string(response.StatusCode()));
    }
    auto last_presentation = std::chrono::steady_clock::now();
    for (;;) {
      auto read = co_await response.Body().ReadAsync(64U * 1024U);
      Check(read);
      if (read.Value().empty()) break;
      model->received_bytes += read.Value().size();
      model->stream_result = "Receiving data";
      if (std::chrono::steady_clock::now() - last_presentation >= 1s) {
        Check(context.UpdatePresentation({"Receive data", std::to_string(model->received_bytes) + " bytes"}));
        last_presentation = std::chrono::steady_clock::now();
      }
    }
  } catch (...) {
    failure = std::current_exception();
  }
  co_await context.Cleanup(SaveSession(model->directory, model->received_bytes));
  model->stream_result = "Connection released and session checkpoint saved";
  if (failure) std::rethrow_exception(failure);
}

const BackgroundTask maintenance_task{"com.huxerui.examples.systemtasks.maintenance", Maintain};
const ContinuedTask export_task{"com.huxerui.examples.systemtasks.export", Export};
const OngoingTask receive_task{"com.huxerui.examples.systemtasks.receive", Receive};

const char* StatusName(TaskStatus status) {
  switch (status) {
    case TaskStatus::Unknown: return "Unknown";
    case TaskStatus::Idle: return "Idle";
    case TaskStatus::Pending: return "Pending";
    case TaskStatus::Running: return "Running";
    case TaskStatus::Stopping: return "Stopping";
    case TaskStatus::Completed: return "Completed";
    case TaskStatus::Stopped: return "Stopped";
    case TaskStatus::Failed: return "Failed";
    case TaskStatus::Interrupted: return "Interrupted";
  }
  return "Unknown";
}

const char* StopReasonName(TaskStopReason reason) {
  switch (reason) {
    case TaskStopReason::Requested: return "Requested";
    case TaskStopReason::System: return "System";
    case TaskStopReason::Shutdown: return "Shutdown";
    case TaskStopReason::Deadline: return "Deadline";
  }
  return "Unknown";
}

/// Uses the same snapshot for lifecycle, platform support, progress, and failure presentation.
View TaskDetails(SystemTaskCapabilities capabilities, const TaskState& state) {
  std::string description = capabilities.supported ? StatusName(state.Status()) : "Unsupported on this host";
  if (state.StopReason()) description += std::string{" | Stop reason "} + StopReasonName(*state.StopReason());
  if (state.Error()) description += " | " + state.Error()->message;
  const auto& progress = state.Progress();
  const float fraction = progress && progress->total
      ? static_cast<float>(static_cast<double>(progress->completed) / *progress->total) : 0.0F;
  return Column {
    Text(description),
    Text(capabilities.supported
        ? (capabilities.can_launch_process ? "Platform cold activation available" : "Process-local execution")
        : "The current backend does not provide this task kind"),
    progress ? Text(std::to_string(progress->completed) +
        (progress->total ? " / " + std::to_string(*progress->total) : "") + " units") : Text("Progress not reported"),
    state.IsActive() && (!progress || !progress->total) ? ProgressBar() : ProgressBar(fraction),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

template <class T>
void ShowResult(const std::shared_ptr<DemoViewModel>& model, const SystemTaskResult<T>& result) {
  model->message = result.Succeeded()
      ? (result.Value() == T::NoChange ? "No change" : "Request accepted") : result.Error().message;
}

/// Observes platform submission from the UI's ordinary TaskScope.
Task<void> ScheduleMaintenance(std::shared_ptr<DemoViewModel> model) {
  ShowResult(model, co_await maintenance_task.ScheduleAsync());
}

/// Requests authorization under the original UI execution context.
Task<void> RequestNotificationPermission(std::shared_ptr<DemoViewModel> model, LocalNotificationHandle notifications) {
  const auto result = co_await notifications.RequestAuthorizationAsync();
  model->message = result == PermissionStatus::Granted || result == PermissionStatus::Provisional
      ? "Notification permission granted" : "Check notification permission in system settings";
}

void StartExport(const std::shared_ptr<DemoViewModel>& model, bool stop_immediately) {
  const auto result = export_task.Start(export_instance, {"Export dataset", "Preparing local output"});
  ShowResult(model, result);
  if (stop_immediately && result.Succeeded()) ShowResult(model, export_task.RequestStop(export_instance));
}

[[huxerui::composable]] View SystemTaskContent() {
  auto model = UseService<DemoViewModel>();
  auto ui_tasks = UseTaskScope();
  auto notifications = UseApplication().LocalNotifications();
  const auto export_state = export_task.GetState(export_instance).value_or(TaskState{});
  const auto maintenance_state = maintenance_task.GetState();
  const auto receive_state = receive_task.GetState();
  const auto export_capabilities = export_task.GetCapabilities();
  const auto maintenance_capabilities = maintenance_task.GetCapabilities();
  const auto receive_capabilities = receive_task.GetCapabilities();
  const auto& theme = UseTheme();

  return ScrollView {
    Column {
      Text("System tasks", TextRole::Title),
      Text("Start work, leave the foreground, then return to observe application-owned state."),
      Text(model->message),
      Button("Request notification permission").OnClick([=] {
        ui_tasks.Launch(RequestNotificationPermission(model, notifications));
      }),
      Text("ContinuedTask | export a dataset", TextRole::Title),
      Flow {
        Button("32 MiB").With(Enabled(!export_state.IsActive())).OnClick([model] { model->export_mebibytes = 32; }),
        Button("128 MiB").With(Enabled(!export_state.IsActive())).OnClick([model] { model->export_mebibytes = 128; }),
        Button("512 MiB").With(Enabled(!export_state.IsActive())).OnClick([model] { model->export_mebibytes = 512; }),
      }.With(Spacing(theme.spacing.small)),
      Text::Format("{} MiB selected", model->export_mebibytes),
      Flow {
        Button("Export").With(Enabled(export_capabilities.supported && !export_state.IsActive()))
            .OnClick([model] { StartExport(model, false); }),
        Button("Start then stop").With(Enabled(export_capabilities.supported && !export_state.IsActive()))
            .OnClick([model] { StartExport(model, true); }),
        Button("Stop export").With(Enabled(export_state.IsActive()))
            .OnClick([model] { ShowResult(model, export_task.RequestStop(export_instance)); }),
      }.With(Spacing(theme.spacing.small)),
      TaskDetails(export_capabilities, export_state),
      Text(model->export_result),
      Text("BackgroundTask | verify completed exports", TextRole::Title),
      Text("The system chooses when maintenance runs. Accepted work may stay Pending."),
      Flow {
        Button("Schedule maintenance")
            .With(Enabled(maintenance_capabilities.supported && !maintenance_state.IsActive()))
            .OnClick([model, ui_tasks] {
              ui_tasks.Launch(ScheduleMaintenance(model));
            }),
        Button("Withdraw or stop").With(Enabled(maintenance_state.IsActive()))
            .OnClick([model] { ShowResult(model, maintenance_task.RequestStop()); }),
      }.With(Spacing(theme.spacing.small)),
      TaskDetails(maintenance_capabilities, maintenance_state),
      Text(model->index_result),
      Text("OngoingTask | receive streaming HTTP data", TextRole::Title),
      Text("Use an HTTPS streaming endpoint. Android data-sync limits can end the session."),
      TextField(model->stream_url).Label("Stream URL").Placeholder("https://your-server.example/stream")
          .With(Enabled(!receive_state.IsActive()))
          .OnChanged([model](const TextEditingValue& value) { model->stream_url = value; }),
      Flow {
        Button("Receive").With(Enabled(receive_capabilities.supported && !receive_state.IsActive() &&
                                      !model->stream_url->text.empty()))
            .OnClick([model] { ShowResult(model, receive_task.Start({"Receive data", "Connecting"})); }),
        Button("Stop receiving").With(Enabled(receive_state.IsActive()))
            .OnClick([model] { ShowResult(model, receive_task.RequestStop()); }),
      }.With(Spacing(theme.spacing.small)),
      TaskDetails(receive_capabilities, receive_state),
      Text::Format("{} bytes received", model->received_bytes),
      Text(model->stream_result),
      Text("Files and checkpoints", TextRole::Title),
      Text(model->directory.Path()),
    }.With(
        Padding(theme.spacing.large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(ScrollBar(), Background(theme.colors.background));
}

void InstallDemo(ApplicationContext& context) {
  auto model = std::make_shared<DemoViewModel>(UseApplication().Directories().data_directory.Child("system_tasks"));
  if (auto index = model->directory.Child("index.txt").ReadString(); index.Succeeded()) {
    model->index_result = std::move(index).Value();
  }
  if (auto session = model->directory.Child("stream-session.txt").ReadString(); session.Succeeded()) {
    model->stream_result = std::move(session).Value();
  }
  context.Provide(std::move(model));
}

View App() {
  return MaterialTheme {
    SystemTaskContent(),
  };
}

} // namespace

const Application application{
    App,
    {
        .window = {.title = "HuxerUI System Tasks", .initial_size = {760.0F, 820.0F}},
        .application_hooks = {InstallDemo},
        .system_tasks = {maintenance_task, export_task, receive_task},
    },
};
