#pragma once

#include <jni.h>

#include <memory>
#include <mutex>

#include "external_texture_internal.h"

namespace huxerui::detail {

class AndroidExternalTextureFrame final {
public:
  AndroidExternalTextureFrame() noexcept = default;
  AndroidExternalTextureFrame(
      JavaVM* virtual_machine, jobject bitmap, jint pixel_width, jint pixel_height, jint generation
  ) noexcept;
  ~AndroidExternalTextureFrame();

  AndroidExternalTextureFrame(const AndroidExternalTextureFrame&) = delete;
  AndroidExternalTextureFrame& operator=(const AndroidExternalTextureFrame&) = delete;
  AndroidExternalTextureFrame(AndroidExternalTextureFrame&& other) noexcept;
  AndroidExternalTextureFrame& operator=(AndroidExternalTextureFrame&& other) noexcept;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] jobject Bitmap() const noexcept;
  [[nodiscard]] jint PixelWidth() const noexcept;
  [[nodiscard]] jint PixelHeight() const noexcept;
  [[nodiscard]] jint Generation() const noexcept;

private:
  void Reset() noexcept;

  JavaVM* virtual_machine_ = nullptr;
  jobject bitmap_ = nullptr;
  jint pixel_width_ = 0;
  jint pixel_height_ = 0;
  jint generation_ = 0;
};

class AndroidExternalTextureState final : public ExternalTextureState {
public:
  [[nodiscard]] static std::shared_ptr<AndroidExternalTextureState> Create(Size intrinsic_size);
  ~AndroidExternalTextureState() override;

  void Publish(JNIEnv* environment, jobject bitmap);
  void Finish() noexcept;
  [[nodiscard]] AndroidExternalTextureFrame AcquireLatestFrame() noexcept;

private:
  explicit AndroidExternalTextureState(Size intrinsic_size) : ExternalTextureState(intrinsic_size) {}

  void InitializeJni(JNIEnv* environment);

  std::mutex frame_mutex_;
  AndroidExternalTextureFrame pending_frame_;
  JavaVM* virtual_machine_ = nullptr;
  jclass bitmap_class_ = nullptr;
  jmethodID bitmap_get_width_ = nullptr;
  jmethodID bitmap_get_height_ = nullptr;
  jmethodID bitmap_get_generation_ = nullptr;
  jmethodID bitmap_is_recycled_ = nullptr;
  bool finished_ = false;
};

} // namespace huxerui::detail
