#include "android_application_internal.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/android/jni.h>

#include "application/application_internal.h"
#include "application/system_task_internal.h"
#include "android_file_internal.h"
#include "io/file_internal.h"


namespace huxerui::detail {

namespace {

constexpr jint android_activation_url = 1;
constexpr jint android_activation_file = 2;
constexpr jint android_activation_notification = 3;

class JniEnvironment final {
public:
  explicit JniEnvironment(JavaVM* virtual_machine) : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr) {
      return;
    }
    if (virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment_), JNI_VERSION_1_6) == JNI_OK) {
      return;
    }
    if (virtual_machine_->AttachCurrentThread(&environment_, nullptr) == JNI_OK) {
      attached_ = true;
    } else {
      environment_ = nullptr;
    }
  }

  JniEnvironment(const JniEnvironment&) = delete;
  JniEnvironment& operator=(const JniEnvironment&) = delete;

  ~JniEnvironment() {
    if (attached_) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  [[nodiscard]] JNIEnv* Get() const noexcept {
    return environment_;
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  JNIEnv* environment_ = nullptr;
  bool attached_ = false;
};

void ClearJavaException(JNIEnv* environment) noexcept {
  if (environment != nullptr && environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

/// Captures the native endpoint generation for the current original-window or application presentation source.
/// @param virtual_machine VM used to acquire JNI access for this application-thread call.
/// @param host Java HuxerUIApplication service host retained by the transport.
/// @param method Cached presentationIdentity(HuxerUIView) method ID.
/// @return Endpoint generation, or zero when JNI facilities are unavailable.
/// @throws std::runtime_error If Java cannot resolve the endpoint identity.
std::uint64_t ReadPresentationIdentity(JavaVM* virtual_machine, jobject host, jmethodID method) {
  JniEnvironment attached(virtual_machine);
  JNIEnv* environment = attached.Get();
  if (!environment || !host || !method) return 0;
  const auto identity = environment->CallLongMethod(host, method, CurrentAndroidPresentationView());
  if (environment->ExceptionCheck()) {
    ClearJavaException(environment);
    throw std::runtime_error("HuxerUI could not capture the Android presentation endpoint");
  }
  return static_cast<std::uint64_t>(identity);
}

PermissionStatus ToPermissionStatus(jint status) noexcept {
  switch (status) {
  case 0:
    return PermissionStatus::NotDetermined;
  case 1:
    return PermissionStatus::Granted;
  case 2:
    return PermissionStatus::Denied;
  case 3:
    return PermissionStatus::PermanentlyDenied;
  case 4:
    return PermissionStatus::Restricted;
  case 5:
  default:
    return PermissionStatus::Unavailable;
  }
}

LocalNotificationOperationStatus ToOperationStatus(jint status) noexcept {
  switch (status) {
  case 0:
    return LocalNotificationOperationStatus::Accepted;
  case 1:
    return LocalNotificationOperationStatus::Unauthorized;
  case 2:
    return LocalNotificationOperationStatus::Unavailable;
  case 3:
  default:
    return LocalNotificationOperationStatus::Failed;
  }
}

LocalNotificationCapabilities ToCapabilities(jint capabilities) noexcept {
  return {
      .can_show = (capabilities & 1) != 0,
      .can_schedule = (capabilities & (1 << 1)) != 0,
      .can_cancel = (capabilities & (1 << 2)) != 0,
      .can_activate = (capabilities & (1 << 3)) != 0,
      .can_use_templates = (capabilities & (1 << 4)) != 0,
  };
}

class AndroidPermissionOperation final {
public:
  explicit AndroidPermissionOperation(PermissionStatusCompletion completion) : completion_(std::move(completion)) {}

  void Complete(jint status) noexcept {
    PermissionStatusCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      completion = std::move(completion_);
    }
    if (completion) {
      completion(ToPermissionStatus(status));
    }
  }

private:
  std::mutex mutex_;
  PermissionStatusCompletion completion_;
};

using AndroidPermissionOperationHandle = std::shared_ptr<AndroidPermissionOperation>;

class AndroidPermissionTransport final : public PermissionTransport {
public:
  AndroidPermissionTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view)
      : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr || environment == nullptr || view == nullptr) {
      throw std::invalid_argument("HuxerUI Android permission transport requires a Java VM, environment, and View");
    }
    view_ = environment->NewGlobalRef(view);
    jclass local_class = environment->GetObjectClass(view);
    if (view_ == nullptr || local_class == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android permission View could not be retained");
    }
    view_class_ = static_cast<jclass>(environment->NewGlobalRef(local_class));
    environment->DeleteLocalRef(local_class);
    check_ = environment->GetMethodID(view_class_, "checkPermission", "(I)I");
    presentation_identity_ = environment->GetMethodID(view_class_, "presentationIdentity", "(Lorg/huxerui/HuxerUIView;)J");
    request_ = environment->GetMethodID(view_class_, "requestPermission", "(JILorg/huxerui/HuxerUIView;)V");
    open_settings_ = environment->GetMethodID(view_class_, "openPermissionSettings", "(ILorg/huxerui/HuxerUIView;)Z");
    if (view_class_ == nullptr || presentation_identity_ == nullptr || check_ == nullptr || request_ == nullptr || open_settings_ == nullptr ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android permission methods do not match the platform backend");
    }
  }

  ~AndroidPermissionTransport() override {
    JniEnvironment attached(virtual_machine_);
    Release(attached.Get());
  }

  std::uint64_t PresentationIdentity() const override {
    return ReadPresentationIdentity(virtual_machine_, view_, presentation_identity_);
  }

  std::function<void()> Check(Permission permission, PermissionStatusCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    const jint status = environment->CallIntMethod(view_, check_, static_cast<jint>(permission));
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(PermissionStatus::Unavailable);
    } else {
      completion(ToPermissionStatus(status));
    }
    return {};
  }

  std::function<void()> Request(Permission permission, PermissionStatusCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    jobject source = CurrentAndroidPresentationView();
    auto operation = std::make_shared<AndroidPermissionOperation>(std::move(completion));
    auto native_handle = std::make_unique<AndroidPermissionOperationHandle>(operation);
    AndroidPermissionOperationHandle* transferred_handle = native_handle.release();
    environment->CallVoidMethod(
        view_,
        request_,
        static_cast<jlong>(reinterpret_cast<std::uintptr_t>(transferred_handle)),
        static_cast<jint>(permission), source);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      std::unique_ptr<AndroidPermissionOperationHandle> owner(transferred_handle);
      operation->Complete(5);
      return {};
    }
    return {};
  }

  std::function<void()> OpenSettings(Permission permission, PermissionSettingsCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(false);
      return {};
    }
    const bool opened =
        environment->CallBooleanMethod(view_, open_settings_, static_cast<jint>(permission),
                                       CurrentAndroidPresentationView()) == JNI_TRUE;
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(false);
    } else {
      completion(opened);
    }
    return {};
  }

private:
  void Release(JNIEnv* environment) noexcept {
    if (environment == nullptr) {
      return;
    }
    if (view_class_ != nullptr) {
      environment->DeleteGlobalRef(view_class_);
      view_class_ = nullptr;
    }
    if (view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject view_ = nullptr;
  jclass view_class_ = nullptr;
  jmethodID check_ = nullptr;
  jmethodID presentation_identity_ = nullptr;
  jmethodID request_ = nullptr;
  jmethodID open_settings_ = nullptr;
};

std::optional<std::string> OptionalString(JNIEnv* environment, jstring value) {
  if (!value) {
    return std::nullopt;
  }
  return android::JavaStringToUtf8(environment, value);
}

} // namespace

std::optional<ApplicationActivation> DecodeAndroidApplicationActivation(
    JavaVM* virtual_machine,
    JNIEnv* environment,
    jobject context,
    const AndroidApplicationActivationInput& input
) {
  if (environment == nullptr || context == nullptr) {
    throw std::invalid_argument("HuxerUI Android application activation requires JNI and Context");
  }
  if (input.kind == 0) {
    return std::nullopt;
  }
  const std::optional<std::string> decoded_value = OptionalString(environment, input.value);
  if (environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android application activation value could not be read");
  }
  if (!decoded_value.has_value() || decoded_value->empty()) {
    return std::nullopt;
  }
  if (input.kind == android_activation_url) {
    std::optional<Uri> parsed = Uri::Parse(*decoded_value);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    return ApplicationActivation{UrlActivation{std::move(*parsed)}};
  }
  if (input.kind == android_activation_notification) {
    PlatformPayload data;
    if (input.data != nullptr) {
      if (environment->GetArrayLength(input.data) > static_cast<jsize>(max_local_notification_data_bytes)) {
        return std::nullopt;
      }
      try {
        data = DecodeLocalNotificationData(android::JavaByteArrayToBytes(environment, input.data));
      } catch (const std::invalid_argument&) {
        return std::nullopt;
      }
    }
    return ApplicationActivation{NotificationActivation{*decoded_value, std::move(data)}};
  }
  if (input.kind != android_activation_file) {
    return std::nullopt;
  }

  const std::optional<std::string> decoded_name = OptionalString(environment, input.file_name);
  const std::optional<std::string> decoded_content_type = OptionalString(environment, input.content_type);
  if (environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android file activation metadata could not be read");
  }
  if (!decoded_name.has_value() || decoded_name->empty() || input.file_size < -1) {
    return std::nullopt;
  }

  FileReferenceMetadata metadata{
      .name = *decoded_name,
      .size = input.file_size < 0
                  ? std::nullopt
                  : std::optional<std::uint64_t>{static_cast<std::uint64_t>(input.file_size)},
      .content_type = decoded_content_type,
      .can_write = input.writable == JNI_TRUE,
  };
  std::vector<FileReference> files;
  files.push_back(
      CreateAndroidFileReference(virtual_machine, environment, context, std::move(metadata), *decoded_value)
  );
  return ApplicationActivation{FileActivation{std::move(files)}};
}

std::shared_ptr<PermissionTransport>
CreateAndroidPermissionTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view) {
  return std::make_shared<AndroidPermissionTransport>(virtual_machine, environment, view);
}

namespace {

class AndroidLocalNotificationTransport final : public LocalNotificationTransport {
public:
  AndroidLocalNotificationTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view)
      : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr || environment == nullptr || view == nullptr) {
      throw std::invalid_argument(
          "HuxerUI Android local notification transport requires a Java VM, environment, and View");
    }
    view_ = environment->NewGlobalRef(view);
    jclass local_class = environment->GetObjectClass(view);
    if (view_ == nullptr || local_class == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android local notification View could not be retained");
    }
    view_class_ = static_cast<jclass>(environment->NewGlobalRef(local_class));
    environment->DeleteLocalRef(local_class);
    if (view_class_ == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android local notification View class could not be retained");
    }
    capabilities_ = environment->GetMethodID(view_class_, "localNotificationCapabilities", "()I");
    presentation_identity_ = environment->GetMethodID(view_class_, "presentationIdentity", "(Lorg/huxerui/HuxerUIView;)J");
    check_authorization_ = environment->GetMethodID(view_class_, "checkLocalNotificationAuthorization", "()I");
    request_authorization_ = environment->GetMethodID(
        view_class_, "requestLocalNotificationAuthorization", "(JLorg/huxerui/HuxerUIView;)V");
    show_ = environment->GetMethodID(view_class_, "showLocalNotification",
                                     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)I");
    schedule_ =
        environment->GetMethodID(view_class_, "scheduleLocalNotification",
                                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BJ)I");
    cancel_ = environment->GetMethodID(view_class_, "cancelLocalNotification", "(Ljava/lang/String;)I");
    if (capabilities_ == nullptr || presentation_identity_ == nullptr || check_authorization_ == nullptr || request_authorization_ == nullptr ||
        show_ == nullptr || schedule_ == nullptr || cancel_ == nullptr || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      Release(environment);
      throw std::runtime_error("HuxerUI Android local notification methods do not match the platform backend");
    }
  }

  ~AndroidLocalNotificationTransport() override {
    JniEnvironment attached(virtual_machine_);
    Release(attached.Get());
  }

  std::uint64_t PresentationIdentity() const override {
    return ReadPresentationIdentity(virtual_machine_, view_, presentation_identity_);
  }

  LocalNotificationCapabilities Capabilities() const noexcept override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    const jint result = environment->CallIntMethod(view_, capabilities_);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      return {};
    }
    return ToCapabilities(result);
  }

  std::function<void()> CheckAuthorization(PermissionStatusCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    const jint result = environment->CallIntMethod(view_, check_authorization_);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(PermissionStatus::Unavailable);
    } else {
      completion(ToPermissionStatus(result));
    }
    return {};
  }

  std::function<void()> RequestAuthorization(PermissionStatusCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(PermissionStatus::Unavailable);
      return {};
    }
    // Share only the one-shot completion holder; notification authorization keeps its own native request lifetime.
    jobject source = CurrentAndroidPresentationView();
    auto operation = std::make_shared<AndroidPermissionOperation>(std::move(completion));
    auto native_handle = std::make_unique<AndroidPermissionOperationHandle>(operation);
    AndroidPermissionOperationHandle* transferred_handle = native_handle.release();
    environment->CallVoidMethod(view_, request_authorization_,
                                static_cast<jlong>(reinterpret_cast<std::uintptr_t>(transferred_handle)), source);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      std::unique_ptr<AndroidPermissionOperationHandle> owner(transferred_handle);
      operation->Complete(5);
    }
    return {};
  }

  std::function<void()> Show(ResolvedLocalNotification notification,
                             LocalNotificationOperationCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(LocalNotificationOperationStatus::Unavailable);
      return {};
    }
    android::LocalRef<jstring> identifier = android::Utf8ToJavaString(environment, notification.identifier);
    android::LocalRef<jstring> title = android::Utf8ToJavaString(environment, notification.title);
    android::LocalRef<jstring> body = android::Utf8ToJavaString(environment, notification.body);
    android::LocalRef<jbyteArray> data = android::BytesToJavaByteArray(environment, notification.data);
    const auto* template_presentation = std::get_if<TemplateNotificationPresentation>(&notification.presentation);
    android::LocalRef<jstring> template_identifier;
    if (template_presentation != nullptr) {
      template_identifier = android::Utf8ToJavaString(environment, template_presentation->identifier);
    }
    if (!identifier || !title || !body || !data || (template_presentation != nullptr && !template_identifier) ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(LocalNotificationOperationStatus::Failed);
      return {};
    }
    const jint result = environment->CallIntMethod(view_, show_, identifier.Get(), title.Get(), body.Get(),
                                                   template_identifier.Get(), data.Get());
    CompleteOperation(environment, result, std::move(completion));
    return {};
  }

  std::function<void()> Schedule(ResolvedLocalNotification notification,
                                 std::chrono::system_clock::time_point delivery_time,
                                 LocalNotificationOperationCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(LocalNotificationOperationStatus::Unavailable);
      return {};
    }
    android::LocalRef<jstring> identifier = android::Utf8ToJavaString(environment, notification.identifier);
    android::LocalRef<jstring> title = android::Utf8ToJavaString(environment, notification.title);
    android::LocalRef<jstring> body = android::Utf8ToJavaString(environment, notification.body);
    android::LocalRef<jbyteArray> data = android::BytesToJavaByteArray(environment, notification.data);
    const auto* template_presentation = std::get_if<TemplateNotificationPresentation>(&notification.presentation);
    android::LocalRef<jstring> template_identifier;
    if (template_presentation != nullptr) {
      template_identifier = android::Utf8ToJavaString(environment, template_presentation->identifier);
    }
    if (!identifier || !title || !body || !data || (template_presentation != nullptr && !template_identifier) ||
        environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(LocalNotificationOperationStatus::Failed);
      return {};
    }
    const auto delivery_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(delivery_time.time_since_epoch()).count();
    const jint result =
        environment->CallIntMethod(view_, schedule_, identifier.Get(), title.Get(), body.Get(),
                                   template_identifier.Get(), data.Get(), static_cast<jlong>(delivery_milliseconds));
    CompleteOperation(environment, result, std::move(completion));
    return {};
  }

  std::function<void()> Cancel(std::string identifier, LocalNotificationOperationCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || view_ == nullptr) {
      completion(LocalNotificationOperationStatus::Unavailable);
      return {};
    }
    android::LocalRef<jstring> java_identifier = android::Utf8ToJavaString(environment, identifier);
    if (!java_identifier || environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(LocalNotificationOperationStatus::Failed);
      return {};
    }
    const jint result = environment->CallIntMethod(view_, cancel_, java_identifier.Get());
    CompleteOperation(environment, result, std::move(completion));
    return {};
  }

private:
  static void CompleteOperation(JNIEnv* environment, jint result, LocalNotificationOperationCompletion completion) {
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      completion(LocalNotificationOperationStatus::Failed);
    } else {
      completion(ToOperationStatus(result));
    }
  }

  void Release(JNIEnv* environment) noexcept {
    if (environment == nullptr) {
      return;
    }
    if (view_class_ != nullptr) {
      environment->DeleteGlobalRef(view_class_);
      view_class_ = nullptr;
    }
    if (view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject view_ = nullptr;
  jclass view_class_ = nullptr;
  jmethodID capabilities_ = nullptr;
  jmethodID presentation_identity_ = nullptr;
  jmethodID check_authorization_ = nullptr;
  jmethodID request_authorization_ = nullptr;
  jmethodID show_ = nullptr;
  jmethodID schedule_ = nullptr;
  jmethodID cancel_ = nullptr;
};

} // namespace

std::shared_ptr<LocalNotificationTransport>
CreateAndroidLocalNotificationTransport(JavaVM* virtual_machine, JNIEnv* environment, jobject view) {
  return std::make_shared<AndroidLocalNotificationTransport>(virtual_machine, environment, view);
}

} // namespace huxerui::detail

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIPermission_nativeComplete(
    JNIEnv*, jclass, jlong native_handle, jint status) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidPermissionOperation>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner && *owner) {
    (*owner)->Complete(status);
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUILocalNotification_nativeCompleteAuthorization(
    JNIEnv*, jclass, jlong native_handle, jint status) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidPermissionOperation>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (owner && *owner) {
    (*owner)->Complete(status);
  }
}

namespace huxerui::detail {

namespace {

constexpr auto no_earliest_start = std::numeric_limits<std::int64_t>::min();

PlatformPayload EncodeSystemTask(const SystemTaskRequest& request) {
  return PlatformPayload::Object{
      {"identifier", request.identifier}, {"instance", request.instance_id}, {"token", request.token},
      {"kind", static_cast<int>(request.kind)},
      {"earliest", request.schedule.earliest_start
          ? std::chrono::ceil<std::chrono::milliseconds>(request.schedule.earliest_start->time_since_epoch()).count()
          : no_earliest_start},
      {"network", request.schedule.requires_network}, {"power", request.schedule.requires_external_power},
      {"title", request.presentation ? request.presentation->title : std::string{}},
      {"subtitle", request.presentation ? request.presentation->subtitle : std::string{}},
  };
}

SystemTaskRequest DecodeSystemTask(const PlatformPayload& payload) {
  const auto& value = payload.AsObject();
  const auto kind = value.at("kind").AsInteger();
  if (kind < 0 || kind > static_cast<int>(SystemTaskKind::Ongoing)) {
    throw std::logic_error("HuxerUI Android task kind is invalid");
  }
  const auto earliest = value.at("earliest").AsInteger();
  SystemTaskRequest request{.identifier = std::string(value.at("identifier").AsString()),
                            .instance_id = std::string(value.at("instance").AsString()),
                            .token = std::string(value.at("token").AsString()),
                            .kind = static_cast<SystemTaskKind>(kind)};
  using Clock = std::chrono::system_clock;
  if (earliest != no_earliest_start) {
    const auto when = std::chrono::milliseconds(earliest);
    if (when > std::chrono::floor<std::chrono::milliseconds>(Clock::time_point::max().time_since_epoch()) ||
        when < std::chrono::ceil<std::chrono::milliseconds>(Clock::time_point::min().time_since_epoch())) {
      throw std::logic_error("HuxerUI Android task time is out of range");
    }
    request.schedule.earliest_start = Clock::time_point(std::chrono::duration_cast<Clock::duration>(when));
  }
  request.schedule.requires_network = value.at("network").AsBoolean();
  request.schedule.requires_external_power = value.at("power").AsBoolean();
  return request;
}

std::optional<SystemTaskError> DecodeSystemTaskError(const PlatformPayload::Object& payload) {
  const auto found = payload.find("error");
  if (found == payload.end()) return std::nullopt;
  const auto code = found->second.AsInteger();
  if (code < 0 || code > static_cast<int>(SystemTaskErrorCode::Handler)) {
    throw std::logic_error("HuxerUI Android task error code is invalid");
  }
  return SystemTaskError{static_cast<SystemTaskErrorCode>(code), std::string(payload.at("message").AsString())};
}

class AndroidSystemTasks final : public SystemTaskTransport, public std::enable_shared_from_this<AndroidSystemTasks> {
public:
  AndroidSystemTasks(JavaVM* machine, JNIEnv* env, jobject host) : machine_(machine) {
    const android::LocalRef<jclass> host_type(env, env->GetObjectClass(host));
    const auto getter = env->GetMethodID(host_type.Get(), "systemTasks", "()Lorg/huxerui/HuxerUISystemTasks;");
    Check(env);
    const android::LocalRef<jobject> tasks(env, env->CallObjectMethod(host, getter));
    Check(env);
    const android::LocalRef<jclass> type(env, env->GetObjectClass(tasks.Get()));
    recover_ = Method(env, type.Get(), "recover");
    submit_ = Method(env, type.Get(), "submit");
    withdraw_ = Method(env, type.Get(), "withdraw");
    finish_ = Method(env, type.Get(), "finish");
    update_ = Method(env, type.Get(), "update");
    disconnect_ = env->GetMethodID(type.Get(), "disconnect", "()V");
    Check(env);
    tasks_ = env->NewGlobalRef(tasks.Get());
    Check(env);
  }

  ~AndroidSystemTasks() override {
    if (JniEnvironment access(machine_); auto* env = access.Get()) env->DeleteGlobalRef(tasks_);
  }

  SystemTaskCapabilities Capabilities(SystemTaskKind kind) const noexcept override {
    return {.supported = true, .can_launch_process = kind == SystemTaskKind::Background};
  }

  void Install(std::span<const SystemTaskRegistration> registrations, EventHandler on_event) override {
    on_event_ = std::move(on_event);
    for (const auto& registration : registrations) {
      if (registration.kind != SystemTaskKind::Background) continue;
      SystemTaskEvent event{SystemTaskEvent::Kind::Reconciled, {.identifier = registration.identifier}};
      const auto result = Call(recover_, registration.identifier);
      if (auto error = DecodeSystemTaskError(result.AsObject())) event.error = std::move(error);
      else if (const auto found = result.AsObject().find("request"); found != result.AsObject().end()) {
        event.request = DecodeSystemTask(found->second);
      }
      on_event_(std::move(event));
    }
  }

  SystemTaskResult<void> Submit(const SystemTaskRequest& request) override {
    return Operation(submit_, EncodeSystemTask(request));
  }

  SystemTaskResult<void> Withdraw(const SystemTaskRequest& request) override {
    return Operation(withdraw_, EncodeSystemTask(request));
  }

  void Disconnect() noexcept override {
    on_event_ = {};
    grants_.clear();
    JniEnvironment access(machine_);
    if (auto* env = access.Get()) {
      env->CallVoidMethod(tasks_, disconnect_);
      ClearJavaException(env);
    }
  }

  void Receive(const PlatformPayload& payload) {
    if (!on_event_) return;
    const auto& value = payload.AsObject();
    const auto event_name = value.at("event").AsString();
    auto request = DecodeSystemTask(value.at("request"));
    if (event_name == "rejected") {
      on_event_({.kind = SystemTaskEvent::Kind::Rejected, .request = std::move(request),
                 .error = DecodeSystemTaskError(value)});
      return;
    }
    const auto allowance = value.at("allowance").AsInteger();
    if (event_name == "granted") {
      if (grants_.contains(allowance)) return;
      const auto weak = weak_from_this();
      auto grant = std::make_shared<SystemTaskGrant>([weak, allowance](SystemTaskOutcome outcome) {
        if (auto owner = weak.lock()) {
          owner->grants_.erase(allowance);
          return owner->Operation(owner->finish_, PlatformPayload::Object{
              {"allowance", allowance}, {"retry", outcome == SystemTaskOutcome::Retry}});
        }
        return SystemTaskResult<void>::Success();
      });
      if (request.kind != SystemTaskKind::Background) {
        grant->update_presentation = [weak, allowance](const ResolvedTaskPresentation& presentation) {
          if (auto owner = weak.lock()) {
            return owner->Operation(owner->update_, PlatformPayload::Object{
                {"allowance", allowance}, {"title", presentation.title}, {"subtitle", presentation.subtitle}});
          }
          return TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI Android task service has retired");
        };
        if (request.kind == SystemTaskKind::Continued) {
          grant->update_progress = [weak, allowance](TaskProgress progress) {
            if (auto owner = weak.lock()) {
              return owner->Operation(owner->update_, PlatformPayload::Object{
                  {"allowance", allowance}, {"completed", std::to_string(progress.completed)},
                  {"total", progress.total ? std::to_string(*progress.total) : std::string{}}});
            }
            return TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI Android task service has retired");
          };
        }
      }
      grants_.emplace(allowance, grant);
      on_event_({SystemTaskEvent::Kind::Granted, std::move(request), std::move(grant)});
      return;
    }
    const auto found = grants_.find(allowance);
    if (found == grants_.end()) return;
    auto grant = found->second.lock();
    if (!grant || !grant->IsLive()) return;
    if (event_name == "revoked") {
      grants_.erase(found);
      grant->Revoke();
      on_event_({SystemTaskEvent::Kind::Revoked, std::move(request), std::move(grant)});
    } else if (event_name == "stop") {
      on_event_({SystemTaskEvent::Kind::StopRequested, std::move(request), std::move(grant)});
    }
  }

private:
  static void Check(JNIEnv* env) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      throw std::runtime_error("HuxerUI Android system task JNI call failed");
    }
  }

  static jmethodID Method(JNIEnv* env, jclass type, const char* name) {
    auto method = env->GetMethodID(type, name, "(Lorg/huxerui/PlatformPayload;)Lorg/huxerui/PlatformPayload;");
    Check(env);
    return method;
  }

  PlatformPayload Call(jmethodID method, const PlatformPayload& value) {
    JniEnvironment access(machine_);
    auto* env = access.Get();
    if (!env) throw std::runtime_error("HuxerUI Android system task JNI environment is unavailable");
    const auto argument = android::PlatformPayloadToJava(env, value);
    Check(env);
    const android::LocalRef<jobject> result(env, env->CallObjectMethod(tasks_, method, argument.Get()));
    Check(env);
    return android::JavaPlatformPayloadToCpp(env, result.Get());
  }

  SystemTaskResult<void> Operation(jmethodID method, const PlatformPayload& value) {
    try {
      const auto result = Call(method, value);
      if (auto error = DecodeSystemTaskError(result.AsObject())) {
        return SystemTaskResult<void>::Failure(std::move(*error));
      }
      return SystemTaskResult<void>::Success();
    } catch (const std::exception& exception) {
      return TaskFailure<void>(SystemTaskErrorCode::Platform, exception.what());
    }
  }

  JavaVM* machine_;
  jobject tasks_ = nullptr;
  jmethodID recover_ = nullptr;
  jmethodID submit_ = nullptr;
  jmethodID withdraw_ = nullptr;
  jmethodID finish_ = nullptr;
  jmethodID update_ = nullptr;
  jmethodID disconnect_ = nullptr;
  EventHandler on_event_;
  std::map<std::int64_t, std::weak_ptr<SystemTaskGrant>> grants_;
};

} // namespace

std::shared_ptr<SystemTaskTransport> CreateAndroidSystemTaskTransport(
    JavaVM* machine, JNIEnv* environment, jobject host, std::function<void(const PlatformPayload&)>& receiver) {
  auto tasks = std::make_shared<AndroidSystemTasks>(machine, environment, host);
  receiver = [weak = std::weak_ptr(tasks)](const PlatformPayload& event) {
    if (auto owner = weak.lock()) owner->Receive(event);
  };
  return tasks;
}

} // namespace huxerui::detail
