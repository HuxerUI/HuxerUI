# Application Permissions

`ApplicationHandle` is the common entry point for application authorization. A library can use the same API as the application. The application package owns permission declarations, privacy text, entitlements, and packaging identity. The application chooses when and why to request access.

## Public contract

```cpp
Task<PermissionStatus> CheckPermissionAsync(Permission permission) const;
Task<PermissionStatus> RequestPermissionAsync(Permission permission) const;
Task<bool> OpenPermissionSettingsAsync(
    Permission permission, PermissionSettingsTarget target = PermissionSettingsTarget::Application) const;
```

`CheckPermissionAsync()` never presents UI. `RequestPermissionAsync()` requests the selected capability and can return its current status without a prompt when the operating system requires the user to change a setting. An operation never opens Settings implicitly. Invalid permissions and settings targets throw `std::invalid_argument` before a Task is returned.

`Application` opens a page where the user can manage this application's access, such as the application details page or a privacy page listing applications. `System` opens a system-wide page for the requested capability. An unsupported target returns `false`. A successful result means the request to open Settings was accepted. Recheck when the application becomes active to observe any change.

| Status | Meaning |
| --- | --- |
| `NotDetermined` | The user has not made an authorization decision. |
| `Granted` | The requested access is available now. |
| `Denied` | Access is denied. Another prompt may be possible. |
| `PermanentlyDenied` | The application cannot prompt again. The user may still change settings. |
| `Restricted` | A policy such as parental controls or device management restricts access. |
| `Unavailable` | The host does not support the permission, or the application lacks a required declaration. |
| `Provisional` | The application has provisional access with reduced behavior, such as quiet notification delivery. |
| `Limited` | The user granted a smaller scope, such as selected photos, approximate location for `PreciseLocation`, or calendar write access for `CalendarReadWrite`. |
| `SystemDisabled` | The switch for this capability is off across applications. Check again after it is enabled. |

The controller reads authorization from the host on each check. It reports permanent denial, policy restrictions, and system-wide disablement only when the underlying API distinguishes those states. A grant can change independently of the availability of the hardware or service.

## Supported capabilities

Each cell describes **check / request / settings**. `Prompt` means the host can present an authorization dialog when needed. `Status` means the request returns the current status without prompting. `App` and `System` are the settings targets. A dash means the operation is unavailable. iOS settings use the application page unless stated otherwise. macOS has no supported settings target in this contract.

| Permission | Android | iOS | macOS | Windows | Web |
| --- | --- | --- | --- | --- | --- |
| `Camera`, `Microphone` | Yes / Prompt / App | Yes / Prompt / App | Yes / Prompt / — | Yes / Prompt / App, System | Yes / — / — |
| `Location` | Yes / Prompt / App, System | Yes / Prompt / App | Yes / Prompt / — | Yes / Prompt / System | Yes / — / — |
| `PreciseLocation` | Yes / Prompt / App, System | Yes / Initial prompt or Status / App | Yes / Initial prompt or Status / — | — | — |
| `BackgroundLocation` | Yes / Android 10 prompt or Status / App, System | Yes / Status / App | — | — | — |
| `Notifications` | Yes / Prompt on Android 13+ / App | Yes / Prompt / App | Yes / Prompt / — | Yes / Status / App, System | Yes / Prompt with browser activation / — |
| `ContactsRead` | Yes / Prompt / App | Yes / Prompt / App | Yes / Prompt / — | — | — |
| `CalendarAdd` | Yes / Prompt / App | Yes / Prompt on iOS 17+ / App | Yes / Prompt on macOS 14+ / — | — | — |
| `CalendarReadWrite` | Yes / Prompt / App | Yes / Prompt / App | Yes / Prompt / — | — | — |
| `PhotosRead`, `VideosRead` | Yes / Prompt / App | Yes / Prompt / App | Yes / Prompt / — | — | — |
| `PhotoLibraryAdd` | Yes / Status on Android 10+, prompt on earlier releases / App | Yes / Prompt / App | Yes / Prompt / — | — | — |
| `BluetoothScan`, `BluetoothConnect` | Yes / Prompt when required / App, System | Yes / Prompt / App | Yes / Prompt / — | — | — |

Linux currently has no standalone permission transport. Its location portal grants access through a location session. Browser permission prompts tied to a feature operation, such as `getUserMedia()` and `getCurrentPosition()`, stay with that operation.

### Android

The manifest declares the Android permissions the application uses. Requests and settings use the original Activity launcher installed on `HuxerUIView` or `HuxerUIApplication`. An embedded host may implement the launcher's array overload for requests that require several Android permission strings, and its Intent overload for specific settings pages. Single-permission launchers keep their existing behavior.

- `Location` uses the `ACCESS_COARSE_LOCATION` declaration and accepts coarse or fine access while the application is in use. `PreciseLocation` requests fine and coarse together and reports `Limited` when only coarse is granted. `BackgroundLocation` first requires foreground access. Android 10 can show a separate permission dialog. Android 11 and later use the application's permission settings. Before Android 10, the permission check uses the foreground grant because there is no independent background grant.
- A confirmed location switch that is off reports `SystemDisabled`. `System` opens `ACTION_LOCATION_SOURCE_SETTINGS`. An Android user restriction on sharing location reports `Restricted`.
- `Notifications` checks app-level settings and, on Android 13 and later, the declared `POST_NOTIFICATIONS` permission. Only apps targeting Android 13 or later control its prompt. The application settings target uses the notification settings page when available. The notification service uses the same application authorization and checks its configured channels separately for delivery.
- `ContactsRead` requests `READ_CONTACTS`. `CalendarAdd` uses `WRITE_CALENDAR` and `CalendarReadWrite` requires both `READ_CALENDAR` and `WRITE_CALENDAR`. Android does not offer a narrower calendar add-only grant.
- `PhotosRead` and `VideosRead` use `READ_MEDIA_IMAGES` and `READ_MEDIA_VIDEO` when both the operating system and target SDK are Android 13 or later. The legacy path uses `READ_EXTERNAL_STORAGE`. On Android 14 and later the granular path requests `READ_MEDIA_VISUAL_USER_SELECTED` alongside the requested media type. Selected-media access reports `Limited` and can yield no items of a particular media type. `PhotoLibraryAdd` describes inserting new app-owned assets through MediaStore. Scoped storage allows that insertion without a permission request. On older releases and legacy storage, the application declares `WRITE_EXTERNAL_STORAGE` when required.
- `BluetoothScan` and `BluetoothConnect` use the corresponding runtime permissions when both the operating system and target SDK are Android 12 or later. The legacy path uses declared Bluetooth permissions and the location grant required for scanning. `System` opens the Bluetooth settings page. A known disabled Bluetooth adapter reports `SystemDisabled`.
- Camera and microphone use their manifest declarations and open the application settings page. Missing declarations report `Unavailable`. If Android cancels the request, HuxerUI returns `Unavailable`.

### iOS and macOS

Each backend checks the corresponding `Info.plist` usage descriptions before invoking protected APIs. The application supplies sandbox entitlements where applicable. Camera and microphone use AVFoundation, contacts use Contacts, calendars use EventKit, and media library access uses PhotoKit.

PhotoKit combines read and write access to both photos and videos in a single grant. An existing read-write grant, including limited access, satisfies `PhotoLibraryAdd`. A new request for `PhotoLibraryAdd` uses the add-only access level.

Calendar add-only requests are available starting on iOS 17 and macOS 14. Older releases can satisfy add access with an existing calendar grant. When an application already has write-only access, `CalendarReadWrite` reports `Limited`. A request can try to upgrade that grant if the application declares `NSCalendarsFullAccessUsageDescription`.

Location uses Core Location and reports approximate access as `Limited` for `PreciseLocation`. Foreground requests use `NSLocationWhenInUseUsageDescription` on iOS and `NSLocationUsageDescription` on macOS. The first foreground request can also serve a precise request and returns the authorization actually selected. An existing approximate grant can be upgraded in Settings. iOS checks background access when the application declares the corresponding usage description. Upgrades use the application settings page because declining an Always upgrade can leave Core Location's status unchanged without a delegate callback. macOS has no separate background scope in this API.

Bluetooth scan and connect share Core Bluetooth authorization. Queries read `CBManager.authorization`. A request can construct a central manager to present the Bluetooth permission dialog without starting a scan or connection.

Notifications reuse UserNotifications authorization for both application and notification-service calls. On iOS 16 and later, the app settings target for notifications uses the dedicated notification settings URL. Other iOS application targets use the app settings URL. iOS does not expose a supported deep link to the global location or Bluetooth switch.

### Windows and Web

Windows uses `AppCapability` for prompt-free queries when the API is present and `Geolocator::RequestAccessAsync()` for `Location` requests. The camera and microphone privacy pages can serve both settings targets. The location privacy page is available through the `System` target. The notification status uses the registered toast identity, with no separate permission dialog. Windows 7 compatibility builds report these permissions and settings as unavailable.

Web queries the Permissions API for `camera`, `microphone`, and `geolocation` when the browser supports them. Notifications use the browser's Notification API and request access only from an active user gesture. Browser site settings have no programmatic navigation in this contract. A browser grant does not imply that HuxerUI provides a notification transport on Web.

## Ownership and execution

One Runtime owns its permission controller through `ApplicationService`. The controller dispatches through the existing `PermissionTransport`, does not cache authorization, and serializes requests and settings launches. Queries may run independently. The [Local Notifications](local-notifications.md) service reuses the application's notification authorization and checks its own delivery requirements.

Completions resume through the owning Task dispatcher. A canceled Task detaches its continuation. A permission dialog that cannot be dismissed programmatically keeps the interaction active until the host completes it. An operation tied to an expired window or launcher cannot present through a replacement endpoint. Launch requests and settings from explicit application actions.

## Future candidates

The same entry points can add `SpeechRecognition`, `ContactsWrite`, `Reminders`, `AudioLibraryRead`, `BluetoothAdvertise`, `NearbyWifiDevices`, `LocalNetwork`, and `ActivityRecognition` once their authorization behavior is defined and tested. Android special access candidates include exact alarms, drawing over other apps, usage access, and all-files access. They require specific settings and state checks. Access to selected files, user-selected devices, and screen capture is acquired through the corresponding picker or session.
