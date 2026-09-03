#include "texture_demo.h"

#include <android/log.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/jni.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/app.h>

namespace {

constexpr char texture_demo_module[] = "example/ExternalTextureDemo";
constexpr char producer_class_name[] = "org/huxerui/examples/externaltexture/ExternalTextureProducer";

bool ClearJavaException(JNIEnv* environment) noexcept {
  if (environment == nullptr || !environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

void LogPublishError(const char* texture, const char* message) noexcept {
  __android_log_print(ANDROID_LOG_ERROR, "HuxerUIExample", "%s publication failed: %s", texture, message);
}

class AndroidTextureDemo final : public huxerui::example::TextureDemo {
public:
  AndroidTextureDemo(JNIEnv* environment, JavaVM* virtual_machine)
      : virtual_machine_(virtual_machine),
        bitmap_texture_(std::make_shared<huxerui::android::BitmapTexture>(huxerui::Size{320.0F, 180.0F})),
        gl_texture_(std::make_shared<huxerui::android::GlTexture>(huxerui::Size{320.0F, 180.0F})),
        surface_texture_(
            huxerui::android::SurfaceStreamTexture::Create(environment, huxerui::Size{320.0F, 180.0F}, 320, 180)
        ),
        entries_{
            {
                "BitmapTexture",
                "Immutable Bitmap frames replayed directly by the Android Canvas renderer.",
                bitmap_texture_,
            },
            {
                "GlTexture",
                "GL_TEXTURE_2D frames copied from the producer's current EGL context.",
                gl_texture_,
            },
            {
                "SurfaceStreamTexture",
                "Frames posted to a producer Surface and consumed through SurfaceTexture/OES.",
                surface_texture_,
            }} {}

  ~AndroidTextureDemo() override {
    Dispose();
  }

  [[nodiscard]] const std::vector<huxerui::example::TextureDemoEntry>& Entries() const noexcept override {
    return entries_;
  }

  [[nodiscard]] std::string_view Message() const noexcept override {
    return {};
  }

  void SetRunning(bool running) noexcept override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || producer_ == nullptr || set_running_ == nullptr) {
      return;
    }
    environment->CallVoidMethod(producer_, set_running_, running ? JNI_TRUE : JNI_FALSE);
    static_cast<void>(ClearJavaException(environment));
  }

  void PublishBitmap(JNIEnv* environment, jobject bitmap) {
    bitmap_texture_->Publish(environment, bitmap);
  }

  void PublishGl(std::uint32_t texture_name, int pixel_width, int pixel_height) {
    gl_texture_->PublishCurrent({
        .texture_name = texture_name,
        .pixel_width = pixel_width,
        .pixel_height = pixel_height,
        .origin = huxerui::android::GlTexture::Origin::TopLeft,
        .alpha = huxerui::android::GlTexture::Alpha::Opaque,
    });
  }

  [[nodiscard]] huxerui::android::LocalRef<jobject> Surface(JNIEnv* environment) const {
    return surface_texture_->Surface(environment);
  }

  void SetProducer(jobject producer, jmethodID set_running, jmethodID dispose, void* bridge) noexcept {
    producer_ = producer;
    set_running_ = set_running;
    dispose_ = dispose;
    bridge_ = bridge;
  }

private:
  JNIEnv* Environment() const noexcept {
    if (virtual_machine_ == nullptr) {
      return nullptr;
    }
    JNIEnv* environment = nullptr;
    if (virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
      return nullptr;
    }
    return environment;
  }

  void Dispose() noexcept {
    JNIEnv* environment = Environment();
    if (environment != nullptr && producer_ != nullptr) {
      environment->CallVoidMethod(producer_, dispose_);
      static_cast<void>(ClearJavaException(environment));
      environment->DeleteGlobalRef(producer_);
      producer_ = nullptr;
    }
    delete static_cast<std::weak_ptr<AndroidTextureDemo>*>(bridge_);
    bridge_ = nullptr;
    bitmap_texture_->Finish();
    gl_texture_->Finish();
    surface_texture_->Finish();
  }

  JavaVM* virtual_machine_ = nullptr;
  std::shared_ptr<huxerui::android::BitmapTexture> bitmap_texture_;
  std::shared_ptr<huxerui::android::GlTexture> gl_texture_;
  std::shared_ptr<huxerui::android::SurfaceStreamTexture> surface_texture_;
  std::vector<huxerui::example::TextureDemoEntry> entries_;
  jobject producer_ = nullptr;
  jmethodID set_running_ = nullptr;
  jmethodID dispose_ = nullptr;
  void* bridge_ = nullptr;
};

std::shared_ptr<huxerui::example::TextureDemo>
CreateAndroidTextureDemo(huxerui::PlatformAdapter&, JNIEnv* environment, jobject context) {
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK || virtual_machine == nullptr) {
    throw std::runtime_error("HuxerUI example could not access the Java VM");
  }

  auto demo = std::make_shared<AndroidTextureDemo>(environment, virtual_machine);
  auto bridge = std::make_unique<std::weak_ptr<AndroidTextureDemo>>(demo);
  huxerui::android::LocalRef<jclass> producer_class(environment, environment->FindClass(producer_class_name));
  if (!producer_class) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example could not find its Android external texture producer");
  }
  const jmethodID constructor =
      environment->GetMethodID(producer_class.Get(), "<init>", "(Landroid/content/Context;JLandroid/view/Surface;)V");
  const jmethodID set_running = environment->GetMethodID(producer_class.Get(), "setRunning", "(Z)V");
  const jmethodID dispose = environment->GetMethodID(producer_class.Get(), "disposePlatformBridge", "()V");
  if (constructor == nullptr || set_running == nullptr || dispose == nullptr || ClearJavaException(environment)) {
    throw std::logic_error("HuxerUI example Android external texture producer methods do not match the bridge");
  }

  huxerui::android::LocalRef<jobject> surface = demo->Surface(environment);
  huxerui::android::LocalRef<jobject> local_producer(
      environment, environment->NewObject(
                       producer_class.Get(), constructor, context,
                       static_cast<jlong>(reinterpret_cast<std::uintptr_t>(bridge.get())), surface.Get()
                   )
  );
  if (!local_producer || ClearJavaException(environment)) {
    throw std::logic_error("HuxerUI example could not create its Android external texture producer");
  }
  jobject producer = environment->NewGlobalRef(local_producer.Get());
  if (producer == nullptr || ClearJavaException(environment)) {
    if (producer != nullptr) {
      environment->DeleteGlobalRef(producer);
    }
    throw std::runtime_error("HuxerUI example could not retain its Android external texture producer");
  }
  demo->SetProducer(producer, set_running, dispose, bridge.release());
  demo->SetRunning(true);
  return demo;
}

std::shared_ptr<AndroidTextureDemo> LockDemo(jlong bridge) noexcept {
  auto* weak = reinterpret_cast<std::weak_ptr<AndroidTextureDemo>*>(static_cast<std::uintptr_t>(bridge));
  return weak == nullptr ? nullptr : weak->lock();
}

} // namespace

namespace huxerui::example {

void InstallTextureDemo(RootContext& root) {
  root.RegisterPlatformModule<std::shared_ptr<TextureDemo>>(
      texture_demo_module,
      android::PlatformModuleFactory<std::shared_ptr<TextureDemo>>{
          .create = CreateAndroidTextureDemo,
      }
  );
  root.Provide(root.OpenPlatformModule<std::shared_ptr<TextureDemo>>(texture_demo_module));
}

} // namespace huxerui::example

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_examples_externaltexture_ExternalTextureProducer_nativePublishBitmap(
    JNIEnv* environment, jclass, jlong bridge, jobject bitmap
) {
  try {
    if (const std::shared_ptr<AndroidTextureDemo> demo = LockDemo(bridge)) {
      demo->PublishBitmap(environment, bitmap);
    }
  } catch (const std::exception& error) {
    LogPublishError("BitmapTexture", error.what());
  } catch (...) {
    LogPublishError("BitmapTexture", "unknown error");
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_examples_externaltexture_ExternalTextureProducer_nativePublishGl(
    JNIEnv*, jclass, jlong bridge, jint texture_name, jint pixel_width, jint pixel_height
) {
  try {
    if (const std::shared_ptr<AndroidTextureDemo> demo = LockDemo(bridge)) {
      demo->PublishGl(
          static_cast<std::uint32_t>(texture_name), static_cast<int>(pixel_width), static_cast<int>(pixel_height)
      );
      return JNI_TRUE;
    }
  } catch (const std::exception& error) {
    LogPublishError("GlTexture", error.what());
  } catch (...) {
    LogPublishError("GlTexture", "unknown error");
  }
  return JNI_FALSE;
}
