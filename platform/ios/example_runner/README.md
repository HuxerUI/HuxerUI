# iOS Example Runner

Open `HuxerUIExamples.xcodeproj` to run and debug repository examples on a Simulator or physical device.

The default application target is `example_ui_gallery`. To select another example, copy `Config/Local.xcconfig.example` to the ignored `Config/Local.xcconfig` and set `HUXERUI_APP_TARGET` to an `example_*` target declared under `examples/`.

Set `DEVELOPMENT_TEAM` in the same local file when physical-device signing requires it. The Xcode build phase configures the repository for the selected SDK and architecture, builds only the selected application core, and links it into the shared native runner.

When `HUXERUI_APP_TARGET` is `example_application`, open its registered URL scheme on the booted Simulator with:

```bash
xcrun simctl openurl booted "huxerui-example://documents/42"
```

The runner also declares text documents, so the Files share sheet can open a UTF-8 text file with the application example. URL schemes and document types are native application metadata; generated applications declare their own values in `App/Info.plist`.

## System tasks example

Set `HUXERUI_APP_TARGET=example_system_tasks` and `HUXERUI_APP_INFO_PLIST=../../../examples/system_tasks/ios/Info.plist` in `Config/Local.xcconfig`. The selected Info.plist supplies the `processing` background mode and permitted task identifiers. The declaration identifiers use the default bundle prefix `com.huxerui.examples`; update the declarations and permitted identifiers together if changing the signing bundle identifier. See the [example instructions](../../../examples/system_tasks/README.md) for SDK requirements, expected support, and physical-device checks.

## Download notification example

Select the shared **HuxerUINotifications** scheme to build the dedicated notification application and its embedded **DownloadNotification** Content Extension. Setting only `HUXERUI_APP_TARGET=example_local_notification` on the generic HuxerUIExamples scheme does not embed the extension and therefore does not enable the example's template. The generic target has no dependency on the extension and keeps its existing behavior.

Both notification targets inherit `DEVELOPMENT_TEAM` from `Config/Local.xcconfig`. Set `HUXERUI_NOTIFICATION_BUNDLE_IDENTIFIER` there when signing requires an application identifier under your team; the extension automatically uses that identifier plus `.content`. Xcode owns extension signing and the PlugIns copy phase. The extension's SDK build precedes the application build through a target dependency; both use the same configuration- and SDK-specific CMake directory. The extension links only the SDK archive, without the application's force-loaded core, and enables extension-only API checks and dead-code stripping.

The extension's [Info.plist](../../../examples/local_notification/ios/Info.plist) declares `huxerui.local-notification.download`, matching the shared C++ template identifier. Its [view controller](../../../examples/local_notification/ios/notification_view_controller.mm) reads `HUXGetLocalNotificationData()` and uses UIKit labels, a progress bar, and an indeterminate indicator without constructing a Runtime, opening files, or starting downloads. The detailed-layout toggle controls extra byte-count text inside the extension; it cannot disable system expansion or customize the ordinary banner.

On a device, request notification authorization, start the 54.8 MB download, and expand the delivered notification to inspect the template. Open the application through the system notification action to inspect `file_name`, byte counts, and `status` in the activation log. Also check cancellation, completion, and launch from a retained notification after the app has terminated. iOS delivers notification interaction through `OnActivation()` even after a cold launch. A received notification is a snapshot: opening the extension does not establish a live progress subscription, and the download may be suspended when the application backgrounds. No background download or cross-process state sharing is provided.

Build and signing require Xcode on macOS. Use a physical device to validate actual delivery, expansion, application activation, and suspension behavior; source checks and Simulator builds do not establish those outcomes. See Apple's [Content Extension contract](https://developer.apple.com/documentation/usernotificationsui/unnotificationcontentextension) for the system presentation boundary.
