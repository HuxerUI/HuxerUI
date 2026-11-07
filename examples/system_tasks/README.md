# System Tasks Example

`example_system_tasks` demonstrates platform task admission, application-owned State, cooperative stopping, and file checkpoints through public HuxerUI APIs.

| Task | Work | Platform support in this example |
| --- | --- | --- |
| `ContinuedTask` | Export a deterministic 32, 128, or 512 MiB binary dataset and report written bytes | Android foreground service; iOS 26 continued processing; process-local desktop execution |
| `BackgroundTask` | Validate completed exports and rebuild `index.txt` | Android JobScheduler; iOS background processing; process-local macOS scheduling |
| `OngoingTask` | Receive bytes from an HTTPS streaming endpoint until it ends or stops | Android foreground service; process-local desktop execution |

The controls use `GetCapabilities()` and show unsupported modes on other hosts. All handlers acquire the same application-owned `DemoViewModel`. HTTP work uses its directly constructed `HttpClient`; worker functions receive owned inputs and stop tokens and update State only after returning to the application thread.

Completed exports, the maintenance index, and the last stream checkpoint live under the displayed application data directory. Each export attempt has its own partial path. Its worker sequence orders cleanup after any in-flight write. Expiration or force termination can leave `.partial` files, which maintenance ignores. Stopping does not roll back an already completed file rename. Clear the example's application data when its retained exports are no longer needed.

Hard platform revocation or process termination can also prevent session cleanup from saving its final checkpoint. The saved file then describes the earlier completed session.

## Desktop

Build `example_system_tasks` in an existing configured repository build and run its output executable or application bundle. Closing the last desktop window exits the application and initiates system task stopping.

```sh
cmake --build <build-directory> --target example_system_tasks --parallel
```

Windows and Linux expose process-local continued and ongoing work; their Background controls are disabled. macOS also exposes discretionary process-local maintenance. The capability text identifies whether platform cold activation is available.

## Android

From `platform/android`, build the shared runner with this example selected. On Windows use `gradlew.bat`.

```sh
./gradlew :example_runner:assembleDebug -PhuxeruiExample=system_tasks
```

Install `example_runner/build/outputs/apk/debug/example_runner-debug.apk`. The example manifest directly declares the SDK's `HuxerUIBackgroundJobService` and `HuxerUIForegroundTaskService`, with data-sync service type, notification permissions, and channel metadata. `SystemTasksApplication` loads `BuildConfig.HUXERUI_APP_LIBRARY` during process initialization, allowing JobService to initialize the same application hooks without an Activity. The Activity and services use the application's default process.

Use **Request notification permission** to enable notification-drawer presentation on Android 13 and later. Export and reception share one data-sync service component and notification. The platform Stop action targets the displayed instance; the UI can stop each task independently. Data-sync foreground services remain subject to Android's eligibility, duration, and user-stop rules. [Android service types](https://developer.android.com/develop/background-work/services/fgs/service-types)

## iOS and iPadOS

Open `platform/ios/example_runner/HuxerUIExamples.xcodeproj` and select the **HuxerUIExamples** scheme. Copy `Config/Local.xcconfig.example` to the ignored `Config/Local.xcconfig`, then set:

```text
HUXERUI_APP_TARGET = example_system_tasks
HUXERUI_APP_INFO_PLIST = ../../../examples/system_tasks/ios/Info.plist
DEVELOPMENT_TEAM = YOUR_TEAM_ID
```

The example uses the runner's default bundle identifier `com.huxerui.examples`. When changing it for signing, update the declaration identifier prefix in `main.cpp` and the permitted identifiers in `ios/Info.plist` together; keep Android's declaration metadata aligned if building the same modified example there. The continued declaration must have the actual bundle identifier as its prefix.

The example Info.plist enables the `processing` background mode and lists maintenance plus the continued-processing wildcard. Use an iOS 26 SDK and iOS 26 or later for continued processing. Earlier supported iOS versions retain the Background demonstration and show Continued as unsupported. Ongoing remains unsupported on iOS. Use a physical device for system scheduling, progress presentation, expiration, and background execution checks. [Apple continued processing](https://developer.apple.com/documentation/backgroundtasks/performing-long-running-tasks-on-ios-and-ipados)

## Device checks

Choose **Export**, leave the foreground while output is being written, and return to inspect byte progress and the completed file path. Use the larger dataset when a smaller export finishes too quickly. **Stop export** exercises bounded cleanup; **Start then stop** exercises withdrawal during platform startup. Also stop from platform task presentation and compare the reported status and stop reason.

Choose **Schedule maintenance** after completing an export. Pending work waits for a system scheduling opportunity; earliest execution is not guaranteed. Withdraw it while pending, then submit again. To check cold activation, allow the OS to end the background process and later deliver eligible work. User force-stop or force-quit has different scheduling policy and is not a cold-activation guarantee. Reopening the app reads the saved index, while TaskState records reflect the current Runtime.

For reception, enter an HTTPS URL that streams response bytes, press **Receive**, leave the foreground, and return to inspect the received count. Use an endpoint under your control for predictable transfer duration. **Stop receiving** releases the response and saves the byte count through `Context::Cleanup()`. Network errors and finite responses also finish the session; the example does not reconnect automatically.

Source and build checks do not establish device behavior. Verify notification denial, rapid Start/Stop, platform Stop, foreground-service timeout, iOS expiration, and process loss on the relevant devices.
