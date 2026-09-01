#include <huxerui/android/external_texture.h>

#include <mutex>
#include <stdexcept>
#include <utility>

#include "android_external_texture_internal.h"

namespace huxerui::detail {

namespace {

void DeleteGlobalReference(JavaVM* virtual_machine, jobject reference) noexcept {
  if (virtual_machine == nullptr || reference == nullptr) {
    return;
  }
  JNIEnv* environment = nullptr;
  bool attached = false;
  const jint result = virtual_machine->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    attached = virtual_machine->AttachCurrentThread(&environment, nullptr) == JNI_OK;
  }
  if (environment != nullptr) {
    environment->DeleteGlobalRef(reference);
  }
  if (attached) {
    virtual_machine->DetachCurrentThread();
  }
}

} // namespace

AndroidBitmapFrame::AndroidBitmapFrame(
    JavaVM* virtual_machine, jobject bitmap, jint pixel_width, jint pixel_height, jint generation
) noexcept
    : virtual_machine_(virtual_machine), bitmap_(bitmap), pixel_width_(pixel_width), pixel_height_(pixel_height),
      generation_(generation) {}

AndroidBitmapFrame::~AndroidBitmapFrame() {
  DeleteGlobalReference(virtual_machine_, bitmap_);
}

AndroidBitmapFrame::operator bool() const noexcept {
  return bitmap_ != nullptr;
}

jobject AndroidBitmapFrame::Bitmap() const noexcept {
  return bitmap_;
}

jint AndroidBitmapFrame::PixelWidth() const noexcept {
  return pixel_width_;
}

jint AndroidBitmapFrame::PixelHeight() const noexcept {
  return pixel_height_;
}

jint AndroidBitmapFrame::Generation() const noexcept {
  return generation_;
}

} // namespace huxerui::detail

namespace huxerui::android {

struct BitmapTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const huxerui::detail::AndroidBitmapFrame> frame;
  JavaVM* virtual_machine = nullptr;
  jclass bitmap_class = nullptr;
  jmethodID bitmap_get_width = nullptr;
  jmethodID bitmap_get_height = nullptr;
  jmethodID bitmap_get_generation = nullptr;
  jmethodID bitmap_is_recycled = nullptr;
  bool finished = false;
};

BitmapTexture::BitmapTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

BitmapTexture::~BitmapTexture() {
  std::shared_ptr<const huxerui::detail::AndroidBitmapFrame> frame;
  JavaVM* virtual_machine = nullptr;
  jclass bitmap_class = nullptr;
  {
    std::lock_guard lock(storage_->mutex);
    frame = std::move(storage_->frame);
    virtual_machine = storage_->virtual_machine;
    bitmap_class = std::exchange(storage_->bitmap_class, nullptr);
  }
  huxerui::detail::DeleteGlobalReference(virtual_machine, bitmap_class);
}

void BitmapTexture::InitializeJni(JNIEnv* environment) {
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK || virtual_machine == nullptr) {
    throw std::runtime_error("HuxerUI could not access the Android Java VM for an external texture");
  }
  if (storage_->virtual_machine != nullptr) {
    if (storage_->virtual_machine != virtual_machine) {
      throw std::invalid_argument("HuxerUI Android external texture frames must belong to the same Java VM");
    }
    return;
  }

  jclass local_bitmap_class = environment->FindClass("android/graphics/Bitmap");
  if (local_bitmap_class == nullptr) {
    throw std::runtime_error("HuxerUI could not resolve android.graphics.Bitmap");
  }
  jclass bitmap_class = static_cast<jclass>(environment->NewGlobalRef(local_bitmap_class));
  environment->DeleteLocalRef(local_bitmap_class);
  if (bitmap_class == nullptr) {
    throw std::runtime_error("HuxerUI could not retain android.graphics.Bitmap");
  }

  const jmethodID bitmap_get_width = environment->GetMethodID(bitmap_class, "getWidth", "()I");
  const jmethodID bitmap_get_height = environment->GetMethodID(bitmap_class, "getHeight", "()I");
  const jmethodID bitmap_get_generation = environment->GetMethodID(bitmap_class, "getGenerationId", "()I");
  const jmethodID bitmap_is_recycled = environment->GetMethodID(bitmap_class, "isRecycled", "()Z");
  if (bitmap_get_width == nullptr || bitmap_get_height == nullptr || bitmap_get_generation == nullptr ||
      bitmap_is_recycled == nullptr) {
    environment->DeleteGlobalRef(bitmap_class);
    throw std::runtime_error("HuxerUI android.graphics.Bitmap methods do not match the platform backend");
  }
  storage_->bitmap_class = bitmap_class;
  storage_->bitmap_get_width = bitmap_get_width;
  storage_->bitmap_get_height = bitmap_get_height;
  storage_->bitmap_get_generation = bitmap_get_generation;
  storage_->bitmap_is_recycled = bitmap_is_recycled;
  storage_->virtual_machine = virtual_machine;
}

void BitmapTexture::Publish(JNIEnv* environment, jobject bitmap) {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android external texture JNI environment must not be null");
  }
  if (bitmap == nullptr) {
    throw std::invalid_argument("HuxerUI Android external texture Bitmap must not be null");
  }

  std::shared_ptr<const huxerui::detail::AndroidBitmapFrame> replaced;
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Android external texture is finished");
    }
    InitializeJni(environment);
    const jboolean is_bitmap = environment->IsInstanceOf(bitmap, storage_->bitmap_class);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture frame type");
    }
    if (is_bitmap != JNI_TRUE) {
      throw std::invalid_argument("HuxerUI Android external texture frame must be an android.graphics.Bitmap");
    }
    const jboolean recycled = environment->CallBooleanMethod(bitmap, storage_->bitmap_is_recycled);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap state");
    }
    if (recycled == JNI_TRUE) {
      throw std::invalid_argument("HuxerUI Android external texture Bitmap must not be recycled");
    }
    const jint pixel_width = environment->CallIntMethod(bitmap, storage_->bitmap_get_width);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap width");
    }
    const jint pixel_height = environment->CallIntMethod(bitmap, storage_->bitmap_get_height);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap height");
    }
    const jint generation = environment->CallIntMethod(bitmap, storage_->bitmap_get_generation);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap generation");
    }
    if (pixel_width <= 0 || pixel_height <= 0) {
      throw std::invalid_argument("HuxerUI Android external texture Bitmap dimensions must be positive");
    }
    jobject retained_bitmap = environment->NewGlobalRef(bitmap);
    if (retained_bitmap == nullptr) {
      throw std::runtime_error("HuxerUI could not retain the Android external texture Bitmap");
    }
    auto frame = std::make_shared<const huxerui::detail::AndroidBitmapFrame>(
        storage_->virtual_machine, retained_bitmap, pixel_width, pixel_height, generation
    );
    replaced = std::move(storage_->frame);
    storage_->frame = std::move(frame);
  }
  NotifyFrameAvailable();
}

void BitmapTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const huxerui::detail::AndroidBitmapFrame> BitmapTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

} // namespace huxerui::android
