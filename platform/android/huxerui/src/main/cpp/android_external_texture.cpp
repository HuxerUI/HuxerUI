#include <huxerui/android/external_texture.h>

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

AndroidExternalTextureFrame::AndroidExternalTextureFrame(
    JavaVM* virtual_machine, jobject bitmap, jint pixel_width, jint pixel_height, jint generation
) noexcept
    : virtual_machine_(virtual_machine), bitmap_(bitmap), pixel_width_(pixel_width), pixel_height_(pixel_height),
      generation_(generation) {}

AndroidExternalTextureFrame::~AndroidExternalTextureFrame() {
  Reset();
}

AndroidExternalTextureFrame::AndroidExternalTextureFrame(AndroidExternalTextureFrame&& other) noexcept
    : virtual_machine_(std::exchange(other.virtual_machine_, nullptr)), bitmap_(std::exchange(other.bitmap_, nullptr)),
      pixel_width_(std::exchange(other.pixel_width_, 0)), pixel_height_(std::exchange(other.pixel_height_, 0)),
      generation_(std::exchange(other.generation_, 0)) {}

AndroidExternalTextureFrame& AndroidExternalTextureFrame::operator=(AndroidExternalTextureFrame&& other) noexcept {
  if (this != &other) {
    Reset();
    virtual_machine_ = std::exchange(other.virtual_machine_, nullptr);
    bitmap_ = std::exchange(other.bitmap_, nullptr);
    pixel_width_ = std::exchange(other.pixel_width_, 0);
    pixel_height_ = std::exchange(other.pixel_height_, 0);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

AndroidExternalTextureFrame::operator bool() const noexcept {
  return bitmap_ != nullptr;
}

jobject AndroidExternalTextureFrame::Bitmap() const noexcept {
  return bitmap_;
}

jint AndroidExternalTextureFrame::PixelWidth() const noexcept {
  return pixel_width_;
}

jint AndroidExternalTextureFrame::PixelHeight() const noexcept {
  return pixel_height_;
}

jint AndroidExternalTextureFrame::Generation() const noexcept {
  return generation_;
}

void AndroidExternalTextureFrame::Reset() noexcept {
  DeleteGlobalReference(virtual_machine_, bitmap_);
  virtual_machine_ = nullptr;
  bitmap_ = nullptr;
  pixel_width_ = 0;
  pixel_height_ = 0;
  generation_ = 0;
}

std::shared_ptr<AndroidExternalTextureState> AndroidExternalTextureState::Create(Size intrinsic_size) {
  return std::shared_ptr<AndroidExternalTextureState>(new AndroidExternalTextureState(intrinsic_size));
}

AndroidExternalTextureState::~AndroidExternalTextureState() {
  AndroidExternalTextureFrame pending_frame;
  JavaVM* virtual_machine = nullptr;
  jclass bitmap_class = nullptr;
  {
    std::lock_guard lock(frame_mutex_);
    pending_frame = std::move(pending_frame_);
    virtual_machine = virtual_machine_;
    bitmap_class = std::exchange(bitmap_class_, nullptr);
  }
  DeleteGlobalReference(virtual_machine, bitmap_class);
}

void AndroidExternalTextureState::InitializeJni(JNIEnv* environment) {
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK || virtual_machine == nullptr) {
    throw std::runtime_error("HuxerUI could not access the Android Java VM for an external texture");
  }
  if (virtual_machine_ != nullptr) {
    if (virtual_machine_ != virtual_machine) {
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
  bitmap_class_ = bitmap_class;
  bitmap_get_width_ = bitmap_get_width;
  bitmap_get_height_ = bitmap_get_height;
  bitmap_get_generation_ = bitmap_get_generation;
  bitmap_is_recycled_ = bitmap_is_recycled;
  virtual_machine_ = virtual_machine;
}

void AndroidExternalTextureState::Publish(JNIEnv* environment, jobject bitmap) {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android external texture JNI environment must not be null");
  }
  if (bitmap == nullptr) {
    throw std::invalid_argument("HuxerUI Android external texture Bitmap must not be null");
  }

  AndroidExternalTextureFrame replaced;
  {
    std::lock_guard lock(frame_mutex_);
    if (finished_) {
      throw std::logic_error("HuxerUI Android external texture source is finished");
    }
    InitializeJni(environment);
    const jboolean is_bitmap = environment->IsInstanceOf(bitmap, bitmap_class_);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture frame type");
    }
    if (is_bitmap != JNI_TRUE) {
      throw std::invalid_argument("HuxerUI Android external texture frame must be an android.graphics.Bitmap");
    }
    const jboolean recycled = environment->CallBooleanMethod(bitmap, bitmap_is_recycled_);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap state");
    }
    if (recycled == JNI_TRUE) {
      throw std::invalid_argument("HuxerUI Android external texture Bitmap must not be recycled");
    }
    const jint pixel_width = environment->CallIntMethod(bitmap, bitmap_get_width_);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap width");
    }
    const jint pixel_height = environment->CallIntMethod(bitmap, bitmap_get_height_);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not inspect the Android external texture Bitmap height");
    }
    const jint generation = environment->CallIntMethod(bitmap, bitmap_get_generation_);
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
    AndroidExternalTextureFrame frame(virtual_machine_, retained_bitmap, pixel_width, pixel_height, generation);
    replaced = std::move(pending_frame_);
    pending_frame_ = std::move(frame);
  }
  NotifyFrameAvailable();
}

void AndroidExternalTextureState::Finish() noexcept {
  std::lock_guard lock(frame_mutex_);
  finished_ = true;
}

AndroidExternalTextureFrame AndroidExternalTextureState::AcquireLatestFrame() noexcept {
  std::lock_guard lock(frame_mutex_);
  return std::move(pending_frame_);
}

} // namespace huxerui::detail

namespace huxerui::android {

ExternalTextureSource::ExternalTextureSource(Size intrinsic_size)
    : state_(huxerui::detail::AndroidExternalTextureState::Create(intrinsic_size)) {}

ExternalTextureSource::~ExternalTextureSource() {
  Finish();
}

ExternalTextureSource::ExternalTextureSource(ExternalTextureSource&& other) noexcept
    : state_(std::move(other.state_)) {}

ExternalTextureSource& ExternalTextureSource::operator=(ExternalTextureSource&& other) noexcept {
  if (this != &other) {
    Finish();
    state_ = std::move(other.state_);
  }
  return *this;
}

ExternalTexture ExternalTextureSource::Texture() const noexcept {
  return state_ ? state_->Texture() : ExternalTexture{};
}

void ExternalTextureSource::Publish(JNIEnv* environment, jobject bitmap) {
  if (!state_) {
    throw std::logic_error("HuxerUI Android external texture source is empty");
  }
  state_->Publish(environment, bitmap);
}

void ExternalTextureSource::Finish() noexcept {
  if (state_) {
    state_->Finish();
  }
}

} // namespace huxerui::android
