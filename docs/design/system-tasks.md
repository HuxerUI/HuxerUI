# System Tasks Design

System task declarations share the application Runtime and ordinary coroutine execution. The [System Tasks guide](../guide/system-tasks.md) covers application setup. Ordinary Task behavior is defined in [Task and Structured Concurrency](tasks.md).

## Capability and ownership model

Three immutable declarations share one application-level execution mechanism. Business functions return ordinary Task values.

| Declaration | Initiation | Normal end | Examples |
| --- | --- | --- | --- |
| `BackgroundTask` | Submitted for discretionary system scheduling | Completion or a request to retry | Refresh, maintenance, deferred synchronization |
| `ContinuedTask` | Explicit user action, submitted immediately | Reaching a recognizable finite goal | One Agent run, export, analysis |
| `OngoingTask` | Explicit user action, submitted immediately | User, application, or system stops the activity | LAN session, ongoing device connection |

The distinction between finite and ongoing work is known before platform admission. A task with a recognizable goal must not be converted into an ongoing task merely because it takes a long time; a persistent session has no artificial completion percentage. The public declarations share execution, cancellation, platform dispatch, and observation internals without a public base, mode enum, executor, or second coroutine type.

The existing platform-derived Runtime owns installed declarations, platform task integration, request records, and active executions. An execution does not depend on any UiWindow, composition scope, Environment, or window service. Each execution retains only the state needed for its own lifetime; retained platform callbacks and Context values must not reconnect to a replacement Runtime or attempt. All handler entry and HuxerUI continuations run on the original application's thread. Blocking and CPU-intensive operations use the existing worker facilities.

Application services and ViewModels own inputs, checkpoints, streamed content, results, connection state, authentication, reconnect policy, and persistence. Framework task snapshots describe execution, not the application's business state. No separate process-global operation manager, general state store, callback bus, or worker pool is introduced. The existing Runtime and UiWindow ownership is defined by [Architecture](architecture.md).

## Common API

The declarations, contexts, snapshots, and results belong in the standalone `<huxerui/system_task.h>`. The application header includes it for AppOptions, and the umbrella header and C++ module expose the same declarations. Ordinary coroutine facilities live in `<huxerui/task.h>`, and permission and local notification APIs live in `<huxerui/system.h>`. The following sketches omit private storage, framework-only construction, special members, and equality operators. The framework supplies Context values to handlers.

```cpp
enum class SystemTaskErrorCode {
  Unsupported,
  Unavailable,
  NotPermitted,
  NotForeground,
  Configuration,
  Busy,
  Rejected,
  Platform,
  Handler,
};

struct SystemTaskError {
  SystemTaskErrorCode code;
  std::string message;
};

template <class T>
using SystemTaskResult = Result<T, SystemTaskError>;

enum class SubmissionStatus {
  Accepted,
  NoChange,
};

enum class StopRequestStatus {
  Requested,
  NoChange,
};

enum class TaskStopReason {
  Requested,
  System,
  Shutdown,
  Deadline,
};

enum class TaskStatus {
  Unknown,
  Idle,
  Pending,
  Running,
  Stopping,
  Completed,
  Stopped,
  Failed,
  Interrupted,
};

struct TaskPresentation {
  StringVariant title;
  StringVariant subtitle;
};

struct TaskProgress {
  std::uint64_t completed = 0;
  std::optional<std::uint64_t> total;
};

struct SystemTaskCapabilities {
  bool supported = false;
  bool can_launch_process = false;
};

class TaskStopped final : public std::exception {
public:
  const char* what() const noexcept override;
};

class TaskState final {
public:
  TaskStatus Status() const noexcept;
  bool IsActive() const noexcept;
  bool IsTerminal() const noexcept;

  const std::optional<TaskProgress>& Progress() const noexcept;
  const std::optional<TaskPresentation>& Presentation() const noexcept;
  std::optional<TaskStopReason> StopReason() const noexcept;
  const std::optional<SystemTaskError>& Error() const noexcept;
};
```

`Accepted` means that the platform host accepted the submission, not that the handler started or will complete. `NoChange` is successful idempotence and does not restart work or modify an existing presentation. `Requested` acknowledges a stop request, not completed teardown.

`Pending` covers admission in progress, accepted scheduling, queueing, and platform startup before handler execution. `Unknown` represents an indeterminate platform state, including initial reconciliation; it must not be treated as confirmation that no request exists. `Idle` means that no current request is known after the relevant state has been established. `IsActive()` includes Pending, Running, and Stopping; Unknown is neither an active nor a terminal assertion. `IsTerminal()` includes Completed, Stopped, Failed, and Interrupted. `Interrupted` records loss of execution or failure to complete cooperative teardown within its budget.

The snapshot contains private Runtime and request identity for safe waiting and forgetting. Request IDs, attempt counters, revision counters, platform handles, and persistent task history are not public API. Snapshot identity changes when a new request replaces a terminal record, even when its visible values are otherwise equal.

Error describes the latest framework mutation or execution error and may accompany a nonterminal state. For example, a failed withdrawal must not publish Stopped while work may still exist. A successful new request clears the previous execution's error, progress, and stop reason; observation alone does not clear errors. Expected platform failures use SystemTaskResult, with Configuration for invalid platform declarations and NotForeground for a rejected initiation context. Invalid caller input throws std::invalid_argument; invalid thread, lifetime, or framework usage throws std::logic_error. Framework diagnostics start with HuxerUI.

Capabilities describe the backend and configured execution mechanism, not current authorization or guaranteed admission. `can_launch_process` means that the platform mechanism can deliver this work when its process is absent, subject to platform configuration and system/user policy. It does not promise delivery after force quit or imply automatic restart of a user-started task. Callers that require cold activation inspect this flag before submitting; submission results remain authoritative.

## Declarations

```cpp
class BackgroundTask final {
public:
  enum class Result {
    Completed,
    Retry,
  };

  struct Schedule {
    std::optional<std::chrono::system_clock::time_point> earliest_start;
    bool requires_network = false;
    bool requires_external_power = false;
  };

  class Context {
  public:
    std::stop_token StopToken() const noexcept;
    std::optional<TaskStopReason> StopReason() const;
    std::optional<std::chrono::steady_clock::time_point> Deadline() const;
    void ThrowIfStopRequested() const;
    Task<void> Cleanup(Task<void> task) const;

    SystemTaskResult<void> ReportProgress(TaskProgress progress) const;
  };

  using Handler = std::function<Task<Result>(Context)>;

  BackgroundTask(std::string identifier, Handler handler);

  std::string_view Identifier() const noexcept;
  SystemTaskCapabilities GetCapabilities() const;

  Task<SystemTaskResult<SubmissionStatus>> ScheduleAsync(Schedule schedule) const;
  Task<SystemTaskResult<SubmissionStatus>> ScheduleAsync() const;
  SystemTaskResult<StopRequestStatus> RequestStop() const;
  TaskState GetState() const;
  Task<SystemTaskResult<TaskState>> WaitAsync() const;
};

class ContinuedTask final {
public:
  enum class SubmissionPolicy {
    RequireImmediate,
    AllowQueue,
  };

  class Context {
  public:
    std::stop_token StopToken() const noexcept;
    std::optional<TaskStopReason> StopReason() const;
    std::optional<std::chrono::steady_clock::time_point> Deadline() const;
    void ThrowIfStopRequested() const;
    Task<void> Cleanup(Task<void> task) const;

    std::string InstanceId() const;
    SystemTaskResult<void> ReportProgress(TaskProgress progress) const;
    SystemTaskResult<void> UpdatePresentation(TaskPresentation presentation) const;
  };

  using Handler = std::function<Task<void>(Context)>;

  ContinuedTask(std::string identifier, Handler handler);

  std::string_view Identifier() const noexcept;
  SystemTaskCapabilities GetCapabilities() const;

  SystemTaskResult<SubmissionStatus> Start(
      std::string instance_id,
      TaskPresentation presentation,
      SubmissionPolicy policy = SubmissionPolicy::RequireImmediate
  ) const;

  SystemTaskResult<StopRequestStatus> RequestStop(std::string_view instance_id) const;
  std::optional<TaskState> GetState(std::string_view instance_id) const;
  Task<SystemTaskResult<TaskState>> WaitAsync(std::string_view instance_id) const;
  bool Forget(std::string_view instance_id, const TaskState& observed_state) const;
};

class OngoingTask final {
public:
  class Context {
  public:
    std::stop_token StopToken() const noexcept;
    std::optional<TaskStopReason> StopReason() const;
    std::optional<std::chrono::steady_clock::time_point> Deadline() const;
    void ThrowIfStopRequested() const;
    Task<void> Cleanup(Task<void> task) const;

    SystemTaskResult<void> UpdatePresentation(TaskPresentation presentation) const;
  };

  using Handler = std::function<Task<void>(Context)>;

  OngoingTask(std::string identifier, Handler handler);

  std::string_view Identifier() const noexcept;
  SystemTaskCapabilities GetCapabilities() const;

  SystemTaskResult<SubmissionStatus> Start(TaskPresentation presentation) const;
  SystemTaskResult<StopRequestStatus> RequestStop() const;
  TaskState GetState() const;
  Task<SystemTaskResult<TaskState>> WaitAsync() const;
};
```

SystemTask is the installation type. Each alternative retains its own handler signature, initiation methods, and Context.

```cpp
using SystemTask = std::variant<BackgroundTask, ContinuedTask, OngoingTask>;
```

AppOptions gains a single declaration list and a shared stop budget after its installation hooks:

```cpp
std::vector<SystemTask> system_tasks{};
std::chrono::duration<double> system_task_stop_timeout = std::chrono::seconds(5);
```

The timeout is the application's maximum cooperative stop budget, not an execution-time guarantee from the operating system. It must be positive and finite, and a shorter platform deadline always wins. All stopping executions share the application shutdown deadline; the application does not wait this duration separately for each execution.

## Declaration identity and installation

A declaration owns an immutable identifier and handler; copies refer to that same declaration. Copying or destroying a declaration does not start or stop work. Construction is independent of Runtime, allowing stable declarations before Application construction. Identifiers are nonempty UTF-8 without embedded nulls and unique throughout `AppOptions::system_tasks`; reverse-DNS identifiers are recommended. Empty handlers and malformed identifiers are invalid caller input; duplicate installed identifiers are rejected during installation, including repeated copies of one declaration.

AppOptions is the only C++ handler catalog. The platform shell supplies required platform metadata and must not become another handler registry. Application services, platform factories, and task declarations freeze before queued handler work is admitted. No Connect or public binding object is required.

Declarations themselves are unbound definitions, and their control methods resolve the current valid application context. Contexts, platform callbacks, waiting objects, and snapshot identity remain bound to their original Runtime and execution. Resolving a stale original context never falls back to a newer hosted Runtime. Side effects and observations require the application thread; immutable declaration metadata and already-extracted stop tokens do not require UI.

Startup follows this sequence:

```text
prepare platform Runtime facilities
    -> install application services and validate task declarations
    -> register platform entry points and retain early platform events
    -> invoke application_hooks
    -> freeze services, factories, and task declarations
    -> admit application dispatch and task execution
    -> create UiWindow only when the activation requires UI
```

Background-only activation invokes neither the root factory nor window hooks. Android must load the application's native library before resolving its Application declaration from a job or service entry; loading only the SDK library is insufficient. iOS registers scheduled handlers during application launch before launch returns. Platform events arriving during installation remain queued until application services are ready.

Start, RequestStop, and Forget are side effects and are rejected during composition. Start is also invalid during application or window installation. ScheduleAsync and WaitAsync create lazy Tasks without submitting platform work or installing observers until consumed. Their wrappers capture owned arguments and original application identity before returning; they do not borrow string views or coroutine-lambda temporaries across suspension. GetState and GetCapabilities may be read during composition or ordinary application-thread execution.

## Submission, instances, and execution

| Stage | BackgroundTask | ContinuedTask and OngoingTask |
| --- | --- | --- |
| Application initiation | Await ScheduleAsync from a running Task | Call Start directly from the user action |
| Platform submission | Submit finite scheduling constraints | Submit through the eligible platform initiation path |
| Awaiting execution | Pending | Pending, including permitted queueing |
| Platform grant established | Start an application-owned handler and publish Running | Same |
| Handler work | Ordinary Task values and application services | Same |
| Handler end | Complete or request system retry | Complete finite work or stop ongoing work |
| Finalization | Retire this execution and finish its platform protocol | Same |

Start reaches the platform host before returning from the initiating event; only handler execution is deferred. The framework never runs the handler speculatively before the required platform grant exists. Runtime must not mark an Android task Running before foreground-service presence is established. RequireImmediate rejects admission when immediate execution cannot be accepted; AllowQueue explicitly permits system queueing, not durable framework job storage. A host without a system queue can still accept AllowQueue when it can start immediately; it must not create a private deferred queue when it cannot start.

ScheduleAsync separates the submission observer from the submitted work. Cancellation before submission begins prevents the submission. After submission begins, canceling the waiting Task detaches observation and does not roll back a platform request that may already have been accepted. RequestStop is the explicit withdrawal path.

| Repeated operation | Result |
| --- | --- |
| Identical BackgroundTask request already pending or running | NoChange |
| Different BackgroundTask schedule while a request is pending or running | Busy; do not silently replace or run in parallel |
| Same ContinuedTask and active instance ID | NoChange; retain the original work and presentation |
| Different ContinuedTask instance IDs | Independent requests, subject to platform capacity and policy |
| OngoingTask already pending or running | NoChange |
| Start while the target is Stopping | Busy |
| Start after a terminal execution | A fresh execution with reset per-execution state |

BackgroundTask and OngoingTask each retain one current record per declaration. ContinuedTask retains one current record per business instance ID, allowing concurrent exports or Agent runs. Instance IDs are nonempty UTF-8 without embedded nulls; the backend encodes them safely when creating platform identifiers rather than appending arbitrary business text directly. Prepare inputs and durable business identity before Start; the handler may run as soon as platform admission allows. Multiple connections within one ongoing session remain the responsibility of the application's session service.

Pending includes submission in progress and does not by itself assert platform acceptance. An admission failure for a new request publishes Failed with its error; a Busy rejection against an existing execution leaves that execution intact. If the platform outcome cannot be determined, publish Unknown and reconcile rather than asserting either Idle or accepted execution. While reconciliation prevents a safe new submission, return Unavailable instead of creating a potential duplicate.

Only BackgroundTask has a retry outcome. Retry requests another system scheduling opportunity with preserved requirements and platform-appropriate backoff; it does not promise a time or eventual execution. The next request remains Pending while awaiting system scheduling, and a known rejection becomes Failed. System interruption may also cause the system scheduler to retry finite background work; handlers must tolerate duplicate delivery and resume from application checkpoints. Explicit withdrawal prevents framework retries; ContinuedTask and OngoingTask have no automatic restart policy. The framework does not promise exactly-once business execution.

## Observation, progress, and retained records

GetState returns an owned coherent snapshot. Reading it during composition subscribes the current scope through existing State dependency mechanisms; multiple windows may observe the same application execution. A ContinuedTask lookup subscribes to appearance and removal of its record even when the initial result is empty. Outside composition, GetState is an immediate query and installs no long-lived subscription. UI invalidations may coalesce; observation does not promise every intermediate progress value or state transition as an event stream. No additional UseTaskStatus hook or public status callback convention is introduced.

TaskState owns no business result or task history. BackgroundTask and OngoingTask replace their bounded current record when a new request creates a record. ContinuedTask terminal records remain until the same instance starts again or Forget removes them. Forget succeeds only for the terminal record identified by the supplied observed snapshot; stale, active, missing, or different-Runtime records return false. Forgetting does not delete application data, cancel work, or clear platform requests.

Progress is optional until first reported. Completed units never decrease within an execution; a known total must be positive, at least the completed value, and may increase when additional work is discovered. An unknown total uses std::nullopt; once known it does not return to unknown during that execution. Platform numeric range limits are checked without truncation. Framework timers, heartbeat messages, title changes, and elapsed time do not fabricate progress. BackgroundTask progress serves observers without requiring platform progress UI; ContinuedTask reports real work to any platform progress mechanism required by its platform. Unknown total is not a promise of unlimited continued-processing time.

Presentation strings are resolved against application resources into platform-owned text at Start or UpdatePresentation. The resolved title is nonempty and text is validated without capturing a window Environment or Theme. Desktop implementations may retain presentation only in the snapshot rather than creating a tray icon or ordinary local notification. The platform owns its task presentation and Stop actions; these are not ordinary local-notification scheduling operations. Clicking platform presentation returns to the application through platform activation behavior; task Context does not acquire or choose a window. Existing Application activation and explicit retained WindowHandles remain the application's routing tools.

Progress and presentation mutations do not control framework status. A late Context update returns Unavailable rather than targeting a newer attempt. A platform update failure is returned and recorded without falsely terminating an otherwise live execution; the application chooses whether the failure should end its business work.

## Waiting

WaitAsync synchronously selects the current request identity when called, then returns a lazy observer Task. It follows that request to a terminal state, including BackgroundTask retries, and never follows a subsequent Start. Cancellation only removes the observer and has no stop or withdrawal side effect. Completing an execution queues waiting continuations through their own Task execution owner; it never resumes them inline inside a platform callback or finalizing handler.

BackgroundTask or OngoingTask with no request returns an Idle snapshot immediately. A missing ContinuedTask instance, an unsupported observation target, or indeterminate platform state that prevents selecting a request returns an appropriate SystemTaskResult failure, normally Unavailable or Unsupported. An already-terminal request returns its current snapshot immediately. The outer Result describes acquisition and observation; a successfully observed execution may itself have Failed, with details in TaskState. Waiting across a replacement Runtime is invalid, and closing the observing TaskScope suppresses delivery as usual.

For example, from an application Task:

```cpp
auto request = lan_task.RequestStop();
if (request.Succeeded()) {
  auto result = co_await lan_task.WaitAsync();
  if (result.Succeeded()) {
    HandleStoppedState(result.Value());
  }
}
```

## Cooperative stop and cleanup

RequestStop is idempotent. For pending work it withdraws the platform request; for running work it signals this execution's stop token and initiates cooperative teardown. Stopping is not Stopped: a failed withdrawal leaves the platform work represented accurately, with the error available to the caller and snapshot. The first known stop cause is retained; platform APIs that do not distinguish user cancellation from system expiration report System rather than an invented distinction. Deadline is optional while executing and becomes the earliest applicable platform or framework stop/cleanup deadline when one is established.

Ordinary TaskHandle cancellation retains its existing frame-destruction semantics and never starts throwing TaskStopped. System stop uses the existing execution machinery with per-suspension retirement guards, safely ends participating framework waits, and delivers TaskStopped at a cooperative boundary. Delay, HTTP, file operations, AwaitCallback, RunWorker, and WorkerSequence must participate in this path before system tasks are advertised as supported. An old completion cannot resume a later suspension of the same coroutine. Running worker code is not preempted; it must observe a copied stop token when prompt stopping is required, and its independently retained resources survive until that code actually returns. Third-party awaitables that directly resume arbitrary coroutines do not automatically gain HuxerUI stop or thread-restoration guarantees.

Cleanup permits one awaited top-level cleanup Task within the execution's remaining budget; that Task may await several cleanup operations. It is valid only from this handler's Task chain, shields repeated framework stop exceptions, and never resets the original stop token or extends platform execution rights. It does not permit launching fresh system work or escaping into a new TaskScope. Normal completion may also use Cleanup for bounded asynchronous release; subsequent stop or shutdown can only shorten the existing budget. After platform execution is revoked or the budget expires, cleanup may be suppressed or canceled and the execution becomes Interrupted. RAII still releases what can be released synchronously, and neither callbacks nor destructors are guaranteed after process termination.

Because C++ does not allow co_await inside an exception handler, asynchronous cleanup is written after the catch block:

```cpp
Task<void> MaintainLan(OngoingTask::Context context) {
  auto session = UseService<LanSession>();
  std::exception_ptr failure;

  try {
    co_await session->RunAsync(context.StopToken());
  } catch (...) {
    failure = std::current_exception();
  }

  co_await context.Cleanup(session->CloseAsync());

  if (failure) {
    std::rethrow_exception(failure);
  }
}
```

LanSession is an application-owned service; it owns discovery, sockets, authentication, heartbeat, reconnect, and resynchronization. Cleanup failure becomes an execution failure; combining business and cleanup diagnostics remains application policy.

| Observed outcome | Published result |
| --- | --- |
| BackgroundTask returns Completed | Completed |
| BackgroundTask returns Retry | Request system rescheduling; Pending, or Failed on known rejection |
| ContinuedTask returns normally | Completed |
| OngoingTask returns normally | Stopped |
| Cooperative stop completes | Stopped |
| Another exception escapes the handler | Failed with a Handler error |
| Platform allowance is revoked or teardown misses its deadline | Interrupted when normal completion or cooperative teardown could not be established |

A completion latched before a stop request wins and the late request returns NoChange. If stop was accepted first, a later normal return is stopped rather than completed; an actual cleanup/handler exception can still publish Failed. System roots catch TaskStopped separately and translate other escaping exceptions into failure and platform finalization. The ordinary root Task exception policy is unchanged.

Local retirement and platform protocol finalization are guarded per request and execution allowance. Duplicate or stale callbacks and platform Stop actions cannot finish an allowance twice or stop a newer execution. Finalization follows the actual platform protocol: Android onStopJob already retires its job allowance, so a later C++ completion does not repeat jobFinished. These guards promise safe local behavior, not exactly-once remote delivery or business execution.

## Application shutdown and recovery

Orderly shutdown uses the existing deferred Runtime shutdown boundary:

```text
close new-work admission
    -> retire UiWindows and composition Tasks
    -> close ordinary application TaskScope
    -> request all active system executions to stop
    -> retain services, HTTP, timers, and dispatch needed by their bounded cleanup
    -> finish platform protocols or exhaust the shared shutdown deadline
    -> release application facilities
```

Only already-admitted system executions can continue their bounded nested cleanup; ordinary Launch/Post admission is not reopened. Shutdown never blocks the application thread waiting for work whose continuations require that thread. Forced Runtime retirement and destructor paths signal system stop tokens before destroying suspended executions and do not wait for asynchronous cleanup. Late platform events must acknowledge or retire their own platform allowance without accessing a destroyed Runtime. Android foreground service startup carries its initial notification so a canceled or retired request can still complete platform promotion and stop without admitting its handler.

Closing a UiWindow does not by itself cancel system work. The existing quit_on_last_window_closed policy still controls whether closing the last owned desktop window requests application shutdown; active tasks do not silently override it. An initially windowless Runtime does not request exit merely because no window exists. Explicit Quit remains effective. BackgroundTask requests already accepted for future system delivery remain installed by default; RequestStop explicitly withdraws them.

Business checkpoints are saved incrementally, not only during stop. After cold activation the backend reconciles platform requests it can actually establish and publishes Pending or another justified state. Framework snapshots, streamed results, attempt counters, and prior terminal records are not persistent. ContinuedTask and OngoingTask do not silently restart after process death; the application reconstructs business state and initiates a new eligible execution. FileReference access retains its existing lifetime and grant rules, and an in-memory or temporary reference does not become durable job input. Ordinary HttpClient transports do not become operating-system-owned background transfer sessions.

## Platform mapping

| Platform | BackgroundTask | ContinuedTask | OngoingTask |
| --- | --- | --- | --- |
| Android API 23+ | JobScheduler, with eligible cold activation | Appropriately declared foreground service | Appropriately declared foreground service |
| iOS/iPadOS 15-25 | BGProcessingTask, with eligible cold activation | Unsupported | Generic ongoing sessions unsupported |
| iOS/iPadOS 26+ | BGProcessingTask, with eligible cold activation | BGContinuedProcessingTask when compiled with the iOS 26 SDK or later | Generic ongoing sessions unsupported |
| macOS | NSBackgroundActivityScheduler while the process lives | Process-local | Process-local |
| Windows and Linux | Unsupported initially | Process-local | Process-local |
| Web | Unsupported initially | Unsupported initially | Unsupported initially |

BackgroundTask initially supports one-shot scheduling with earliest start, network, and external-power requirements. Earliest start is a lower bound, not an exact execution time; a requested requirement is preserved or rejected as Unsupported. An in-process timer is not a fallback for unavailable system scheduling. macOS reports can_launch_process as false and rejects requirements it cannot honor. Windows Task Scheduler, Linux service/timer installation, and macOS launch helpers require separately configured platform-shell integrations before support is advertised.

Android uses the system scheduler without introducing a required WorkManager dependency for this initial contract. Job IDs and platform metadata must not overwrite unrelated application or library work. SDK-owned platform entry code is shared; applications configure components by actual foreground-service type and may associate several declaration IDs with one appropriate component. Completing one instance must not stop other instances using that component, and unrelated platform service types are not merged just to share code. Platform notification channels, small icons, permissions, component declarations, and eligibility remain shell-owned. Task presentation is established within platform startup deadlines, and timeout callbacks stop platform foreground execution within the platform budget. Denied POST_NOTIFICATIONS authorization is not itself proof that foreground-service startup is prohibited, although the service must still supply its notification. User-initiated data-transfer jobs are not a generic substitute for arbitrary Agent computation or ongoing sessions.

Applications can declare `org.huxerui.HuxerUIForegroundTaskService` directly in the manifest. Subclasses provide distinct platform component identities when independent services are needed.

Android task components and the UI Runtime use the application's default process. Validate the resolved service configuration before submission and return Configuration for a different process or an isolated service. The default process can be cold-started by JobService without an Activity. The application shell loads its native library during process initialization so that the job entry can initialize the Runtime and run the same application hooks. If an Activity subsequently attaches to that live Runtime, it observes the same execution records and State. A future independent-process backend requires explicit routing of commands and snapshots, including disconnection and recovery semantics.

Android uses a scheduler namespace on API 34 and later. On earlier versions the backend checks occupied job IDs before submission and verifies the component and token before cancellation. JobService revocation ends the current attempt and publishes Interrupted. A handler that returns Retry requests system backoff through jobFinished.

iOS preserves the deployment minimum while compiling the continued-processing path with a suitable SDK and checking runtime availability. The shell lists permitted identifiers and processing capabilities, including a valid identifier strategy for continued instances. ContinuedTask begins through an explicit foreground user action, reports actual progress, handles expiration, and completes its platform task. RequireImmediate and AllowQueue preserve their distinct platform submission semantics; system queueing is not a durable framework queue. UIApplication.beginBackgroundTask is not an equivalent fallback for many minutes of Agent work on older systems. Generic LAN TCP, UDP, or WebSocket work cannot use unrelated audio, location, accessory, or VoIP modes to acquire continuous execution.

iOS retains only the submitted background schedule and its correlation token in application preferences so cold delivery can preserve constraints on Retry. If a delivered grant has neither recoverable scheduling data nor a platform pending snapshot, the handler can complete and Retry reports Unavailable.

Platform manifests, Info.plist, entitlements, permissions, notification content, and packaging remain application-shell responsibilities. CMake can select reusable platform code and frameworks but does not generate or mutate those declarations. GetCapabilities does not grant permission, and starting a system task never implicitly creates a window or displays a permission prompt.

## Agent application behavior

One user-requested reply or finite Agent run belongs in ContinuedTask when the application wants it to continue after leaving the foreground. The Send or Run action saves business identity and submits ContinuedTask before beginning the turn's asynchronous work. The handler owns the streamed request and local tools, writes response content and checkpoints to application-owned state/storage, and returns when the goal completes. Backgrounding then changes UI availability, not this execution's owner; returning to the foreground observes the same application's latest data while its process survives.

An operation already launched in an ordinary TaskScope is not automatically promoted by a lifecycle callback. UseApplicationTaskScope survives window retirement but does not request platform background execution. A long-running request is not BackgroundTask merely because it runs in the background, and a finite Agent turn is not OngoingTask merely because its duration is unknown.

Android execution requires acceptance under an appropriate real foreground-service category and remains subject to system limits. iOS 26 continued processing can support qualifying finite user-visible work, but progress reporting, resource conditions, expiration, or user action can end it before completion. Unknown work totals and streamed token counts are not guarantees of extended execution; an indefinitely waiting conversation or a run needing foreground user consent must checkpoint and yield appropriately. Older iOS versions have no equivalent general long-running continued-processing guarantee in this design.

Remote generation and client execution have separate lifetimes. A server-owned run may continue after the client loses its execution allowance only if the application protocol provides that behavior. Persist a server run ID and sequence/checkpoint so foreground recovery can query or resume delivery; completion notifications may use a separately supported notification/push integration. Stopping a local task does not automatically cancel a remote job, and neither an SSE nor WebSocket connection is promised to survive suspension or process termination.

## Future backend extension

Shared contracts use capabilities and platform outcomes rather than a closed platform list. A future HarmonyOS backend can reuse the three declarations while implementing application-thread dispatch, platform admission and expiration, notification/presentation, platform completion, cold entry where available, and host-owned configuration. This is an extension direction, not a support claim or an implementation included in the initial platform matrix.

WorkScheduler and its ExtensionAbility are candidates for finite discretionary work; permitted continuous-background modes are candidates for eligible finite continued or ongoing activities. Actual mapping must be established by supported SDK version, device class, permissions, task purpose, concurrency limits, and platform lifecycle. A computational mode or data-transfer mode is not automatically available or appropriate for every Agent application. An asynchronous platform admission result can arrive while the request is Pending; the handler starts only after a grant, and platform rejection is reflected in TaskState. Start must not block the application thread waiting for a platform promise. No fourth task class or HarmonyOS-specific enum in shared declarations is required merely to add the backend.

A backend whose background handler runs in another process creates its own application execution context and services there. State objects, service instances, and standard snapshots are not shared C++ memory across those processes; durable business data and any supported platform snapshot relay need explicit platform integration. The backend must report Unsupported for unavailable semantics rather than emulate them with a timer or promise uninterrupted background execution.

## Declaration and UI example

This example uses Delay as a stand-in for asynchronous business operations. Its single report and counters live in memory. Store production checkpoints and inputs by business instance ID before submission. The application root owns its composition scope.

```cpp
using namespace huxerui;

View App();

struct DemoViewModel {
  State<int> refreshes{0};
  State<int> exported_sections{0};
  State<int> received_batches{0};
  State<std::string> error{std::string{}};
};

void Check(SystemTaskResult<void> result) {
  if (!result.Succeeded()) {
    throw std::runtime_error(result.Error().message);
  }
}

template <class T>
void ShowResult(const std::shared_ptr<DemoViewModel>& model, const SystemTaskResult<T>& result) {
  model->error = result.Succeeded() ? std::string{} : result.Error().message;
}

Task<void> ExportSections(
    std::shared_ptr<DemoViewModel> model,
    std::function<void(TaskProgress)> progress = {}
) {
  for (int section = model->exported_sections; section < 10; ++section) {
    co_await Delay(1s);
    model->exported_sections = section + 1;
    if (progress) {
      progress({static_cast<std::uint64_t>(section + 1), 10});
    }
  }
}

Task<BackgroundTask::Result> Refresh(BackgroundTask::Context context) {
  auto model = UseService<DemoViewModel>();
  co_await Delay(1s);
  context.ThrowIfStopRequested();
  model->refreshes += 1;
  co_return BackgroundTask::Result::Completed;
}

Task<void> Export(ContinuedTask::Context context) {
  Check(context.UpdatePresentation({"Export report", context.InstanceId()}));
  co_await ExportSections(UseService<DemoViewModel>(), [context](TaskProgress progress) {
    Check(context.ReportProgress(progress));
  });
}

Task<void> Receive(OngoingTask::Context context) {
  auto model = UseService<DemoViewModel>();
  while (!context.StopToken().stop_requested()) {
    co_await Delay(2s);
    model->received_batches += 1;
    Check(context.UpdatePresentation({
      "LAN session",
      std::to_string(model->received_batches) + " batches",
    }));
  }
}

const BackgroundTask refresh_task{"com.example.refresh", Refresh};
const ContinuedTask export_task{"com.example.export", Export};
const OngoingTask lan_task{"com.example.lan", Receive};

void InstallApplication(ApplicationContext& context) {
  context.Provide(std::make_shared<DemoViewModel>());
}

View App() {
  auto model = UseService<DemoViewModel>();
  auto tasks = UseApplicationTaskScope();
  auto export_state = export_task.GetState("report-42");
  const bool exporting = export_state && export_state->IsActive();

  return Column {
    Text(exporting ? "Exporting" : "Ready"),
    Text::Format("Refreshes: {}", model->refreshes),
    Text::Format("Sections: {}", model->exported_sections),
    Text::Format("Batches: {}", model->received_batches),
    Button("Schedule refresh").OnClick([tasks, model] {
      tasks.Launch([model]() -> Task<void> {
        ShowResult(model, co_await refresh_task.ScheduleAsync());
      });
    }),
    Button("Export").OnClick([model] {
      ShowResult(model, export_task.Start("report-42", {"Export report", "Preparing"}));
    }),
    Button("Connect").OnClick([model] {
      ShowResult(model, lan_task.Start({"LAN session", "Connecting"}));
    }),
    Button("Withdraw refresh").OnClick([model] {
      ShowResult(model, refresh_task.RequestStop());
    }),
    Button("Stop export").OnClick([model] {
      ShowResult(model, export_task.RequestStop("report-42"));
    }),
    Button("Disconnect").OnClick([model] {
      ShowResult(model, lan_task.RequestStop());
    }),
    Text(model->error),
  }.With(Padding(16.0F), Spacing(8.0F));
}

const Application application{
  App,
  {
    .quit_on_last_window_closed = false,
    .application_hooks = {InstallApplication},
    .system_tasks = {refresh_task, export_task, lan_task},
  },
};
```

For a delayed refresh, a running application Task can await ScheduleAsync with an explicit schedule:

```cpp
auto result = co_await refresh_task.ScheduleAsync({
  .earliest_start = std::chrono::system_clock::now() + 15min,
  .requires_network = true,
});
ShowResult(model, result);
```

## Business reuse and UI lifetime

Handlers begin on the application thread and framework awaits restore that thread, allowing direct updates to application-owned State even when no UiWindow is present. Worker code does not read or write State; external callbacks enter through AwaitCallback or the original application's TaskScope::Post. Progress callbacks that outlive their attempt cannot redirect Context updates to another attempt. No PostToUi member or queue of captured UI callbacks is introduced.

The same business Task may be launched as ordinary UI work instead:

```cpp
auto ui_tasks = UseTaskScope();
ui_tasks.Launch([model]() -> Task<void> {
  co_await ExportSections(model);
});
```

Choose the owner when starting work; use ContinuedTask from the outset when platform continuation is part of the requirement. Launching another Task does not transfer an existing coroutine, platform allowance, or scope ownership. For an intentional change of owner, first submit the system request and stop the old execution after acceptance; the new handler waits through an application-owned resource protocol until prior work has actually quiesced, then resumes from a checkpoint. TaskHandle::Cancel and an AwaitCallback cancellation action do not prove the external operation has stopped, so business exclusivity and release acknowledgement must prevent overlapping work. If platform admission fails, the original execution can remain in place. The framework adds no Transfer, Promote, or general operation-lease API.

## Validation

Implementation must cover declaration validation, lazy argument ownership, per-Runtime binding, platform admission versus execution, identical and conflicting submissions, concurrent continued instances, stop/start and completion/stop races, initial platform reconciliation, and conditional terminal-record removal. Wait tests cover identity capture before consumption, observer cancellation, already-terminal and absent targets, retrying background requests, Runtime replacement, and non-reentrant completion. Stop tests cover each framework await path, AwaitCallback registration races, stale completions reaching a later suspension, worker resource lifetime, bounded Cleanup, platform expiration, and exactly-once local/platform retirement. Shutdown tests preserve services and timers only for existing cleanup, reject new launches, honor last-window policy, and prove that explicit Quit is not prevented by ongoing work. UI tests cover zero-window execution, multiple-window observation, compatible recomposition, record appearance/removal, and no replay of stale UI callbacks. Platform tests cover cold library loading, manifest/identifier mismatch, permissions and foreground eligibility, notification denial, shared service instance teardown, timeout, force termination, and iOS continued-processing queue/progress/expiration behavior.

Public declarations update header self-containment checks, umbrella exposure, generated module exports, and the owning documentation together. Implementation-only system-task contracts remain under src/application, ordinary coroutine changes stay under src/runtime, and platform code stays under its platform owner. Source/configuration changes update both CMake and mcpp as required by repository rules; generated files are regenerated by their tool rather than edited manually. No prebuilt patch is treated as an implementation authority over these contracts or current repository code.

## Primary references

- [Apple: Finish tasks in the background](https://developer.apple.com/videos/play/wwdc2025/227/) describes discretionary work, user-started continued processing, progress, queue/fail strategies, and expiration.
- [Apple: BGContinuedProcessingTask](https://developer.apple.com/documentation/backgroundtasks/bgcontinuedprocessingtask) defines platform continued-processing lifecycle and progress reporting.
- [Apple: Continued-processing queue strategy](https://developer.apple.com/documentation/backgroundtasks/bgcontinuedprocessingtaskrequest/submissionstrategy/queue) states queueing behavior and cancellation on app-switcher closure.
- [Apple: iOS Background Execution Limits](https://developer.apple.com/forums/thread/685525) distinguishes purpose-specific background mechanisms from arbitrary continuous execution.
- [Apple: NSBackgroundActivityScheduler](https://developer.apple.com/documentation/foundation/nsbackgroundactivityscheduler) defines discretionary process-local background activity scheduling.
- [Android: JobService](https://developer.android.com/reference/android/app/job/JobService) defines platform job activation, stop, and completion protocols.
- [Android: Foreground service types](https://developer.android.com/develop/background-work/services/fgs/service-types) defines category and permission requirements.
- [Android: Foreground service timeouts](https://developer.android.com/develop/background-work/services/fgs/timeout) defines system timeout and teardown requirements.
- [Android: Notification runtime permission](https://developer.android.com/develop/ui/compose/notifications/notification-permission) distinguishes notification authorization from foreground-service startup.
- [Android: User stopping foreground-service apps](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping) documents process termination without a cleanup callback.
- [Huawei: Continuous background tasks](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/continuous-task) describes purpose-specific system background modes and their constraints for future backend evaluation.
- [Huawei: Background task usage](https://developer.huawei.com/consumer/cn/doc/doccenter-app-quality/bpta-use-of-background-tasks) distinguishes short, continuous, and deferred system work.
