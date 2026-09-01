# Application Permissions

This document defines the application-facing contract for querying and requesting runtime permissions without moving native declarations or product policy into shared C++.

## Public model

Permissions are application capabilities exposed directly by `ApplicationHandle`:

```cpp
enum class Permission {
  Camera,
  Microphone,
};

enum class PermissionStatus {
  NotDetermined,
  Granted,
  Denied,
  PermanentlyDenied,
  Restricted,
  Unavailable,
};

Task<PermissionStatus> ApplicationHandle::CheckPermissionAsync(Permission permission) const;
Task<PermissionStatus> ApplicationHandle::RequestPermissionAsync(Permission permission) const;
Task<bool> ApplicationHandle::OpenPermissionSettingsAsync(Permission permission) const;
```

`CheckPermissionAsync()` never presents system UI.
`RequestPermissionAsync()` may present platform UI only when the native shell has declared the capability and the platform still permits a prompt.
`OpenPermissionSettingsAsync()` returns whether the platform accepted the request to open a relevant settings surface; it does not imply that authorization changed.

`Unavailable` is the explicit outcome for an unsupported permission, missing native declaration, unavailable platform API, or host that cannot provide the capability.
An invalid enum value remains caller input and throws `std::invalid_argument` before a Task is launched.

## Ownership and execution

One Runtime owns one private permission controller through its existing application service:

```text
ApplicationHandle
    -> ApplicationService
    -> PermissionController
    -> PermissionTransport
    -> native permission API
```

The controller does not cache authorization state, publish observers, or mirror platform policy.
Queries may run independently.
Requests and settings transitions are serialized because supported hosts permit only one application-owned permission interaction at a time.

Every transport completion is posted through the owning `UIThreadDispatcher` before the awaiting Task resumes.
Application code may therefore update `State` directly after `co_await`.
Canceling the owning Task detaches its continuation and invokes the platform cancellation callback when one exists.
An operation with a cancellation callback relinquishes its serialized interaction slot when that callback runs.
A native prompt without cancellation support retains the slot until its terminal platform callback, preventing overlapping system UI.
A late platform completion cannot resume retired application code.

Permission is not a Root Service and does not introduce `PermissionHandle`, `UsePermissions()`, a registry, or a second callback convention.
The private transport uses the same callback-to-Task bridge pattern as other asynchronous platform capabilities.

## Native declarations and policy

The platform application shell remains authoritative for manifest entries, privacy strings, entitlements, capabilities, packaging identity, request timing, rationale UI, and product policy.
HuxerUI does not infer or generate those declarations from CMake, `AppOptions`, or a permission call.

An application normally checks or requests a permission only in response to a user-visible feature:

```cpp
tasks.Launch([=]() -> Task<void> {
  const PermissionStatus status =
      co_await application.RequestPermissionAsync(Permission::Camera);
  camera_status = status;
});
```

The application decides how to explain denial, whether to offer settings navigation, and whether the requested feature has a non-permission fallback.

## Platform mapping

### Android

`Camera` maps to `android.permission.CAMERA` and `Microphone` maps to `android.permission.RECORD_AUDIO`.
The permission must appear in the application manifest.
A missing declaration reports `Unavailable`; a granted permission reports `Granted`; other query results report `Denied` because Android does not expose a durable first-request versus permanently-denied distinction without adding framework-owned history.
Requests use the Activity-owned launcher installed on `HuxerUIView`, and application settings use `ACTION_APPLICATION_DETAILS_SETTINGS`.
An embedded View whose owner does not install that launcher reports interactive operations as unavailable.

### iOS and macOS

Camera and microphone use `AVCaptureDevice` authorization.
The native bundle must provide a non-empty `NSCameraUsageDescription` or `NSMicrophoneUsageDescription`; a missing usage string reports `Unavailable` before invoking the API.
Apple authorization maps directly to `NotDetermined`, `Granted`, `Restricted`, or `PermanentlyDenied`.
iOS opens the application settings URL through UIKit.
macOS currently reports settings navigation as unavailable because there is no stable application-specific AV authorization settings API in the supported contract.

### Windows

The default Windows 10 backend uses `AppCapability` names `webcam` and `microphone` when that API is present.
`Allowed`, `UserPromptRequired`, `DeniedByUser`, `DeniedBySystem`, and `NotDeclaredByApp` map to `Granted`, `NotDetermined`, `PermanentlyDenied`, `Restricted`, and `Unavailable` respectively.
The application package remains responsible for capability declarations.
Settings navigation uses the corresponding Windows privacy settings URI.
The Windows 7 compatibility build reports the capability as unavailable.

### Web

Web queries `navigator.permissions` for `camera` or `microphone` when the browser supports that descriptor.
Browser states map to `Granted`, `NotDetermined`, or `Denied`, while rejected or unsupported queries report `Unavailable`.
The shared permission request does not call `getUserMedia()` because that API both requests authorization and acquires media; Web requests and settings navigation therefore report unavailable.

### Linux

The current Linux backend reports runtime camera and microphone permissions as unavailable.
The XDG PermissionStore is an implementation detail used by portals rather than an application-facing prompt API, so it is not treated as a general permission backend.

## Invariants

- Native declarations and application policy never flow into shared configuration.
- One query or request produces one typed terminal result.
- Unsupported behavior follows the normal Task path and returns `Unavailable` or `false`.
- Task resumption occurs through the owning Runtime UI dispatcher.
- Interactive operations are serialized without introducing global permission state.
- Runtime does not branch on a concrete permission or platform.
