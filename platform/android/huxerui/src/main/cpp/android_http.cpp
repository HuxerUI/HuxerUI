#include "android_http_internal.h"

#include <jni.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/android/jni.h>

#include "http_internal.h"

namespace huxerui::detail {

namespace {

enum class AndroidHttpResult : jint {
  Response,
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

class AndroidHttpRequestControl final {
public:
  AndroidHttpRequestControl(JavaVM* virtual_machine, jmethodID cancel, HttpTransportCompletion completion)
      : virtual_machine_(virtual_machine), cancel_(cancel), completion_(std::move(completion)) {}

  ~AndroidHttpRequestControl() {
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

  void Complete(HttpResult result) noexcept {
    jobject request = nullptr;
    HttpTransportCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      request = std::exchange(request_, nullptr);
      completion = std::move(completion_);
    }
    DeleteRequest(request);
    if (completion) {
      completion(std::move(result));
    }
  }

  void Cancel() noexcept {
    jobject request = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion_ = {};
      request = std::exchange(request_, nullptr);
    }
    if (request == nullptr) {
      return;
    }

    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment != nullptr) {
      environment->CallVoidMethod(request, cancel_);
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      environment->DeleteGlobalRef(request);
    }
  }

private:
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
  jmethodID cancel_ = nullptr;
  std::mutex mutex_;
  jobject request_ = nullptr;
  HttpTransportCompletion completion_;
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
    cancel_ = environment->GetMethodID(request_class_, "cancel", "()V");
    if (constructor_ == nullptr || start_ == nullptr || cancel_ == nullptr) {
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

  std::function<void()> Start(HttpRequest request, HttpTransportCompletion completion) override {
    JniEnvironment attached(virtual_machine_);
    JNIEnv* environment = attached.Get();
    if (environment == nullptr) {
      completion(HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Android HTTP could not access JNI"}));
      return {};
    }

    std::shared_ptr<AndroidHttpRequestControl> control;
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
      control = std::make_shared<AndroidHttpRequestControl>(virtual_machine_, cancel_, std::move(completion));
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
      environment->CallVoidMethod(java_request.Get(), start_);
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        control->Complete(HttpResult(HttpError{
            HttpErrorCode::Transport,
            "HuxerUI Android HTTP request could not be started",
        }));
        environment->CallVoidMethod(java_request.Get(), cancel_);
        if (environment->ExceptionCheck()) {
          environment->ExceptionClear();
        }
        return {};
      }
      return [control] { control->Cancel(); };
    } catch (const std::exception& exception) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      HttpResult error(HttpError{HttpErrorCode::Transport, exception.what()});
      if (control) {
        control->Complete(std::move(error));
      } else {
        completion(std::move(error));
      }
      return {};
    }
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  jclass request_class_ = nullptr;
  jmethodID constructor_ = nullptr;
  jmethodID start_ = nullptr;
  jmethodID cancel_ = nullptr;
};

HttpResult MakeAndroidHttpResult(
    JNIEnv* environment,
    jint result,
    jstring url,
    jint status_code,
    jobjectArray header_names,
    jobjectArray header_values,
    jbyteArray body,
    jstring message
) {
  switch (static_cast<AndroidHttpResult>(result)) {
  case AndroidHttpResult::Response: {
    if (url == nullptr || header_names == nullptr || header_values == nullptr || body == nullptr) {
      return HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Android HTTP response is incomplete"});
    }
    const jsize header_count = environment->GetArrayLength(header_names);
    if (header_count != environment->GetArrayLength(header_values)) {
      return HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Android HTTP response headers are invalid"});
    }
    HttpResponse response{
        .url = android::JavaStringToUtf8(environment, url),
        .status_code = status_code,
        .headers = {},
        .body = {},
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
        return HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Android HTTP response header is invalid"});
      }
      response.headers.push_back({
          android::JavaStringToUtf8(environment, name.Get()),
          android::JavaStringToUtf8(environment, value.Get()),
      });
    }
    response.body = android::JavaByteArrayToBytes(environment, body);
    return HttpResult(std::move(response));
  }
  case AndroidHttpResult::Timeout:
    return HttpResult(HttpError{
        HttpErrorCode::Timeout,
        JavaStringOrFallback(environment, message, "HuxerUI HTTP request timed out"),
    });
  case AndroidHttpResult::TransportError:
  case AndroidHttpResult::Canceled:
    return HttpResult(HttpError{
        HttpErrorCode::Transport,
        JavaStringOrFallback(environment, message, "HuxerUI HTTP request failed"),
    });
  }
  return HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI Android HTTP result is invalid"});
}

} // namespace

std::shared_ptr<HttpTransport> CreateAndroidHttpTransport(JavaVM* virtual_machine, JNIEnv* environment) {
  return std::make_shared<AndroidHttpTransport>(virtual_machine, environment);
}

} // namespace huxerui::detail

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeComplete(
    JNIEnv* environment,
    jclass,
    jlong native_handle,
    jint result,
    jstring url,
    jint status_code,
    jobjectArray header_names,
    jobjectArray header_values,
    jbyteArray body,
    jstring message
) {
  using Handle = std::shared_ptr<huxerui::detail::AndroidHttpRequestControl>;
  std::unique_ptr<Handle> owner(reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(native_handle)));
  if (!owner || !*owner) {
    return;
  }
  try {
    (*owner)->Complete(huxerui::detail::MakeAndroidHttpResult(
        environment,
        result,
        url,
        status_code,
        header_names,
        header_values,
        body,
        message
    ));
  } catch (...) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    (*owner)->Complete(huxerui::HttpResult(huxerui::HttpError{
        huxerui::HttpErrorCode::Transport,
        "HuxerUI Android HTTP response could not be decoded",
    }));
  }
}
