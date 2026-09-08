#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <huxerui/platform_registry.h>
#include <huxerui/resource.h>
#include <huxerui/task.h>

namespace huxerui {

/// Identifies an application-level runtime permission with shared cross-platform semantics.
enum class Permission {
  /// Access to cameras used for still-image or video capture.
  Camera,
  /// Access to microphones used for audio capture.
  Microphone,
};

/// Describes the current authorization state of an application capability.
///
/// Each capability returns only states supported by its native authorization model. A grant describes current access
/// and does not promise indefinite authorization. Notification authorization also uses this type.
enum class PermissionStatus {
  /// The platform has not yet asked the user to decide.
  NotDetermined,
  /// The application currently has access.
  Granted,
  /// Access is currently denied, but the platform does not reliably expose whether another prompt is possible.
  Denied,
  /// Access is denied and the platform explicitly reports that an in-application request cannot prompt again.
  PermanentlyDenied,
  /// Access is blocked by system, parental, or administrative policy.
  Restricted,
  /// The host, platform API, or native application declaration cannot provide this permission capability.
  Unavailable,
  /// Trial access is granted with reduced behavior, such as quiet notification delivery.
  ///
  /// Returned only when the native service explicitly reports provisional authorization. This does not describe
  /// a fixed expiration time; applications may request ordinary authorization when appropriate.
  Provisional,
};

class ApplicationHandle;
class Environment;

namespace detail {
class LocalNotificationService;
}

/// Reports independently available local-notification operations for the currently connected host.
///
/// These flags describe configured native capabilities, not authorization, delivery policy, or whether a submitted
/// notification will become visible. Applications must not infer one flag from another.
struct LocalNotificationCapabilities {
  /// Whether the host can submit an immediate notification to its native notification service.
  bool can_show = false;
  /// Whether the host can retain a one-shot scheduled request after the HuxerUI process terminates.
  bool can_schedule = false;
  /// Whether the host can cancel pending and remove delivered HuxerUI notifications by stable identifier.
  bool can_cancel = false;
  /// Whether a primary notification interaction can enter the application activation path.
  bool can_activate = false;
  /// Whether the host has any application-shell template mechanism.
  ///
  /// This does not guarantee that a particular template identifier is declared or available.
  bool can_use_templates = false;

  bool operator==(const LocalNotificationCapabilities&) const = default;
};

/// Describes the terminal result of a local-notification mutation.
enum class LocalNotificationOperationStatus {
  /// The native service accepted the requested state transition, without a delivery or visibility guarantee.
  Accepted,
  /// The host supports the operation, but current notification authorization prevents it.
  Unauthorized,
  /// The operation, requested template, or required native application configuration is unavailable.
  Unavailable,
  /// A supported and configured native notification operation failed while applying the request.
  Failed,
};

/// Requests the host's ordinary system-controlled notification presentation.
///
/// The operating system retains control over the final layout and foreground presentation behavior.
struct DefaultNotificationPresentation {
  bool operator==(const DefaultNotificationPresentation&) const = default;
};

/// Selects an application-shell-defined native notification template.
///
/// A template is not a HuxerUI View and has no cross-platform layout schema. The native application shell declares
/// supported identifiers and owns the template UI. Unsupported identifiers return `Unavailable` rather than falling
/// back to `DefaultNotificationPresentation`.
struct TemplateNotificationPresentation {
  /// Non-empty application-owned UTF-8 identifier, without null characters, declared by the native shell.
  std::string identifier;

  bool operator==(const TemplateNotificationPresentation&) const = default;
};

/// Selects ordinary system presentation by default or one application-shell template.
using LocalNotificationPresentation = std::variant<DefaultNotificationPresentation, TemplateNotificationPresentation>;

/// Declares the platform-neutral content of one local operating-system notification.
///
/// Submission validates all identifiers and resolved text as UTF-8 without embedded null characters. At least one of
/// the resolved title or body must be non-empty.
struct LocalNotification {
  /// Non-empty stable application-owned UTF-8 identifier, without null characters.
  ///
  /// This is a logical identifier used for replacement, cancellation, and activation rather than a native handle,
  /// integer tag, or request identifier.
  std::string identifier;
  /// Optional title resolved when submitted against the Environment captured by `LocalNotificationHandle`.
  StringVariant title;
  /// Optional body resolved with the title. An accepted schedule retains the resolved strings.
  StringVariant body;
  /// Presentation request. The default uses ordinary system-controlled notification UI.
  LocalNotificationPresentation presentation;
  /// Application-owned snapshot supplied to native templates and returned with primary activation.
  ///
  /// Null is the default. Only scalar, byte, list, and object values are supported; retained ExternalTexture,
  /// FileReference, and BufferReference capabilities are rejected recursively. The HUXP encoding must not exceed 64 KiB.
  /// This is persisted notification content, not protected storage or live application state.
  PlatformPayload data;
};

/// Represents application activation through primary interaction with a local notification.
///
/// This value enters `ApplicationActivation`; dismissals and custom actions are not represented. It intentionally
/// carries the submitted data snapshot, which may be stale or originate from an external activation. Applications
/// validate business fields and authorization before acting on it.
struct NotificationActivation {
  /// Stable application-owned identifier supplied with the submitted notification.
  std::string identifier;
  /// The submitted application data snapshot, or Null when no data was supplied.
  PlatformPayload data;

  bool operator==(const NotificationActivation&) const = default;
};

/// Provides application access to local operating-system notifications for the current Runtime.
///
/// Obtain this handle during composition through `UseApplication().LocalNotifications()` and capture it into
/// UI-thread event handlers or Tasks. The handle captures the current Environment for localized content resolution.
/// Authorization checks run independently; authorization requests, presentation, scheduling, and cancellation retain
/// submission order within one Runtime. Awaiting Tasks resume through the Runtime's UI dispatcher.
///
/// Native application configuration and final product policy remain application-shell responsibilities. A captured
/// handle remains safe after Runtime destruction and then reports no capabilities and `Unavailable` operation results.
/// Destroying a Runtime does not withdraw notifications or schedules already accepted by the operating system.
class LocalNotificationHandle final {
public:
  /// Returns the connected host's current independent capability snapshot.
  ///
  /// All flags are false after the owning Runtime disconnects. This result is not an authorization query.
  ///
  /// @return The independently available operations for the currently connected host.
  [[nodiscard]] LocalNotificationCapabilities Capabilities() const;

  /// Queries the current notification authorization without presenting system UI.
  ///
  /// `Granted` and `Provisional` permit submission; provisional authorization retains native presentation limits.
  /// Native ephemeral authorization maps to `Granted` while effective, without exposing its expiration time.
  ///
  /// @return A Task resolving to the current notification authorization, or `Unavailable` when it cannot be queried.
  [[nodiscard]] Task<PermissionStatus> CheckAuthorizationAsync() const;

  /// Requests notification authorization when the host and native application configuration permit it.
  ///
  /// This operation may present native UI, but a host with no applicable prompt may only return its current state.
  ///
  /// @return A Task resolving to the notification authorization reported after the request.
  [[nodiscard]] Task<PermissionStatus> RequestAuthorizationAsync() const;

  /// Requests immediate operating-system presentation.
  ///
  /// Reusing an identifier replaces both the matching pending schedule and delivered presentation. `Accepted` means
  /// only that the native service accepted the state transition; authorization settings and operating-system policy
  /// may still suppress or delay presentation. An unknown or unsupported template returns `Unavailable` without
  /// falling back to system presentation.
  ///
  /// @param notification Platform-neutral content, stable identifier, and requested presentation.
  /// @return A Task resolving to the terminal submission result reported by the connected host.
  /// @throws std::invalid_argument if an identifier or resolved text is invalid, both resolved text fields are empty,
  /// a template identifier is invalid, or data contains retained capabilities or exceeds the encoded size limit.
  [[nodiscard]] Task<LocalNotificationOperationStatus> ShowAsync(LocalNotification notification) const;

  /// Requests best-effort one-shot delivery at an absolute system-clock time.
  ///
  /// Reusing an identifier replaces the matching pending schedule but does not remove an already delivered
  /// presentation. The absolute time is not a local calendar value and does not change with the device time zone.
  /// `Accepted` does not guarantee an exact delivery time, user visibility, or persistence across device restart.
  /// Localized content is resolved when this method is called and is not relocalized after acceptance. An unknown or
  /// unsupported template returns `Unavailable` without falling back to system presentation.
  ///
  /// @param notification Platform-neutral content, stable identifier, and requested presentation.
  /// @param delivery_time Absolute system-clock time that must be later than this call.
  /// @return A Task resolving to the terminal scheduling result reported by the connected host.
  /// @throws std::invalid_argument under the same content conditions as `ShowAsync()`, or when `delivery_time` is not
  /// later than the call.
  [[nodiscard]] Task<LocalNotificationOperationStatus>
  ScheduleAsync(LocalNotification notification, std::chrono::system_clock::time_point delivery_time) const;

  /// Idempotently cancels a pending schedule and removes a delivered presentation for an identifier.
  ///
  /// An identifier that is already absent returns `Accepted` when the host supports the operation.
  ///
  /// @param identifier Stable logical identifier previously supplied through `LocalNotification`.
  /// @return A Task resolving to the terminal cancellation result reported by the connected host.
  /// @throws std::invalid_argument if `identifier` is empty, malformed UTF-8, or contains a null character.
  [[nodiscard]] Task<LocalNotificationOperationStatus> CancelAsync(std::string_view identifier) const;

private:
  LocalNotificationHandle(std::shared_ptr<detail::LocalNotificationService> service,
                          std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::LocalNotificationService> service_;
  std::shared_ptr<const Environment> environment_;

  friend class ApplicationHandle;
};

#if defined(_WIN32)
namespace windows {

/// Registers an application-owned URL scheme for the current executable and Windows user.
///
/// Call explicitly from application entry code, normally before RunApplication(). Creates a persistent
/// HKCU registration with a quoted executable path and URL argument; no installer, administrator, COM, or Runtime
/// is required. Repeated registration by the same executable may update its display name. Other executables,
/// non-protocol keys, and machine-wide registrations are rejected rather than replaced. The user's default-app
/// choice is never changed, so registration does not guarantee that Windows will route every URL to this app.
/// Registration survives process exit. Validate incoming UrlActivation content before performing application actions.
/// Available in the Windows 7 compatibility backend as well as the default Windows backend.
///
/// @param scheme Bare, case-insensitive ASCII scheme: a letter followed by letters, digits, '+', '-', or '.'.
/// Use 2 to 255 characters; single-letter schemes conflict with Windows drive designators. Do not include ':'.
/// @param display_name Non-empty UTF-8 application name without embedded nulls or line breaks.
/// @throws std::invalid_argument if a parameter is invalid.
/// @throws std::runtime_error if ownership conflicts or a native operation fails.
void RegisterUrlScheme(std::string_view scheme, std::string_view display_name);

/// Removes only a matching current-user URL scheme registration owned by the current executable.
///
/// An absent registration is a no-op. Prior registration in this process is not required. Call explicitly before
/// removing or relocating the executable, not during ordinary exit. Does not alter machine-wide registrations,
/// other users, or default-app choices. An installer running as another user cannot clean up this user's registration.
///
/// @param scheme Bare scheme originally passed to RegisterUrlScheme(), with the same syntax requirements.
/// @throws std::invalid_argument if the scheme is invalid.
/// @throws std::runtime_error if ownership conflicts or a native operation fails.
void UnregisterUrlScheme(std::string_view scheme);

/// Supplies application-owned ToastGeneric XML for a Windows notification template.
///
/// Called synchronously on the host UI thread when a template is submitted, including when scheduling it, not at
/// delivery time. Parameters are borrowed for the call only; title/body are resolved UTF-8 and data is the submitted
/// resource-free snapshot. Do not retain references, block on I/O, or capture composition/Runtime-owned state.
/// Return nullopt for an unknown template (Unavailable). Empty, malformed, oversized XML or a thrown exception causes
/// Failed. Return one UTF-8 `<toast>` document with one `<visual>` and `<binding template="ToastGeneric">`, escaping
/// dynamic text and attributes. DTDs and external entities are forbidden. Image resources must remain accessible until
/// native delivery. The final XML, including framework activation data, must fit 5 KiB.
/// The framework injects launch and owns identity, primary activation, replacement, scheduling, and cancellation.
/// Do not supply launch, activationType, or protocolActivationTargetApplicationPfn on `<toast>`. Actions, input, and
/// header elements are unsupported because their separate activation semantics are not part of NotificationActivation.
/// Native XML feature/version requirements and final layout remain Windows-owned. This is not a HuxerUI View or an
/// in-place progress update API; ShowAsync still replaces the notification and may display another popup.
using LocalNotificationTemplateProvider =
    std::function<std::optional<std::string>(std::string_view template_identifier, std::string_view title,
                                            std::string_view body, const PlatformPayload& data)>;

/// Registers the current executable's local-notification identity for the current Windows user.
///
/// Call on the application entry thread before RunApplication() on every launch, including notification launches.
/// The framework copies the identity for this process's notification host; no CMake metadata is required.
/// Only one identity may be configured per process until explicitly unregistered. Repeated registration is allowed
/// for the same executable; an identity owned by another executable or a machine-wide COM registration is rejected.
/// Creates a current-user Start menu shortcut named after app_id, or preserves a matching shortcut at that path.
/// Other shortcut locations are not searched or modified. No administrator privileges, installer, Runtime, or
/// notification submission is required.
/// Persistent registration survives process exit so scheduled notifications and later clicks can launch the app.
/// This operation does not grant notification permission or guarantee visible delivery.
/// Successful registration replaces the in-process template provider, including clearing it when omitted. Configure
/// it before RunApplication(); do not register again while a host is running. The provider is retained in process
/// configuration and copied into the host; it is never persisted in the registry or required to handle a later click.
///
/// @param app_id Stable ASCII application ID, starting with a letter or digit, using letters, digits, '.', '_',
/// and '-', with at most 128 characters. Use a distinct ID and CLSID for a development executable.
/// @param display_name Non-empty UTF-8 application name without embedded nulls or line breaks.
/// @param activator_clsid Stable, non-null COM GUID in braced form: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}".
/// @param template_provider Optional native XML provider. An empty callback disables template capability; default
/// presentation never calls it. Unknown template identifiers return Unavailable without fallback.
/// @throws std::invalid_argument if an identity parameter is invalid.
/// @throws std::logic_error if this process already configured a different identity.
/// @throws std::runtime_error if ownership conflicts, a native operation fails,
/// or the Windows 7 compatibility backend is in use.
void RegisterLocalNotifications(
    std::string_view app_id, std::string_view display_name, std::string_view activator_clsid,
    LocalNotificationTemplateProvider template_provider = {});

/// Removes the current executable's current-user local-notification registration.
///
/// Call on the application entry thread before RunApplication() or after it returns, never while its notification
/// host is running. Cancels scheduled notifications and clears delivered history before removing owned registration.
/// Only the matching current-user COM/app identity and the matching framework-created shortcut path are removed;
/// installer-owned shortcuts, other executables, other users, application files, and application data are preserved.
/// An absent registration is a no-op. Do not call this during ordinary exit: notifications can outlive the process.
/// The application owns when to invoke cleanup before removal or relocation; machine-wide uninstall does not imply
/// cleanup for every user. Native cleanup is not transactional and can partially complete before an error is reported.
///
/// @param app_id Application ID originally passed to RegisterLocalNotifications().
/// @param activator_clsid COM GUID originally passed to RegisterLocalNotifications(). Prior registration in this
/// process is not required. Successful cleanup also clears the matching in-process identity.
/// @throws std::invalid_argument if an identity parameter is invalid.
/// @throws std::runtime_error if ownership conflicts, a native operation fails,
/// or the Windows 7 compatibility backend is in use.
void UnregisterLocalNotifications(std::string_view app_id, std::string_view activator_clsid);

} // namespace windows
#endif

} // namespace huxerui
