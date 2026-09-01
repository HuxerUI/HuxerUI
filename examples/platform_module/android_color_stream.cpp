#include "color_stream.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/jni.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/app.h>

namespace {

constexpr char platform_color_stream_class[] = "org/huxerui/examples/platformmodule/PlatformColorStream";

bool ClearJavaException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

struct AndroidColorStreamState : huxerui::example::ColorStreamService {
  AndroidColorStreamState(huxerui::PlatformAdapter& adapter_value, JNIEnv* environment_value)
      : adapter(&adapter_value), environment(environment_value),
        texture(std::make_shared<huxerui::android::BitmapTexture>(
            huxerui::Size{320.0F, 180.0F}
        )) {}

  ~AndroidColorStreamState() override {
    Dispose();
  }

  huxerui::PlatformRequestId
  Texture(std::function<void(huxerui::PlatformResult<std::shared_ptr<huxerui::ExternalTexture>>)> completion) override {
    if (!completion) {
      throw std::invalid_argument("HuxerUI example color stream completion must not be empty");
    }
    environment->CallVoidMethod(producer, start);
    if (ClearJavaException(environment)) {
      huxerui::PlatformError error{
          "example/color-stream-java",
          "The Android color stream could not be started",
          {},
      };
      adapter->DispatchToUIThread(
          [completion = std::move(completion), error = std::move(error)]() mutable { completion(std::move(error)); });
      return 0;
    }
    adapter->DispatchToUIThread([completion = std::move(completion), texture = texture]() mutable {
      completion(std::move(texture));
    });
    return ++request_id;
  }

  void Publish(JNIEnv* callback_environment, jobject bitmap) {
    texture->Publish(callback_environment, bitmap);
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
    texture->Finish();
  }

  huxerui::PlatformAdapter* adapter = nullptr;
  JNIEnv* environment = nullptr;
  std::shared_ptr<huxerui::android::BitmapTexture> texture;
  jobject producer = nullptr;
  jmethodID start = nullptr;
  jmethodID dispose_bridge = nullptr;
  std::weak_ptr<AndroidColorStreamState>* bridge = nullptr;
  huxerui::PlatformRequestId request_id = 0;
};

std::shared_ptr<huxerui::example::ColorStreamService> CreateAndroidColorStream(huxerui::PlatformAdapter& adapter,
                                                                               JNIEnv* environment, jobject context) {
  huxerui::android::LocalRef<jclass> producer_class(environment, environment->FindClass(platform_color_stream_class));
  if (!producer_class) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example could not find the Android platform color stream class");
  }
  const jmethodID constructor =
      environment->GetMethodID(producer_class.Get(), "<init>", "(Landroid/content/Context;J)V");
  const jmethodID start = environment->GetMethodID(producer_class.Get(), "start", "()V");
  const jmethodID dispose_bridge = environment->GetMethodID(producer_class.Get(), "disposePlatformBridge", "()V");
  if (constructor == nullptr || start == nullptr || dispose_bridge == nullptr) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example Android color stream methods do not match the platform bridge");
  }

  auto state = std::make_shared<AndroidColorStreamState>(adapter, environment);
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
    throw std::logic_error("HuxerUI example could not create the Android platform color stream");
  }
  state->producer = environment->NewGlobalRef(local_producer.Get());
  if (state->producer == nullptr) {
    ClearJavaException(environment);
    throw std::runtime_error("HuxerUI example could not retain the Android platform color stream");
  }
  state->start = start;
  state->dispose_bridge = dispose_bridge;
  state->bridge = bridge.release();
  return state;
}

} // namespace

namespace huxerui::example {

void InstallColorStream(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<ColorStreamService>>(
      color_stream::type, android::PlatformModuleFactory<std::shared_ptr<ColorStreamService>>{
                              .create = CreateAndroidColorStream,
                          });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<ColorStreamService>>(color_stream::type));
}

} // namespace huxerui::example

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_examples_platformmodule_PlatformColorStream_nativePublish(
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
