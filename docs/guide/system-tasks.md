# System Tasks

Include `<huxerui/system_task.h>` for the task declarations, or use the umbrella header or `import huxerui;`. Install declarations together in `AppOptions::system_tasks`. A declaration's identifier must be unique in that list and match any required platform configuration.

| Work | Declaration | Entry |
| --- | --- | --- |
| Maintenance or refresh at a system-selected opportunity | `BackgroundTask` | Await `ScheduleAsync()` |
| Finite work initiated by the user, such as an export or one Agent turn | `ContinuedTask` | Call `Start(instance_id, presentation)` in the user action |
| A user-started activity that lasts until it stops, such as an eligible device session | `OngoingTask` | Call `Start(presentation)` in the user action |

`Start()` submits to the platform host before returning. `ScheduleAsync()` submits when consumed by a running Task. Check `Succeeded()` on the returned `SystemTaskResult`, then inspect `Value()` or `Error()`. An accepted request may remain Pending until the platform host grants execution. `GetCapabilities()` reports host support and whether the mechanism can launch a missing application process.

Handlers return ordinary `Task` values and use application services on their original application's thread. Update application-owned `State` from the handler or its resumed Task chain. A view can read `GetState()` during composition to observe task lifecycle and progress. `WaitAsync()` observes the request selected when called, even if a later start replaces its record. Canceling the observing Task detaches that waiter. Use `RequestStop()` to withdraw or stop the work. `ContinuedTask::Forget()` releases a terminal instance snapshot identified by the observed state.

After a stop, participating framework awaits throw `TaskStopped`. The handler can catch it and then await one bounded `Context::Cleanup()` for asynchronous release. Pass `Context::StopToken()` to worker code that needs to cooperate. Store business inputs and checkpoints in application storage when they must survive process death. `UseApplicationTaskScope()` gives ordinary work application ownership. Use a system declaration when the operation also needs platform admission.

The [declaration and UI example](../design/system-tasks.md#declaration-and-ui-example) installs all three types, starts them from buttons, and observes their application state.

The runnable [`example_system_tasks`](../../examples/system_tasks/README.md) exports real files, verifies them during discretionary maintenance, and receives streaming HTTP data. It includes Android service configuration, iOS task identifiers, and device validation steps.

## Platform setup

| Host | Background | Continued | Ongoing |
| --- | --- | --- | --- |
| Android | JobScheduler with eligible cold activation | Foreground service | Foreground service |
| iOS and iPadOS | BGProcessingTask with eligible cold activation | BGContinuedProcessingTask with the iOS 26 SDK and iOS 26 or later | Unsupported |
| macOS | Process-local NSBackgroundActivityScheduler | Process-local | Process-local |
| Windows and Linux | Unsupported | Process-local | Process-local |
| Web | Unsupported | Unsupported | Unsupported |

### Android

Declare the background service in the application manifest. The service and the HuxerUI Activity must run in the application's default process. Load the application's C++ library in `Application.onCreate()` so a scheduled job can initialize HuxerUI before any Activity exists. In the CLI application shell the library name is available as `BuildConfig.HUXERUI_APP_LIBRARY`. The same application hooks install services for a windowless launch.

When scheduling with `requires_network = true`, declare `android.permission.ACCESS_NETWORK_STATE` in the manifest. Android enforces this permission for network-constrained jobs when targeting API 34 or later. [Android JobScheduler requirements](https://developer.android.com/about/versions/14/behavior-changes-14#jobscheduler-reinforces-callback-and-network-behavior)

```xml
<service
    android:name="org.huxerui.HuxerUIBackgroundJobService"
    android:exported="true"
    android:permission="android.permission.BIND_JOB_SERVICE" />
```

For user-started work, declare `org.huxerui.HuxerUIForegroundTaskService` directly in the application manifest. Match its type and permissions to the actual work. The following service handles a finite export eligible for the data sync category. Create the `exports` notification channel in application startup on API 26 and later.

```xml
<uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_DATA_SYNC" />

<application android:name=".DemoApplication">
    <service
        android:name="org.huxerui.HuxerUIForegroundTaskService"
        android:exported="false"
        android:foregroundServiceType="dataSync">
        <meta-data
            android:name="org.huxerui.system_task.identifiers"
            android:value="com.example.export" />
        <meta-data
            android:name="org.huxerui.system_task.channel"
            android:value="exports" />
        <meta-data
            android:name="org.huxerui.system_task.icon"
            android:resource="@drawable/ic_export" />
    </service>
</application>
```

A component can list comma-separated declaration identifiers that use the same platform service type. Applications needing independent platform service components can declare subclasses with their own component metadata. Active instances share the component's foreground notification. Its Stop action targets the displayed instance. Foreground service limits and runtime permissions depend on the selected type and Android version. [Android foreground service types](https://developer.android.com/develop/background-work/services/fgs/service-types)

### Apple platforms

For iOS background processing, enable the `processing` background mode and add each `BackgroundTask` identifier to `BGTaskSchedulerPermittedIdentifiers`. For iOS continued processing, use a declaration identifier prefixed by the app's bundle identifier, such as `com.example.app.export`. Add `com.example.app.export.*` to the permitted list. Each request registers and submits a platform identifier with its own safe suffix. Submit from a foreground user action and report actual completed units. [Apple background tasks](https://developer.apple.com/documentation/backgroundtasks)

On macOS, a scheduled background activity can run while the application process lives. Schedules requiring network availability or external power return Unsupported. On desktop, set `quit_on_last_window_closed` to false if the application should keep running after its last window closes. An explicit Quit begins bounded system task shutdown.
