#include "android_http_internal.h"

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/android/jni.h>

#include "io/http_internal.h"

namespace huxerui::detail {

namespace {

enum class AndroidHttpTerminal : jint {
  Complete,
  TransportError,
  Timeout,
  Canceled,
};

const char* HttpMethodName(HttpMethod method) noexcept {
  switch (method) {
  case HttpMethod::Get:
    return "GET";
  case HttpMethod::Head:
    return "HEAD";
  case HttpMethod::Post:
    return "POST";
  case HttpMethod::Put:
    return "PUT";
  case HttpMethod::Patch:
    return "PATCH";
  case HttpMethod::Delete:
    return "DELETE";
  case HttpMethod::Options:
    return "OPTIONS";
  }
  return "GET";
}

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

std::string JavaStringOrFallback(JNIEnv* environment, jstring value, std::string fallback) {
  if (value == nullptr) {
    return fallback;
  }
  try {
    std::string result = android::JavaStringToUtf8(environment, value);
    return result.empty() ? std::move(fallback) : result;
  } catch (...) {
    return fallback;
  }
}

class AndroidHttpRequestControl final : public HttpTransportOperation {
public:
  AndroidHttpRequestControl(
      JavaVM* virtual_machine, jmethodID read, jmethodID cancel, HttpTransportCallbacks callbacks
  )
      : virtual_machine_(virtual_machine), read_(read), cancel_(cancel), callbacks_(std::move(callbacks)) {}

  ~AndroidHttpRequestControl() override {
    jobject request = nullptr;
    {
      std::scoped_lock lock(mutex_);
      request = std::exchange(request_, nullptr);
    }
    DeleteRequest(request);
  }

  bool SetRequest(JNIEnv* environment, jobject request) {
    jobject retained = environment->NewGlobalRef(request);
    if (retained == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      return false;
    }

    bool release = false;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        release = true;
      } else {
        request_ = retained;
      }
    }
    if (release) {
      environment->DeleteGlobalRef(retained);
    }
    return !release;
  }

  void RequestRead() override {
    InvokeJava(read_);
  }

  void Cancel() noexcept override {
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks_ = {};
    }
    InvokeJava(cancel_);
  }

  void Upload(std::uint64_t transferred_bytes) {
    std::function<void(std::uint64_t)> callback;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        callback = callbacks_.upload_progress;
      }
    }
    if (callback) {
      callback(transferred_bytes);
    }
  }

  void Response(HttpTransportResponse response) {
    std::function<void(HttpTransportResponse)> callback;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        callback = callbacks_.response;
      }
    }
    if (callback) {
      callback(std::move(response));
    }
  }

  void Body(Bytes body) {
    std::function<void(Bytes)> callback;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_) {
        callback = callbacks_.body;
      }
    }
    if (callback) {
      callback(std::move(body));
    }
  }

  void Terminal(AndroidHttpTerminal terminal, std::string message) noexcept {
    jobject request = nullptr;
    HttpTransportCallbacks callbacks;
    bool notify = false;
    {
      std::scoped_lock lock(mutex_);
      request = std::exchange(request_, nullptr);
      if (!finished_) {
        finished_ = true;
        callbacks = std::move(callbacks_);
        notify = true;
      }
    }
    DeleteRequest(request);
    if (!notify) {
      return;
    }
    switch (terminal) {
    case AndroidHttpTerminal::Complete:
      if (callbacks.complete) {
        callbacks.complete();
      }
      return;
    case AndroidHttpTerminal::Timeout:
      if (callbacks.error) {
        callbacks.error(HttpError{
            HttpErrorCode::Timeout,
            message.empty() ? "HuxerUI HTTP request timed out" : std::move(message),
        });
      }
      return;
    case AndroidHttpTerminal::TransportError:
      if (callbacks.error) {
        callbacks.error(HttpError{
            HttpErrorCode::Transport,
            message.empty() ? "HuxerUI HTTP request failed" : std::move(message),
        });
      }
      return;
    case AndroidHttpTerminal::Canceled:
      return;
    }
    if (callbacks.error) {
      callbacks.error(HttpError{
          HttpErrorCode::Transport,
          "HuxerUI Android HTTP bridge returned an invalid terminal result",
      });
    }
  }

  void Fail(HttpError error, bool cancel_java) noexcept {
    HttpTransportCallbacks callbacks;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks = std::move(callbacks_);
    }
    if (callbacks.error) {
      callbacks.error(std::move(error));
    }
    if (cancel_java) {
      InvokeJava(cancel_);
    }
  }

private:
  void InvokeJava(jmethodID method) noexcept {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || method == nullptr) {
      return;
    }

    jobject request = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (request_ != nullptr) {
        request = environment->NewLocalRef(request_);
      }
    }
    if (request == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      return;
    }
    environment->CallVoidMethod(request, method);
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    environment->DeleteLocalRef(request);
  }

  void DeleteRequest(jobject request) noexcept {
    if (request == nullptr) {
      return;
    }
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get()) {
      environment->DeleteGlobalRef(request);
    }
  }

  JavaVM* virtual_machine_ = nullptr;
  jmethodID read_ = nullptr;
  jmethodID cancel_ = nullptr;
  std::mutex mutex_;
  jobject request_ = nullptr;
  HttpTransportCallbacks callbacks_;
  bool finished_ = false;
};

using AndroidHttpControlHandle = std::shared_ptr<AndroidHttpRequestControl>;

android::LocalRef<jobjectArray>
MakeStringArray(JNIEnv* environment, jclass string_class, const std::vector<HttpHeader>& headers, bool names) {
  if (headers.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android HTTP header count exceeds the JNI array range");
  }
  android::LocalRef<jobjectArray> result(
      environment,
      environment->NewObjectArray(static_cast<jsize>(headers.size()), string_class, nullptr)
  );
  if (!result) {
    throw std::runtime_error("HuxerUI Android HTTP header array could not be allocated");
  }
  for (std::size_t index = 0; index < headers.size(); ++index) {
    const std::string_view value =
        names ? std::string_view(headers[index].name) : std::string_view(headers[index].value);
    android::LocalRef<jstring> item = android::Utf8ToJavaString(environment, value);
    if (!item) {
      throw std::runtime_error("HuxerUI Android HTTP header could not be allocated");
    }
    environment->SetObjectArrayElement(result.Get(), static_cast<jsize>(index), item.Get());
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android HTTP header could not be copied");
    }
  }
  return result;
}

HttpTransportResponse MakeAndroidHttpResponse(
    JNIEnv* environment,
    jstring url,
    jint status_code,
    jobjectArray header_names,
    jobjectArray header_values,
    jlong body_size
) {
  if (url == nullptr || header_names == nullptr || header_values == nullptr) {
    throw std::runtime_error("HuxerUI Android HTTP response is incomplete");
  }
  const jsize header_count = environment->GetArrayLength(header_names);
  if (header_count != environment->GetArrayLength(header_values)) {
    throw std::runtime_error("HuxerUI Android HTTP response headers are invalid");
  }
  HttpTransportResponse response{
      .url = android::JavaStringToUtf8(environment, url),
      .status_code = status_code,
      .headers = {},
      .body_size = std::nullopt,
  };
  response.headers.reserve(static_cast<std::size_t>(header_count));
  for (jsize index = 0; index < header_count; ++index) {
    android::LocalRef<jstring> name(
        environment,
        static_cast<jstring>(environment->GetObjectArrayElement(header_names, index))
    );
    android::LocalRef<jstring> value(
        environment,
        static_cast<jstring>(environment->GetObjectArrayElement(header_values, index))
    );
    if (!name || !value) {
      throw std::runtime_error("HuxerUI Android HTTP response header is invalid");
    }
    response.headers.push_back({
        android::JavaStringToUtf8(environment, name.Get()),
        android::JavaStringToUtf8(environment, value.Get()),
    });
  }
  if (body_size >= 0) {
    response.body_size = static_cast<std::uint64_t>(body_size);
  }
  return response;
}

class AndroidHttpTransport final : public HttpTransport {
public:
  AndroidHttpTransport(JavaVM* virtual_machine, JNIEnv* environment) : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr || environment == nullptr) {
      throw std::invalid_argument("HuxerUI Android HTTP requires a Java VM and JNI environment");
    }
    jclass local_class = environment->FindClass("org/huxerui/HuxerUIHttpRequest");
    if (local_class == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      throw std::runtime_error("HuxerUI Android HTTP Java implementation is unavailable");
    }
    request_class_ = static_cast<jclass>(environment->NewGlobalRef(local_class));
    environment->DeleteLocalRef(local_class);
    if (request_class_ == nullptr) {
      throw std::runtime_error("HuxerUI Android HTTP Java implementation could not be retained");
    }

    constructor_ = environment->GetMethodID(
        request_class_,
        "<init>",
        "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[BJ)V"
    );
    start_ = environment->GetMethodID(request_class_, "start", "()V");
    read_ = environment->GetMethodID(request_class_, "read", "()V");
    cancel_ = environment->GetMethodID(request_class_, "cancel", "()V");
    if (constructor_ == nullptr || start_ == nullptr || read_ == nullptr || cancel_ == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      environment->DeleteGlobalRef(request_class_);
      request_class_ = nullptr;
      throw std::runtime_error("HuxerUI Android HTTP Java methods do not match the platform backend");
    }
  }

  ~AndroidHttpTransport() override {
    JniEnvironment attached(virtual_machine_);
    if (JNIEnv* environment = attached.Get(); environment != nullptr && request_class_ != nullptr) {
      environment->DeleteGlobalRef(request_class_);
    }
  }

  std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool, HttpTransportCallbacks callbacks) override {
    auto control = std::make_shared<AndroidHttpRequestControl>(virtual_machine_, read_, cancel_, std::move(callbacks));
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr) {
      control->Fail(HttpError{HttpErrorCode::Transport, "HuxerUI Android HTTP could not access JNI"}, false);
      return control;
    }

    bool native_owner_released = false;
    try {
      android::LocalRef<jclass> string_class(environment, environment->FindClass("java/lang/String"));
      if (!string_class) {
        throw std::runtime_error("HuxerUI Android HTTP Java String class is unavailable");
      }
      android::LocalRef<jstring> url = android::Utf8ToJavaString(environment, request.url);
      android::LocalRef<jstring> method = android::Utf8ToJavaString(environment, HttpMethodName(request.method));
      android::LocalRef<jobjectArray> header_names =
          MakeStringArray(environment, string_class.Get(), request.headers, true);
      android::LocalRef<jobjectArray> header_values =
          MakeStringArray(environment, string_class.Get(), request.headers, false);
      android::LocalRef<jbyteArray> java_body =
          android::BytesToJavaByteArray(environment, std::span<const std::byte>(request.body));
      if (!url || !method || !java_body) {
        throw std::runtime_error("HuxerUI Android HTTP request values could not be allocated");
      }

      const jlong timeout = request.timeout.has_value() ? static_cast<jlong>(request.timeout->count()) : -1;
      auto native_handle = std::make_unique<AndroidHttpControlHandle>(control);
      android::LocalRef<jobject> java_request(
          environment,
          environment->NewObject(
              request_class_,
              constructor_,
              static_cast<jlong>(reinterpret_cast<std::uintptr_t>(native_handle.get())),
              url.Get(),
              method.Get(),
              header_names.Get(),
              header_values.Get(),
              java_body.Get(),
              timeout
          )
      );
      if (environment->ExceptionCheck() || !java_request) {
        if (environment->ExceptionCheck()) {
          environment->ExceptionClear();
        }
        throw std::runtime_error("HuxerUI Android HTTP request could not be created");
      }
      if (!control->SetRequest(environment, java_request.Get())) {
        throw std::runtime_error("HuxerUI Android HTTP request could not be retained");
      }
      native_handle.release();
      native_owner_released = true;
      environment->CallVoidMethod(java_request.Get(), start_);
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        throw std::runtime_error("HuxerUI Android HTTP request could not be started");
      }
    } catch (const std::exception& exception) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      control->Fail(HttpError{HttpErrorCode::Transport, exception.what()}, native_owner_released);
    }
    return control;
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  jclass request_class_ = nullptr;
  jmethodID constructor_ = nullptr;
  jmethodID start_ = nullptr;
  jmethodID read_ = nullptr;
  jmethodID cancel_ = nullptr;
};

} // namespace

std::shared_ptr<HttpTransport> CreateAndroidHttpTransport(JavaVM* virtual_machine, JNIEnv* environment) {
  return std::make_shared<AndroidHttpTransport>(virtual_machine, environment);
}

} // namespace huxerui::detail

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeUpload(
    JNIEnv*, jclass, jlong native_handle, jlong transferred_bytes
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidHttpRequestControl>;
  auto* owner = reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle));
  if (owner != nullptr && *owner && transferred_bytes >= 0) {
    (*owner)->Upload(static_cast<std::uint64_t>(transferred_bytes));
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeResponse(
    JNIEnv* environment,
    jclass,
    jlong native_handle,
    jstring url,
    jint status_code,
    jobjectArray header_names,
    jobjectArray header_values,
    jlong body_size
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidHttpRequestControl>;
  auto* owner = reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle));
  if (owner == nullptr || !*owner) {
    return;
  }
  const std::shared_ptr<huxerui::detail::AndroidHttpRequestControl> control = *owner;
  try {
    control->Response(huxerui::detail::MakeAndroidHttpResponse(
        environment, url, status_code, header_names, header_values, body_size
    ));
  } catch (...) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    control->Fail(
        huxerui::HttpError{
            huxerui::HttpErrorCode::Transport,
            "HuxerUI Android HTTP response could not be decoded",
        },
        true
    );
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeBody(
    JNIEnv* environment, jclass, jlong native_handle, jbyteArray body
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidHttpRequestControl>;
  auto* owner = reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle));
  if (owner == nullptr || !*owner) {
    return;
  }
  const std::shared_ptr<huxerui::detail::AndroidHttpRequestControl> control = *owner;
  try {
    if (body == nullptr) {
      throw std::runtime_error("HuxerUI Android HTTP response body is missing");
    }
    control->Body(huxerui::android::JavaByteArrayToBytes(environment, body));
  } catch (...) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    control->Fail(
        huxerui::HttpError{
            huxerui::HttpErrorCode::Transport,
            "HuxerUI Android HTTP response body could not be decoded",
        },
        true
    );
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeTerminal(
    JNIEnv* environment, jclass, jlong native_handle, jint result, jstring message
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidHttpRequestControl>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (!owner || !*owner) {
    return;
  }
  (*owner)->Terminal(
      static_cast<huxerui::detail::AndroidHttpTerminal>(result),
      huxerui::detail::JavaStringOrFallback(environment, message, {})
  );
}
