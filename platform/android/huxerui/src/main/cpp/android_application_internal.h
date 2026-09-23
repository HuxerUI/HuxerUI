#pragma once

#include <jni.h>

#include <memory>
#include <optional>

#include <huxerui/app.h>

namespace huxerui::detail {

class LocalNotificationTransport;
class PermissionTransport;

/// Selects the originating window's Java presentation source for permissions and authorization.
/// @return A borrowed HuxerUIView for this application-thread call, or null for the explicit application endpoint.
/// An expired captured window is rejected instead of silently routing the request to another Activity.
jobject CurrentAndroidPresentationView();

/// Borrowed JNI activation envelope decoded synchronously before the native call returns.
/// kind selects the payload: absent (0), URI (1), file (2), or local notification (3).
/// value carries the URI or notification identifier; data contains notification action data when present.
struct AndroidApplicationActivationInput {
  jint kind = 0;
  jstring value = nullptr;
  jstring file_name = nullptr;
  jlong file_size = -1;
  jstring content_type = nullptr;
  jboolean writable = JNI_FALSE;
  jbyteArray data = nullptr;
};

/// Converts a Java activation envelope and retains any native resource required by its C++ value.
/// @param virtual_machine Owning VM used for retained file access.
/// @param environment JNI environment for the current application-thread invocation.
/// @param context Borrowed Android Context used to resolve file content.
/// @param input Borrowed strings and byte array; file_size is -1 when unknown.
/// @return A decoded activation, or nullopt when the envelope cannot identify a supported activation.
[[nodiscard]] std::optional<ApplicationActivation> DecodeAndroidApplicationActivation(
    JavaVM* virtual_machine,
    JNIEnv* environment,
    jobject context,
    const AndroidApplicationActivationInput& input
);

/// Creates the application's permission bridge; presentation is selected separately for each request.
/// @param virtual_machine VM used by the retained Java bridge.
/// @param environment JNI environment used to acquire the host's global reference and method IDs.
/// @param view Java HuxerUIApplication service host providing permission methods.
/// @return A transport retaining the Java application host until it is released.
[[nodiscard]] std::shared_ptr<PermissionTransport>
CreateAndroidPermissionTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view);

/// Creates the notification bridge independently of any attached HuxerUIView.
/// @param virtual_machine VM used by the retained Java bridge.
/// @param environment Current application-thread JNI environment used during initialization.
/// @param view Java HuxerUIApplication service host whose methods implement notification operations.
/// @return A transport retaining a global host reference until it is released.
[[nodiscard]] std::shared_ptr<LocalNotificationTransport>
CreateAndroidLocalNotificationTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view);

} // namespace huxerui::detail
