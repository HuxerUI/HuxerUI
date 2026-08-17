#include "color_stream.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/jni.h>
#include <huxerui/android/platform_module.h>

namespace {

constexpr char native_color_stream_class[] = "org/huxerui/examples/platformmodule/NativeColorStream";

huxerui::PlatformError ColorStreamError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

bool ClearJavaException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

struct AndroidColorStreamState {
  explicit AndroidColorStreamState(JNIEnv* environment_value)
      : environment(environment_value), source({320.0F, 180.0F}) {}

  bool Start() {
    environment->CallVoidMethod(producer, start);
    return !ClearJavaException(environment);
  }

  void Publish(JNIEnv* callback_environment, jobject bitmap) {
    source.Publish(callback_environment, bitmap);
  }

  void Dispose() noexcept {
    if (producer != nullptr) {
      environment->CallVoidMethod(producer, dispose_bridge);
      static_cast<void>(ClearJavaException(environment));
      environment->DeleteGlobalRef(producer);
      producer = nullptr;
    }
    delete bridge;
    bridge = nullptr;
    source.Finish();
  }

  JNIEnv* environment = nullptr;
  huxerui::android::ExternalTextureSource source;
  jobject producer = nullptr;
  jmethodID start = nullptr;
  jmethodID dispose_bridge = nullptr;
  std::weak_ptr<AndroidColorStreamState>* bridge = nullptr;
};

huxerui::PlatformModuleFactory::Instance CreateAndroidColorStream(
    JNIEnv* environment, jobject context, const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events
) {
  if (!options.IsNull()) {
    throw std::invalid_argument("HuxerUI example color stream options must be null");
  }
  static_cast<void>(events);
  huxerui::android::LocalRef<jclass> producer_class(environment, environment->FindClass(native_color_stream_class));
  if (!producer_class) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example could not find the Android native color stream class");
  }
  const jmethodID constructor =
      environment->GetMethodID(producer_class.Get(), "<init>", "(Landroid/content/Context;J)V");
  const jmethodID start = environment->GetMethodID(producer_class.Get(), "start", "()V");
  const jmethodID dispose_bridge = environment->GetMethodID(producer_class.Get(), "disposeNativeBridge", "()V");
  if (constructor == nullptr || start == nullptr || dispose_bridge == nullptr) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example Android color stream methods do not match the native bridge");
  }

  auto state = std::make_shared<AndroidColorStreamState>(environment);
  auto bridge = std::make_unique<std::weak_ptr<AndroidColorStreamState>>(state);
  huxerui::android::LocalRef<jobject> local_producer(
      environment,
      environment->NewObject(
          producer_class.Get(),
          constructor,
          context,
          static_cast<jlong>(reinterpret_cast<std::uintptr_t>(bridge.get()))
      )
  );
  if (!local_producer || environment->ExceptionCheck()) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example could not create the Android native color stream");
  }
  state->producer = environment->NewGlobalRef(local_producer.Get());
  if (state->producer == nullptr) {
    ClearJavaException(environment);
    throw std::runtime_error("HuxerUI example could not retain the Android native color stream");
  }
  state->start = start;
  state->dispose_bridge = dispose_bridge;
  state->bridge = bridge.release();

  huxerui::PlatformModuleFactory::Instance instance;
  instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
      -> std::function<void()> {
    if (method == huxerui::example::color_stream::texture_method && arguments.IsNull()) {
      if (!state->Start()) {
        result(ColorStreamError("example/color-stream-java", "The Android color stream could not be started"));
        return {};
      }
      result(huxerui::PlatformPayload(state->source.Texture()));
      return {};
    }
    result(
        ColorStreamError("example/color-stream-method", "The native color stream method or payload is not supported")
    );
    return {};
  };
  instance.dispose = [state] { state->Dispose(); };
  return instance;
}

huxerui::android::PlatformModuleFactory AndroidColorStreamFactory() {
  return {.create = CreateAndroidColorStream};
}

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.Modules().Register(color_stream::type, AndroidColorStreamFactory());
  root.Provide(std::make_shared<ColorStreamService>(root.Modules().Open(color_stream::type)));
}

} // namespace huxerui::example

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_examples_platformmodule_NativeColorStream_nativePublish(
    JNIEnv* environment, jclass, jlong bridge, jobject bitmap
) {
  auto* state = reinterpret_cast<std::weak_ptr<AndroidColorStreamState>*>(static_cast<std::uintptr_t>(bridge));
  if (state != nullptr) {
    try {
      if (const std::shared_ptr<AndroidColorStreamState> locked_state = state->lock()) {
        locked_state->Publish(environment, bitmap);
      }
    } catch (...) {
    }
  }
}
